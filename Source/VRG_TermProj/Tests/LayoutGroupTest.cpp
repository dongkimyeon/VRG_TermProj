#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "GameFramework/DefaultPawn.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGroupTest, "VRG.Interaction.Layout.MultiSelectionAndGroupDrag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutGroupTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Pawn = World->SpawnActor<ADefaultPawn>();
	Controller->Possess(Pawn);
	Controller->bFrameEquipmentOnStart = false;
	auto* A = World->SpawnActor<ARackThermalActor>(FVector(2, 3, 7), FRotator(0, 17, 0));
	auto* B = World->SpawnActor<ARackThermalActor>(FVector(62, 3, 12), FRotator(0, 90, 0));
	auto* Obstacle = World->SpawnActor<ACrahUnitActor>(FVector(1000, 0, 0), FRotator::ZeroRotator);
	A->RackSizeM = B->RackSizeM = Obstacle->SizeM = FVector(.2, .2, 1);
	A->SetActorScale3D(FVector(2, 1, 1));
	Sim->EnterLayoutMode();
	const FTransform StartA = A->GetActorTransform(), StartB = B->GetActorTransform();
	const FVector Down(0, 0, -1);
	auto Click = [&](AActor* Actor, bool bToggle = false) {
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), Down, bToggle);
	};
	auto SelectPair = [&]() { Click(A); Click(B, true); };
	SelectPair();
	TestEqual(TEXT("Shift adds to selection"), Controller->GetSelectedLayoutActors().Num(), 2);
	TestFalse(TEXT("Shift selection does not arm a drag"), Controller->IsLayoutDragging());
	Click(B, true);
	TestEqual(TEXT("Shift removes from selection"), Controller->GetSelectedLayoutActors().Num(), 1);
	Click(B, true);
	Controller->OnResetSim();
	TestTrue(TEXT("R ignores multiple selection"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorTransform().Equals(StartB, 0));
	TestFalse(TEXT("Legacy G shared move blocked for multiple selection"), Controller->TryLayoutMove(A, FVector(300, 0, 7)));
	TestFalse(TEXT("Legacy T shared rotation blocked for multiple selection"), Controller->TryLayoutRotation(A, FRotator::ZeroRotator));
	Controller->OnFocusEquipment();
	const FVector GroupEye = Pawn->GetActorLocation();
	const FRotationMatrix View(Controller->GetControlRotation());
	double MaxFrustumRatio = 0.0;
	for (AActor* Actor : Controller->GetSelectedLayoutActors())
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local(Corner & 1 ? 10 : -10, Corner & 2 ? 10 : -10, Corner & 4 ? 100 : 0);
			const FVector Offset = Actor->GetActorTransform().TransformPosition(Local) - GroupEye;
			const double Depth = FVector::DotProduct(Offset, View.GetScaledAxis(EAxis::X));
			TestTrue(TEXT("Selected corner is in front of camera"), Depth > 0);
			const double Horizontal = FMath::Abs(FVector::DotProduct(Offset, View.GetScaledAxis(EAxis::Y))) / Depth;
			const double Vertical = FMath::Abs(FVector::DotProduct(Offset, View.GetScaledAxis(EAxis::Z))) / Depth * 16.0 / 9.0;
			MaxFrustumRatio = FMath::Max3(MaxFrustumRatio, Horizontal, Vertical);
		}
	TestTrue(TEXT("F contains all scaled selected corners in 90 degree 16:9 frustum"), MaxFrustumRatio <= 1.0);
	TestEqual(TEXT("F preserves selection"), Controller->GetSelectedLayoutActors().Num(), 2);

	// A was selected first, but B is clicked to begin the drag: snapping must use B.
	const FVector EyeB = StartB.GetLocation() + FVector(0, 0, 500);
	Controller->BeginLayoutDragAlongRay(EyeB, Down);
	TestEqual(TEXT("Clicking existing selection preserves group"), Controller->GetSelectedLayoutActors().Num(), 2);
	Controller->UpdateLayoutDragAlongRay(EyeB + FVector(14, -17, 0), Down);
	FTransform PreviewA, PreviewB;
	TestTrue(TEXT("Every member has preview"), Controller->GetLayoutDragPreviewTransform(A, PreviewA) && Controller->GetLayoutDragPreviewTransform(B, PreviewB));
	TestTrue(TEXT("Clicked B snaps, common offset applied to A"), PreviewB.GetLocation() == FVector(75, -15, 12) && PreviewA.GetLocation() == FVector(15, -15, 7));
	TestTrue(TEXT("Live group unchanged during preview"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorTransform().Equals(StartB, 0));
	TestFalse(TEXT("Free group preview allowed"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(true);
	const double RelativeErrorCm = FVector::Distance(B->GetActorLocation() - A->GetActorLocation(), StartB.GetLocation() - StartA.GetLocation());
	TestEqual(TEXT("Relative position preserved"), RelativeErrorCm, 0.0);
	TestTrue(TEXT("Every member committed with height/orientation/scale preserved"), A->GetActorTransform().Equals(PreviewA, 0) && B->GetActorTransform().Equals(PreviewB, 0));
	A->SetActorTransform(StartA); B->SetActorTransform(StartB);
	auto PreviewPair = [&](const FVector& Offset) {
		SelectPair(); Controller->BeginLayoutDragAlongRay(EyeB, Down);
		Controller->UpdateLayoutDragAlongRay(EyeB + Offset, Down);
	};
	PreviewPair(FVector(60, 0, 0));
	TestFalse(TEXT("Member old positions do not block group translation"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(false);
	TestTrue(TEXT("Cancel leaves entire group original"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorTransform().Equals(StartB, 0));
	PreviewPair(FVector(300, 0, 0));
	Controller->GetLayoutDragPreviewTransform(B, PreviewB);
	Obstacle->SetActorLocation(PreviewB.GetLocation());
	TestTrue(TEXT("One member collision marks whole preview blocked"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Late obstacle blocks whole group, no partial movement"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorTransform().Equals(StartB, 0));
	Obstacle->SetActorLocation(FVector(1000, 0, 0));
	PreviewPair(FVector(300, 0, 0));
	B->SetActorLocation(StartB.GetLocation() + FVector(1, 0, 0));
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("External member edit aborts group without overwriting edit"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorLocation() == StartB.GetLocation() + FVector(1, 0, 0));
	B->SetActorTransform(StartB);
	PreviewPair(FVector(300, 0, 0));
	B->GetRootComponent()->SetMobility(EComponentMobility::Static);
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Immovable member rejected before moving any member"), A->GetActorTransform().Equals(StartA, 0) && B->GetActorTransform().Equals(StartB, 0));
	B->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	PreviewPair(FVector(300, 0, 0));
	Controller->SetPatchMapOpen(true); Controller->SetPatchMapOpen(false);
	TestFalse(TEXT("Patch opening cancels group"), Controller->IsLayoutDragging());
	Controller->BeginLayoutDragAlongRay(Obstacle->GetActorLocation() + FVector(0, 0, 500), Down);
	TestEqual(TEXT("New plain-clicked equipment replaces group"), Controller->GetSelectedLayoutActors().Num(), 1);
	Controller->EndLayoutDrag(false);
	SelectPair();
	Controller->SelectLayoutAlongRay(FVector(-10000, -10000, 500), Down, true);
	TestTrue(TEXT("Empty Shift click clears whole selection"), Controller->GetSelectedLayoutActors().IsEmpty());
	Controller->OnFocusEquipment();
	TestTrue(TEXT("Empty selection F shows all including distant obstacle"), FVector::Distance(Pawn->GetActorLocation(), GroupEye) > 100);
	PreviewPair(FVector(300, 0, 0));
	B->Destroy(); Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Destroyed member prevents partial movement"), A->GetActorTransform().Equals(StartA, 0));
	TestEqual(TEXT("Destroyed selection reference ignored"), Controller->GetSelectedLayoutActors().Num(), 1);
	TestEqual(TEXT("Group editing leaves SimTime unchanged"), Sim->GetSnapshot().SimTimeS, 0.0);
	Sim->EnterSimulationMode(); Controller->RefreshInteractionInputMode();
	TestTrue(TEXT("Simulation transition clears selection"), Controller->GetSelectedLayoutActors().IsEmpty());
	Click(A, true);
	TestTrue(TEXT("Simulation ignores Shift selection"), Controller->GetSelectedLayoutActors().IsEmpty());
	AddInfo(FString::Printf(TEXT("Group: anchor B snap (75,-15,12) cm; relative offset error %.3e cm; accepted/rejected/cancelled transactions, Shift selection, scaled F max frustum ratio %.9f; SimTime change 0 s."), RelativeErrorCm, MaxFrustumRatio));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
