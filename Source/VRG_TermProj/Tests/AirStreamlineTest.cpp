// Fill out your copyright notice in the Description page of Project Settings.

// Air view (2026-10-27): streamlines against analytic fields, the time-average window, and the view in a live room.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Simulation/Air/AirFlowAverager.h"
#include "Simulation/Air/AirStreamlines.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgStreamFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FUniformAirGrid StreamGrid(int32 X, int32 Y, int32 Z, double H)
	{
		FAirGridParams P; P.NumCellsX = X; P.NumCellsY = Y; P.NumCellsZ = Z; P.CellSizeM = H;
		FUniformAirGrid G; G.Initialize(P);
		return G;
	}

	// Face velocities from a field u(x) evaluated at each face centre (linear fields are then reproduced exactly by the
	// grid's linear interpolation).
	void StreamFill(FUniformAirGrid& G, TFunctionRef<FVector(const FVector&)> U)
	{
		const double H = G.GetCellSizeM();
		for (int32 Z = 0; Z < G.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < G.GetNumCellsY(); ++Y) for (int32 X = 0; X <= G.GetNumCellsX(); ++X)
			G.VelocityXMs[G.FaceXIndex(X, Y, Z)] = U(FVector(X * H, (Y + .5) * H, (Z + .5) * H)).X;
		for (int32 Z = 0; Z < G.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y <= G.GetNumCellsY(); ++Y) for (int32 X = 0; X < G.GetNumCellsX(); ++X)
			G.VelocityYMs[G.FaceYIndex(X, Y, Z)] = U(FVector((X + .5) * H, Y * H, (Z + .5) * H)).Y;
		for (int32 Z = 0; Z <= G.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < G.GetNumCellsY(); ++Y) for (int32 X = 0; X < G.GetNumCellsX(); ++X)
			G.VelocityZMs[G.FaceZIndex(X, Y, Z)] = U(FVector((X + .5) * H, (Y + .5) * H, Z * H)).Z;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirStreamAnalyticTest, "VRG.Air.Streamlines.AnalyticFields", VrgStreamFlags)
bool FAirStreamAnalyticTest::RunTest(const FString& Parameters)
{
	// 1. Uniform flow (1, 0.5, 0.25) m/s: a straight line along it, ending at the domain edge.
	FUniformAirGrid U = StreamGrid(40, 20, 10, .1);
	StreamFill(U, [](const FVector&) { return FVector(1.0, .5, .25); });
	const FVector Seed(.3, .3, .2), Dir = FVector(1.0, .5, .25).GetSafeNormal();
	const FAirStreamline L1 = AirStreamlines::Trace(U, Seed, FAirStreamlineSettings());
	double OffLine = 0.0;
	for (const FVector& P : L1.PointsM) OffLine = FMath::Max(OffLine, FVector::CrossProduct(P - Seed, Dir).Size());
	const double ArcEnd = FVector::Dist(L1.PointsM.Last(), Seed);
	const double ArcExpected = (L1.PointsM.Num() - 1) * .05;

	// 2. Solid-body rotation about (1, 1): u = (-(y-1), x-1, 0). One turn of a 0.5 m circle must close on itself.
	FUniformAirGrid R = StreamGrid(20, 20, 4, .1);
	StreamFill(R, [](const FVector& P) { return FVector(-(P.Y - 1.0), P.X - 1.0, 0.0); });
	FAirStreamlineSettings Turn; Turn.MaxLengthM = 2.0 * PI * .5;
	FAirStreamlineSettings Coarse = Turn; Coarse.StepM = .1;
	const FVector RSeed(1.5, 1.0, .2);
	auto Closure = [&](const FAirStreamlineSettings& S, double& OutRadiusDev)
	{
		const FAirStreamline L = AirStreamlines::Trace(R, RSeed, S);
		OutRadiusDev = 0.0;
		for (const FVector& P : L.PointsM) OutRadiusDev = FMath::Max(OutRadiusDev, FMath::Abs(FVector2D(P.X - 1.0, P.Y - 1.0).Size() - .5));
		// The last point is at arc length Steps x StepM >= 2 pi r; step back along the circle to exactly one turn.
		const double Over = (L.PointsM.Num() - 1) * S.StepM - 2.0 * PI * .5;
		const double A = Over / .5;    // counter-clockwise angle past one full turn (radians)
		const FVector Expected(1.0 + .5 * FMath::Cos(A), 1.0 + .5 * FMath::Sin(A), .2);
		return FVector::Dist(L.PointsM.Last(), Expected) / .5;
	};
	double RDev5 = 0.0, RDev10 = 0.0;
	const double Close5 = Closure(Turn, RDev5), Close10 = Closure(Coarse, RDev10);

	// 3. Backward from the end of a forward trace returns to the seed.
	FAirStreamlineSettings Fwd; Fwd.MaxLengthM = 1.0;
	const FAirStreamline F = AirStreamlines::Trace(R, RSeed, Fwd);
	FAirStreamlineSettings Back = Fwd; Back.bBackward = true; Back.MaxLengthM = (F.PointsM.Num() - 1) * Fwd.StepM;
	const FAirStreamline B = AirStreamlines::Trace(R, F.PointsM.Last(), Back);
	const double Return = FVector::Dist(B.PointsM.Last(), RSeed) / .5;

	// 4. Planar: a field with a vertical part; the plan line stays at the seed's height.
	FUniformAirGrid V = StreamGrid(20, 20, 8, .1);
	StreamFill(V, [](const FVector& P) { return FVector(-(P.Y - 1.0), P.X - 1.0, .3); });
	FAirStreamlineSettings Plan; Plan.bPlanar = true; Plan.MaxLengthM = 2.0;
	const FAirStreamline PL = AirStreamlines::Trace(V, FVector(1.4, 1.0, .35), Plan);
	double ZDev = 0.0;
	for (const FVector& P : PL.PointsM) ZDev = FMath::Max(ZDev, FMath::Abs(P.Z - .35));

	AddInfo(FString::Printf(TEXT("균일 흐름: 직선에서 벗어남 최대 %.2e m, 끝까지 %d점 %.3f m(호 %.3f m), 멈춤 %d(밖) | 강체 회전 한 바퀴(r 0.5 m): 닫힘 오차/r 5 cm %.2e, 10 cm %.2e (비 %.1f), 반지름 편차/r %.2e / %.2e | 1 m 갔다 되돌아오기 %.2e | 평면 유선 높이 편차 %.1e m"),
		OffLine, L1.PointsM.Num(), ArcEnd, ArcExpected, int32(L1.Stop), Close5, Close10, Close10 / FMath::Max(Close5, 1e-300), RDev5 / .5, RDev10 / .5, Return, ZDev));
	TestTrue(TEXT("uniform flow: a straight line (< 1e-12 m), length = steps x step, stops at the domain edge"),
		OffLine < 1e-12 && FMath::Abs(ArcEnd - ArcExpected) < 1e-9 && L1.Stop == FAirStreamline::EStop::Outside);
	TestTrue(TEXT("rotation: one turn closes (RK4, < 1e-6 of the radius at 5 cm) and converges with the step"), Close5 < 1e-6 && Close10 > Close5);
	TestTrue(TEXT("backward trace returns to the seed (< 1e-6 of the radius)"), Return < 1e-6);
	TestTrue(TEXT("planar line stays at its height"), ZDev == 0.0 && PL.PointsM.Num() > 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirStreamSolidTest, "VRG.Air.Streamlines.StopAtSolids", VrgStreamFlags)
bool FAirStreamSolidTest::RunTest(const FString& Parameters)
{
	// Flow along +X into a solid block: the line ends at the block (stop = solid) and never has a point inside it.
	FUniformAirGrid G = StreamGrid(30, 10, 10, .1);
	StreamFill(G, [](const FVector&) { return FVector(1.0, 0.0, 0.0); });
	for (int32 Z = 2; Z < 8; ++Z) for (int32 Y = 2; Y < 8; ++Y) for (int32 X = 15; X < 18; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
	const FAirStreamline L = AirStreamlines::Trace(G, FVector(.2, .5, .5), FAirStreamlineSettings());
	int32 Inside = 0;
	for (const FVector& P : L.PointsM) Inside += G.IsSolidCell(FMath::FloorToInt32(P.X / .1), FMath::FloorToInt32(P.Y / .1), FMath::FloorToInt32(P.Z / .1)) ? 1 : 0;
	const double EndX = L.PointsM.Num() ? L.PointsM.Last().X : -1.0;
	// Still air: a line from where nothing moves has one point and stops as still.
	FUniformAirGrid Z = StreamGrid(10, 10, 10, .1);
	const FAirStreamline S = AirStreamlines::Trace(Z, FVector(.5, .5, .5), FAirStreamlineSettings());
	AddInfo(FString::Printf(TEXT("고체 앞 멈춤: 점 %d개, 끝 x %.3f m(고체 시작 1.500 m), 고체 안 점 %d, 멈춤 %d | 정지 공기: 점 %d, 멈춤 %d"), L.PointsM.Num(), EndX, Inside, int32(L.Stop), S.PointsM.Num(), int32(S.Stop)));
	TestTrue(TEXT("stops at the solid, no point inside it, within one step of its face"), L.Stop == FAirStreamline::EStop::Solid && Inside == 0 && EndX <= 1.5 && EndX > 1.5 - .05 - 1e-9);
	TestTrue(TEXT("still air: one point, stop = still"), S.PointsM.Num() == 1 && S.Stop == FAirStreamline::EStop::Still);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirFlowAverageTest, "VRG.Air.Streamlines.TimeAverageWindow", VrgStreamFlags)
bool FAirFlowAverageTest::RunTest(const FString& Parameters)
{
	// 30 s of field A (u 1 m/s, 300 K) then 15 s of B (u 3 m/s, 310 K) in 0.02 s steps: the 30 s window holds the last
	// 15 s of A and the 15 s of B, so the average is u 2 m/s, 305 K.
	FUniformAirGrid G = StreamGrid(6, 5, 4, .25);
	FAirFlowAverager A;
	auto Set = [&G](double U, double T)
	{
		for (double& V : G.VelocityXMs) V = U;
		for (double& V : G.TemperatureK) V = T;
	};
	FUniformAirGrid Out;
	const bool bBefore = A.Build(G, Out);
	Set(1.0, 300.0);
	for (int32 S = 0; S < 1500; ++S) A.Add(G, .02);
	const double Covered1 = A.GetCoveredS();
	Set(3.0, 310.0);
	for (int32 S = 0; S < 750; ++S) A.Add(G, .02);
	const bool bBuilt = A.Build(G, Out);
	double UErr = 0.0, TErr = 0.0;
	for (const double V : Out.VelocityXMs) UErr = FMath::Max(UErr, FMath::Abs(V - 2.0) / 2.0);
	for (const double V : Out.TemperatureK) TErr = FMath::Max(TErr, FMath::Abs(V - 305.0) / 305.0);
	AddInfo(FString::Printf(TEXT("A 30 s 뒤 덮은 시간 %.4f s, B 15 s 뒤 %.4f s | 평균 u 상대 오차 %.2e, T %.2e | 첫 블록 전 만들기 %d"), Covered1, A.GetCoveredS(), UErr, TErr, bBefore ? 1 : 0));
	TestTrue(TEXT("nothing to show before the first block"), !bBefore);
	TestTrue(TEXT("window = 30 s and the average is the exact mean of its last 30 s (< 1e-12)"),
		bBuilt && FMath::Abs(A.GetCoveredS() - 30.0) < 1e-9 && UErr < 1e-12 && TErr < 1e-12);
	return true;
}

namespace
{
	struct FStreamLiveRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		FStreamLiveRoom()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->bShowSpray = false; Sim->MaxStepsPerFrame = 16;
			ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
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
		~FStreamLiveRoom()
		{
			Sim->EnterLayoutMode();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 Fr = 0; Fr < FMath::RoundToInt32(Seconds / 0.1); ++Fr) Sim->Tick(.1f); }
		const FUniformAirGrid* Grid() const
		{
			for (TActorIterator<AAirFieldDebugActor> It(World); It; ++It) return &It->GetMutableSolver().GetGrid();
			return nullptr;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirStreamLiveTest, "VRG.Air.Streamlines.LiveRoomView", VrgStreamFlags)
bool FAirStreamLiveTest::RunTest(const FString& Parameters)
{
	// The live room: streamtubes from the coolers and rack exhausts follow the averaged field without entering a solid
	// cell; the plan slice draws one layer and level lines; the volume mode and layout mode leave nothing drawn.
	FStreamLiveRoom R;
	R.Sim->EnterSimulationMode();
	R.Sim->SetAirView(EAirView::StreamSpeed);
	R.Run(3.0);
	R.Sim->RebuildAirFlowView();
	const FUniformAirGrid* G = R.Grid();
	const FVector O = R.Sim->GetStreamlineOriginM();
	auto SolidPoints = [G](const TArray<FAirStreamline>& Lines, int32& OutPoints)
	{
		int32 Bad = 0; OutPoints = 0;
		const double H = G->GetCellSizeM();
		for (const FAirStreamline& L : Lines)
			for (const FVector& P : L.PointsM)
			{
				++OutPoints;
				Bad += G->IsFluidCell(FMath::FloorToInt32(P.X / H), FMath::FloorToInt32(P.Y / H), FMath::FloorToInt32(P.Z / H)) ? 0 : 1;
			}
		return Bad;
	};
	int32 Points = 0, PlanPoints = 0;
	const int32 Lines = R.Sim->GetStreamlines().Num();
	const int32 Bad = G ? SolidPoints(R.Sim->GetStreamlines(), Points) : -1;
	const int32 Tubes = R.Sim->GetAirFlowInstanceCount();
	const double Avg = R.Sim->GetAirFlowAverageS();
	TArray<FAirFlowLegend> Legends; FString Note;
	R.Sim->GetAirFlowLegends(Legends, Note);
	R.Sim->SetAirView(EAirView::Plan);
	R.Sim->RebuildAirFlowView();
	const int32 PlanBad = G ? SolidPoints(R.Sim->GetPlanStreamlines(), PlanPoints) : -1;
	double PlanZDev = 0.0;
	const double H = G ? G->GetCellSizeM() : .25;
	const double Zc = (FMath::FloorToInt32(R.Sim->PlanHeightM / H) + .5) * H;
	for (const FAirStreamline& L : R.Sim->GetPlanStreamlines()) for (const FVector& P : L.PointsM) PlanZDev = FMath::Max(PlanZDev, FMath::Abs(P.Z - Zc));
	const int32 PlanShown = R.Sim->GetAirFlowInstanceCount();
	const int32 PlanLines = R.Sim->GetPlanStreamlines().Num();
	R.Sim->SetAirView(EAirView::Volume);
	const int32 VolumeShown = R.Sim->GetAirFlowInstanceCount();
	R.Sim->SetAirView(EAirView::StreamTemperature);
	R.Sim->RebuildAirFlowView();
	R.Sim->EnterLayoutMode();
	const int32 LayoutShown = R.Sim->GetAirFlowInstanceCount();
	AddInfo(FString::Printf(TEXT("유선 %d개(점 %d, 고체·밖 점 %d), 튜브 %d개, 평균 %.1f s, 범례 %d (%s) | 평면: 선 %d개(점 %d, 고체 %d), 높이 편차 %.1e m, 그린 것 %d | 볼륨 모드 %d, 배치 모드 %d"),
		Lines, Points, Bad, Tubes, Avg, Legends.Num(), *Note, PlanLines, PlanPoints, PlanBad, PlanZDev, PlanShown, VolumeShown, LayoutShown));
	TestTrue(TEXT("streamlines from every seed, none through a solid, tubes drawn, averaged field in use"), Lines > 0 && Points > Lines && Bad == 0 && Tubes > 0 && Avg >= 2.0 && Legends.Num() == 1);
	TestTrue(TEXT("plan: level lines outside solids and a drawn slice"), PlanPoints > 0 && PlanBad == 0 && PlanZDev == 0.0 && PlanShown > 0);
	TestTrue(TEXT("volume and layout modes leave nothing drawn"), VolumeShown == 0 && LayoutShown == 0);
	return true;
}

#endif
