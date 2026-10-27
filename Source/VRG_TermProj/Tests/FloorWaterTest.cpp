// Fill out your copyright notice in the Description page of Project Settings.

// W3 floor water (Tools/누수_실시간_계획.md L3, 2026-10-27): shallow water on the room floor. Analytic dam breaks
// (Ritter dry bed, Stoker wet bed; formulas as collected in SWASHES, Delestre et al. 2013, Int. J. Numer. Meth.
// Fluids 72, 269), a lake at rest over a bump (well balanced), the pinned puddle thickness, volume, determinism and the
// spray feeding the floor in a live room.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Simulation/Water/FloorWater.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgFloorFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// Frictionless, unpinned settings for the analytic checks.
	FFloorWaterSettings FloorIdeal(double Cell)
	{
		FFloorWaterSettings S;
		S.CellM = Cell; S.ManningN = 0.0; S.bContactLinePinning = false;
		return S;
	}

	// Stoker wet-bed dam break: middle-state celerity c_m from
	// -8 g h1 c^2 (c0 - c)^2 + (c^2 - g h1)^2 (c^2 + g h1) = 0, sqrt(g h1) < c < c0 (bisection).
	double StokerCm(double H0, double H1, double G)
	{
		const double C0 = FMath::Sqrt(G * H0), C1 = FMath::Sqrt(G * H1);
		auto F = [&](double C) { return -8.0 * G * H1 * C * C * FMath::Square(C0 - C) + FMath::Square(C * C - G * H1) * (C * C + G * H1); };
		double Lo = C1, Hi = C0;
		for (int32 It = 0; It < 200; ++It) { const double M = .5 * (Lo + Hi); (F(Lo) * F(M) <= 0.0 ? Hi : Lo) = M; }
		return .5 * (Lo + Hi);
	}

	// Exact depth at x of a dam at X0 released at t = 0 (H1 = 0: Ritter).
	double DamBreakDepth(double X, double X0, double T, double H0, double H1, double G)
	{
		const double C0 = FMath::Sqrt(G * H0);
		const double Xi = (X - X0) / T;
		if (Xi <= -C0) return H0;
		if (H1 <= 0.0)
		{
			if (Xi >= 2.0 * C0) return 0.0;
			return FMath::Square(2.0 * C0 - Xi) / (9.0 * G);
		}
		const double Cm = StokerCm(H0, H1, G);
		const double S = 2.0 * Cm * Cm * (C0 - Cm) / (Cm * Cm - G * H1);
		if (Xi <= 2.0 * C0 - 3.0 * Cm) return FMath::Square(2.0 * C0 - Xi) / (9.0 * G);
		if (Xi <= S) return Cm * Cm / G;
		return H1;
	}

	struct FDamResult { double L1 = 0.0, FrontM = 0.0, Residual = 0.0, Clipped = 0.0; int32 Substeps = 0; };
	// A 1D channel (3 cells wide, walls at the sides) 10 m long, dam at 5 m, run T seconds; L1 depth error over the
	// channel / (H0 * 10 m).
	FDamResult RunDam(double Cell, double H0, double H1, double T)
	{
		FFloorWater W;
		const int32 Nx = FMath::RoundToInt32(10.0 / Cell);
		W.Reset(FVector2D::ZeroVector, Nx, 3, 0.0, FloorIdeal(Cell));
		for (int32 J = 0; J < 3; ++J)
			for (int32 I = 0; I < Nx; ++I)
			{
				const double X = (I + .5) * Cell;
				W.SetState(I, J, X < 5.0 ? H0 : H1);
			}
		const int32 Steps = FMath::RoundToInt32(T / .02);
		for (int32 S = 0; S < Steps; ++S) W.Step(.02, S * .02);
		FDamResult R;
		const double G = W.GetSettings().GravityMs2;
		for (int32 I = 0; I < Nx; ++I)
		{
			const double X = (I + .5) * Cell;
			const double Hn = W.GetDepthM()[W.Index(I, 1)];
			R.L1 += FMath::Abs(Hn - DamBreakDepth(X, 5.0, T, H0, H1, G)) * Cell;
			if (Hn > 1e-3 * H0 && H1 <= 0.0) R.FrontM = X + .5 * Cell;
		}
		R.L1 /= H0 * 10.0;
		R.Residual = W.GetTotals().Residual; R.Clipped = W.GetTotals().ClippedM3; R.Substeps = W.GetTotals().Substeps;
		return R;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterDamBreakTest, "VRG.Water.Floor.DamBreakAnalytic", VrgFloorFlags)
bool FFloorWaterDamBreakTest::RunTest(const FString& Parameters)
{
	// 5 cm of water released for 1 s. Ritter: dry bed downstream, front 2 c0 t. Stoker: 1 cm downstream, a shock.
	const double H0 = .05, G = 9.81, T = 1.0;
	const FDamResult R5 = RunDam(.05, H0, 0.0, T), R25 = RunDam(.025, H0, 0.0, T), R125 = RunDam(.0125, H0, 0.0, T);
	const FDamResult S5 = RunDam(.05, H0, .01, T), S25 = RunDam(.025, H0, .01, T), S125 = RunDam(.0125, H0, .01, T);
	const double Front = 5.0 + 2.0 * FMath::Sqrt(G * H0) * T;
	const double Cm = StokerCm(H0, .01, G);
	AddInfo(FString::Printf(TEXT("Ritter L1/(h0 L): 5 cm %.3e, 2.5 cm %.3e, 1.25 cm %.3e (비 %.2f, %.2f) | 앞부분 %.3f / %.3f / %.3f m vs 해석 %.3f m | Stoker(h1 1 cm, c_m %.4f m/s, 가운데 수심 %.2f mm): %.3e, %.3e, %.3e (비 %.2f, %.2f) | 부피 잔차 최대 %.2e, 음수 보정 %.1e | 부분 스텝 %d"),
		R5.L1, R25.L1, R125.L1, R5.L1 / R25.L1, R25.L1 / R125.L1, R5.FrontM, R25.FrontM, R125.FrontM, Front, Cm, Cm * Cm / G * 1e3,
		S5.L1, S25.L1, S125.L1, S5.L1 / S25.L1, S25.L1 / S125.L1,
		FMath::Max(FMath::Max(R5.Residual, R25.Residual), FMath::Max(S5.Residual, S25.Residual)), FMath::Max(R25.Clipped, S25.Clipped), R25.Substeps));
	TestTrue(TEXT("Ritter: 2.5 cm error below 1 % of h0 L and falling with the cell size"), R25.L1 < .01 && R125.L1 < R25.L1 && R25.L1 < R5.L1);
	TestTrue(TEXT("Stoker: 2.5 cm error below 1 % of h0 L and falling with the cell size"), S25.L1 < .01 && S125.L1 < S25.L1 && S25.L1 < S5.L1);
	// The front measured where the depth falls to 1e-3 h0; the exact profile reaches that depth at
	// 2 c0 t - sqrt(9 g 1e-3 h0) t. First-order schemes are known to lag at a dry front (the thin tip moves slower).
	const double FrontAt = Front - FMath::Sqrt(9.0 * G * 1e-3 * H0) * T;
	AddInfo(FString::Printf(TEXT("Ritter 앞부분(수심 1e-3 h0) 해석 %.3f m, 2.5 cm 계산 %.3f m: 뒤처짐 %.3f m = 앞부분 이동의 %.1f %%"), FrontAt, R25.FrontM, FrontAt - R25.FrontM, (FrontAt - R25.FrontM) / (FrontAt - 5.0) * 100.0));
	TestTrue(TEXT("Ritter front (depth 1e-3 h0) within 15 % of its travel at 2.5 cm"), FMath::Abs(R25.FrontM - FrontAt) / (FrontAt - 5.0) < .15);
	TestTrue(TEXT("volume conserved (relative < 1e-12), no clipped depth"), R25.Residual < 1e-12 && S25.Residual < 1e-12 && R25.Clipped == 0.0 && S25.Clipped == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterLakeTest, "VRG.Water.Floor.LakeAtRest", VrgFloorFlags)
bool FFloorWaterLakeTest::RunTest(const FString& Parameters)
{
	// A 4 mm deep lake (surface 4 mm) over a bed with a smooth bump and an island that sticks out (dry cells):
	// the hydrostatic reconstruction must keep it exactly at rest.
	FFloorWater W;
	const int32 N = 80;
	const double Cell = .025, Eta = .004;
	W.Reset(FVector2D::ZeroVector, N, N, 0.0, FloorIdeal(Cell));
	for (int32 J = 0; J < N; ++J)
		for (int32 I = 0; I < N; ++I)
		{
			const double X = (I + .5) * Cell, Y = (J + .5) * Cell;
			const double Bump = .003 * FMath::Exp(-(FMath::Square(X - .6) + FMath::Square(Y - .6)) / .04);
			const double Island = .006 * FMath::Exp(-(FMath::Square(X - 1.4) + FMath::Square(Y - 1.3)) / .02);
			const double Bed = Bump + Island;
			W.SetBed(I, J, Bed);
			W.SetState(I, J, FMath::Max(0.0, Eta - Bed));
		}
	int32 Dry = 0;
	for (int32 C = 0; C < N * N; ++C) Dry += W.GetDepthM()[C] <= 0.0 ? 1 : 0;
	for (int32 S = 0; S < 500; ++S) W.Step(.02, S * .02);
	double MaxSpeed = 0.0, MaxEta = 0.0;
	for (int32 C = 0; C < N * N; ++C)
	{
		const double Hc = W.GetDepthM()[C];
		if (Hc <= 0.0) continue;
		MaxSpeed = FMath::Max(MaxSpeed, FMath::Sqrt(FMath::Square(W.GetHu()[C]) + FMath::Square(W.GetHv()[C])) / Hc);
		MaxEta = FMath::Max(MaxEta, FMath::Abs(Hc + W.GetBedM()[C] - Eta));
	}
	AddInfo(FString::Printf(TEXT("호수 10 s (%d칸, 마른 섬 %d칸): 최대 속도 %.3e m/s, 수면 최대 변화 %.3e m, 부분 스텝 %d, 부피 잔차 %.2e"), N * N, Dry, MaxSpeed, MaxEta, W.GetTotals().Substeps, W.GetTotals().Residual));
	TestTrue(TEXT("dry island exists"), Dry > 0);
	TestTrue(TEXT("lake at rest stays at rest (speed < 1e-12 m/s, surface < 1e-12 m)"), MaxSpeed < 1e-12 && MaxEta < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterPuddleTest, "VRG.Water.Floor.PuddleEquilibrium", VrgFloorFlags)
bool FFloorWaterPuddleTest::RunTest(const FString& Parameters)
{
	// 0.5 L poured slowly (over 50 s, quasi-static) at one point of an epoxy floor (Manning, pinned contact line). It
	// spreads, stops at the equilibrium thickness h_eq = 2 l_c sin(theta / 2), and keeps every drop. A wall line stops
	// it on one side.
	auto Pour = [](double Cell, bool bWall, FFloorWater& W)
	{
		FFloorWaterSettings S; S.CellM = Cell;
		const int32 N = FMath::RoundToInt32(3.0 / Cell);
		W.Reset(FVector2D::ZeroVector, N, N, 0.0, S);
		if (bWall) for (int32 J = 0; J < N; ++J) W.SetSolid(FMath::RoundToInt32(1.65 / Cell), J, true);   // 0.15 m from the pour: the puddle reaches it
		double WorstResidual = 0.0;
		for (int32 Step = 0; Step < 4000; ++Step)
		{
			if (Step < 2500) W.AddWater(FVector(1.5, 1.5, 0.0), .5e-3 / 2500.0);
			W.Step(.02, Step * .02);
			WorstResidual = FMath::Max(WorstResidual, W.GetTotals().Residual);
		}
		return WorstResidual;
	};
	FFloorWater A, B, C, D;
	const double ResA = Pour(.025, true, A);
	Pour(.025, true, B);
	Pour(.05, true, C);
	Pour(.025, false, D);
	const double Heq = FFloorWater::EquilibriumDepthM(A.GetSettings());
	auto Stats = [](const FFloorWater& W, double& Area, double& Mean, double& BeyondWall)
	{
		Area = 0.0; BeyondWall = 0.0;
		const double A2 = W.GetCellM() * W.GetCellM();
		for (int32 J = 0; J < W.GetNy(); ++J)
			for (int32 I = 0; I < W.GetNx(); ++I)
			{
				const double Hc = W.GetDepthM()[W.Index(I, J)];
				if (Hc > W.GetSettings().DryDepthM) Area += A2;
				if ((I + .5) * W.GetCellM() > 1.65 + W.GetCellM()) BeyondWall += Hc * A2;
			}
		Mean = Area > 0.0 ? W.GetTotals().StoredM3 / Area : 0.0;
	};
	double AreaA, MeanA, BeyondA, AreaC, MeanC, BeyondC, AreaD, MeanD, BeyondD;
	Stats(A, AreaA, MeanA, BeyondA); Stats(C, AreaC, MeanC, BeyondC); Stats(D, AreaD, MeanD, BeyondD);
	const bool bSame = A.GetDepthM() == B.GetDepthM() && A.GetHu() == B.GetHu() && A.GetHv() == B.GetHv();
	AddInfo(FString::Printf(TEXT("0.5 L 50 s에 붓고 80 s: h_eq %.3f mm(접촉각 70°, 추정) | 벽 있음 2.5 cm: 젖은 넓이 %.4f m2, 평균 두께 %.3f mm (h_eq 대비 %.3f), 최대 속도 %.2e m/s, 벽 너머 %.1e L | 5 cm: 넓이 %.4f m2, 평균 %.3f mm (2.5 cm 대비 넓이 %.3f) | 벽 없음: 넓이 %.4f m2, 평균 %.3f mm | 스텝마다 최대 부피 잔차 %.2e, 음수 보정 %.1e | 재실행 동일 %d"),
		Heq * 1e3, AreaA, MeanA * 1e3, MeanA / Heq, A.GetTotals().MaxSpeedMs, BeyondA * 1e3, AreaC, MeanC * 1e3, AreaC / AreaA, AreaD, MeanD * 1e3, ResA, A.GetTotals().ClippedM3, bSame ? 1 : 0));
	TestTrue(TEXT("the puddle stops near the equilibrium thickness (mean within 25 % of h_eq, at rest)"), FMath::Abs(MeanA / Heq - 1.0) < .25 && A.GetTotals().MaxSpeedMs < 1e-3);
	TestTrue(TEXT("grid convergence: 5 cm and 2.5 cm wet areas within 15 %"), FMath::Abs(AreaC / AreaA - 1.0) < .15);
	TestTrue(TEXT("the wall stops the water (and it did reach it: the free puddle is larger on that side)"), BeyondA == 0.0 && BeyondD > 0.0);
	TestTrue(TEXT("volume conserved every step (relative < 1e-12), nothing clipped"), ResA < 1e-12 && A.GetTotals().ClippedM3 == 0.0);
	TestTrue(TEXT("deterministic: two runs identical"), bSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterThinBarrierTest, "VRG.Water.Floor.ThinBarrierBlocks", VrgFloorFlags)
bool FFloorWaterThinBarrierTest::RunTest(const FString& Parameters)
{
	// A 2 cm curtain (the layout's barrier) standing on a cell boundary of the 2.5 cm grid, straight and turned 30 deg:
	// no cell centre falls inside 2 cm there, yet it must stop the water (found by the 2026-10-27 screen check).
	bool bOk = true;
	FString Line;
	for (const double Yaw : { 0.0, 30.0 })
	{
		FSprayGeometry G;
		G.RoomM = FBox(FVector::ZeroVector, FVector(2.0, 2.0, 3.0));
		FSprayObstacle& O = G.Obstacles.AddDefaulted_GetRef();
		O.CenterM = FVector(1.025, 1.0, 1.2); O.HalfM = FVector(.01, 1.2, 1.2); O.Kind = ESprayObstacleKind::Barrier;
		O.Rotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw));
		FFloorWater W;
		FFloorWaterSettings S;
		W.ResetForRoom(G, S);
		int32 SolidCells = 0;
		for (const uint8 Sd : W.GetSolid()) SolidCells += Sd;
		for (int32 Step = 0; Step < 1500; ++Step)
		{
			if (Step < 500) W.AddWater(FVector(.85, 1.0, 0.0), 1.0e-3 / 500.0);
			W.Step(.02, Step * .02);
		}
		// Water on the far side of the curtain's line (signed distance > 2 cells).
		const FVector2D N(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw)));
		double Beyond = 0.0, Near = 0.0;
		for (int32 J = 0; J < W.GetNy(); ++J)
			for (int32 I = 0; I < W.GetNx(); ++I)
			{
				const FVector2D P((I + .5) * W.GetCellM() - 1.025, (J + .5) * W.GetCellM() - 1.0);
				const double D = W.GetDepthM()[W.Index(I, J)] * W.GetCellM() * W.GetCellM();
				(FVector2D::DotProduct(P, N) > 2.0 * W.GetCellM() ? Beyond : Near) += D;
			}
		Line += FString::Printf(TEXT(" | %.0f°: 막는 칸 %d, 벽 앞 %.4f L, 벽 너머 %.1e L, 잔차 %.1e"), Yaw, SolidCells, Near * 1e3, Beyond * 1e3, W.GetTotals().Residual);
		bOk &= SolidCells > 0 && Beyond == 0.0 && Near > .99e-3 && W.GetTotals().Residual < 1e-12;
	}
	AddInfo(TEXT("2 cm 차단막(칸 경계 위)") + Line);
	TestTrue(TEXT("a 2 cm barrier between cell centres stops the water, straight and turned"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterCostTest, "VRG.Water.Floor.CostWholeRoomWet", VrgFloorFlags)
bool FFloorWaterCostTest::RunTest(const FString& Parameters)
{
	// Worst case for the CPU path: the whole L_Test-sized floor (13.5 x 10.3 m at 2.5 cm) wet, 5 mm with a 2 cm mound
	// moving. A measurement (D10, not a guarantee) that decides whether the GPU path is needed; checks the ledger only.
	FFloorWater W;
	FFloorWaterSettings S;
	const int32 Nx = 540, Ny = 412;
	W.Reset(FVector2D::ZeroVector, Nx, Ny, 0.0, S);
	for (int32 J = 0; J < Ny; ++J)
		for (int32 I = 0; I < Nx; ++I)
			W.SetState(I, J, .005 + .015 * FMath::Exp(-(FMath::Square((I - 270) * .025) + FMath::Square((J - 206) * .025)) / .25));
	const double T0 = FPlatformTime::Seconds();
	for (int32 Step = 0; Step < 50; ++Step) W.Step(.02, Step * .02);
	const double Ms = (FPlatformTime::Seconds() - T0) * 1e3 / 50.0;
	AddInfo(FString::Printf(TEXT("방 전체 젖음 %d칸: 스텝(0.02 s)당 %.2f ms, 부분 스텝 평균 %.1f, 최대 속도 %.3f m/s, 부피 잔차 %.2e (측정값, 보장 아님)"),
		Nx * Ny, Ms, W.GetTotals().Substeps / 50.0, W.GetTotals().MaxSpeedMs, W.GetTotals().Residual));
	TestTrue(TEXT("volume conserved (relative < 1e-12)"), W.GetTotals().Residual < 1e-12 && W.GetTotals().ClippedM3 == 0.0);
	return true;
}

namespace
{
	struct FFloorLiveRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		FFloorLiveRoom()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->bShowSpray = false; Sim->MaxStepsPerFrame = 16;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
			World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
		}
		~FFloorLiveRoom()
		{
			Sim->EnterLayoutMode();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 Fr = 0; Fr < FMath::RoundToInt32(Seconds / 0.1); ++Fr) Sim->Tick(.1f); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterLiveTest, "VRG.Water.Floor.SprayFeedsFloor", VrgFloorFlags)
bool FFloorWaterLiveTest::RunTest(const FString& Parameters)
{
	// The live room: a 5 mm header leak until the tank is empty. The floor gets exactly what the spray delivered to
	// it; the chain tank -> spray -> floor closes.
	FFloorLiveRoom R;
	R.Sim->EnterSimulationMode();
	R.Run(0.5);
	int32 Site = INDEX_NONE;
	for (int32 I = 0; I < R.Sim->GetLiveLeakSites().Num(); ++I) if (R.Sim->GetLiveLeakSites()[I].Label == TEXT("랙 0 공급 헤더")) Site = I;
	R.Sim->SetLiveLeak(Site, 5.0e-3, TEXT("test"));
	R.Run(12.0);
	const FLeakSpray& S = R.Sim->GetLeakSpray();
	const FFloorWater& F = R.Sim->GetFloorWater();
	double Tank = 0.0;
	for (const FCoolantInventory::FTank& T : R.Room->GetCoolantInventory().GetTanks()) Tank += T.State.LeakedM3;
	const double FloorMatch = FMath::Abs(F.GetTotals().ReceivedM3 + R.Sim->GetFloorOffGridM3() - S.GetTotals().FloorM3) / S.GetTotals().FloorM3;
	const double Chain = FMath::Abs(Tank - (S.GetTotals().AirborneM3 + S.GetTotals().WaitingM3 + S.GetTotals().FilmM3 + S.GetTotals().GroundM3 + F.GetTotals().StoredM3 + R.Sim->GetFloorOffGridM3())) / Tank;
	int32 Flooded = 0;
	FString Names;
	for (const FFloorFlood& Fl : F.GetFloods()) if (Fl.FirstWetS >= 0.0) { ++Flooded; Names += FString::Printf(TEXT(" %s(최고 %.2f mm, t=%.2f s)"), *Fl.Name, Fl.PeakDepthM * 1e3, Fl.FirstWetS); }
	const double Heq = FFloorWater::EquilibriumDepthM(F.GetSettings());
	AddInfo(FString::Printf(TEXT("탱크 누적 %.4f L, 분사 바닥 %.4f L, 바닥 받음 %.4f L(격자 밖 %.4f) - 상대 %.2e | 탱크→분사→바닥 사슬 %.2e | 바닥 물 %.4f L, 젖은 칸 %d(%.3f m2), 최대 수심 %.2f mm(h_eq %.2f), 최대 속도 %.3f m/s, 잔차 %.2e | 물 밑 장비 %d:%s | 격자 %dx%d"),
		Tank * 1e3, S.GetTotals().FloorM3 * 1e3, F.GetTotals().ReceivedM3 * 1e3, R.Sim->GetFloorOffGridM3() * 1e3, FloorMatch, Chain, F.GetTotals().StoredM3 * 1e3,
		F.GetTotals().WetCells, F.GetTotals().WetCells * F.GetCellM() * F.GetCellM(), F.GetTotals().MaxDepthM * 1e3, Heq * 1e3, F.GetTotals().MaxSpeedMs, F.GetTotals().Residual,
		Flooded, *Names, F.GetNx(), F.GetNy()));
	TestTrue(TEXT("the floor got exactly what the spray put on it (relative < 1e-12)"), FloorMatch < 1e-12);
	TestTrue(TEXT("tank -> spray -> floor ledger closes (relative < 1e-12)"), Chain < 1e-12);
	TestTrue(TEXT("the water spread over the floor and the floor ledger closes"), F.GetTotals().WetCells > 100 && F.GetTotals().Residual < 1e-12 && F.GetTotals().ClippedM3 == 0.0);
	TestTrue(TEXT("plausibility: floor water a few mm deep at most"), F.GetTotals().MaxDepthM < .02);
	return true;
}

#endif
