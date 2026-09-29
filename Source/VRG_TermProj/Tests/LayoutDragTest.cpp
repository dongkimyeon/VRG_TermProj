#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutDragTest, "VRG.Interaction.Layout.SingleEquipmentDrag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutDragTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	Sim->EnterLayoutMode();
	TArray<AActor*> Equipment;
	Equipment.Add(World->SpawnActor<ARackThermalActor>(FVector(2, 3, 7), FRotator(0, 90, 0)));
	Equipment.Add(World->SpawnActor<ASpineCabinetActor>(FVector(302, 3, 7), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACoolingDistributionUnitActor>(FVector(602, 3, 7), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACrahUnitActor>(FVector(902, 3, 7), FRotator::ZeroRotator));
	double MaxMoveErrorCm = 0.0, MaxCancelErrorCm = 0.0;
	for (AActor* Actor : Equipment)
	{
		const FTransform Original = Actor->GetActorTransform();
		// Vertical rays hit off-centre: verify the click offset is preserved instead of jumping to the cursor.
		const FVector Eye = Original.GetLocation() + FVector(10, -10, 500);
		const FVector Down(0, 0, -1);
		Controller->BeginLayoutDragAlongRay(Eye, Down);
		TestTrue(TEXT("Supported equipment arms drag"), Controller->IsLayoutDragging());
		Controller->UpdateLayoutDragAlongRay(Eye, Down);
		Controller->EndLayoutDrag(true);
		TestTrue(TEXT("Click alone does not snap off-grid origin"), Actor->GetActorTransform().Equals(Original, 0));

		Controller->BeginLayoutDragAlongRay(Eye, Down);
		Controller->UpdateLayoutDragAlongRay(Eye + FVector(14, -17, 0), Down);
		const FVector Expected(Original.GetLocation().X + 13, -15, 7);
		MaxMoveErrorCm = FMath::Max(MaxMoveErrorCm, FVector::Distance(Controller->GetLayoutDragPreviewCm(), Expected));
		TestTrue(TEXT("Preview preserves live actor, so no transform-driven pipe rebuild"), Actor->GetActorTransform().Equals(Original, 0));
		Controller->UpdateLayoutDragAlongRay(Eye, FVector::ForwardVector);
		TestTrue(TEXT("Parallel ray preserves last preview"), Controller->GetLayoutDragPreviewCm() == Expected);
		Controller->UpdateLayoutDragAlongRay(Eye, FVector::ZeroVector);
		TestTrue(TEXT("Zero ray preserves last preview"), Controller->GetLayoutDragPreviewCm() == Expected);
		Controller->EndLayoutDrag(false);
		MaxCancelErrorCm = FMath::Max(MaxCancelErrorCm, FVector::Distance(Actor->GetActorLocation(), Original.GetLocation()));
		TestTrue(TEXT("Cancel preserves entire transform"), Actor->GetActorTransform().Equals(Original, 0));

		Controller->BeginLayoutDragAlongRay(Eye, Down);
		Controller->UpdateLayoutDragAlongRay(Eye + FVector(14, -17, 0), Down);
		Controller->EndLayoutDrag(true);
		MaxMoveErrorCm = FMath::Max(MaxMoveErrorCm, FVector::Distance(Actor->GetActorLocation(), Expected));
		TestTrue(TEXT("Release commits snapped position and preserves height"), Actor->GetActorLocation() == Expected);
		TestTrue(TEXT("Rotation remains unchanged"), Actor->GetActorQuat().Equals(Original.GetRotation(), 0));
		TestFalse(TEXT("Release ends drag"), Controller->IsLayoutDragging());
		Actor->SetActorTransform(Original);
	}
	AActor* Rack = Equipment[0];
	const FVector Origin = Rack->GetActorLocation();
	const FVector Eye = Origin + FVector(0, 0, 500);
	const FVector Down(0, 0, -1);
	auto Preview = [&]() {
		Controller->BeginLayoutDragAlongRay(Eye, Down);
		Controller->UpdateLayoutDragAlongRay(Eye + FVector(50, 50, 0), Down);
	};
	Preview();
	Controller->SetPatchMapOpen(true);
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Patch opening cancels without committing"), Rack->GetActorLocation() == Origin);
	TestFalse(TEXT("Patch opening disarms drag"), Controller->IsLayoutDragging());
	Controller->SetPatchMapOpen(false);
	Preview();
	Controller->OnFocusEquipment();
	TestFalse(TEXT("F cancels pending movement before changing view"), Controller->IsLayoutDragging());
	TestTrue(TEXT("F preserves original equipment position"), Rack->GetActorLocation() == Origin);
	Preview();
	Rack->SetActorLocation(Origin + FVector(100, 0, 0));
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("External edits are not overwritten by stale drag"), Rack->GetActorLocation() == Origin + FVector(100, 0, 0));
	Rack->SetActorLocation(Origin);
	Preview();
	Sim->EnterSimulationMode();
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Mode transition cannot commit pending movement"), Rack->GetActorLocation() == Origin);
	Controller->BeginLayoutDragAlongRay(Eye, Down);
	TestFalse(TEXT("Simulation cannot begin drag"), Controller->IsLayoutDragging());
	TestEqual(TEXT("Editing never advances SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);
	Sim->EnterLayoutMode();
	Preview();
	Rack->Destroy();
	Controller->UpdateLayoutDragAlongRay(Eye, Down);
	Controller->EndLayoutDrag(true);
	TestFalse(TEXT("Destroyed actor safely clears drag"), Controller->IsLayoutDragging());
	Controller->BeginLayoutDragAlongRay(FVector(-10000, -10000, 500), Down);
	TestFalse(TEXT("Empty space never starts drag"), Controller->IsLayoutDragging());
	AddInfo(FString::Printf(TEXT("Drag: four equipment kinds; max preview/commit error %.3e cm; max cancel error %.3e cm; fixed height/rotation, unchanged live preview and SimTime."), MaxMoveErrorCm, MaxCancelErrorCm));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
