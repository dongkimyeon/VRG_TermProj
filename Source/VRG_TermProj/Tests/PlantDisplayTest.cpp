// Fill out your copyright notice in the Description page of Project Settings.

// S8 F7 (Tools/설비냉각_계획.md, "보이는 것 = 계산하는 것"): the plant yard drawn from the plant's networks.
// Every network pipe and pump of every built loop appears exactly once with its solved flow and its end points on its
// nodes, the topology decides which pipes exist, equipment counts are the model's, faults and leaks show where the
// solve put them. 2026-10-21: every route is orthogonal and no two drawn pipes / headers clash, and the integrated
// actor shows the yard in layout and simulation mode, also with vrg.Plant.Enable 0 (structure only, not coupled).

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

	double DistanceToPolyline(const FVector& P, const TArray<FVector>& Line)
	{
		if (Line.Num() == 1) return FVector::Distance(P, Line[0]);
		double Best = TNumericLimits<double>::Max();
		for (int32 K = 0; K + 1 < Line.Num(); ++K) Best = FMath::Min(Best, FMath::PointDistToSegment(P, Line[K], Line[K + 1]));
		return Best;
	}

	// Every segment runs along one axis (no diagonal line on screen).
	bool IsOrthogonal(const TArray<FVector>& Line)
	{
		for (int32 K = 0; K + 1 < Line.Num(); ++K)
		{
			const FVector D = (Line[K + 1] - Line[K]).GetAbs();
			const int32 Moving = (D.X > 1e-9 ? 1 : 0) + (D.Y > 1e-9 ? 1 : 0) + (D.Z > 1e-9 ? 1 : 0);
			if (Moving != 1) return false;
		}
		return Line.Num() >= 2;
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
				const TArray<TArray<FVector>>& Shapes = S.NodeShapesM[uint8(Id)];
				bEnds &= D.PointsM.Num() >= 2 && DistanceToPolyline(D.PointsM[0], Shapes[N.GetPipeFromNodeIndex(D.Edge)]) < 1e-9
					&& DistanceToPolyline(D.PointsM.Last(), Shapes[N.GetPipeToNodeIndex(D.Edge)]) < 1e-9;
			}
			for (const int32 C : Seen) bOnce &= C == 1;
		}
		const bool bFws = P.GetLoopFor(EPlantLoopId::Fws) != nullptr;
		bool bAnyBypass = false, bFwsPipe = false;
		for (const FPlantDisplayPipe& D : S.Pipes) { bAnyBypass |= D.Role == EPlantPipeRole::Bypass; bFwsPipe |= D.Loop == EPlantLoopId::Fws; }
		// Orthogonal routes (pipes and headers) and no clash between drawn pipes / headers.
		int32 Diagonal = 0;
		for (const FPlantDisplayPipe& D : S.Pipes) Diagonal += IsOrthogonal(D.PointsM) ? 0 : 1;
		for (const FPlantDisplayHeader& Hd : S.Headers) Diagonal += IsOrthogonal(Hd.PointsM) ? 0 : 1;
		const TArray<FPlantDisplayClash> Clashes = PlantDisplay::FindClashes(S);
		for (const FPlantDisplayClash& C : Clashes)
		{
			auto Describe = [&](bool bHeader, int32 I)
			{
				return bHeader ? FString::Printf(TEXT("header %d (loop %d node %d)"), I, int32(S.Headers[I].Loop), S.Headers[I].Node)
					: FString::Printf(TEXT("pipe %d (loop %d role %d edge %d)"), I, int32(S.Pipes[I].Loop), int32(S.Pipes[I].Role), S.Pipes[I].Edge);
			};
			AddInfo(FString::Printf(TEXT("%s clash: %s / %s, gap %.3f m"), FFacilityPlant::TopologyName(T), *Describe(C.bAHeader, C.A), *Describe(C.bBHeader, C.B), C.GapM));
		}
		int32 DryCoolers = 0, Chillers = 0;
		for (const FPlantDisplayEquipment& E : S.Equipment)
		{
			DryCoolers += E.Kind == EPlantEquipmentKind::DryCooler ? 1 : 0;
			Chillers += E.Kind == EPlantEquipmentKind::Chiller ? 1 : 0;
		}
		Lines += FString::Printf(TEXT(" [%s: 배관 %d(망 %d), 헤더 %d, 펌프 %d, 드라이쿨러 %d(모델 %d + %d), 칠러 %d, 바이패스 %d, 대각선 %d, 겹침 %d]"), FFacilityPlant::TopologyName(T),
			S.Pipes.Num(), Expected, S.Headers.Num(), Pumps, DryCoolers, P.GetNumDryCoolers(), P.GetNumCwDryCoolers(), Chillers, bAnyBypass ? 1 : 0, Diagonal, Clashes.Num());
		const TCHAR* Name = FFacilityPlant::TopologyName(T);
		TestTrue(FString::Printf(TEXT("%s: every network pipe and pump drawn exactly once"), Name), bOnce && S.Pipes.Num() == Expected);
		TestTrue(FString::Printf(TEXT("%s: drawn flows are the solved flows (exact) and ends sit on their nodes"), Name), bFlowsExact && bEnds);
		TestTrue(FString::Printf(TEXT("%s: every pipe and header runs along the axes (no diagonal)"), Name), Diagonal == 0);
		TestTrue(FString::Printf(TEXT("%s: no two drawn pipes / headers clash"), Name), Clashes.IsEmpty());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantDisplayActorTest, "VRG.Plant.Display.ActorInBothModes", VrgPlantDisplayFlags)
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
	const bool bNoneBeforeBuilt = Sim->GetPlantDisplay() == nullptr;
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 20; ++F) Sim->Tick(.1f);
	AFacilityPlantDisplayActor* D = Sim->GetPlantDisplay();
	const int32 PipeSegs = D ? D->GetNumPipeSegments() : -1, EquipDrawn = D ? D->GetNumEquipmentDrawn() : -1;
	int32 Segments = 0;
	if (D) for (const FPlantDisplayPipe& P : D->GetSnapshot().Pipes) Segments += P.PointsM.Num() - 1;
	const int32 Equip = D ? D->GetSnapshot().Equipment.Num() : -2;
	const bool bLive = D && D->GetSnapshot().bLive;
	// A facility leak through the console path: a marker appears at once.
	const bool bFault = Sim->ApplyPlantFault({ TEXT("leak"), TEXT("fws"), TEXT("branchsupply"), TEXT("0"), TEXT("10") });
	const int32 Leaks = D ? D->GetNumLeakMarkers() : -1;
	Sim->EnterLayoutMode();
	AFacilityPlantDisplayActor* DL = Sim->GetPlantDisplay();
	const bool bLayoutStructure = DL != nullptr && !DL->GetSnapshot().bLive && DL->GetNumPipeSegments() == PipeSegs;
	// vrg.Plant.Enable 0: the plant is still built and drawn (structure only) but not coupled.
	Sim->bFacilityPlant = false;
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 5; ++F) Sim->Tick(.1f);
	AFacilityPlantDisplayActor* DOff = Sim->GetPlantDisplay();
	const bool bOffDrawn = DOff != nullptr && !DOff->GetSnapshot().bLive && DOff->GetNumPipeSegments() > 0 && !Sim->IsFacilityPlantActive();
	Sim->EnterLayoutMode();
	AddInfo(FString::Printf(TEXT("빌드 전 표시 없음 %d, 시뮬레이션(설비 켬): 배관 선분 %d = %d, 장비 %d = %d, 계산값 %d, 누수 표시 %d | 배치 모드: 구조 표시 %d | 설비 끔: 표시 %d"),
		bNoneBeforeBuilt ? 1 : 0, PipeSegs, Segments, EquipDrawn, Equip, bLive ? 1 : 0, Leaks, bLayoutStructure ? 1 : 0, bOffDrawn ? 1 : 0));
	TestTrue(TEXT("nothing drawn before a plant is built"), bNoneBeforeBuilt);
	TestTrue(TEXT("simulation mode: every pipe segment drawn, every automatic unit drawn, live colours"), D != nullptr && PipeSegs == Segments && Segments > 0 && EquipDrawn == Equip && bLive);
	TestTrue(TEXT("a facility leak shows a marker immediately"), bFault && Leaks == 1);
	TestTrue(TEXT("layout mode keeps the yard (structure colours)"), bLayoutStructure);
	TestTrue(TEXT("vrg.Plant.Enable 0: yard drawn, plant not coupled"), bOffDrawn);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// 2026-10-22 user report: some yard units had no pipe at all (standby chillers, the automatic FWS dry coolers beyond the
// flow-counted coils, the economizer that had no network element). Every drawn unit must have a drawn pipe reaching it, with the
// automatic counts and with placed counts (FacilityPlantLayout::ApplyEquipmentCounts), in every topology.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantDisplayConnectedTest, "VRG.Plant.Display.EveryUnitConnected", VrgPlantDisplayFlags)
bool FPlantDisplayConnectedTest::RunTest(const FString& Parameters)
{
	// A pipe reaches a unit when one of its vertices lies on the unit's box (5 cm display tolerance, not a physical one).
	auto Reaches = [](const FPlantDisplayPipe& P, const FPlantDisplayEquipment& E)
	{
		const FVector Half = 0.5 * E.SizeM + FVector(0.05);
		for (const FVector& V : P.PointsM)
		{
			const FVector L = E.Rotation.UnrotateVector(V - E.CenterM).GetAbs();
			if (L.X <= Half.X && L.Y <= Half.Y && L.Z <= Half.Z) return true;
		}
		return false;
	};
	auto KindName = [](const FPlantDisplayEquipment& E)
	{
		switch (E.Kind)
		{
		case EPlantEquipmentKind::Pump: return TEXT("펌프");
		case EPlantEquipmentKind::DryCooler: return E.Loop == EPlantLoopId::Fws ? TEXT("FWS 드라이쿨러") : TEXT("CW 드라이쿨러");
		case EPlantEquipmentKind::Chiller: return TEXT("냉수 칠러");
		case EPlantEquipmentKind::TrimChiller: return TEXT("트림 칠러");
		default: return TEXT("이코노마이저");
		}
	};
	FString Lines;
	for (const bool bPlaced : { false, true })
		for (const EPlantTopology T : { EPlantTopology::Separate, EPlantTopology::SharedChw, EPlantTopology::Series })
		{
			FFacilityLoopSpec Loop;
			Loop.BranchDesignFlowM3s = { 0.4e6 / (DisplayCp * 10.0) / DisplayRho, 0.4e6 / (DisplayCp * 10.0) / DisplayRho };
			FFacilityThermalSpec Th;
			Th.AmbientK = 303.15;
			Th.Topology = T;
			if (bPlaced)
			{
				FPlantEquipmentCounts Counts;
				Counts.FwsDryCoolers = 5; Counts.CwDryCoolers = 4; Counts.Chillers = 4; Counts.TrimChillers = 3;
				FacilityPlantLayout::ApplyEquipmentCounts(Counts, Th, Loop);
			}
			TArray<FPlantCrahCoil> Coils;
			for (int32 U = 0; U < 4; ++U)
			{
				FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
				C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
				C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
			}
			FFacilityPlant P;
			const bool bBuilt = P.Build(Loop, Th, 0.8e6, DisplayRho, DisplayCp, FHydraulicSolveSettings(), Coils, 291.15);
			const FPlantDisplaySnapshot S = PlantDisplay::Build(P, MakeDisplayInput());
			TMap<FString, FIntPoint> ByKind;   // kind -> (units, unconnected)
			for (const FPlantDisplayEquipment& E : S.Equipment)
			{
				bool bAny = false;
				for (const FPlantDisplayPipe& D : S.Pipes) if (Reaches(D, E)) { bAny = true; break; }
				FIntPoint& C = ByKind.FindOrAdd(KindName(E));
				++C.X; C.Y += bAny ? 0 : 1;
			}
			int32 Loose = 0;
			FString Kinds;
			for (const auto& K : ByKind)
			{
				Loose += K.Value.Y;   // 2026-10-22: the economizer too (its two sides are in the CHW / CW mains)
				Kinds += FString::Printf(TEXT(" %s %d(안 이어짐 %d)"), *K.Key, K.Value.X, K.Value.Y);
			}
			Lines += FString::Printf(TEXT(" [%s %s:%s]"), bPlaced ? TEXT("배치 대수") : TEXT("자동 대수"), FFacilityPlant::TopologyName(T), *Kinds);
			TestTrue(FString::Printf(TEXT("%s %s: plant built"), bPlaced ? TEXT("placed") : TEXT("automatic"), FFacilityPlant::TopologyName(T)), bBuilt);
			TestEqual(FString::Printf(TEXT("%s %s: units with no pipe"), bPlaced ? TEXT("placed") : TEXT("automatic"), FFacilityPlant::TopologyName(T)), Loose, 0);
		}
	AddInfo(FString::Printf(TEXT("장비별 배관 연결:%s"), *Lines));
	return true;
}

#endif

#include "Misc/AutomationTest.h"
#include "Visualization/PlantVisual.h"
#include "Actors/FacilityPlantDisplayActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantReferenceModelTest, "VRG.Plant.Display.ReferenceModelsAndAccent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FPlantReferenceModelTest::RunTest(const FString& Parameters)
{
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if(!TestNotNull(TEXT("World"),World))return false;
    auto* Owner=World->SpawnActor<AFacilityPlantDisplayActor>();
    TArray<FPlantInstance> List;
    for(EPlantVisualKind Kind:{EPlantVisualKind::DryCooler,EPlantVisualKind::Chiller,EPlantVisualKind::Pump,EPlantVisualKind::Economizer,
        EPlantVisualKind::Valve2Way,EPlantVisualKind::CheckValve,EPlantVisualKind::LeakMarker})
    {
        if(!TestNotNull(TEXT("Authored model available"),PlantVisual::ReferenceMesh(Kind)))continue;
        const int32 Before=List.Num();
        PlantVisual::AppendEquipment(Kind,PlantVisual::NominalSizeM(Kind),FLinearColor(.1f,.4f,.7f),List);
        TestEqual(TEXT("One authored instance per equipment"),List.Num()-Before,1);
    }
    PlantInstances::FSets Sets;
    TestEqual(TEXT("All seven models drawn together"),PlantInstances::Apply(Owner,Owner->GetRootComponent(),Sets,List,false),7);
    TestEqual(TEXT("Distinct meshes never share an instance set"),Sets.Num(),7);
    TSet<UStaticMesh*> Drawn;
    for(const auto& Pair:Sets)
    {
        auto* C=Pair.Value.Get(); UStaticMesh* Mesh=C->GetStaticMesh(); Drawn.Add(Mesh);
        TestEqual(TEXT("One instance in every set"),C->GetInstanceCount(),1);
        for(int32 I=0;I<Mesh->GetStaticMaterials().Num();++I)
        {
            if(Mesh->GetStaticMaterials()[I].MaterialSlotName==TEXT("MC_Plant_Accent"))
            {
                auto* M=Cast<UMaterialInstanceDynamic>(C->GetMaterial(I));
                if(TestNotNull(TEXT("Accent uses a dynamic material"),M))
                    TestTrue(TEXT("Quantised status colour reaches the accent"),M->K2_GetVectorParameterValue(TEXT("Color")).Equals(FLinearColor(.1f,.4f,.7f),.016f));
            }
            else TestTrue(TEXT("Metal, coil and panel materials stay authored"),C->GetMaterial(I)==Mesh->GetMaterial(I));
        }
    }
    TestEqual(TEXT("Seven unique mesh assets"),Drawn.Num(),7);
    const uint32 OldHash=PlantInstances::Hash(List);
    for(auto& I:List)I.Color=FLinearColor::Red;
    TestTrue(TEXT("Fault colour changes the upload key"),PlantInstances::Hash(List)!=OldHash);
    PlantInstances::Apply(Owner,Owner->GetRootComponent(),Sets,List,false);
    int32 Live=0;for(const auto& Pair:Sets)Live+=Pair.Value->GetInstanceCount();
    TestEqual(TEXT("Status changes replace rather than duplicate instances"),Live,7);
    IConsoleVariable* Switch=IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Plant.ReferenceModels"));
    const int32 Old=Switch->GetInt();Switch->Set(0,ECVF_SetByCode);
    TArray<FPlantInstance> Fallback;
    PlantVisual::AppendEquipment(EPlantVisualKind::DryCooler,FVector(2.4,1.2,1.3),FLinearColor::White,Fallback);
    Switch->Set(Old,ECVF_SetByCode);
    TestTrue(TEXT("Primitive fallback remains available"),Fallback.Num()>1);
    World->DestroyWorld(false);return true;
}
#endif
