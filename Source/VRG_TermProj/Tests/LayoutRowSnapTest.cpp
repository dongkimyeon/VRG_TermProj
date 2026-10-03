#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRowSnapTest, "VRG.Interaction.Layout.SingleRackRowSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRowSnapTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-1000, 0, 0), FRotator::ZeroRotator);
	auto* A = World->SpawnActor<ARackThermalActor>(FVector(0, 0, 0), FRotator::ZeroRotator);
	auto* B = World->SpawnActor<ARackThermalActor>(FVector(0, 80, 0), FRotator::ZeroRotator);
	auto* Moving = World->SpawnActor<ARackThermalActor>(FVector(300, 200, 7), FRotator(0, 90, 0));
	auto* Obstacle = World->SpawnActor<ACrahUnitActor>(FVector(1000, 0, 0), FRotator::ZeroRotator);
	A->RackSizeM = B->RackSizeM = Moving->RackSizeM = FVector(.4, .4, 1);
	Moving->SetActorScale3D(FVector(2, 1, 1));
	Obstacle->SizeM = FVector(.2, .2, 1);
	Room->RowGroupToleranceM = .50;
	Room->RackPitchM = .66; // Deliberately differs from observed 0.80 m; must not be used by snapping.
	Sim->EnterLayoutMode();
	const FTransform Original = Moving->GetActorTransform();
	const FVector Down(0, 0, -1), Eye = Original.GetLocation() + FVector(0, 0, 500);
	auto Preview = [&](const FVector& Position, bool bAlt = false) {
		Controller->UpdateLayoutDragAlongRay(Position + FVector(0, 0, 500), Down, bAlt);
	};
	Controller->BeginLayoutDragAlongRay(Eye, Down);
	Preview(Original.GetLocation()); Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Click does not align rotation or location"), Moving->GetActorTransform().Equals(Original, 0));
	Controller->BeginLayoutDragAlongRay(Eye, Down);
	Preview(FVector(29, 151, 7));
	FTransform Proposed;
	TestTrue(TEXT("Row preview transform exists"), Controller->GetLayoutDragPreviewTransform(Moving, Proposed));
	// Reference rear is x=-20 cm; scaled moving rear at x-40 cm => origin x=20, observed next y=160.
	const double PositionErrorCm = FVector::Distance(Proposed.GetLocation(), FVector(20, 160, 7));
	const double YawErrorDeg = FMath::Abs(FMath::FindDeltaAngleDegrees(Proposed.Rotator().Yaw, 0.0));
	TestTrue(TEXT("Observed pitch and scaled rear align"), PositionErrorCm < 1e-9 && YawErrorDeg < 1e-10);
	TestTrue(TEXT("Preview preserves height and scale"), Proposed.GetLocation().Z == 7 && Proposed.GetScale3D() == Original.GetScale3D());
	TestTrue(TEXT("Preview never moves live rack"), Moving->GetActorTransform().Equals(Original, 0));
	FVector GuideStart, GuideEnd;
	TestTrue(TEXT("Guide spans reference and destination rear line"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd)
		&& FVector::Distance(GuideStart, FVector(-20, 160, 7)) < 1e-9 && FVector::Distance(GuideEnd, FVector(-20, 0, 7)) < 1e-9);
	Preview(FVector(29, 151, 7), true);
	Controller->GetLayoutDragPreviewTransform(Moving, Proposed);
	TestTrue(TEXT("Alt returns to five-cm grid and original orientation"), Proposed.GetLocation() == FVector(30, 150, 7) && Proposed.GetRotation().Equals(Original.GetRotation(), 0));
	TestFalse(TEXT("Alt hides row guide"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Preview(FVector(29, 151, 7));
	TestTrue(TEXT("Releasing Alt re-enables row guide"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Controller->EndLayoutDrag(false);
	TestTrue(TEXT("Esc-equivalent keeps full original transform"), Moving->GetActorTransform().Equals(Original, 0));
	TestFalse(TEXT("Cancel removes guide"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Controller->BeginLayoutDragAlongRay(Eye, Down); Preview(FVector(29, 151, 7));
	Obstacle->SetActorLocation(FVector(20, 160, 7));
	TestTrue(TEXT("Snapped target collision is blocked"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Collision at release keeps location and rotation"), Moving->GetActorTransform().Equals(Original, 0));
	Obstacle->SetActorLocation(FVector(1000, 0, 0));
	Controller->BeginLayoutDragAlongRay(Eye, Down); Preview(FVector(29, 151, 7));
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Commit applies preview position and orientation"), FVector::Distance(Moving->GetActorLocation(), FVector(20, 160, 7)) < 1e-9 && FMath::Abs(Moving->GetActorRotation().Yaw) < 1e-10);
	Room->RebuildRoomNow();
	const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
	const TArray<int32> Rows = Room->GetRoutedRackRows();
	const int32 Ai = Routed.IndexOfByKey(A), Mi = Routed.IndexOfByKey(Moving);
	TestTrue(TEXT("Committed rack joins actual routing row"), Ai != INDEX_NONE && Mi != INDEX_NONE && Rows[Ai] == Rows[Mi]);
	const FTransform Snapped = Moving->GetActorTransform();
	TestTrue(TEXT("Undo row attachment"), Controller->TryUndoLayout());
	TestTrue(TEXT("Undo restores pre-snap position/orientation/scale"), Moving->GetActorTransform().Equals(Original, 1e-9));
	const auto UndoRouted = Room->GetRoutedRacks();
	const auto UndoRows = Room->GetRoutedRackRows();
	TestTrue(TEXT("Undo rebuild removes old row membership"), UndoRows[UndoRouted.IndexOfByKey(A)] != UndoRows[UndoRouted.IndexOfByKey(Moving)]);
	TestTrue(TEXT("Redo row attachment"), Controller->TryRedoLayout());
	TestTrue(TEXT("Redo restores full snapped transform"), Moving->GetActorTransform().Equals(Snapped, 1e-9));
	const auto RedoRouted = Room->GetRoutedRacks();
	const auto RedoRows = Room->GetRoutedRackRows();
	TestTrue(TEXT("Redo immediately rebuilds actual routing row"), RedoRows[RedoRouted.IndexOfByKey(A)] == RedoRows[RedoRouted.IndexOfByKey(Moving)]);
	Moving->SetActorTransform(Original);
	// Begin captures the actual UE-restored transform, not a pre-normalization value.
	Controller->BeginLayoutDragAlongRay(Eye, Down); Preview(FVector(70.01, 151, 7));
	TestFalse(TEXT("Outside rear tolerance does not attach"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Preview(FVector(70, 151, 7));
	TestTrue(TEXT("Rear tolerance inclusive boundary attaches"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Preview(FVector(29, 160.01, 7));
	TestFalse(TEXT("Beyond one neighbour interval does not attach"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Preview(FVector(29, 79, 7));
	TestTrue(TEXT("Occupied row slot uses overlap guard"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(false);
	// Move the reference row after preview: release must derive the new pitch, not commit a stale slot.
	Controller->BeginLayoutDragAlongRay(Eye, Down); Preview(FVector(29, 151, 7));
	B->SetActorLocation(FVector(0, 100, 0)); Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Release refreshes observed row spacing"), FVector::Distance(Moving->GetActorLocation(), FVector(20, 200, 7)) < 1e-9);
	Moving->SetActorTransform(Original); B->SetActorLocation(FVector(0, 80, 0));
	Controller->SelectLayoutAlongRay(Eye, Down);
	Controller->SelectLayoutAlongRay(B->GetActorLocation() + FVector(0, 0, 500), Down, true);
	Controller->BeginLayoutDragAlongRay(Eye, Down); Preview(FVector(29, 151, 7));
	Controller->GetLayoutDragPreviewTransform(Moving, Proposed);
	TestTrue(TEXT("Group remains grid-only and does not change direction"), Proposed.GetLocation() == FVector(30, 150, 7) && Proposed.GetRotation().Equals(Original.GetRotation(), 1e-12));
	TestFalse(TEXT("Group drag has no row guide"), Controller->GetLayoutRowSnapGuide(GuideStart, GuideEnd));
	Controller->EndLayoutDrag(false);
	TestEqual(TEXT("Placement does not advance SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);
	AddInfo(FString::Printf(TEXT("Row snap: observed pitch 0.80 m vs configured 0.66 m; max position error %.3e cm, yaw error %.3e deg; actual routing row joins, Alt/collision/cancel/tolerance/one-pitch/release-refresh/group checked; SimTime change 0 s. Guide pixels and OS input unverified."), PositionErrorCm, YawErrorDeg));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
