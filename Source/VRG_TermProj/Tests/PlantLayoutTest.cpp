// Fill out your copyright notice in the Description page of Project Settings.

// S8 F8-1 (Tools/설비냉각_계획.md): plant units placed in layout mode. Placed units set the count of their kind (the same
// sizing rules decide the rest), the yard draws them where they stand, layout edits move / rotate them with one-command
// Undo / Redo and refuse the room's footprint, and the layout file v2 carries them with the plant on/off and topology.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/FacilityPlantDisplayActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/RackThermalActor.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/GjRackLayout.h"
#include "Visualization/PlantDisplay.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Math/PerspectiveMatrix.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgPlantLayoutFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double LayoutRho = 997.0, LayoutCp = 4180.0;

	// The F7 display test plant: two CDU branches (0.8 MW), four CRAH coils, outdoor 30 C.
	bool BuildLayoutPlant(FFacilityPlant& Plant, const FPlantEquipmentCounts& Counts)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.4e6 / (LayoutCp * 10.0) / LayoutRho, 0.4e6 / (LayoutCp * 10.0) / LayoutRho };
		FFacilityThermalSpec Th;
		Th.AmbientK = 303.15;
		FacilityPlantLayout::ApplyEquipmentCounts(Counts, Th, Loop);
		TArray<FPlantCrahCoil> Coils;
		for (int32 U = 0; U < 4; ++U)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		return Plant.Build(Loop, Th, 0.8e6, LayoutRho, LayoutCp, FHydraulicSolveSettings(), Coils, 291.15);
	}

	TArray<FPlantDisplayUnit> MakeUnits(int32 N, double X, double Y)
	{
		TArray<FPlantDisplayUnit> Out;
		for (int32 K = 0; K < N; ++K)
		{
			FPlantDisplayUnit& U = Out.AddDefaulted_GetRef();
			U.CenterM = FVector(X + 3.1 * K, Y + 0.37 * K, 0.65);
			U.SizeM = FVector(2.4, 1.2, 1.3);
			U.Rotation = FQuat(FVector::UpVector, 0.3 * K);
		}
		return Out;
	}

	void DestroyLayoutWorld(UWorld* W) { GEngine->DestroyWorldContext(W); W->DestroyWorld(false); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutCountsTest, "VRG.Plant.Layout.CountsSetUnits", VrgPlantLayoutFlags)
bool FPlantLayoutCountsTest::RunTest(const FString& Parameters)
{
	FFacilityPlant Auto, Zero, Placed;
	const bool bAuto = BuildLayoutPlant(Auto, FPlantEquipmentCounts());
	const bool bZero = BuildLayoutPlant(Zero, FPlantEquipmentCounts());
	FPlantEquipmentCounts Counts;
	Counts.FwsDryCoolers = 5; Counts.CwDryCoolers = 4; Counts.Chillers = 4; Counts.TrimChillers = 1;
	const bool bPlaced = BuildLayoutPlant(Placed, Counts);
	if (!TestTrue(TEXT("the three plants build"), bAuto && bZero && bPlaced)) return false;
	const FFacilityThermalSpec& A = Auto.GetThermalSpec();
	const FFacilityThermalSpec& P = Placed.GetThermalSpec();
	AddInfo(FString::Printf(TEXT("자동: FWS 드라이쿨러 %d(수력 코일 %d), CW %d, 칠러 %d+%d(기준 %.3f kW), 트림 %s %d+%d / 배치: FWS %d(코일 %d), CW %d, 칠러 %d+%d(기준 %.3f kW), 트림 %d+%d"),
		Auto.GetNumDryCoolers(), Auto.GetLoop().GetHandles().ReturnCoilPipes.Num(), Auto.GetNumCwDryCoolers(), A.DutyChillers, A.StandbyChillers,
		Auto.GetChillerRefCapacityW() * 1e-3, Auto.IsTrimActive() ? TEXT("있음") : TEXT("없음"), A.TrimDutyChillers, A.TrimStandbyChillers,
		Placed.GetNumDryCoolers(), Placed.GetLoop().GetHandles().ReturnCoilPipes.Num(), Placed.GetNumCwDryCoolers(), P.DutyChillers, P.StandbyChillers,
		Placed.GetChillerRefCapacityW() * 1e-3, P.TrimDutyChillers, P.TrimStandbyChillers));

	// No placed units: exactly the automatic plant.
	TestTrue(TEXT("no placed units = the automatic counts and capacities"), Zero.GetNumDryCoolers() == Auto.GetNumDryCoolers()
		&& Zero.GetNumCwDryCoolers() == Auto.GetNumCwDryCoolers() && Zero.GetChillerRefCapacityW() == Auto.GetChillerRefCapacityW()
		&& Zero.GetLoop().GetHandles().ReturnCoilPipes.Num() == Auto.GetLoop().GetHandles().ReturnCoilPipes.Num()
		&& Zero.GetTrimRefCapacityW() == Auto.GetTrimRefCapacityW());
	// Placed units: the counts, one FWS coil per unit, chillers N = (N - 1) duty + 1 standby, trim 1 = 1 duty.
	TestTrue(TEXT("dry cooler banks have the placed counts (FWS coils follow)"), Placed.GetNumDryCoolers() == 5
		&& Placed.GetLoop().GetHandles().ReturnCoilPipes.Num() == 5 && Placed.GetNumCwDryCoolers() == 4);
	TestTrue(TEXT("chillers: 4 placed = 3 duty + 1 standby, trim 1 placed = 1 duty + 0 standby"),
		P.DutyChillers == 3 && P.StandbyChillers == 1 && P.TrimDutyChillers == 1 && P.TrimStandbyChillers == 0);
	// Same sizing rule: the duty units together still make the design load (the unit size follows the count).
	const double CapRel = FMath::Abs(Placed.GetChillerRefCapacityW() * P.DutyChillers - Auto.GetChillerRefCapacityW() * A.DutyChillers)
		/ (Auto.GetChillerRefCapacityW() * A.DutyChillers);
	AddInfo(FString::Printf(TEXT("운전 칠러 합계 기준 용량 상대 차 %.3e"), CapRel));
	TestTrue(TEXT("duty chillers' total reference capacity unchanged (network solve tolerance)"), CapRel < 1e-9);
	TestTrue(TEXT("placed plant still holds its network control"), Placed.GetLoopResult().bControlConverged || Placed.GetTopology() != EPlantTopology::Separate);

	// The yard draws placed units where they stand; model units beyond the placed ones keep the schematic positions.
	FPlantDisplayInput In;
	In.RoomM = FBox(FVector(0, 0, 0), FVector(10, 8, 3));
	In.CduPositionsM = { FVector(1, 1, 0), FVector(1, 5, 0) };
	In.CrahPositionsM = { FVector(8, 1, 0), FVector(8, 3, 0), FVector(8, 5, 0), FVector(8, 7, 0) };
	In.FwsDryCoolers = MakeUnits(5, 20.0, -6.0);
	In.CwDryCoolers = MakeUnits(4, 20.0, 12.0);
	In.Chillers = MakeUnits(3, 30.0, 4.0);   // the model has 4 chillers: 3 placed, the 4th schematic
	const FPlantDisplaySnapshot S = PlantDisplay::Build(Placed, In);
	double MaxPosErr = 0.0, MaxViaErr = 0.0;
	int32 PlacedFws = 0, PlacedCw = 0, PlacedChillers = 0, Coils = 0;
	for (const FPlantDisplayEquipment& E : S.Equipment)
	{
		if (E.Kind == EPlantEquipmentKind::DryCooler && E.bPlaced)
		{
			const FPlantDisplayUnit& U = (E.Loop == EPlantLoopId::Fws ? In.FwsDryCoolers : In.CwDryCoolers)[E.Index];
			MaxPosErr = FMath::Max(MaxPosErr, FVector::Distance(E.CenterM, U.CenterM));
			if (!E.Rotation.Equals(U.Rotation, 0.0) || E.SizeM != U.SizeM) MaxPosErr = 1.0;
			(E.Loop == EPlantLoopId::Fws ? PlacedFws : PlacedCw)++;
		}
		if (E.Kind == EPlantEquipmentKind::Chiller && E.bPlaced) ++PlacedChillers;
	}
	// FWS return coils: coil K runs through placed unit K (one coil per unit): its route passes both connections.
	auto OnRoute = [](const FVector& P, const TArray<FVector>& Line)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 I = 0; I + 1 < Line.Num(); ++I) Best = FMath::Min(Best, FMath::PointDistToSegment(P, Line[I], Line[I + 1]));
		return Best;
	};
	for (const FPlantDisplayPipe& Pipe : S.Pipes)
		if (Pipe.Loop == EPlantLoopId::Fws && Pipe.Role == EPlantPipeRole::ReturnCoil)
		{
			const int32 K = Placed.GetLoop().GetHandles().ReturnCoilPipes.IndexOfByKey(Pipe.Edge);
			const FPlantDisplayEquipment* Unit = S.Equipment.FindByPredicate([K](const FPlantDisplayEquipment& E)
				{ return E.Loop == EPlantLoopId::Fws && E.Kind == EPlantEquipmentKind::DryCooler && E.Index == K && E.bPlaced; });
			if (Unit == nullptr) { MaxViaErr = 1.0; continue; }
			MaxViaErr = FMath::Max(MaxViaErr, FMath::Max(OnRoute(PlantDisplay::PortM(*Unit, EPlantPort::CoilIn), Pipe.PointsM),
				OnRoute(PlantDisplay::PortM(*Unit, EPlantPort::CoilOut), Pipe.PointsM)));
			++Coils;
		}
	AddInfo(FString::Printf(TEXT("표시: 배치 FWS %d / CW %d / 칠러 %d(모델 4대 중 배치 3), 위치 최대 차 %.3e m, 코일 %d개 접속점 최대 차 %.3e m"),
		PlacedFws, PlacedCw, PlacedChillers, MaxPosErr, Coils, MaxViaErr));
	TestTrue(TEXT("placed dry coolers drawn at their units (exact)"), PlacedFws == 5 && PlacedCw == 4 && MaxPosErr == 0.0);
	TestTrue(TEXT("placed chillers drawn at their units, the model's extra one in the yard"), PlacedChillers == 3);
	TestTrue(TEXT("FWS return coil K runs through unit K's connections"), Coils == 5 && MaxViaErr < 1e-9);
	// Without placed units the snapshot is the F7 one (no unit flagged placed).
	FPlantDisplayInput In0 = In; In0.FwsDryCoolers.Reset(); In0.CwDryCoolers.Reset(); In0.Chillers.Reset();
	const FPlantDisplaySnapshot S0 = PlantDisplay::Build(Auto, In0);
	TestTrue(TEXT("no placed units: nothing flagged placed"), !S0.Equipment.ContainsByPredicate([](const FPlantDisplayEquipment& E) { return E.bPlaced; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutEditTest, "VRG.Plant.Layout.MoveRotateUndoSave", VrgPlantLayoutFlags)
bool FPlantLayoutEditTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>();
	W->SpawnActor<ARackThermalActor>(FVector(300, 300, 0), FRotator::ZeroRotator);
	// Air domain 10 x 8 x 3 m at the origin (automatic room: the grid actor's domain is the footprint).
	auto* Air = W->SpawnActor<AAirFieldDebugActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	Air->bShowSummary = false;
	Air->GridParams.CellSizeM = 0.25; Air->GridParams.NumCellsX = 40; Air->GridParams.NumCellsY = 32; Air->GridParams.NumCellsZ = 12;
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };

	FPlantEquipmentRecord RA{ TEXT("plant/test/a"), TEXT("PlantUnitA"), FTransform(FVector(1500, 200, 0)), EPlantUnitKind::FwsDryCooler,
		APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::FwsDryCooler) };
	// B: yaw 90 at x = 10.65 m, its X half extent 0.6 m clears the room edge (10 m); at yaw 0 (half 1.2 m) it would not.
	FPlantEquipmentRecord RB{ TEXT("plant/test/b"), TEXT("PlantUnitB"), FTransform(FRotator(0, 90, 0), FVector(1065, 500, 0)), EPlantUnitKind::Chiller,
		APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::Chiller) };
	APlantEquipmentActor* A = VrgPlantEquipment::Spawn(W, RA);
	APlantEquipmentActor* B = VrgPlantEquipment::Spawn(W, RB);
	if (!TestTrue(TEXT("units spawned outside the room"), A && B && !VrgPlantEquipment::OverlapsRoom(W, A->SizeM, A->GetActorTransform())
		&& !VrgPlantEquipment::OverlapsRoom(W, B->SizeM, B->GetActorTransform()))) { DestroyLayoutWorld(W); return false; }
	FVector SizeM;
	TestTrue(TEXT("a plant unit is a placeable box of its size"), ADataCenterDebugController::PlaceableBoxM(A, SizeM) && SizeM == A->SizeM);
	const FTransform A0 = A->GetActorTransform(), B0 = B->GetActorTransform();

	// Drag into the room: shown blocked, release changes nothing.
	C->BeginLayoutDragAlongRay(Above(A0.GetLocation()), Down);
	TestTrue(TEXT("plant unit arms the drag"), C->IsLayoutDragging());
	C->UpdateLayoutDragAlongRay(Above(A0.GetLocation()) - FVector(600, 0, 0), Down);
	TestTrue(TEXT("over the room is blocked"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true);
	TestTrue(TEXT("blocked release: no move, no history"), A->GetActorTransform().Equals(A0, 0.0) && C->GetLayoutUndoCount() == 0);
	// Drag along the yard: committed with the 5 cm snap, one command.
	C->BeginLayoutDragAlongRay(Above(A0.GetLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(A0.GetLocation()) + FVector(203, 0, 0), Down);
	TestFalse(TEXT("yard move not blocked"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true);
	const FTransform A1 = A->GetActorTransform();
	TestTrue(TEXT("moved 205 cm (snap), one command"), A1.GetLocation().Equals(A0.GetLocation() + FVector(205, 0, 0), 1e-9) && C->GetLayoutUndoCount() == 1);
	// Onto B: equipment overlap blocks as for any layout box.
	C->BeginLayoutDragAlongRay(Above(A1.GetLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(A1.GetLocation()) + (B0.GetLocation() - A1.GetLocation()), Down);
	TestTrue(TEXT("onto another unit is blocked"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(false);
	// Rotation: A turns (recorded); B's turn would reach into the room and is refused.
	TestTrue(TEXT("rotate A 90 deg, one command"), C->TryLayoutRotation(A, FRotator(0, 90, 0), true) && C->GetLayoutUndoCount() == 2);
	const FTransform A2 = A->GetActorTransform();
	TestFalse(TEXT("B turned into the room is refused"), C->TryLayoutRotation(B, FRotator::ZeroRotator, true));
	TestTrue(TEXT("refused turn leaves B and the history"), B->GetActorTransform().Equals(B0, 0.0) && C->GetLayoutUndoCount() == 2);
	TestTrue(TEXT("Undo rotate + move"), C->TryUndoLayout() && C->TryUndoLayout() && A->GetActorTransform().Equals(A0, 1e-9));
	TestTrue(TEXT("Redo move + rotate"), C->TryRedoLayout() && C->TryRedoLayout() && A->GetActorTransform().Equals(A2, 1e-9));

	// Save -> change everything -> Load: the same units (name, id, kind, size, transform) and the plant settings.
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.Topology = EPlantTopology::Series;
	const FString Name = TEXT("VrgTest_PlantLayoutF8"), OldName = TEXT("VrgTest_PlantLayoutF8_PreF8");
	FString Summary;
	const bool bSaved = VrgLayoutFile::Save(W, Name, Summary);
	AddInfo(TEXT("저장: ") + Summary);
	const FPlantEquipmentRecord SavedA = VrgPlantEquipment::Capture(A), SavedB = VrgPlantEquipment::Capture(B);
	TestTrue(TEXT("removed both"), bSaved && VrgPlantEquipment::RemoveBatch({ A, B }));
	Sim->bFacilityPlant = false;
	Sim->PlantThermalSpec.Topology = EPlantTopology::Separate;
	const bool bLoaded = VrgLayoutFile::Load(W, Name, Summary);
	AddInfo(TEXT("불러오기: ") + Summary);
	auto Same = [](const APlantEquipmentActor* U, const FPlantEquipmentRecord& R)
	{
		return U && U->GetFName() == R.Name && VrgPlantEquipment::Identity(U) == R.Id && U->Kind == R.Kind && U->SizeM.Equals(R.SizeM, 1e-12)
			&& U->GetActorTransform().Equals(R.Transform, 1e-6);
	};
	A = VrgPlantEquipment::Find(W, SavedA.Id); B = VrgPlantEquipment::Find(W, SavedB.Id);
	TestTrue(TEXT("load restores the units"), bLoaded && Same(A, SavedA) && Same(B, SavedB));
	TestTrue(TEXT("load restores plant on + series"), Sim->bFacilityPlant && Sim->PlantThermalSpec.Topology == EPlantTopology::Series);

	// A file whose units would stand in the room is refused and changes nothing (the room moved over the yard).
	Air->SetActorLocation(FVector(1200, 0, 0));
	Sim->bFacilityPlant = false;
	const bool bRefused = !VrgLayoutFile::Load(W, Name, Summary);
	AddInfo(TEXT("방과 겹치는 불러오기: ") + Summary);
	TestTrue(TEXT("refused load keeps units and settings"), bRefused && Same(VrgPlantEquipment::Find(W, SavedA.Id), SavedA)
		&& Same(VrgPlantEquipment::Find(W, SavedB.Id), SavedB) && !Sim->bFacilityPlant);
	Air->SetActorLocation(FVector::ZeroVector);

	// A file written before F8 (no plant fields): the units and the plant settings stay as they are.
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (FFileHelper::LoadFileToString(Text, *VrgLayoutFile::PathFor(Name)) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
	{
		Root->RemoveField(TEXT("plantInventory")); Root->RemoveField(TEXT("plantEquipment"));
		Root->GetObjectField(TEXT("settings"))->RemoveField(TEXT("plantEnabled"));
		Root->GetObjectField(TEXT("settings"))->RemoveField(TEXT("plantTopology"));
		FString Old;
		FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Old));
		FFileHelper::SaveStringToFile(Old, *VrgLayoutFile::PathFor(OldName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	A = VrgPlantEquipment::Find(W, SavedA.Id);
	const FTransform Moved(FRotator(0, 180, 0), FVector(2000, -300, 0));
	if (A) A->SetActorTransform(Moved);
	Sim->bFacilityPlant = false;
	Sim->PlantThermalSpec.Topology = EPlantTopology::SharedChw;
	const bool bOldLoaded = VrgLayoutFile::Load(W, OldName, Summary);
	AddInfo(TEXT("F8 이전 형식: ") + Summary);
	TestTrue(TEXT("pre-F8 file keeps the units and plant settings"), bOldLoaded && A == VrgPlantEquipment::Find(W, SavedA.Id)
		&& A->GetActorTransform().Equals(Moved, 1e-9) && !Sim->bFacilityPlant && Sim->PlantThermalSpec.Topology == EPlantTopology::SharedChw);

	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(OldName));
	DestroyLayoutWorld(W);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutCoupledTest, "VRG.Plant.Layout.PlacedUnitsDrivePlant", VrgPlantLayoutFlags)
bool FPlantLayoutCoupledTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
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
	auto Run = [&]() { Sim->EnterSimulationMode(); for (int32 F = 0; F < 20; ++F) Sim->Tick(.1f); };

	// Reference: automatic counts.
	Run();
	const FFacilityPlant& Plant = Sim->GetFacilityPlant();
	const int32 AutoFws = Plant.GetNumDryCoolers(), AutoCw = Plant.GetNumCwDryCoolers();
	const int32 AutoDuty = Plant.GetThermalSpec().DutyChillers, AutoStandby = Plant.GetThermalSpec().StandbyChillers;
	const bool bChw = Plant.IsChilledWaterActive();
	Sim->EnterLayoutMode();

	// Placed: 3 FWS dry coolers and 2 chillers far out in the yard (+X, beyond any automatic room fit).
	TArray<FPlantEquipmentRecord> Records;
	for (int32 K = 0; K < 3; ++K)
		Records.Add({ FString::Printf(TEXT("plant/test/fws%d"), K), FName(*FString::Printf(TEXT("PlantFws%d"), K)),
			FTransform(FRotator(0, 30.0 * K, 0), FVector(4000 + 400 * K, -500, 0)), EPlantUnitKind::FwsDryCooler, APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::FwsDryCooler) });
	for (int32 K = 0; K < 2; ++K)
		Records.Add({ FString::Printf(TEXT("plant/test/chiller%d"), K), FName(*FString::Printf(TEXT("PlantChiller%d"), K)),
			FTransform(FVector(4000 + 400 * K, 800, 0)), EPlantUnitKind::Chiller, APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::Chiller) });
	TArray<APlantEquipmentActor*> Units;
	for (const FPlantEquipmentRecord& R : Records) Units.Add(VrgPlantEquipment::Spawn(World, R));
	if (!TestFalse(TEXT("units spawned"), Units.Contains(nullptr))) { DestroyLayoutWorld(World); return false; }
	Run();
	const FFacilityThermalSpec& T = Plant.GetThermalSpec();
	const int32 Fws = Plant.GetNumDryCoolers(), Coils = Plant.GetLoop().GetHandles().ReturnCoilPipes.Num();
	const double Site = Sim->GetSiteEnergyResidual(), Ledger = Plant.GetLedgerResidual();
	double MaxPosErr = 0.0;
	int32 PlacedDrawn = 0;
	if (const AFacilityPlantDisplayActor* D = Sim->GetPlantDisplay())
	{
		const TArray<APlantEquipmentActor*> FwsActors = VrgPlantEquipment::Gather(World, EPlantUnitKind::FwsDryCooler);
		for (const FPlantDisplayEquipment& E : D->GetSnapshot().Equipment)
		{
			if (!E.bPlaced) continue;
			++PlacedDrawn;
			if (E.Kind == EPlantEquipmentKind::DryCooler && E.Loop == EPlantLoopId::Fws && FwsActors.IsValidIndex(E.Index))
			{
				const APlantEquipmentActor* U = FwsActors[E.Index];
				const FVector Centre = SimUnits::UnrealToMeters(U->GetActorTransform().TransformPosition(FVector(0, 0, 50.0 * U->SizeM.Z)));
				MaxPosErr = FMath::Max(MaxPosErr, FVector::Distance(Centre, E.CenterM));
				if (!E.Rotation.Equals(U->GetActorQuat(), 1e-12)) MaxPosErr = 1.0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("자동: FWS 드라이쿨러 %d, CW %d, 칠러 %d+%d (냉수 %s) / 배치: FWS %d(코일 %d), 칠러 %d+%d, 그려진 배치 장비 %d, 위치 최대 차 %.3e m, 설비 장부 %.3e, 외기까지 %.3e"),
		AutoFws, AutoCw, AutoDuty, AutoStandby, bChw ? TEXT("있음") : TEXT("없음"), Fws, Coils, T.DutyChillers, T.StandbyChillers, PlacedDrawn, MaxPosErr, Ledger, Site));
	TestTrue(TEXT("placed FWS dry coolers set the bank and the coils"), Plant.IsThermalActive() && Fws == 3 && Coils == 3);
	TestTrue(TEXT("placed chillers set duty + standby (when the plant has chilled water)"), !bChw || (T.DutyChillers == 1 && T.StandbyChillers == 1));
	TestTrue(TEXT("unplaced kind keeps its automatic count"), Plant.GetNumCwDryCoolers() > 0 || !Plant.IsCondenserActive());
	TestTrue(TEXT("yard draws the placed units at their actors"), PlacedDrawn == (bChw ? 5 : 3) && MaxPosErr < 1e-9);
	TestTrue(TEXT("ledgers still close"), FMath::Abs(Ledger) < 1e-9 && FMath::Abs(Site) < 1e-9);
	TestTrue(TEXT("the integrated actor's spec is not overwritten"), Sim->PlantThermalSpec.DryCoolers == 0 && Sim->PlantThermalSpec.DutyChillers == AutoDuty);
	Sim->EnterLayoutMode();

	// Remove the units: back to the automatic sizing.
	TestTrue(TEXT("units removed"), VrgPlantEquipment::RemoveBatch(Units));
	Run();
	TestTrue(TEXT("no units again = the automatic counts"), Plant.GetNumDryCoolers() == AutoFws && Plant.GetNumCwDryCoolers() == AutoCw
		&& Plant.GetThermalSpec().DutyChillers == AutoDuty && Plant.GetThermalSpec().StandbyChillers == AutoStandby);
	Sim->EnterLayoutMode();
	DestroyLayoutWorld(World);
	return true;
}

// S8 F8-2: palette addition / deletion with one-command Undo / Redo. The first addition of a kind turns the automatic
// units of the last simulation entry into actors at their yard positions in the same command.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutAddDeleteTest, "VRG.Plant.Layout.AddDeleteUndo", VrgPlantLayoutFlags)
bool FPlantLayoutAddDeleteTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	auto* C = World->SpawnActor<ADataCenterDebugController>();
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
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };
	auto CountFws = [&]() { return VrgPlantEquipment::Gather(World, EPlantUnitKind::FwsDryCooler).Num(); };

	// No plant built yet: the automatic count is unknown, the addition does not start.
	C->BeginPlantAddition(EPlantUnitKind::FwsDryCooler);
	TestFalse(TEXT("refused before any plant was built"), C->IsAddingPlant());

	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	const int32 AutoFws = Sim->GetFacilityPlant().GetNumDryCoolers();
	Sim->EnterLayoutMode();
	TArray<FPlantEquipmentRecord> Expected; FString Reason;
	const bool bCaptured = Sim->CaptureAutomaticPlantUnits(EPlantUnitKind::FwsDryCooler, Expected, Reason);
	if (!TestTrue(TEXT("automatic FWS units captured"), bCaptured && Expected.Num() == AutoFws && AutoFws > 0)) { DestroyLayoutWorld(World); return false; }

	C->BeginPlantAddition(EPlantUnitKind::FwsDryCooler);
	TestTrue(TEXT("addition starts after a plant was built"), C->IsAddingPlant());
	C->UpdateRackAdditionAlongRay(Above(FVector::ZeroVector), Down);
	TestTrue(TEXT("over the room is blocked"), C->IsRackAdditionBlocked());
	C->UpdateRackAdditionAlongRay(Above(Expected[0].Transform.GetLocation()), Down);
	TestTrue(TEXT("over an automatic unit to be converted is blocked"), C->IsRackAdditionBlocked());
	TestNull(TEXT("blocked confirm adds nothing"), C->ConfirmPlantAddition());
	C->UpdateRackAdditionAlongRay(Above(FVector(5002, -498, 0)), Down);
	TestFalse(TEXT("yard spot is free"), C->IsRackAdditionBlocked());
	APlantEquipmentActor* Added = C->ConfirmPlantAddition();
	if (!TestNotNull(TEXT("first addition"), Added)) { DestroyLayoutWorld(World); return false; }
	const FString AddedId = VrgPlantEquipment::Identity(Added);
	const FName AddedName = Added->GetFName();
	const FTransform AddedT = Added->GetActorTransform();
	// Converted units stand where the automatic yard drew them.
	double MaxErrCm = 0.0;
	int32 Matched = 0;
	for (const FPlantEquipmentRecord& R : Expected)
		for (const APlantEquipmentActor* U : VrgPlantEquipment::Gather(World, EPlantUnitKind::FwsDryCooler))
			if (U != Added && FVector::Distance(U->GetActorLocation(), R.Transform.GetLocation()) < 1.0)
			{ ++Matched; MaxErrCm = FMath::Max(MaxErrCm, FVector::Distance(U->GetActorLocation(), R.Transform.GetLocation())); }
	AddInfo(FString::Printf(TEXT("자동 FWS 드라이쿨러 %d대 → 첫 추가 뒤 %d대(전환 위치 일치 %d, 최대 차 %.3e cm), 새 장비 %s"),
		AutoFws, CountFws(), Matched, MaxErrCm, *AddedT.GetLocation().ToCompactString()));
	TestTrue(TEXT("first addition = automatic units + 1, one command"), CountFws() == AutoFws + 1 && C->GetLayoutUndoCount() == 1);
	TestTrue(TEXT("converted units at the automatic positions"), Matched == AutoFws && MaxErrCm == 0.0);
	TestTrue(TEXT("snapped to 5 cm, new unit selected"), AddedT.GetLocation().Equals(FVector(5000, -500, 0), 1e-9) && C->GetSelectedLayoutActor() == Added);
	TestTrue(TEXT("nothing left to convert"), Sim->CaptureAutomaticPlantUnits(EPlantUnitKind::FwsDryCooler, Expected, Reason) && Expected.IsEmpty());

	// One Undo returns to the automatic plant (no FWS units), Redo brings all of them back with the same identity.
	TestTrue(TEXT("Undo removes converted + new"), C->TryUndoLayout() && CountFws() == 0);
	TestTrue(TEXT("Redo restores them"), C->TryRedoLayout() && CountFws() == AutoFws + 1);
	Added = VrgPlantEquipment::Find(World, AddedId);
	TestTrue(TEXT("same name / transform after Redo"), Added && Added->GetFName() == AddedName && Added->GetActorTransform().Equals(AddedT, 1e-9));

	// A second addition converts nothing.
	C->BeginPlantAddition(EPlantUnitKind::FwsDryCooler);
	C->UpdateRackAdditionAlongRay(Above(FVector(5600, -500, 0)), Down);
	APlantEquipmentActor* Second = C->ConfirmPlantAddition();
	TestTrue(TEXT("second addition adds one"), Second != nullptr && CountFws() == AutoFws + 2 && C->GetLayoutUndoCount() == 2);

	// Delete the first (one command), Undo puts back the same unit.
	C->SelectLayoutAlongRay(Above(AddedT.GetLocation()), Down);
	TestTrue(TEXT("first unit selected"), C->GetSelectedLayoutActor() == Added);
	TestTrue(TEXT("delete one command"), C->TryDeleteLayoutPlant() && CountFws() == AutoFws + 1 && C->GetLayoutUndoCount() == 3);
	TestTrue(TEXT("Undo delete"), C->TryUndoLayout() && CountFws() == AutoFws + 2);
	Added = VrgPlantEquipment::Find(World, AddedId);
	TestTrue(TEXT("restored same name / transform"), Added && Added->GetFName() == AddedName && Added->GetActorTransform().Equals(AddedT, 1e-9));
	// A rack selection is not a plant deletion.
	C->SelectLayoutAlongRay(Above(FVector(0, 0, 0)), Down);
	TestFalse(TEXT("rack selection is not deleted as plant"), C->TryDeleteLayoutPlant());

	// The plant at the next entry has the placed count.
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	const FFacilityPlant& Plant = Sim->GetFacilityPlant();
	int32 Drawn = 0;
	if (const AFacilityPlantDisplayActor* D = Sim->GetPlantDisplay())
		for (const FPlantDisplayEquipment& E : D->GetSnapshot().Equipment) Drawn += E.bPlaced && E.Loop == EPlantLoopId::Fws ? 1 : 0;
	AddInfo(FString::Printf(TEXT("진입: FWS 드라이쿨러 %d대(배치로 그림 %d), 설비 장부 %.3e, 외기까지 %.3e"), Plant.GetNumDryCoolers(), Drawn,
		Plant.GetLedgerResidual(), Sim->GetSiteEnergyResidual()));
	TestTrue(TEXT("plant built with the placed count"), Plant.GetNumDryCoolers() == AutoFws + 2 && Drawn == AutoFws + 2);
	TestTrue(TEXT("ledgers close"), FMath::Abs(Plant.GetLedgerResidual()) < 1e-9 && FMath::Abs(Sim->GetSiteEnergyResidual()) < 1e-9);
	Sim->EnterLayoutMode();
	DestroyLayoutWorld(World);
	return true;
}

// S8 F8-3: Ctrl+D of a plant unit selection (one or several, kinds mixed) and the shared multi-selection edits
// (rectangle selection, group drag / quarter turn / delete) on plant units, each one command with Undo / Redo.
// No plant is built here: duplication converts nothing, so unlike a palette addition it needs no automatic count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutDuplicateMultiTest, "VRG.Plant.Layout.DuplicateMulti", VrgPlantLayoutFlags)
bool FPlantLayoutDuplicateMultiTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>();
	ARackThermalActor* Rack = W->SpawnActor<ARackThermalActor>(FVector(300, 300, 0), FRotator::ZeroRotator);
	// Air domain 10 x 8 x 3 m at the origin, as in MoveRotateUndoSave.
	auto* Air = W->SpawnActor<AAirFieldDebugActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	Air->bShowSummary = false;
	Air->GridParams.CellSizeM = 0.25; Air->GridParams.NumCellsX = 40; Air->GridParams.NumCellsY = 32; Air->GridParams.NumCellsZ = 12;
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };
	auto Count = [&]() { const FPlantEquipmentCounts N = VrgPlantEquipment::CountInWorld(W); return N.FwsDryCoolers + N.CwDryCoolers + N.Chillers + N.TrimChillers; };

	FPlantEquipmentRecord RA{ TEXT("plant/test/dup-a"), TEXT("PlantDupA"), FTransform(FVector(1500, 200, 0)), EPlantUnitKind::FwsDryCooler,
		APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::FwsDryCooler) };
	FPlantEquipmentRecord RB{ TEXT("plant/test/dup-b"), TEXT("PlantDupB"), FTransform(FRotator(0, 90, 0), FVector(1500, 600, 0)), EPlantUnitKind::Chiller,
		APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::Chiller) };
	APlantEquipmentActor* A = VrgPlantEquipment::Spawn(W, RA);
	APlantEquipmentActor* B = VrgPlantEquipment::Spawn(W, RB);
	if (!TestTrue(TEXT("units spawned"), A && B)) { DestroyLayoutWorld(W); return false; }
	const FTransform A0 = A->GetActorTransform(), B0 = B->GetActorTransform();

	// Single duplication: same kind, size and rotation, a new identity, outside the room and clear of other units.
	C->SelectLayoutAlongRay(Above(A0.GetLocation()), Down);
	TestTrue(TEXT("plant duplication starts without a built plant"), C->BeginPlantDuplication() && C->IsAddingPlant() && C->GetRackAdditionPreviewCount() == 1);
	C->UpdateRackAdditionAlongRay(Above(FVector(500, 400, 0)), Down);
	TestTrue(TEXT("copy over the room is blocked"), C->IsRackAdditionBlocked());
	C->UpdateRackAdditionAlongRay(Above(B0.GetLocation()), Down);
	TestTrue(TEXT("copy onto another unit is blocked"), C->IsRackAdditionBlocked());
	TestNull(TEXT("blocked confirm adds nothing"), C->ConfirmPlantAddition());
	C->UpdateRackAdditionAlongRay(Above(FVector(2002, 201, 0)), Down);
	TestFalse(TEXT("yard spot is free"), C->IsRackAdditionBlocked());
	APlantEquipmentActor* Copy = C->ConfirmPlantAddition();
	if (!TestNotNull(TEXT("single copy"), Copy)) { DestroyLayoutWorld(W); return false; }
	const FString CopyId = VrgPlantEquipment::Identity(Copy);
	const FName CopyName = Copy->GetFName();
	const FTransform CopyT = Copy->GetActorTransform();
	TestTrue(TEXT("copy: same kind / size, new identity, snapped, selected, one command"), Copy->Kind == A->Kind && Copy->SizeM == A->SizeM
		&& CopyId != VrgPlantEquipment::Identity(A) && CopyName != A->GetFName() && CopyT.GetLocation().Equals(FVector(2000, 200, 0), 1e-9)
		&& CopyT.GetRotation().Equals(A0.GetRotation(), 0.0) && C->GetSelectedLayoutActors().Num() == 1 && C->GetSelectedLayoutActor() == Copy
		&& C->GetLayoutUndoCount() == 1 && Count() == 3 && !C->IsAddingRack());
	TestTrue(TEXT("Undo removes the copy"), C->TryUndoLayout() && Count() == 2 && VrgPlantEquipment::Find(W, CopyId) == nullptr);
	TestTrue(TEXT("Redo restores it with the same name / transform"), C->TryRedoLayout() && Count() == 3);
	Copy = VrgPlantEquipment::Find(W, CopyId);
	TestTrue(TEXT("same copy after Redo"), Copy && Copy->GetFName() == CopyName && Copy->GetActorTransform().Equals(CopyT, 0.0));
	// Esc: the preview goes, nothing changes.
	C->SelectLayoutAlongRay(Above(A0.GetLocation()), Down);
	C->BeginPlantDuplication();
	C->CancelRackAddition();
	TestTrue(TEXT("cancelled duplication adds nothing"), !C->IsAddingRack() && Count() == 3 && C->GetLayoutUndoCount() == 1);

	// Rectangle selection takes plant units: the projected boxes of A and B from a camera above the yard.
	const FVector Eye(1500, 400, 3000);
	const FMatrix ViewProjection = FLookAtMatrix(Eye, Eye + Down, FVector(1, 0, 0)) * FReversedZPerspectiveMatrix(UE_DOUBLE_PI / 4.0, 1.0, 1.0, 10.0);
	const FIntRect View(0, 0, 400, 400);
	FBox2D BoxA, BoxB;
	const bool bProjected = ADataCenterDebugController::ProjectLayoutBoxToScreen(A, ViewProjection, View, BoxA)
		&& ADataCenterDebugController::ProjectLayoutBoxToScreen(B, ViewProjection, View, BoxB);
	TestTrue(TEXT("plant boxes project"), bProjected);
	const FBox2D PairRect = BoxA + BoxB;
	C->SelectLayoutInScreenRect(PairRect, ViewProjection, View, false);
	const TArray<AActor*> Marquee = C->GetSelectedLayoutActors();
	TestTrue(TEXT("rectangle selects A and B (not the copy or the rack)"), Marquee.Num() == 2 && Marquee.Contains(A) && Marquee.Contains(B));

	// Multiple duplication, kinds mixed: each keeps its kind, size, rotation and offset from the first.
	TestTrue(TEXT("group duplication starts"), C->BeginPlantDuplication() && C->GetRackAdditionPreviewCount() == 2);
	const FVector Shift(0, 1500, 0);
	C->UpdateRackAdditionAlongRay(Above(Marquee[0]->GetActorLocation() + Shift), Down);
	TestFalse(TEXT("group copy in the yard is free"), C->IsRackAdditionBlocked());
	TestNotNull(TEXT("group copy confirmed"), C->ConfirmPlantAddition());
	const TArray<AActor*> Copies = C->GetSelectedLayoutActors();
	double MaxErrCm = 0.0;
	bool bKinds = Copies.Num() == 2;
	for (int32 I = 0; I < Copies.Num() && bKinds; ++I)
	{
		const auto* Src = CastChecked<APlantEquipmentActor>(Marquee[I]);
		const auto* Dup = Cast<APlantEquipmentActor>(Copies[I]);
		bKinds &= Dup && Dup != Src && Dup->Kind == Src->Kind && Dup->SizeM == Src->SizeM && Dup->GetActorQuat().Equals(Src->GetActorQuat(), 0.0);
		if (Dup) MaxErrCm = FMath::Max(MaxErrCm, FVector::Distance(Dup->GetActorLocation(), Src->GetActorLocation() + Shift));
	}
	const FPlantEquipmentCounts AfterGroup = VrgPlantEquipment::CountInWorld(W);
	AddInfo(FString::Printf(TEXT("묶음 복제: %d대, 위치 최대 차 %.3e cm, FWS %d / 칠러 %d"), Copies.Num(), MaxErrCm, AfterGroup.FwsDryCoolers, AfterGroup.Chillers));
	TestTrue(TEXT("copies keep kind / size / rotation / offsets"), bKinds && MaxErrCm == 0.0);
	TestTrue(TEXT("counts per kind grow by the copies, one command"), AfterGroup.FwsDryCoolers == 3 && AfterGroup.Chillers == 2 && C->GetLayoutUndoCount() == 2);
	TestTrue(TEXT("one Undo removes the whole group"), C->TryUndoLayout() && Count() == 3);
	TestTrue(TEXT("Redo brings it back"), C->TryRedoLayout() && Count() == 5);
	// A copy that would overlap its own sources is blocked (shifted by 50 cm).
	C->SelectLayoutInScreenRect(PairRect, ViewProjection, View, false);
	C->BeginPlantDuplication();
	C->UpdateRackAdditionAlongRay(Above(Marquee[0]->GetActorLocation() + FVector(0, 50, 0)), Down);
	TestTrue(TEXT("group copy over its sources is blocked"), C->IsRackAdditionBlocked());
	C->CancelRackAddition();

	// Mixed with a rack: neither plant duplication nor plant deletion takes the selection.
	C->SelectLayoutAlongRay(Above(A0.GetLocation()), Down);
	C->SelectLayoutAlongRay(Above(Rack->GetActorLocation()), Down, true);
	TestTrue(TEXT("plant + rack selected"), C->GetSelectedLayoutActors().Num() == 2);
	TestFalse(TEXT("mixed selection is not a plant duplication"), C->BeginPlantDuplication());
	TestFalse(TEXT("mixed selection is not a plant deletion"), C->TryDeleteLayoutPlant());

	// Group drag of A and B: into the room blocked, along the yard one command.
	const int32 Undo0 = C->GetLayoutUndoCount();
	C->SelectLayoutInScreenRect(PairRect, ViewProjection, View, false);
	C->BeginLayoutDragAlongRay(Above(A0.GetLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(A0.GetLocation()) - FVector(600, 0, 0), Down);
	TestTrue(TEXT("group drag into the room is blocked"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true);
	TestTrue(TEXT("blocked group release changes nothing"), A->GetActorTransform().Equals(A0, 0.0) && B->GetActorTransform().Equals(B0, 0.0)
		&& C->GetLayoutUndoCount() == Undo0);
	C->BeginLayoutDragAlongRay(Above(A0.GetLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(A0.GetLocation()) + FVector(0, -1003, 0), Down);
	TestFalse(TEXT("group drag along the yard is free"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true);
	const FVector Moved(0, -1005, 0);
	TestTrue(TEXT("both moved by the same snapped offset, one command"), A->GetActorLocation().Equals(A0.GetLocation() + Moved, 1e-9)
		&& B->GetActorLocation().Equals(B0.GetLocation() + Moved, 1e-9) && C->GetLayoutUndoCount() == Undo0 + 1);
	// Group quarter turn about the pair's footprint centre (stays outside the room), one command.
	const FTransform A1 = A->GetActorTransform(), B1 = B->GetActorTransform();
	TestTrue(TEXT("group turn"), C->TryRotateLayoutSelection() && C->GetLayoutUndoCount() == Undo0 + 2);
	const double YawA = FRotator::NormalizeAxis(A->GetActorRotation().Yaw - A1.Rotator().Yaw);
	const double YawB = FRotator::NormalizeAxis(B->GetActorRotation().Yaw - B1.Rotator().Yaw);
	TestTrue(TEXT("both turned 90 deg, outside the room"), FMath::IsNearlyEqual(YawA, 90.0, 1e-6) && FMath::IsNearlyEqual(YawB, 90.0, 1e-6)
		&& !VrgPlantEquipment::OverlapsRoom(W, A->SizeM, A->GetActorTransform()) && !VrgPlantEquipment::OverlapsRoom(W, B->SizeM, B->GetActorTransform()));
	TestTrue(TEXT("Undo turn + drag"), C->TryUndoLayout() && C->TryUndoLayout() && A->GetActorTransform().Equals(A0, 1e-9) && B->GetActorTransform().Equals(B0, 1e-9));
	TestTrue(TEXT("Redo drag + turn"), C->TryRedoLayout() && C->TryRedoLayout() && !A->GetActorTransform().Equals(A0, 1e-3));
	TestTrue(TEXT("Undo again"), C->TryUndoLayout() && C->TryUndoLayout() && A->GetActorTransform().Equals(A0, 1e-9));

	// Group deletion: one command, Undo brings both back with their identity.
	C->SelectLayoutInScreenRect(PairRect, ViewProjection, View, false);
	TestTrue(TEXT("group delete, one command"), C->TryDeleteLayoutPlant() && Count() == 3 && !VrgPlantEquipment::Find(W, RA.Id) && !VrgPlantEquipment::Find(W, RB.Id));
	TestTrue(TEXT("Undo group delete"), C->TryUndoLayout() && Count() == 5);
	A = VrgPlantEquipment::Find(W, RA.Id); B = VrgPlantEquipment::Find(W, RB.Id);
	TestTrue(TEXT("same units back"), A && B && A->GetFName() == RA.Name && B->GetFName() == RB.Name
		&& A->GetActorTransform().Equals(A0, 1e-9) && B->GetActorTransform().Equals(B0, 1e-9));
	DestroyLayoutWorld(W);
	return true;
}

// 2026-10-22 user report: "배치 모드에서는 모든 오브젝트들을 옮길 수 있어야 한다" - the yard's automatic units were drawn
// instances that no click could pick, and a unit added in layout mode had no pipe until the next simulation entry.
// A press on an automatic unit converts its kind at the drawn positions (one command) and the unit then drags like any
// placed one; adding / moving units in layout mode redraws the plant at once with every unit piped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantLayoutAutomaticMovableTest, "VRG.Plant.Layout.AutomaticUnitsMovable", VrgPlantLayoutFlags)
bool FPlantLayoutAutomaticMovableTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	auto* C = World->SpawnActor<ADataCenterDebugController>();
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
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };
	// The display redraws on a 0.25 s wall-clock throttle (display only): wait it out, then tick the layout frame.
	auto Redraw = [&]() { FPlatformProcess::Sleep(0.3f); Sim->Tick(.1f); return Sim->GetPlantDisplay(); };
	// Units of the drawn yard with no drawn pipe reaching their box (5 cm display tolerance).
	auto Loose = [](const FPlantDisplaySnapshot& S)
	{
		int32 N = 0;
		for (const FPlantDisplayEquipment& E : S.Equipment)
		{
			const FVector Half = 0.5 * E.SizeM + FVector(0.05);
			bool bAny = false;
			for (const FPlantDisplayPipe& P : S.Pipes)
				for (const FVector& V : P.PointsM)
				{
					const FVector L = E.Rotation.UnrotateVector(V - E.CenterM).GetAbs();
					bAny |= L.X <= Half.X && L.Y <= Half.Y && L.Z <= Half.Z;
				}
			N += bAny ? 0 : 1;
		}
		return N;
	};
	auto Chillers = [&]() { return VrgPlantEquipment::Gather(World, EPlantUnitKind::Chiller).Num(); };

	TArray<FPlantEquipmentRecord> Auto; FString Reason;
	if (!TestTrue(TEXT("automatic chillers drawn"), Sim->CaptureAutomaticPlantUnits(EPlantUnitKind::Chiller, Auto, Reason) && Auto.Num() >= 2))
	{ DestroyLayoutWorld(World); return false; }
	const AFacilityPlantDisplayActor* D0 = Redraw();
	const int32 Loose0 = D0 ? Loose(D0->GetSnapshot()) : -1;

	// A press beside the yard converts nothing.
	TestNull(TEXT("press on empty ground converts nothing"), C->ConvertAutomaticPlantAlongRay(Above(FVector(9000, -3000, 0)), Down));
	// A press on automatic chiller 1: the kind becomes actors where they were drawn, the pressed one is returned.
	const FVector P1 = Auto[1].Transform.GetLocation();
	APlantEquipmentActor* U1 = C->ConvertAutomaticPlantAlongRay(Above(P1 + FVector(0, 0, 50)), Down);
	if (!TestNotNull(TEXT("press converts the pressed unit"), U1)) { DestroyLayoutWorld(World); return false; }
	TestTrue(TEXT("whole kind converted at the drawn positions, one command"), Chillers() == Auto.Num()
		&& U1->GetActorLocation().Equals(P1, 1e-9) && C->GetLayoutUndoCount() == 1);
	// Through the normal press path the converted unit is selected and drags along the yard (one more command).
	C->BeginLayoutDragAlongRay(Above(P1), Down);
	TestTrue(TEXT("converted unit selected and drag armed"), C->IsLayoutDragging() && C->GetSelectedLayoutActor() == U1);
	const FVector Delta(6000.0 - P1.X, -800.0 - P1.Y, 0.0);
	C->UpdateLayoutDragAlongRay(Above(P1) + Delta, Down);
	TestFalse(TEXT("free yard spot"), C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true);
	TestTrue(TEXT("moved, one command"), U1->GetActorLocation().Equals(FVector(6000, -800, P1.Z), 1e-9) && C->GetLayoutUndoCount() == 2);
	const AFacilityPlantDisplayActor* D1 = Redraw();
	const int32 Loose1 = D1 ? Loose(D1->GetSnapshot()) : -1;
	TestEqual(TEXT("moved chiller and every other unit still piped"), Loose1, 0);

	// A unit added in layout mode: the drawn plant takes the new count at once and pipes it.
	const int32 Fws0 = Sim->GetFacilityPlant().GetNumDryCoolers();
	C->BeginPlantAddition(EPlantUnitKind::FwsDryCooler);
	C->UpdateRackAdditionAlongRay(Above(FVector(7000, -800, 0)), Down);
	APlantEquipmentActor* Added = C->ConfirmPlantAddition();
	const AFacilityPlantDisplayActor* D2 = Redraw();
	const int32 Loose2 = D2 ? Loose(D2->GetSnapshot()) : -1;
	const int32 Fws2 = Sim->GetFacilityPlant().GetNumDryCoolers();
	TestTrue(TEXT("added in layout mode: the drawn plant has the new count"), Added != nullptr && Fws2 == Fws0 + 1);
	TestEqual(TEXT("added unit piped at once"), Loose2, 0);

	// Undo addition, move and conversion: the automatic yard again.
	TestTrue(TEXT("Undo x3 back to automatic"), C->TryUndoLayout() && C->TryUndoLayout() && C->TryUndoLayout() && Chillers() == 0
		&& VrgPlantEquipment::Gather(World, EPlantUnitKind::FwsDryCooler).Num() == 0);

	// Pumps and the economizer press and drag the same way; their moved pipes still reach them, the plant keeps its
	// counts (one economizer, the loops' pump counts), and a second economizer is refused.
	FString Moved;
	int32 LooseMoved = 0;
	double Nx = 6000.0;
	for (const EPlantUnitKind Kind : { EPlantUnitKind::FwsPump, EPlantUnitKind::ChwPump, EPlantUnitKind::CwPump, EPlantUnitKind::Economizer })
	{
		TArray<FPlantEquipmentRecord> Drawn;
		if (!TestTrue(FString::Printf(TEXT("%s drawn"), APlantEquipmentActor::KindName(Kind)), Sim->CaptureAutomaticPlantUnits(Kind, Drawn, Reason) && Drawn.Num() > 0)) continue;
		const FVector P = Drawn[0].Transform.GetLocation();
		APlantEquipmentActor* U = C->ConvertAutomaticPlantAlongRay(Above(P + FVector(0, 0, 10)), Down);
		if (!TestNotNull(FString::Printf(TEXT("%s converts on a press"), APlantEquipmentActor::KindName(Kind)), U)) continue;
		C->BeginLayoutDragAlongRay(Above(P), Down);
		C->UpdateLayoutDragAlongRay(Above(P) + FVector(Nx - P.X, -1500.0 - P.Y, 0), Down);
		const bool bFree = !C->IsLayoutDragPreviewBlocked();
		C->EndLayoutDrag(true);
		const bool bMovedThere = U->GetActorLocation().Equals(FVector(Nx, -1500.0, P.Z), 1e-9);
		TestTrue(FString::Printf(TEXT("%s dragged to a free yard spot"), APlantEquipmentActor::KindName(Kind)), bFree && bMovedThere);
		Nx += 300.0;
		const AFacilityPlantDisplayActor* D = Redraw();
		const int32 L = D ? Loose(D->GetSnapshot()) : -1;
		LooseMoved = FMath::Max(LooseMoved, L);
		Moved += FString::Printf(TEXT(" %s %d대(안 이어짐 %d)"), APlantEquipmentActor::KindName(Kind), Drawn.Num(), L);
	}
	const FFacilityPlant& Built = Sim->GetFacilityPlant();
	TestEqual(TEXT("moved pumps / economizer still piped"), LooseMoved, 0);
	TestTrue(TEXT("plant keeps the economizer and 2 + 1 pumps per loop"), Built.HasEconomizer()
		&& Built.GetLoop().GetSpec().DutyPumps == 2 && Built.GetLoop().GetSpec().StandbyPumps == 1
		&& Built.GetChwLoop().GetHandles().Pumps.Num() == 3 && Built.GetCwLoop().GetHandles().Pumps.Num() == 3);
	C->BeginPlantAddition(EPlantUnitKind::Economizer);
	TestFalse(TEXT("a second economizer is refused"), C->IsAddingPlant());
	AddInfo(FString::Printf(TEXT("펌프·이코노마이저 눌러 옮기기:%s"), *Moved));
	const AFacilityPlantDisplayActor* D3 = Redraw();
	AddInfo(FString::Printf(TEXT("자동 칠러 %d대 누름 전환 → 이동, 안 이어진 장비: 처음 %d / 이동 뒤 %d / 배치 모드 추가 뒤 %d (FWS %d → %d대) / Undo 뒤 자동 FWS %d대"),
		Auto.Num(), Loose0, Loose1, Loose2, Fws0, Fws2, Sim->GetFacilityPlant().GetNumDryCoolers()));
	TestTrue(TEXT("Undo: the drawn plant is automatic again"), D3 != nullptr && Sim->GetFacilityPlant().GetNumDryCoolers() == Fws0);
	DestroyLayoutWorld(World);
	return true;
}

#endif
