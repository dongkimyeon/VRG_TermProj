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
	TestEqual(TEXT("Stale history invalidated"), Controller->GetLayoutUndoCount(), 0);
	Drag(Rack, FVector(-5, 0, 0)); Rack->Destroy();
	TestFalse(TEXT("Destroyed actor never restored by name"), Controller->TryUndoLayout());
	AddInfo(FString::Printf(TEXT("Single move Undo/Redo four kinds: max position error %.3e cm, quaternion angular error %.3e deg; history cap100, collision/cancel/no-op/input/mode/load/stale identity tested. No physics accuracy claim."), MaxPositionErrorCm, MaxAngleErrorDeg));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
