// Fill out your copyright notice in the Description page of Project Settings.

// S2b height term (option B of Tools/S2b_누수_계획.md, 2026-10-20). Pipes and pumps act on the piezometric
// pressure P + rho*g*z. Checked against closed forms, against the same network without heights (closed loops must
// not change), and against the drawn pipe routes of a real room (the solver's z must be the mesh's z).

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgElevationTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FHydraulicNode MakeElevNode(bool bFixed, double PressurePa, double ElevationM)
	{
		FHydraulicNode Node;
		Node.bFixedPressure = bFixed;
		Node.PressurePa = PressurePa;
		Node.ElevationM = ElevationM;
		return Node;
	}

	FHydraulicPipe MakeElevPipe(int32 From, int32 To, double ResistancePaS2M6)
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = From;
		Pipe.ToNodeIndex = To;
		Pipe.ResistancePaS2M6 = ResistancePaS2M6;
		return Pipe;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicElevationAnalyticTest, "VRG.Hydraulics.Elevation.StaticAndGravityAnalytic", VrgElevationTestFlags)
bool FHydraulicElevationAnalyticTest::RunTest(const FString& Parameters)
{
	// Synthetic numbers (not a facility): 2.2 m is about a rack height, resistances are of the test-network scale.
	const double Rho = 997.0, G = FHydraulicNetwork::GravityMS2, H = 2.2, P0 = 2.0e5;
	const double RhoGH = Rho * G * H;

	// (a) Two fixed ends of a vertical pipe at hydrostatic equilibrium: no flow.
	{
		FHydraulicNetwork Net;
		const int32 Top = Net.AddNode(MakeElevNode(true, P0, H));
		const int32 Bottom = Net.AddNode(MakeElevNode(true, P0 + RhoGH, 0.0));
		Net.AddPipe(MakeElevPipe(Top, Bottom, 1.0e6));
		Net.Solve(FHydraulicSolveSettings());
		const double Drive = Net.GetNodePiezometricPa(Top) - Net.GetNodePiezometricPa(Bottom);
		AddInfo(FString::Printf(TEXT("(a) 정지 수직관: 구동압 %.3e Pa, 유량 %.3e m3/s (rho*g*h = %.6f Pa)"), Drive, Net.GetPipeFlowM3s(0), RhoGH));
		TestTrue(TEXT("(a) hydrostatic column: |drive| / (rho g h) < 1e-12"), FMath::Abs(Drive) / RhoGH < 1e-12);
	}

	// (b) Dead-end column solved by Newton: top fixed, bottom free, no outflow -> P_bottom = P_top + rho*g*h, Q = 0.
	{
		FHydraulicNetwork Net;
		const int32 Top = Net.AddNode(MakeElevNode(true, P0, H));
		const int32 Bottom = Net.AddNode(MakeElevNode(false, 0.0, 0.0));
		Net.AddPipe(MakeElevPipe(Top, Bottom, 1.0e6));
		const FHydraulicSolveResult R = Net.Solve(FHydraulicSolveSettings());
		const double Err = FMath::Abs(Net.GetNodePressurePa(Bottom) - (P0 + RhoGH)) / RhoGH;
		AddInfo(FString::Printf(TEXT("(b) 막힌 수직관: 바닥 정압 %.6f Pa (해석 %.6f), 정규화 오차 %.3e, 반복 %d"),
			Net.GetNodePressurePa(Bottom), P0 + RhoGH, Err, R.IterationsUsed));
		TestTrue(TEXT("(b) converged"), R.bConverged);
		TestTrue(TEXT("(b) bottom static pressure = P_top + rho g h (rel to rho g h < 1e-12)"), Err < 1e-12);
		TestTrue(TEXT("(b) no flow"), FMath::Abs(Net.GetPipeFlowM3s(0)) < 1e-15);
	}

	// (c) Gravity-driven flow through a free mid node: equal static pressure at both ends, height h between them.
	//     Q = sqrt(rho g h / (R1 + R2)),  P*_M = P*_A - R1 Q^2,  static P_M = P*_M - rho g h/2.
	{
		const double R1 = 2.0e6, R2 = 3.0e6;
		FHydraulicNetwork Net;
		const int32 A = Net.AddNode(MakeElevNode(true, P0, H));
		const int32 M = Net.AddNode(MakeElevNode(false, 0.0, 0.5 * H));
		const int32 B = Net.AddNode(MakeElevNode(true, P0, 0.0));
		Net.AddPipe(MakeElevPipe(A, M, R1));
		Net.AddPipe(MakeElevPipe(M, B, R2));
		const FHydraulicSolveResult R = Net.Solve(FHydraulicSolveSettings());
		const double Q = FMath::Sqrt(RhoGH / (R1 + R2));
		const double PM = P0 + RhoGH - R1 * Q * Q - Rho * G * 0.5 * H;
		const double FlowErr = FMath::Max(FMath::Abs(Net.GetPipeFlowM3s(0) - Q), FMath::Abs(Net.GetPipeFlowM3s(1) - Q)) / Q;
		const double PressErr = FMath::Abs(Net.GetNodePressurePa(M) - PM) / RhoGH;
		AddInfo(FString::Printf(TEXT("(c) 중력 유동: 유량 %.9e (해석 %.9e) m3/s, 오차 %.3e / 중간 정압 오차 %.3e (rho g h 기준), 반복 %d"),
			Net.GetPipeFlowM3s(0), Q, FlowErr, PressErr, R.IterationsUsed));
		TestTrue(TEXT("(c) converged"), R.bConverged);
		TestTrue(TEXT("(c) flow = sqrt(rho g h / (R1+R2)) (rel < 1e-9)"), FlowErr < 1e-9);
		TestTrue(TEXT("(c) mid static pressure (rel to rho g h < 1e-9)"), PressErr < 1e-9);
	}

	// (d) Same source and resistance, outlet at the floor vs at height h (both 0 Pa): Q_high/Q_low = sqrt(1 - rho g h / P_s).
	{
		const double Ps = 2.0e5, Rr = 1.0e7;
		FHydraulicNetwork Net;
		const int32 Src = Net.AddNode(MakeElevNode(true, Ps, 0.0));
		const int32 Low = Net.AddNode(MakeElevNode(true, 0.0, 0.0));
		const int32 High = Net.AddNode(MakeElevNode(true, 0.0, H));
		Net.AddPipe(MakeElevPipe(Src, Low, Rr));
		Net.AddPipe(MakeElevPipe(Src, High, Rr));
		Net.Solve(FHydraulicSolveSettings());
		const double Ratio = Net.GetPipeFlowM3s(1) / Net.GetPipeFlowM3s(0);
		const double Expected = FMath::Sqrt(1.0 - RhoGH / Ps);
		AddInfo(FString::Printf(TEXT("(d) 출구 높이 비: %.12f (해석 %.12f), 오차 %.3e"), Ratio, Expected, FMath::Abs(Ratio - Expected) / Expected));
		TestTrue(TEXT("(d) outlet height ratio (rel < 1e-9)"), FMath::Abs(Ratio - Expected) / Expected < 1e-9);
	}

	// (e) Pump lifting into a loop that comes back down: the loop flow does not depend on h; the static rise at the
	//     pump ports is the curve value minus rho g h (the pump curve is on piezometric head).
	{
		auto Loop = [&](double Height, double& OutFlow, double& OutStaticRise, double& OutPiezoRise)
		{
			FHydraulicNetwork Net;
			const int32 Ref = Net.AddNode(MakeElevNode(true, 1.0e5, 0.0));
			const int32 Top = Net.AddNode(MakeElevNode(false, 0.0, Height));
			FHydraulicPump Pump;
			Pump.FromNodeIndex = Ref; Pump.ToNodeIndex = Top; Pump.ShutoffHeadPa = 3.0e5; Pump.CurveCoeffPaS2M6 = 1.0e6;
			Net.AddPump(Pump);
			Net.AddPipe(MakeElevPipe(Top, Ref, 2.0e6));
			Net.Solve(FHydraulicSolveSettings());
			OutFlow = Net.GetPumpFlowM3s(0);
			OutStaticRise = Net.GetNodePressurePa(Top) - Net.GetNodePressurePa(Ref);
			OutPiezoRise = Net.GetPumpDeltaPPa(0);
		};
		double F0, S0, Z0, F1, S1, Z1;
		Loop(0.0, F0, S0, Z0);
		Loop(H, F1, S1, Z1);
		AddInfo(FString::Printf(TEXT("(e) 펌프 루프: 유량 %.9e / %.9e m3/s, 정압 상승 %.3f / %.3f Pa, 피에조 상승 %.3f / %.3f Pa"), F0, F1, S0, S1, Z0, Z1));
		TestTrue(TEXT("(e) loop flow independent of height (rel < 1e-9)"), FMath::Abs(F1 - F0) / F0 < 1e-9);
		TestTrue(TEXT("(e) static rise = piezometric rise - rho g h (rel to rho g h < 1e-9)"), FMath::Abs(S1 - (Z1 - RhoGH)) / RhoGH < 1e-9);
		TestTrue(TEXT("(e) piezometric rise unchanged (rel < 1e-9)"), FMath::Abs(Z1 - Z0) / Z0 < 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicElevationLoopTest, "VRG.Hydraulics.Elevation.ClosedLoopFlowsUnchanged", VrgElevationTestFlags)
bool FHydraulicElevationLoopTest::RunTest(const FString& Parameters)
{
	// NVL72 row of 8 racks (FInRowLoopSpec defaults). Heights: synthetic layout like the room actor's plain path
	// (headers 0.25 m, rack manifolds 1.1 m, CDU node 0.6 m); only their differences matter.
	FInRowLoopSpec Spec;
	Spec.NumRacks = 8;
	FHydraulicNetwork Flat, Tall;
	const FInRowLoopHandles H0 = InRowLoopBuilder::AddLoop(Flat, Spec);
	const FInRowLoopHandles H1 = InRowLoopBuilder::AddLoop(Tall, Spec);
	TArray<double> Z;
	Z.Init(0.25, Tall.GetNumNodes());
	Z[H1.CduNodeIndex] = 0.6;
	for (const int32 Node : H1.RackNodeIndices) if (Node != INDEX_NONE) Z[Node] = 1.1;
	for (int32 N = 0; N < Tall.GetNumNodes(); ++N) Tall.SetNodeElevationM(N, Z[N]);

	const FHydraulicSolveResult R0 = Flat.Solve(FHydraulicSolveSettings());
	const FHydraulicSolveResult R1 = Tall.Solve(FHydraulicSolveSettings());
	TestTrue(TEXT("both converged"), R0.bConverged && R1.bConverged);

	const double PumpFlow = Flat.GetPumpFlowM3s(0);
	double FlowDiff = FMath::Abs(Tall.GetPumpFlowM3s(0) - PumpFlow) / PumpFlow;
	for (int32 P = 0; P < Flat.GetNumPipes(); ++P)
		FlowDiff = FMath::Max(FlowDiff, FMath::Abs(Tall.GetPipeFlowM3s(P) - Flat.GetPipeFlowM3s(P)) / PumpFlow);

	// Same piezometric solution: static_tall = static_flat + rho g (z_cdu - z) (the CDU node holds its static pressure).
	const double RhoG = Tall.GetFluidDensityKgM3() * FHydraulicNetwork::GravityMS2;
	double MaxDz = 0.0, PressDiff = 0.0;
	for (int32 N = 0; N < Tall.GetNumNodes(); ++N)
	{
		const double Expected = Flat.GetNodePressurePa(N) + RhoG * (Z[H1.CduNodeIndex] - Z[N]);
		PressDiff = FMath::Max(PressDiff, FMath::Abs(Tall.GetNodePressurePa(N) - Expected));
		MaxDz = FMath::Max(MaxDz, FMath::Abs(Z[N] - Z[H1.CduNodeIndex]));
	}
	const double PressNorm = PressDiff / (RhoG * MaxDz);
	AddInfo(FString::Printf(TEXT("NVL72 한 줄: 펌프 %.6f L/s, 유량 차 최대 %.3e (펌프 유량 기준), 정압 차 %.3e Pa = %.3e (rho g dz_max %.3f Pa 기준), 반복 %d / %d"),
		PumpFlow * 1e3, FlowDiff, PressDiff, PressNorm, RhoG * MaxDz, R0.IterationsUsed, R1.IterationsUsed));
	TestTrue(TEXT("circulating flows unchanged by heights (rel to pump flow < 1e-9)"), FlowDiff < 1e-9);
	TestTrue(TEXT("static pressures shift by rho g (z_cdu - z) (rel to rho g dz_max < 1e-6)"), PressNorm < 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicElevationRoomTest, "VRG.Hydraulics.Elevation.RoomNodesFollowPipeRoutes", VrgElevationTestFlags)
bool FHydraulicElevationRoomTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	for (const double Y : { 0.0, 66.0 })   // one row of two racks (synthetic placement), CDU off the row
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);

	for (const bool bGj : { false, true })
	{
		const TCHAR* Style = bGj ? TEXT("GJ") : TEXT("plain");
		Room->bUseGjConnections = bGj;
		Room->RebuildRoom();
		Room->RebuildRoomNow();
		const FHydraulicNetwork& Net = Room->GetNetwork();
		// Independent recount from the drawn routes: first point = from node, last point = to node.
		TArray<double> Sum; TArray<int32> Count;
		Sum.Init(0.0, Net.GetNumNodes()); Count.Init(0, Net.GetNumNodes());
		for (const FRoomPipeRoute& Route : Room->GetPipeRoutesM())
		{
			if (Route.PointsM.Num() < 2) continue;
			const int32 From = Route.Edge >= 0 ? Net.GetPipeFromNodeIndex(Route.Edge) : Net.GetPumpFromNodeIndex(-1 - Route.Edge);
			const int32 To = Route.Edge >= 0 ? Net.GetPipeToNodeIndex(Route.Edge) : Net.GetPumpToNodeIndex(-1 - Route.Edge);
			if (Sum.IsValidIndex(From)) { Sum[From] += Route.PointsM[0].Z; ++Count[From]; }
			if (Sum.IsValidIndex(To)) { Sum[To] += Route.PointsM.Last().Z; ++Count[To]; }
		}
		double MaxErr = 0.0, MinZ = 1e9, MaxZ = -1e9;
		int32 Covered = 0;
		for (int32 N = 0; N < Net.GetNumNodes(); ++N)
		{
			const double Zn = Net.GetNodeElevationM(N);
			MinZ = FMath::Min(MinZ, Zn); MaxZ = FMath::Max(MaxZ, Zn);
			if (Count[N] == 0) continue;
			++Covered;
			MaxErr = FMath::Max(MaxErr, FMath::Abs(Zn - Sum[N] / Count[N]));
		}
		const FHydraulicSolveResult& Res = Room->GetLastSolveResult();
		AddInfo(FString::Printf(TEXT("%s: 노드 %d개 중 경로 끝점이 닿는 노드 %d개, 높이 = 끝점 평균 오차 최대 %.3e m, 높이 범위 %.3f ~ %.3f m, 수력 %s (반복 %d)"),
			Style, Net.GetNumNodes(), Covered, MaxErr, MinZ, MaxZ, Res.bConverged ? TEXT("수렴") : TEXT("실패"), Res.IterationsUsed));
		TestEqual(FString::Printf(TEXT("%s: every node is reached by a drawn pipe end"), Style), Covered, Net.GetNumNodes());
		TestTrue(FString::Printf(TEXT("%s: node height = mean height of its drawn pipe ends (< 1e-9 m)"), Style), MaxErr < 1e-9);
		TestTrue(FString::Printf(TEXT("%s: heights inside the room (0 ~ 3 m)"), Style), MinZ >= 0.0 && MaxZ <= 3.0 && MaxZ > MinZ);
		TestTrue(FString::Printf(TEXT("%s: hydraulics converged"), Style), Res.bConverged);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
