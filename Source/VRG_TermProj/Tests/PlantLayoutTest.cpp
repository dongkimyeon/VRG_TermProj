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

	// The yard draws placed units where they stand; a count that does not match keeps the schematic positions.
	FPlantDisplayInput In;
	In.RoomM = FBox(FVector(0, 0, 0), FVector(10, 8, 3));
	In.CduPositionsM = { FVector(1, 1, 0), FVector(1, 5, 0) };
	In.CrahPositionsM = { FVector(8, 1, 0), FVector(8, 3, 0), FVector(8, 5, 0), FVector(8, 7, 0) };
	In.FwsDryCoolers = MakeUnits(5, 20.0, -6.0);
	In.CwDryCoolers = MakeUnits(4, 20.0, 12.0);
	In.Chillers = MakeUnits(3, 30.0, 4.0);   // the model has 4 chillers: schematic
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
	// FWS return coils: coil K runs through placed unit K (one coil per unit).
	for (const FPlantDisplayPipe& Pipe : S.Pipes)
		if (Pipe.Loop == EPlantLoopId::Fws && Pipe.Role == EPlantPipeRole::ReturnCoil && Pipe.PointsM.Num() == 3)
		{
			const int32 K = Placed.GetLoop().GetHandles().ReturnCoilPipes.IndexOfByKey(Pipe.Edge);
			if (In.FwsDryCoolers.IsValidIndex(K)) { MaxViaErr = FMath::Max(MaxViaErr, FVector::Distance(Pipe.PointsM[1], In.FwsDryCoolers[K].CenterM + FVector(0, 0, 0.9))); ++Coils; }
			else MaxViaErr = 1.0;
		}
	AddInfo(FString::Printf(TEXT("표시: 배치 FWS %d / CW %d / 칠러 %d(수 불일치라 0), 위치 최대 차 %.3e m, 코일 %d개 경유점 최대 차 %.3e m"),
		PlacedFws, PlacedCw, PlacedChillers, MaxPosErr, Coils, MaxViaErr));
	TestTrue(TEXT("placed dry coolers drawn at their units (exact)"), PlacedFws == 5 && PlacedCw == 4 && MaxPosErr == 0.0);
	TestTrue(TEXT("mismatched chiller count stays schematic"), PlacedChillers == 0);
	TestTrue(TEXT("FWS return coil K runs through unit K"), Coils == 5 && MaxViaErr == 0.0);
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

#endif
