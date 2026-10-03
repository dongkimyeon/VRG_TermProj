#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "InputKeyEventArgs.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutMoveUndoTest, "VRG.Interaction.Layout.SingleMoveUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutMoveUndoTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	Sim->EnterLayoutMode();
	TArray<AActor*> Equipment;
	Equipment.Add(World->SpawnActor<ARackThermalActor>(FVector(0, 0, 7), FRotator(0, 17, 0)));
	Equipment.Add(World->SpawnActor<ASpineCabinetActor>(FVector(0, 500, 7), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACoolingDistributionUnitActor>(FVector(0, 1000, 7), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACrahUnitActor>(FVector(0, 1500, 7), FRotator::ZeroRotator));
	const FVector Down(0, 0, -1);
	auto Drag = [&](AActor* Actor, const FVector& Delta, bool bCommit = true) {
		const FVector Eye = Actor->GetActorLocation() + FVector(0, 0, 500);
		Controller->BeginLayoutDragAlongRay(Eye, Down);
		Controller->UpdateLayoutDragAlongRay(Eye + Delta, Down, true);
		Controller->EndLayoutDrag(bCommit);
	};
	double MaxPositionErrorCm = 0, MaxAngleErrorDeg = 0;
	auto Measure = [&](AActor* Actor, const FTransform& Expected) {
		MaxPositionErrorCm = FMath::Max(MaxPositionErrorCm, FVector::Distance(Actor->GetActorLocation(), Expected.GetLocation()));
		MaxAngleErrorDeg = FMath::Max(MaxAngleErrorDeg, FMath::RadiansToDegrees(Actor->GetActorQuat().AngularDistance(Expected.GetRotation())));
		TestTrue(TEXT("Entire transform restored"), Actor->GetActorTransform().Equals(Expected, 1e-9));
	};
	for (AActor* Actor : Equipment)
	{
		Controller->ClearLayoutHistory();
		const FTransform Before = Actor->GetActorTransform();
		Drag(Actor, FVector(-150, 0, 0));
		const FTransform After = Actor->GetActorTransform();
		TestEqual(TEXT("One confirmed move is one command"), Controller->GetLayoutUndoCount(), 1);
		TestTrue(TEXT("Undo move"), Controller->TryUndoLayout()); Measure(Actor, Before);
		TestTrue(TEXT("Redo move"), Controller->TryRedoLayout()); Measure(Actor, After);
		TestTrue(TEXT("Undo again"), Controller->TryUndoLayout());
		Drag(Actor, FVector(-50, 0, 0), false);
		Drag(Actor, FVector::ZeroVector);
		TestEqual(TEXT("Cancel and click retain Redo"), Controller->GetLayoutRedoCount(), 1);
		Drag(Actor, FVector(-50, 0, 0));
		TestEqual(TEXT("New edit drops Redo"), Controller->GetLayoutRedoCount(), 0);
	}
	AActor* Rack = Equipment[0];
	Controller->ClearLayoutHistory();
	const FTransform Start = Rack->GetActorTransform();
	Drag(Rack, FVector(-150, 0, 0));
	const FTransform Moved = Rack->GetActorTransform();
	auto* Obstacle = World->SpawnActor<ACrahUnitActor>(Start.GetLocation(), FRotator::ZeroRotator);
	TestFalse(TEXT("Occupied Undo destination rejected"), Controller->TryUndoLayout());
	TestEqual(TEXT("Collision does not pop command"), Controller->GetLayoutUndoCount(), 1);
	Measure(Rack, Moved);
	Drag(Rack, FVector(150, 0, 0));
	TestEqual(TEXT("Rejected drag not recorded"), Controller->GetLayoutUndoCount(), 1);
	Obstacle->SetActorLocation(FVector(5000, 5000, 0));
	Controller->BeginLayoutDragAlongRay(Rack->GetActorLocation() + FVector(0, 0, 500), Down);
	TestFalse(TEXT("Armed drag blocks Undo"), Controller->TryUndoLayout());
	Controller->EndLayoutDrag(false);
	Controller->BeginLayoutMarquee(FVector2D(10, 10), false);
	TestFalse(TEXT("Marquee blocks Undo"), Controller->TryUndoLayout());
	Controller->EndLayoutMarquee(false);
	Controller->BeginRackAddition();
	TestFalse(TEXT("Addition preview blocks Undo"), Controller->TryUndoLayout());
	Controller->CancelRackAddition();
	Controller->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch blocks Undo"), Controller->TryUndoLayout());
	Controller->SetPatchMapOpen(false);
	Sim->EnterSimulationMode();
	TestFalse(TEXT("Simulation blocks Undo"), Controller->TryUndoLayout());
	TestEqual(TEXT("Mode switch retains history"), Controller->GetLayoutUndoCount(), 1);
	Sim->EnterLayoutMode();
	TestTrue(TEXT("Undo after mode return"), Controller->TryUndoLayout()); Measure(Rack, Start);
	Obstacle->SetActorLocation(Moved.GetLocation());
	TestFalse(TEXT("Occupied Redo destination rejected"), Controller->TryRedoLayout());
	TestEqual(TEXT("Redo collision retains command"), Controller->GetLayoutRedoCount(), 1);
	Obstacle->SetActorLocation(FVector(5000, 5000, 0));
	TestTrue(TEXT("Redo after obstruction removed"), Controller->TryRedoLayout());

	// Short event-time chords must work even without a PlayerTick between press and release.
	auto Key = [&](FKey Code, EInputEvent Event) {
		FInputKeyEventArgs Args; Args.Key = Code; Args.Event = Event; Args.AmountDepressed = 1;
		Controller->InputKey(Args);
	};
	Key(EKeys::RightControl, IE_Pressed); Key(EKeys::Z, IE_Pressed); Key(EKeys::Z, IE_Repeat);
	Measure(Rack, Start);
	TestEqual(TEXT("Repeat does not undo again"), Controller->GetLayoutUndoCount(), 0);
	Key(EKeys::Z, IE_Released); Key(EKeys::Y, IE_Pressed); Key(EKeys::Y, IE_Released); Key(EKeys::RightControl, IE_Released);
	Measure(Rack, Moved);
	Key(EKeys::Z, IE_Pressed); Key(EKeys::Z, IE_Released); Measure(Rack, Moved);

	Controller->ClearLayoutHistory();
	for (int32 I = 0; I < 105; ++I) Drag(Rack, FVector(-5, 0, 0));
	TestEqual(TEXT("History bounded to last100 commands"), Controller->GetLayoutUndoCount(), 100);
	for (int32 I = 0; I < 100; ++I) TestTrue(TEXT("Undo bounded history"), Controller->TryUndoLayout());
	TestFalse(TEXT("Evicted commands unavailable"), Controller->TryUndoLayout());
	TestEqual(TEXT("Bounded Redo"), Controller->GetLayoutRedoCount(), 100);
	for (int32 I = 0; I < 100; ++I) TestTrue(TEXT("Redo bounded history"), Controller->TryRedoLayout());
	TestTrue(TEXT("Legacy G move"), Controller->TryLayoutMove(Rack, Rack->GetActorLocation() + FVector(-5, 0, 0)));
	TestEqual(TEXT("Unsupported G clears history"), Controller->GetLayoutUndoCount(), 0);
	Drag(Rack, FVector(-5, 0, 0));
	const FString Name = TEXT("AutomationUndoHistory"); FString Summary;
	TestTrue(TEXT("Save existing format"), VrgLayoutFile::Save(World, Name, Summary));
	TestTrue(TEXT("Load existing format"), VrgLayoutFile::Load(World, Name, Summary));
	TestEqual(TEXT("Load clears Undo"), Controller->GetLayoutUndoCount(), 0);
	TestEqual(TEXT("Load clears Redo"), Controller->GetLayoutRedoCount(), 0);
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name));
	Drag(Rack, FVector(-5, 0, 0));
	Rack->AddActorWorldOffset(FVector(1, 0, 0));
	TestFalse(TEXT("External transform change cannot be overwritten"), Controller->TryUndoLayout());
	TestEqual(TEXT("Stale history rejected without popping"), Controller->GetLayoutUndoCount(), 1);
	Controller->ClearLayoutHistory();
	Drag(Rack, FVector(-5, 0, 0)); Rack->Destroy();
	TestFalse(TEXT("Destroyed actor never restored by name"), Controller->TryUndoLayout());
	AddInfo(FString::Printf(TEXT("Single move Undo/Redo four kinds: max position error %.3e cm, quaternion angular error %.3e deg; history cap100, collision/cancel/no-op/input/mode/load/stale identity tested. No physics accuracy claim."), MaxPositionErrorCm, MaxAngleErrorDeg));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRotationUndoTest, "VRG.Interaction.Layout.SingleRotationUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRotationUndoTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, 0, 7), FRotator(0, 17, 0));
	auto* Spine = World->SpawnActor<ASpineCabinetActor>(FVector(0, 800, 7), FRotator::ZeroRotator);
	auto* Obstacle = World->SpawnActor<ACrahUnitActor>(FVector(5000, 5000, 0), FRotator::ZeroRotator);
	Rack->RackSizeM = FVector(1.2, .4, 1);
	Rack->SetActorScale3D(FVector(1.2, .9, 1.1));
	Obstacle->SizeM = FVector(.1, .1, 1);
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	const FTransform Original = Rack->GetActorTransform(), SpineOriginal = Spine->GetActorTransform();
	auto Select = [&](AActor* Actor, bool bToggle = false) {
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), Down, bToggle);
	};
	auto Drag = [&](const FVector& Delta) {
		const FVector Eye = Rack->GetActorLocation() + FVector(0, 0, 500);
		Controller->BeginLayoutDragAlongRay(Eye, Down);
		Controller->UpdateLayoutDragAlongRay(Eye + Delta, Down, true);
		Controller->EndLayoutDrag(true);
	};
	double MaxPositionErrorCm = 0, MaxYawErrorDeg = 0;
	auto Measure = [&](const FTransform& Expected) {
		MaxPositionErrorCm = FMath::Max(MaxPositionErrorCm, FVector::Distance(Rack->GetActorLocation(), Expected.GetLocation()));
		MaxYawErrorDeg = FMath::Max(MaxYawErrorDeg, FMath::Abs(FMath::FindDeltaAngleDegrees(Rack->GetActorRotation().Yaw, Expected.Rotator().Yaw)));
		TestTrue(TEXT("Undo/Redo restores position/orientation/height/scale"), Rack->GetActorTransform().Equals(Expected, 1e-9));
	};
	Drag(FVector(-150, 0, 0));
	const FTransform Moved = Rack->GetActorTransform();
	Controller->OnResetSim();
	const FTransform Rotated = Rack->GetActorTransform();
	TestEqual(TEXT("Move then R are separate commands"), Controller->GetLayoutUndoCount(), 2);
	TestTrue(TEXT("R preserves height and scale"), Rotated.GetLocation() == Moved.GetLocation() && Rotated.GetScale3D() == Moved.GetScale3D());
	// A small obstacle along the old long axis is clear of the rotated narrow footprint.
	Obstacle->SetActorLocation(Moved.GetLocation() + Moved.GetRotation().RotateVector(FVector(65, 0, 0)));
	TestFalse(TEXT("Undo rotation respects old footprint collision"), Controller->TryUndoLayout());
	Measure(Rotated);
	TestEqual(TEXT("Rotation collision retains stack"), Controller->GetLayoutUndoCount(), 2);
	Controller->OnResetSim();
	TestEqual(TEXT("Rejected R creates no record"), Controller->GetLayoutUndoCount(), 2);
	Obstacle->SetActorLocation(FVector(5000, 5000, 0));
	TestTrue(TEXT("Undo R"), Controller->TryUndoLayout()); Measure(Moved);
	TestTrue(TEXT("Undo move"), Controller->TryUndoLayout()); Measure(Original);
	TestTrue(TEXT("Redo move"), Controller->TryRedoLayout()); Measure(Moved);
	TestTrue(TEXT("Redo R"), Controller->TryRedoLayout()); Measure(Rotated);
	TestTrue(TEXT("Undo R for fresh edit"), Controller->TryUndoLayout());
	Controller->OnResetSim();
	TestEqual(TEXT("New R clears Redo"), Controller->GetLayoutRedoCount(), 0);
	Controller->SetPatchMapOpen(true); Controller->OnResetSim();
	TestEqual(TEXT("Patch-rejected R preserves records"), Controller->GetLayoutUndoCount(), 2);
	Controller->SetPatchMapOpen(false);
	TestEqual(TEXT("Undo/Redo does not advance clock"), Sim->GetSnapshot().SimTimeS, 0.0);

	// Legacy T remains a barrier; supported group/presence edits now add commands.
	Controller->ClearLayoutHistory(); Rack->SetActorTransform(Original);
	Drag(FVector(-5, 0, 0));
	TestTrue(TEXT("Legacy T rotation applies"), Controller->TryLayoutRotation(Rack, FRotator(0, 107, 0)));
	TestEqual(TEXT("Legacy T clears old commands"), Controller->GetLayoutUndoCount(), 0);
	Rack->SetActorTransform(Original); Drag(FVector(-5, 0, 0)); Select(Spine, true);
	TestFalse(TEXT("Multi-selected delete rejected"), Controller->TryDeleteLayoutRack());
	TestEqual(TEXT("Rejected unsupported edit retains history"), Controller->GetLayoutUndoCount(), 1);
	TestTrue(TEXT("Group R applies"), Controller->TryRotateLayoutSelection());
	TestEqual(TEXT("Group R adds one command"), Controller->GetLayoutUndoCount(), 2);
	Controller->ClearLayoutHistory();
	Rack->SetActorTransform(Original); Spine->SetActorTransform(SpineOriginal); Select(Rack);
	Drag(FVector(-5, 0, 0)); Select(Spine, true); Drag(FVector(0, 100, 0));
	TestEqual(TEXT("Group drag is one command after single move"), Controller->GetLayoutUndoCount(), 2);
	Controller->ClearLayoutHistory(); // The following fixture independently tests presence-edit barriers.
	Rack->SetActorTransform(Original); Spine->SetActorTransform(SpineOriginal); Select(Rack);
	Drag(FVector(-5, 0, 0));
	TestTrue(TEXT("Begin duplication"), Controller->BeginRackDuplication());
	Controller->CancelRackAddition();
	TestEqual(TEXT("Cancelled duplication retains history"), Controller->GetLayoutUndoCount(), 1);
	TestTrue(TEXT("Begin duplication again"), Controller->BeginRackDuplication());
	Controller->UpdateRackAdditionAlongRay(FVector(-1500, -1500, 500), Down);
	auto* Duplicate = Controller->ConfirmRackAddition();
	TestNotNull(TEXT("Duplicate confirmed"), Duplicate);
	TestEqual(TEXT("Duplication adds presence command"), Controller->GetLayoutUndoCount(), 2);
	Select(Rack); Drag(FVector(-5, 0, 0));
	Controller->BeginRackAddition(); Controller->UpdateRackAdditionAlongRay(FVector(1500, -1500, 500), Down);
	TestNotNull(TEXT("Addition confirmed"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Addition adds presence command"), Controller->GetLayoutUndoCount(), 4);
	Select(Rack); Drag(FVector(-5, 0, 0));
	TestTrue(TEXT("Selected rack deleted"), Controller->TryDeleteLayoutRack());
	TestEqual(TEXT("Deletion adds presence command"), Controller->GetLayoutUndoCount(), 6);
	TestTrue(TEXT("Deleted rack recreated by Undo"), Controller->TryUndoLayout());
	AddInfo(FString::Printf(TEXT("Single R mixed with move: max absolute position error %.3e cm, yaw delta %.3e deg, SimTime change0s; old-footprint collision, fresh-edit Redo reset and unsupported-edit barriers tested."), MaxPositionErrorCm, MaxYawErrorDeg));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
