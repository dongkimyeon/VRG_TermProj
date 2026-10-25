// Fill out your copyright notice in the Description page of Project Settings.

// W2 spray (Tools/누수_실시간_계획.md L2, 2026-10-26): the water that leaves a leak hole - jet, drops, impacts, films.
// Analytic and correlation checks on FLeakSpray alone, then the live room: the spray gets exactly the volume the
// hydraulics booked (rule 6) and its ledger closes.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Simulation/Water/LeakSpray.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgSprayFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// Gauge pressure that gives a discharge speed v (inverse of v = Cv sqrt(2 p / rho)).
	double SprayPressureFor(double SpeedMs, const FSpraySettings& S, const FSprayFluid& F)
	{
		return .5 * F.WaterDensityKgM3 * FMath::Square(SpeedMs / S.VelocityCoefficient);
	}

	FSpraySource SpraySourceAt(int32 Id, const FVector& OriginM, const FVector& VelocityMs, double VolumeM3, const FSpraySettings& S, const FSprayFluid& F)
	{
		FSpraySource Src;
		Src.Id = Id; Src.OriginM = OriginM; Src.DirectionM = VelocityMs.GetSafeNormal();
		Src.GaugePressurePa = SprayPressureFor(VelocityMs.Size(), S, F); Src.VolumeM3 = VolumeM3;
		return Src;
	}

	// Runs a spray with the given sources for the first step only, then N more steps.
	void SprayRun(FLeakSpray& Spray, const TArray<FSpraySource>& First, int32 Steps, double DtS)
	{
		Spray.Step(First, DtS, 0.0);
		for (int32 I = 1; I <= Steps; ++I) Spray.Step({}, DtS, I * DtS);
	}

	FSprayGeometry SprayRoom(const FVector& MaxM)
	{
		FSprayGeometry G;
		G.RoomM = FBox(FVector::ZeroVector, MaxM);
		return G;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayBallisticTest, "VRG.Water.Spray.BallisticAnalytic", VrgSprayFlags)
bool FLeakSprayBallisticTest::RunTest(const FString& Parameters)
{
	// No air drag, no spread: every parcel follows the exact parabola, so the landing point and time are analytic.
	FSpraySettings S; S.bAirDrag = false; S.BreakupSpreadRad = 0.0;
	const FSprayFluid F;
	FSprayGeometry G = SprayRoom(FVector(10, 10, 4));
	FSprayObstacle& Box = G.Obstacles.AddDefaulted_GetRef();
	Box.CenterM = FVector(5, 2, .5); Box.HalfM = FVector(.5, .5, .5); Box.Rotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(30.0)); Box.Name = TEXT("box");
	FLeakSpray Spray;
	Spray.Reset(G, S, F);
	const double Gr = F.GravityMs2, Dt = .02;
	// 0: floor from (1, 5, 1) with (3, 0, 2) m/s. 1: wall x = 10 from (8, 5, 1) with (4, 0, 3). 2: rotated box top from (5, 2, 3) straight down at 1 m/s.
	TArray<FSpraySource> Src = { SpraySourceAt(0, FVector(1, 5, 1), FVector(3, 0, 2), 1e-7, S, F), SpraySourceAt(1, FVector(8, 5, 1), FVector(4, 0, 3), 1e-7, S, F),
		SpraySourceAt(2, FVector(5, 2, 3), FVector(0, 0, -1), 1e-7, S, F) };
	SprayRun(Spray, Src, 200, Dt);
	const auto& Hits = Spray.GetFirstImpacts();
	const double T0 = (2.0 + FMath::Sqrt(4.0 + 2.0 * Gr * 1.0)) / Gr;
	const FVector P0(1 + 3 * T0, 5, 0);
	const double T1 = 2.0 / 4.0;
	const FVector P1(10, 5, 1 + 3 * T1 - .5 * Gr * T1 * T1);
	const double T2 = (-1.0 + FMath::Sqrt(1.0 + 2.0 * Gr * 2.0)) / Gr;   // 3 - t - g t^2 / 2 = 1
	const FVector P2(5, 2, 1);
	auto Err = [&Hits](int32 Id, const FVector& P, double T, double& OutP, double& OutT, ESpraySurface& OutS)
	{
		const FLeakSpray::FFirstImpact* H = Hits.Find(Id);
		OutP = H ? FVector::Dist(H->PointM, P) / P.Size() : 1.0; OutT = H ? FMath::Abs(H->TimeS - T) / T : 1.0; OutS = H ? H->Surface : ESpraySurface::Ground;
	};
	double EP0, ET0, EP1, ET1, EP2, ET2; ESpraySurface S0, S1, S2;
	Err(0, P0, T0, EP0, ET0, S0); Err(1, P1, T1, EP1, ET1, S1); Err(2, P2, T2, EP2, ET2, S2);
	AddInfo(FString::Printf(TEXT("바닥 착지 (%.6f, %.1f, 0) m t=%.6f s: 위치 %.3e, 시각 %.3e | 벽 (10, 5, %.6f) t=%.4f s: %.3e, %.3e | 30° 돌린 상자 윗면 t=%.6f s: %.3e, %.3e | 잔차 %.3e"),
		P0.X, P0.Y, T0, EP0, ET0, P1.Z, T1, EP1, ET1, T2, EP2, ET2, Spray.GetTotals().Residual));
	TestTrue(TEXT("floor landing = ballistic solution (relative < 1e-9)"), S0 == ESpraySurface::Floor && EP0 < 1e-9 && ET0 < 1e-9);
	TestTrue(TEXT("wall impact = ballistic solution (relative < 1e-9)"), S1 == ESpraySurface::Wall && EP1 < 1e-9 && ET1 < 1e-9);
	TestTrue(TEXT("rotated box top = ballistic solution (relative < 1e-9)"), S2 == ESpraySurface::Equipment && EP2 < 1e-9 && ET2 < 1e-9);
	TestTrue(TEXT("ledger closes"), Spray.GetTotals().Residual < 1e-12);

	// Air drag: a drop falling from rest reaches its terminal speed. Plausibility against measured raindrops
	// (Gunn & Kinzer 1949, J. Meteorol. 6, 243: 1.0 mm 4.03 m/s, 2.0 mm 6.49 m/s, air near 20 °C), and the model's own
	// balance m g = Cd(Re) rho_a A v^2 / 2 (consistency).
	double Worst = 0.0, WorstOwn = 0.0;
	FString Line;
	for (const TPair<double, double>& Case : { TPair<double, double>(1.0e-3, 4.03), TPair<double, double>(2.0e-3, 6.49) })
	{
		FSpraySettings SD; SD.DripDiameterM = Case.Key;
		FLeakSpray Drop;
		Drop.Reset(SprayRoom(FVector(10, 10, 200)), SD, F);
		FSpraySource Src0; Src0.Id = 0; Src0.OriginM = FVector(5, 5, 199); Src0.GaugePressurePa = 0.0; Src0.VolumeM3 = 1e-9;   // drips: zero pressure
		SprayRun(Drop, { Src0 }, 1500, .01);   // 15 s: many settling times (v_t / g < 0.8 s)
		const double V = Drop.GetParcelVelocitiesMs().Num() ? Drop.GetParcelVelocitiesMs()[0].Size() : 0.0;
		// Own terminal speed: fixed point of v = sqrt(4 g D rho_w / (3 Cd(Re) rho_a)).
		double Vt = 5.0;
		for (int32 It = 0; It < 200; ++It)
			Vt = FMath::Sqrt(4.0 * Gr * Case.Key * F.WaterDensityKgM3 / (3.0 * FLeakSpray::DropDragCoefficient(F.AirDensityKgM3 * Vt * Case.Key / F.AirViscosityPaS) * F.AirDensityKgM3));
		Worst = FMath::Max(Worst, FMath::Abs(V - Case.Value) / Case.Value);
		WorstOwn = FMath::Max(WorstOwn, FMath::Abs(V - Vt) / Vt);
		Line += FString::Printf(TEXT(" %.1f mm: 15 s 뒤 %.3f m/s (모델 종단 %.3f, Gunn-Kinzer %.2f)"), Case.Key * 1e3, V, Vt, Case.Value);
	}
	AddInfo(FString::Printf(TEXT("종단 속도:%s | 실측 대비 최대 %.3f, 모델 자체 균형 대비 %.2e"), *Line, Worst, WorstOwn));
	TestTrue(TEXT("drag: the drop settles at the model's own terminal speed (relative < 1e-6)"), WorstOwn < 1e-6);
	TestTrue(TEXT("drag plausibility: terminal speed within 10 % of the measured raindrops"), Worst < .1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayBreakupTest, "VRG.Water.Spray.BreakupAndDrops", VrgSprayFlags)
bool FLeakSprayBreakupTest::RunTest(const FString& Parameters)
{
	// Two 2 mm jets: slow (the Rayleigh drop 1.89 d survives) and fast (the air caps the drop at the critical Weber).
	const FSpraySettings S;
	const FSprayFluid F;
	const double Dt = .002, D = 2.0e-3;
	FLeakSpray Spray;
	Spray.Reset(SprayRoom(FVector(400, 20, 400)), S, F);
	TArray<FSpraySource> Src;
	for (const TPair<int32, double>& Case : { TPair<int32, double>(0, 3.0), TPair<int32, double>(1, 30.0) })
		Src.Add(SpraySourceAt(Case.Key, FVector(1, 10, 390), FVector(Case.Value, 0, 0), Case.Value * PI * D * D / 4.0 * Dt, S, F));
	SprayRun(Spray, Src, 3000, Dt);
	bool bOk = true;
	FString Line;
	for (const TPair<int32, double>& Case : { TPair<int32, double>(0, 3.0), TPair<int32, double>(1, 30.0) })
	{
		const FLeakSpray::FFirstBreakup* B = Spray.GetFirstBreakups().Find(Case.Key);
		if (B == nullptr) { bOk = false; continue; }
		const double We = F.WaterDensityKgM3 * Case.Value * Case.Value * D / F.SurfaceTensionNm;
		const double Lb = D * 8.51 * FMath::Pow(We, .32);
		const double Expected = FMath::Min(S.RayleighDropRatio * B->JetDiameterM, S.CriticalWeber * F.SurfaceTensionNm / (F.AirDensityKgM3 * B->SpeedMs * B->SpeedMs));
		const double JetErr = FMath::Abs(B->JetDiameterM - D) / D;
		const double LbErr = FMath::Abs(B->BreakupLengthM - Lb) / Lb;
		const bool bWithinStep = B->PathM >= Lb && B->PathM - Lb <= B->StepTravelM;
		const double DropErr = FMath::Abs(B->DropDiameterM - Expected) / Expected;
		const double GasWe = F.AirDensityKgM3 * B->SpeedMs * B->SpeedMs * B->DropDiameterM / F.SurfaceTensionNm;
		bOk &= JetErr < 1e-9 && LbErr < 1e-12 && bWithinStep && DropErr < 1e-12 && GasWe <= S.CriticalWeber * (1 + 1e-12);
		Line += FString::Printf(TEXT(" | %.0f m/s: 물줄기 지름 %.4f mm(오차 %.1e), We %.0f, L_b %.4f m (= %.1f d), 분열 경로 %.4f m(스텝 이동 %.4f m), 액적 %.3f mm = %s, 기체 We %.2f"),
			Case.Value, B->JetDiameterM * 1e3, JetErr, We, Lb, Lb / D, B->PathM, B->StepTravelM, B->DropDiameterM * 1e3,
			B->DropDiameterM < S.RayleighDropRatio * D - 1e-12 ? TEXT("임계 We 상한") : TEXT("Rayleigh 1.89 d"), GasWe);
	}
	AddInfo(TEXT("분열") + Line);
	TestTrue(TEXT("jet diameter from continuity, breakup at L_b (within one step), drop size = min(Rayleigh, critical Weber)"), bOk);
	TestTrue(TEXT("the slow jet gives Rayleigh drops, the fast one air-limited drops"),
		Spray.GetFirstBreakups().Contains(0) && Spray.GetFirstBreakups().Contains(1)
		&& FMath::IsNearlyEqual(Spray.GetFirstBreakups()[0].DropDiameterM, S.RayleighDropRatio * D, 1e-12)
		&& Spray.GetFirstBreakups()[1].DropDiameterM < S.RayleighDropRatio * D);
	// In flight: a drop above the critical Weber shrinks to it (secondary breakup), none stays above.
	double WorstWe = 0.0;
	for (int32 I = 0; I < Spray.GetParcelDiametersM().Num(); ++I)
		if (!Spray.GetParcelIsJet()[I])
		{
			const double V = Spray.GetParcelVelocitiesMs()[I].Size();
			WorstWe = FMath::Max(WorstWe, F.AirDensityKgM3 * V * V * Spray.GetParcelDiametersM()[I] / F.SurfaceTensionNm);
		}
	// Splash threshold (Mundo et al. 1995): K = We^0.5 Re^0.25 = 57.7, on both sides.
	const double Dk = 1.0e-3;
	double Vk = 1.0;
	for (int32 It = 0; It < 100; ++It)
	{
		const double K = FMath::Sqrt(F.WaterDensityKgM3 * Vk * Vk * Dk / F.SurfaceTensionNm) * FMath::Pow(F.WaterDensityKgM3 * Vk * Dk / F.WaterViscosityPaS, .25);
		Vk *= FMath::Pow(S.SplashK / K, 1.0 / 1.25);   // K ~ v^1.25
	}
	const bool bBelow = !FLeakSpray::Splashes(Vk * .999, Dk, S, F), bAbove = FLeakSpray::Splashes(Vk * 1.001, Dk, S, F);
	AddInfo(FString::Printf(TEXT("공중 액적 최대 기체 We %.3f (임계 %.0f) | 1 mm 액적 튐 문턱 속도 %.3f m/s: 0.1%% 아래 %s, 위 %s"),
		WorstWe, S.CriticalWeber, Vk, bBelow ? TEXT("안 튐") : TEXT("튐"), bAbove ? TEXT("튐") : TEXT("안 튐")));
	TestTrue(TEXT("no airborne drop above the critical Weber number"), WorstWe <= S.CriticalWeber * (1 + 1e-9));
	TestTrue(TEXT("splash threshold K = 57.7 on both sides"), bBelow && bAbove);
	TestTrue(TEXT("ledger closes"), Spray.GetTotals().Residual < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayFilmTest, "VRG.Water.Spray.FilmNusselt", VrgSprayFlags)
bool FLeakSprayFilmTest::RunTest(const FString& Parameters)
{
	// A gentle jet (below the splash threshold) onto a 1 m wide box face keeps a steady film: its thickness must reach
	// the Nusselt solution delta = (3 mu Gamma / (rho g))^(1/3) with Gamma = inflow / width.
	const FSpraySettings S;
	const FSprayFluid F;
	FSprayGeometry G = SprayRoom(FVector(10, 10, 4));
	FSprayObstacle& Box = G.Obstacles.AddDefaulted_GetRef();
	Box.CenterM = FVector(5, 5, 1); Box.HalfM = FVector(.5, .5, 1); Box.Name = TEXT("cabinet");
	FLeakSpray Spray;
	Spray.Reset(G, S, F);
	const double Dt = .02, FlowM3s = 1.0e-5;
	double SteadyIn = 0.0;
	for (int32 I = 0; I < 15000; ++I)
	{
		Spray.Step({ SpraySourceAt(0, FVector(4.45, 5, 1.2), FVector(.5, 0, 0), FlowM3s * Dt, S, F) }, Dt, I * Dt);
		SteadyIn = FlowM3s;
	}
	const FSprayFilm* Film = Spray.GetFilms().Num() ? &Spray.GetFilms()[0] : nullptr;
	const double Gamma = SteadyIn / 1.0;
	const double Delta = FLeakSpray::NusseltThicknessM(Gamma, F);
	const double Err = Film ? FMath::Abs(Film->LastThicknessM - Delta) / Delta : 1.0;
	const double OutErr = Film ? FMath::Abs(Film->LastOutflowM3s - FlowM3s) / FlowM3s : 1.0;
	// The jet leaves at 1.2 m and falls on its 0.05 m way to the face: it lands at 1.2 - g (0.05 / 0.5)^2 / 2.
	const double HitZ = 1.2 - .5 * F.GravityMs2 * FMath::Square(.05 / .5);
	const double WettedErr = Film ? FMath::Abs(Film->WetHeightM - HitZ) : 1.0;
	AddInfo(FString::Printf(TEXT("물막 %d개, 폭 %.3f m, 젖은 높이 %.4f m, 두께 %.4f mm vs Nusselt %.4f mm (상대 %.2e), 흘러내림 %.4e vs 유입 %.4e m3/s (%.2e) | 튐 %d | 잔차 %.2e | 젖음 처음 %.2f s, 높이 %.3f m"),
		Spray.GetFilms().Num(), Film ? Film->WidthM : 0.0, Film ? Film->WetHeightM : 0.0, Film ? Film->LastThicknessM * 1e3 : 0.0, Delta * 1e3, Err,
		Film ? Film->LastOutflowM3s : 0.0, FlowM3s, OutErr, Spray.GetTotals().Splashes, Spray.GetTotals().Residual,
		Spray.GetWetting()[0].FirstWetS, Spray.GetWetting()[0].TopWetZM));
	TestTrue(TEXT("one film on the hit face (1 m wide), wetted from the impact height down"), Spray.GetFilms().Num() == 1 && Film && FMath::IsNearlyEqual(Film->WidthM, 1.0) && WettedErr < 1e-9);
	TestTrue(TEXT("no splash below the threshold"), Spray.GetTotals().Splashes == 0);
	TestTrue(TEXT("steady film thickness = Nusselt (relative < 1e-6) and outflow = inflow"), Err < 1e-6 && OutErr < 1e-6);
	TestTrue(TEXT("the box is recorded wet"), Spray.GetWetting()[0].FirstWetS >= 0.0 && FMath::Abs(Spray.GetWetting()[0].TopWetZM - HitZ) < 1e-9);
	TestTrue(TEXT("ledger closes"), Spray.GetTotals().Residual < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayLedgerTest, "VRG.Water.Spray.VolumeLedgerDeterministic", VrgSprayFlags)
bool FLeakSprayLedgerTest::RunTest(const FString& Parameters)
{
	// Everything at once (splashes, breakup, drips off a plate, films, the parcel limit, an outdoor hole): the water is
	// conserved at every step, and two runs give the same parcels bit for bit (fixed seeds, no hidden state).
	FSpraySettings S; S.MaxParcels = 60;
	const FSprayFluid F;
	FSprayGeometry G = SprayRoom(FVector(12, 8, 4));
	G.GroundZM = 0.0;
	auto Add = [&G](const FVector& C, const FVector& H, double YawDeg, ESprayObstacleKind K)
	{
		FSprayObstacle& O = G.Obstacles.AddDefaulted_GetRef();
		O.CenterM = C; O.HalfM = H; O.Rotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(YawDeg)); O.Kind = K; O.Name = TEXT("o");
	};
	Add(FVector(3, 3, 1.1), FVector(.6, .3, 1.1), 0, ESprayObstacleKind::Equipment);
	Add(FVector(6, 3, 1.1), FVector(.6, .3, 1.1), 90, ESprayObstacleKind::Equipment);
	Add(FVector(4.5, 5, 1.5), FVector(1.5, .01, 1.5), 15, ESprayObstacleKind::Barrier);
	Add(FVector(5, 4, 3.0), FVector(2, 2, .125), 0, ESprayObstacleKind::Ceiling);
	double MaxWaitingM3 = 0.0;
	auto RunOnce = [&](FLeakSpray& Spray, double& WorstResidual)
	{
		Spray.Reset(G, S, F);
		WorstResidual = 0.0;
		const double Dt = .02;
		for (int32 I = 0; I < 1000; ++I)
		{
			TArray<FSpraySource> Src;
			if (I < 500)
				for (int32 K = 0; K < 6; ++K)
				{
					FSpraySource& One = Src.AddDefaulted_GetRef();
					One.Id = K; One.OriginM = K < 5 ? FVector(2 + K, 2 + .5 * K, .4 + .5 * K) : FVector(14, 4, 2);   // the last one outdoors
					// one jet up into the plate, one sideways onto a cabinet face
					One.DirectionM = K == 2 ? FVector(0, 0, 1) : K == 3 ? FVector(1, 0, 0) : FLeakSpray::ConeDirection(1000u + K, S.ConeHalfAngleRad);
					One.GaugePressurePa = 5.0e4 + 4.0e4 * K; One.VolumeM3 = (1.0 + K) * 1e-4 * Dt;
				}
			Spray.Step(Src, Dt, I * Dt);
			WorstResidual = FMath::Max(WorstResidual, Spray.GetTotals().Residual);
			MaxWaitingM3 = FMath::Max(MaxWaitingM3, Spray.GetTotals().WaitingM3);
		}
	};
	FLeakSpray A, B;
	double ResA = 0.0, ResB = 0.0;
	RunOnce(A, ResA);
	RunOnce(B, ResB);
	const FSprayTotals& T = A.GetTotals();
	const bool bSame = A.GetParcelPositionsM() == B.GetParcelPositionsM() && A.GetParcelVolumesM3() == B.GetParcelVolumesM3()
		&& T.FloorM3 == B.GetTotals().FloorM3 && T.FilmM3 == B.GetTotals().FilmM3 && T.GroundM3 == B.GetTotals().GroundM3;
	double MaxCone = -1.0;
	bool bConeOk = true;
	for (uint32 Seed = 0; Seed < 2000; ++Seed)
	{
		const FVector D = FLeakSpray::ConeDirection(Seed, S.ConeHalfAngleRad);
		bConeOk &= FMath::IsNearlyEqual(D.Size(), 1.0, 1e-6) && D.Z <= -FMath::Cos(S.ConeHalfAngleRad) + 1e-6;
		MaxCone = FMath::Max(MaxCone, FMath::RadiansToDegrees(FMath::Acos(-D.Z)));
	}
	int32 FilmsWet = 0;
	for (const FSprayWetting& W : A.GetWetting()) FilmsWet += W.FirstWetS >= 0.0 ? 1 : 0;
	AddInfo(FString::Printf(TEXT("누적 %.3f L = 공중 %.3f + 대기 %.3f + 물막 %.3f + 바닥 %.3f + 바깥 땅 %.3f L | 입자 %d(상한 %d), 구멍 대기 최대 %.3f L, 분열 %d, 튐 %d, 물막 %d개, 젖은 상자 %d | 스텝마다 최대 잔차 %.2e / %.2e, 두 실행 동일 %d | 원뿔 방향 2000개 최대 %.2f° (반각 %.0f°)"),
		T.EmittedM3 * 1e3, T.AirborneM3 * 1e3, T.WaitingM3 * 1e3, T.FilmM3 * 1e3, T.FloorM3 * 1e3, T.GroundM3 * 1e3, T.Parcels, S.MaxParcels, MaxWaitingM3 * 1e3, T.Breakups, T.Splashes,
		A.GetFilms().Num(), FilmsWet, ResA, ResB, bSame ? 1 : 0, MaxCone, FMath::RadiansToDegrees(S.ConeHalfAngleRad)));
	TestTrue(TEXT("water conserved at every step (relative < 1e-12)"), ResA < 1e-12 && ResB < 1e-12);
	TestTrue(TEXT("every path was used: films, floor, ground, splashes, the parcel limit"), T.FilmM3 > 0.0 && T.FloorM3 > 0.0 && T.GroundM3 > 0.0 && T.Splashes > 0 && MaxWaitingM3 > 0.0);
	TestTrue(TEXT("deterministic: two runs identical"), bSame);
	TestTrue(TEXT("default directions lie in the downward cone"), bConeOk);
	return true;
}

namespace
{
	// The W1 test room: one row of two GB200 racks + one CDU + air.
	struct FSprayLiveRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;

		explicit FSprayLiveRoom(bool bPlant)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false;
			Sim->bShowSpray = false;
			Sim->MaxStepsPerFrame = 16;
			Sim->bFacilityPlant = bPlant;
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
		~FSprayLiveRoom()
		{
			Sim->EnterLayoutMode();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 Fr = 0; Fr < FMath::RoundToInt32(Seconds / 0.1); ++Fr) Sim->Tick(.1f); }
		int32 SiteOf(const FString& Label) const
		{
			const auto& Sites = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < Sites.Num(); ++I) if (Sites[I].Label == Label) return I;
			return INDEX_NONE;
		}
		double TanksLeakedM3() const
		{
			double Sum = 0.0;
			for (const FCoolantInventory::FTank& T : Room->GetCoolantInventory().GetTanks()) Sum += T.State.LeakedM3;
			return Sum;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayLiveRoomTest, "VRG.Water.Spray.LiveLeakFeedsSpray", VrgSprayFlags)
bool FLeakSprayLiveRoomTest::RunTest(const FString& Parameters)
{
	FSprayLiveRoom R(false);
	R.Sim->EnterSimulationMode();
	R.Run(0.5);
	const FLeakSpray& Spray = R.Sim->GetLeakSpray();
	const bool bQuiet = !Spray.HasWater() && Spray.GetTotals().Parcels == 0;
	int32 Inside = 0;
	for (const auto& Site : R.Sim->GetLiveLeakSites()) Inside += Spray.FindObstacleContaining(Site.PositionM) != INDEX_NONE ? 1 : 0;
	const int32 Site = R.SiteOf(TEXT("랙 0 공급 헤더"));
	R.Sim->SetLiveLeak(Site, 5.0e-3, TEXT("test"));
	R.Run(2.0);
	// The jet against the orifice: its area Q / v0 must be (Cd / Cv) x the hole area (the same pressure both used).
	const FHydraulicNetwork& Net = R.Room->GetNetwork();
	const int32 NetLeak = R.Room->GetLeakNetworkIndex(R.Sim->GetLiveLeakSites()[Site].RoomSpec);
	const FHydraulicLeak& Leak = Net.GetLeak(NetLeak);
	const double V0 = FLeakSpray::JetVelocityMs(Net.GetNodePressurePa(Leak.NodeIndex) - Leak.AmbientPressurePa, Spray.GetSettings(), Spray.GetFluid());
	const double JetArea = Net.GetLeakFlowM3s(NetLeak) / V0;
	const double OrificeRel = FMath::Abs(JetArea / (PI * .25 * Leak.HoleDiameterM * Leak.HoleDiameterM) - Leak.DischargeCoefficient / Spray.GetSettings().VelocityCoefficient)
		/ (Leak.DischargeCoefficient / Spray.GetSettings().VelocityCoefficient);
	const double LeakedOpen = R.TanksLeakedM3();
	const double MatchOpen = FMath::Abs(Spray.GetTotals().EmittedM3 - LeakedOpen) / LeakedOpen;
	const FSprayTotals Open = Spray.GetTotals();
	// On to depletion (the step that empties the tank closes the hole: the split falls back to the hole areas).
	R.Run(10.0);
	const double LeakedEnd = R.TanksLeakedM3();
	const double MatchEnd = FMath::Abs(Spray.GetTotals().EmittedM3 - LeakedEnd) / LeakedEnd;
	bool bDepleted = false;
	for (const FCoolantInventory::FTank& T : R.Room->GetCoolantInventory().GetTanks()) bDepleted |= T.State.bDepleted;
	const FSprayTotals& T = Spray.GetTotals();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("닫힌 지점 동안 물 없음 %d, 장비 상자 안 지점 %d/%d | 5 mm 헤더: v0 %.2f m/s, 물줄기 면적/구멍 면적 = Cd/Cv 상대 %.2e | 2 s: 분사 누적 %.4f L vs 탱크 누적 %.4f L (상대 %.2e), 입자 %d, 바닥 %.4f L, 물막 %.4f L | 고갈 %d 뒤: %.4f vs %.4f L (상대 %.2e), 바닥 %.4f, 물막 %.4f, 공중 %.4f L, 잔차 %.2e, 전역 열수지 %.2e"),
		bQuiet ? 1 : 0, Inside, R.Sim->GetLiveLeakSites().Num(), V0, OrificeRel, Open.EmittedM3 * 1e3, LeakedOpen * 1e3, MatchOpen, Open.Parcels, Open.FloorM3 * 1e3, Open.FilmM3 * 1e3,
		bDepleted ? 1 : 0, T.EmittedM3 * 1e3, LeakedEnd * 1e3, MatchEnd, T.FloorM3 * 1e3, T.FilmM3 * 1e3, T.AirborneM3 * 1e3, T.Residual, Global));
	TestTrue(TEXT("closed sites spray nothing"), bQuiet);
	// The network flow is a Newton solution (D5 tolerance), so the identity holds to the solve's residual, not to round-off.
	TestTrue(TEXT("the jet uses the solve's pressure: jet area / hole area = Cd / Cv (relative < 1e-6)"), OrificeRel < 1e-6);
	TestTrue(TEXT("emitted water = the tanks' leaked volume (relative < 1e-12), also across depletion"), MatchOpen < 1e-12 && MatchEnd < 1e-12 && bDepleted);
	TestTrue(TEXT("water reached the room (floor or films) and the ledger closes"), T.FloorM3 + T.FilmM3 > 0.0 && T.Residual < 1e-12 && Global < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLeakSprayPlantTest, "VRG.Water.Spray.PlantLeakOutdoors", VrgSprayFlags)
bool FLeakSprayPlantTest::RunTest(const FString& Parameters)
{
	// A plant header leak in the yard (outside the room): its water is the loop's leak flow x Dt and lands outdoors.
	FSprayLiveRoom R(true);
	R.Sim->EnterSimulationMode();
	R.Run(0.5);
	int32 Fws = INDEX_NONE;
	for (int32 I = 0; I < R.Sim->GetLiveLeakSites().Num(); ++I) if (R.Sim->GetLiveLeakSites()[I].Label == TEXT("설비 FWS 공급 헤더")) Fws = I;
	const bool bOpened = R.Sim->SetLiveLeak(Fws, 20.0e-3, TEXT("test"));
	const FFacilityWaterLoop& L = R.Sim->GetFacilityPlant().GetLoop();
	const int32 K = L.FindLeak(EPlantLeakSite::SupplyHeader, INDEX_NONE);
	const double T0 = R.Sim->BuildDashboardSummary().SimTimeS;
	R.Run(2.0);
	const double FlowNow = K != INDEX_NONE ? L.GetLeakFlowM3s(K) : 0.0;
	const double Expected = FlowNow * (R.Sim->BuildDashboardSummary().SimTimeS - T0);
	const FLeakSpray& Spray = R.Sim->GetLeakSpray();
	const FSprayTotals& T = Spray.GetTotals();
	const bool bOutdoor = Fws != INDEX_NONE && Spray.GetGeometry().RoomM.IsValid && !Spray.GetGeometry().RoomM.IsInsideOrOn(R.Sim->GetLiveLeakSites()[Fws].PositionM);
	const double Rel = Expected > 0.0 ? FMath::Abs(T.EmittedM3 - Expected) / Expected : 1.0;
	AddInfo(FString::Printf(TEXT("FWS 공급 헤더 지점 %d (방 밖 %d), 20 mm 누수 %.4f L/s, 2 s 분사 누적 %.4f L vs 유량 x 시간 %.4f L (상대 %.2e) | 바깥 땅 %.4f L, 바닥 %.4f, 공중 %.4f L, 잔차 %.2e"),
		Fws, bOutdoor ? 1 : 0, FlowNow * 1e3, T.EmittedM3 * 1e3, Expected * 1e3, Rel, T.GroundM3 * 1e3, T.FloorM3 * 1e3, T.AirborneM3 * 1e3, T.Residual));
	TestTrue(TEXT("plant leak opened outside the room"), bOpened && bOutdoor);
	// The plant leak flow is steady after the opening solve (fixed suction pressure): emitted = flow x time.
	TestTrue(TEXT("emitted = leak flow x time (relative < 1e-6)"), Rel < 1e-6);
	TestTrue(TEXT("its water lands outdoors and the ledger closes"), T.GroundM3 > 0.0 && T.Residual < 1e-12);
	return true;
}

#endif
