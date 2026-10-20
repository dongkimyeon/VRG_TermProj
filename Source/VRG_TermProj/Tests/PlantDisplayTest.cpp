// Fill out your copyright notice in the Description page of Project Settings.

// S8 F7 (Tools/설비냉각_계획.md, "보이는 것 = 계산하는 것"): the plant yard drawn from the plant's networks.
// Every network pipe and pump of every built loop appears exactly once with its solved flow and its end points on its
// nodes, the topology decides which pipes exist, equipment counts are the model's, faults and leaks show where the
// solve put them, and the integrated actor shows the yard only in simulation mode.

#include "Misc/AutomationTest.h"
#include "Visualization/PlantDisplay.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/FacilityPlantDisplayActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgPlantDisplayFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double DisplayRho = 997.0, DisplayCp = 4180.0;

	FFacilityPlant MakeDisplayPlant(EPlantTopology T)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.4e6 / (DisplayCp * 10.0) / DisplayRho, 0.4e6 / (DisplayCp * 10.0) / DisplayRho };
		FFacilityThermalSpec Th;
		Th.AmbientK = 303.15;
		Th.Topology = T;
		TArray<FPlantCrahCoil> Coils;
		for (int32 U = 0; U < 4; ++U)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		FFacilityPlant Plant;
		Plant.Build(Loop, Th, 0.8e6, DisplayRho, DisplayCp, FHydraulicSolveSettings(), Coils, 291.15);
		return Plant;
	}

	FPlantDisplayInput MakeDisplayInput()
	{
		FPlantDisplayInput In;
		In.RoomM = FBox(FVector(0, 0, 0), FVector(10, 8, 3));
		In.CduPositionsM = { FVector(1, 1, 0), FVector(1, 5, 0) };
		In.CrahPositionsM = { FVector(8, 1, 0), FVector(8, 3, 0), FVector(8, 5, 0), FVector(8, 7, 0) };
		return In;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantDisplayPipesTest, "VRG.Plant.Display.PipesMatchNetwork", VrgPlantDisplayFlags)
bool FPlantDisplayPipesTest::RunTest(const FString& Parameters)
{
	FString Lines;
	for (const EPlantTopology T : { EPlantTopology::Separate, EPlantTopology::SharedChw, EPlantTopology::Series })
	{
		const FFacilityPlant P = MakeDisplayPlant(T);
		const FPlantDisplaySnapshot S = PlantDisplay::Build(P, MakeDisplayInput());
		int32 Expected = 0, Pumps = 0;
		bool bOnce = true, bFlowsExact = true, bEnds = true;
		for (const EPlantLoopId Id : { EPlantLoopId::Fws, EPlantLoopId::Chw, EPlantLoopId::Cw })
		{
			const FFacilityWaterLoop* L = P.GetLoopFor(Id);
			if (L == nullptr) continue;
			const FHydraulicNetwork& N = L->GetNetwork();
			Expected += N.GetNumPipes() + L->GetHandles().Pumps.Num();
			Pumps += L->GetHandles().Pumps.Num();
			TArray<int32> Seen;
			Seen.Init(0, N.GetNumPipes());
			for (const FPlantDisplayPipe& D : S.Pipes)
			{
				if (D.Loop != Id || D.Role == EPlantPipeRole::Pump) continue;
				if (!Seen.IsValidIndex(D.Edge)) { bOnce = false; continue; }
				++Seen[D.Edge];
				bFlowsExact &= D.FlowM3s == (N.IsPipeClosed(D.Edge) ? 0.0 : N.GetPipeFlowM3s(D.Edge));
				const TArray<FVector>& Pos = S.NodePositionsM[uint8(Id)];
				bEnds &= D.PointsM[0] == Pos[N.GetPipeFromNodeIndex(D.Edge)] && D.PointsM.Last() == Pos[N.GetPipeToNodeIndex(D.Edge)];
			}
			for (const int32 C : Seen) bOnce &= C == 1;
		}
		const bool bFws = P.GetLoopFor(EPlantLoopId::Fws) != nullptr;
		bool bAnyBypass = false, bFwsPipe = false;
		for (const FPlantDisplayPipe& D : S.Pipes) { bAnyBypass |= D.Role == EPlantPipeRole::Bypass; bFwsPipe |= D.Loop == EPlantLoopId::Fws; }
		int32 DryCoolers = 0, Chillers = 0;
		for (const FPlantDisplayEquipment& E : S.Equipment)
		{
			DryCoolers += E.Kind == EPlantEquipmentKind::DryCooler ? 1 : 0;
			Chillers += E.Kind == EPlantEquipmentKind::Chiller ? 1 : 0;
		}
		Lines += FString::Printf(TEXT(" [%s: 배관 %d(망 %d), 펌프 %d, 드라이쿨러 %d(모델 %d + %d), 칠러 %d, 바이패스 %d]"), FFacilityPlant::TopologyName(T),
			S.Pipes.Num(), Expected, Pumps, DryCoolers, P.GetNumDryCoolers(), P.GetNumCwDryCoolers(), Chillers, bAnyBypass ? 1 : 0);
		const TCHAR* Name = FFacilityPlant::TopologyName(T);
		TestTrue(FString::Printf(TEXT("%s: every network pipe and pump drawn exactly once"), Name), bOnce && S.Pipes.Num() == Expected);
		TestTrue(FString::Printf(TEXT("%s: drawn flows are the solved flows (exact) and ends sit on their nodes"), Name), bFlowsExact && bEnds);
		TestTrue(FString::Printf(TEXT("%s: the topology decides the pipes (FWS only when separate, bypass only in series)"), Name),
			bFwsPipe == bFws && bFws == (T == EPlantTopology::Separate) && bAnyBypass == (T == EPlantTopology::Series));
		TestTrue(FString::Printf(TEXT("%s: equipment counts are the model's"), Name), DryCoolers == P.GetNumDryCoolers() + P.GetNumCwDryCoolers()
			&& Chillers == P.GetThermalSpec().DutyChillers + P.GetThermalSpec().StandbyChillers);
	}
	AddInfo(FString::Printf(TEXT("구성별 설비 마당:%s"), *Lines));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantDisplayFaultTest, "VRG.Plant.Display.FaultsShowWhereSolved", VrgPlantDisplayFlags)
bool FPlantDisplayFaultTest::RunTest(const FString& Parameters)
{
	FFacilityPlant P = MakeDisplayPlant(EPlantTopology::Separate);
	const FPlantDisplaySnapshot Before = PlantDisplay::Build(P, MakeDisplayInput());
	P.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::BranchSupply, 1, 0.02);
	P.TripPump(EPlantLoopId::Chw, 0);
	P.SetTrippedDryCoolers(EPlantLoopId::Cw, 1);
	const FPlantDisplaySnapshot S = PlantDisplay::Build(P, MakeDisplayInput());
	const FFacilityWaterLoop& Fws = P.GetLoop();
	const FHydraulicNetwork& N = Fws.GetNetwork();
	const int32 Node = N.GetLeak(Fws.GetLeak(0).NetworkLeak).NodeIndex;
	const bool bLeak = S.Leaks.Num() == 1 && S.Leaks[0].PositionM == S.NodePositionsM[0][Node] && S.Leaks[0].FlowM3s == Fws.GetLeakFlowM3s(0) && S.Leaks[0].FlowM3s > 0.0;
	bool bPumpFailed = false, bStandbyRuns = false, bDryFailed = false;
	for (const FPlantDisplayEquipment& E : S.Equipment)
	{
		if (E.Kind == EPlantEquipmentKind::Pump && E.Loop == EPlantLoopId::Chw && E.Index == 0) bPumpFailed = E.bFailed && !E.bRunning;
		if (E.Kind == EPlantEquipmentKind::Pump && E.Loop == EPlantLoopId::Chw && E.Index == 2) bStandbyRuns = E.bRunning && !E.bFailed;
		if (E.Kind == EPlantEquipmentKind::DryCooler && E.Loop == EPlantLoopId::Cw && E.Index == P.GetNumCwDryCoolers() - 1) bDryFailed = E.bFailed;
	}
	bool bPumpPipeGrey = false;
	for (const FPlantDisplayPipe& D : S.Pipes)
	{
		if (D.Loop == EPlantLoopId::Chw && D.Role == EPlantPipeRole::Pump && D.Edge == 0)
			bPumpPipeGrey = D.bClosed && PlantDisplay::PipeColor(S, D, false).Equals(FLinearColor(.35f, .35f, .35f));
	}
	// A header leak big enough to reverse the branches: reversed branch pipes are flagged.
	FFacilityPlant R = MakeDisplayPlant(EPlantTopology::Separate);
	R.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::SupplyHeader, INDEX_NONE, 0.15);
	const FPlantDisplaySnapshot SR = PlantDisplay::Build(R, MakeDisplayInput());
	int32 Reversed = 0;
	for (const FPlantDisplayPipe& D : SR.Pipes) Reversed += (D.Loop == EPlantLoopId::Fws && D.Role == EPlantPipeRole::Valve && D.bReversed) ? 1 : 0;
	AddInfo(FString::Printf(TEXT("누수 표시 %d개(유량 %.4f L/s), CHW 펌프 0 고장 %d / 대기 2 운전 %d / 배관 회색 %d, CW 드라이쿨러 마지막 고장 %d, 구조 키 바뀜 %d | 150 mm 헤더 누수: 역류 밸브 배관 %d개"),
		S.Leaks.Num(), S.Leaks.Num() ? S.Leaks[0].FlowM3s * 1e3 : 0.0, bPumpFailed ? 1 : 0, bStandbyRuns ? 1 : 0, bPumpPipeGrey ? 1 : 0, bDryFailed ? 1 : 0,
		S.StructureHash != Before.StructureHash ? 1 : 0, Reversed));
	TestTrue(TEXT("the leak marker sits on the leaking node with its solved flow"), bLeak);
	TestTrue(TEXT("tripped pump drawn failed with a grey (closed) pump pipe, standby running"), bPumpFailed && bStandbyRuns && bPumpPipeGrey);
	TestTrue(TEXT("failed dry cooler fan drawn failed"), bDryFailed);
	TestTrue(TEXT("a new leak marker changes the structure key (meshes rebuilt)"), S.StructureHash != Before.StructureHash);
	TestTrue(TEXT("reversed branches flagged (both CDU valves)"), Reversed == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantDisplayActorTest, "VRG.Plant.Display.ActorOnlyInSimulation", VrgPlantDisplayFlags)
bool FPlantDisplayActorTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
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
	const bool bNoneInLayout = Sim->GetPlantDisplay() == nullptr;
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 20; ++F) Sim->Tick(.1f);
	AFacilityPlantDisplayActor* D = Sim->GetPlantDisplay();
	const int32 PipeMeshes = D ? D->GetNumPipeMeshes() : -1, EquipMeshes = D ? D->GetNumEquipmentMeshes() : -1;
	int32 Segments = 0;
	if (D) for (const FPlantDisplayPipe& P : D->GetSnapshot().Pipes) Segments += P.PointsM.Num() - 1;
	const int32 Equip = D ? D->GetSnapshot().Equipment.Num() : -2;
	// A facility leak through the console path: a marker appears at once.
	const bool bFault = Sim->ApplyPlantFault({ TEXT("leak"), TEXT("fws"), TEXT("branchsupply"), TEXT("0"), TEXT("10") });
	const int32 LeakMeshes = D ? D->GetNumLeakMeshes() : -1;
	Sim->EnterLayoutMode();
	const bool bGoneInLayout = Sim->GetPlantDisplay() == nullptr;
	AddInfo(FString::Printf(TEXT("배치 모드 표시 없음 %d, 시뮬레이션: 배관 메쉬 %d = 선분 %d, 장비 메쉬 %d = %d, 누수 표시 %d, 다시 배치 모드: 없어짐 %d"),
		bNoneInLayout ? 1 : 0, PipeMeshes, Segments, EquipMeshes, Equip, LeakMeshes, bGoneInLayout ? 1 : 0));
	TestTrue(TEXT("no plant yard in layout mode"), bNoneInLayout && bGoneInLayout);
	TestTrue(TEXT("simulation mode: one pipe mesh per drawn segment, one box per equipment unit"), D != nullptr && PipeMeshes == Segments && Segments > 0 && EquipMeshes == Equip);
	TestTrue(TEXT("a facility leak shows a marker immediately"), bFault && LeakMeshes == 1);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
