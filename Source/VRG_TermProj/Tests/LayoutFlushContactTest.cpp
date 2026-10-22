#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// Found by hand in L_Test (2026-10-23): racks stand flush (gap 0) against the level-placed in-row coolers at the row
// ends. A move of such a rack could not be undone (the original spot failed the 1 cm clearance against the cooler),
// and a group holding the rack and its cooler could never be dragged or rotated (members checked against each other).
// Undo/Redo may restore a flush contact, a rigid group may keep one; ordinary placement keeps the 1 cm rule and a
// real overlap is still refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutFlushContactTest, "VRG.Interaction.Layout.FlushContactUndoAndGroup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutFlushContactTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>();
	auto* Rack = W->SpawnActor<ARackThermalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	auto* Cooler = W->SpawnActor<ACrahUnitActor>(FVector(0, -100.0 * GjRackLayout::FrameWidthM, 0), FRotator::ZeroRotator);
	Cooler->SizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, 2.0);   // flush with the rack, gap 0
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };
	const FTransform Rack0 = Rack->GetActorTransform(), Cooler0 = Cooler->GetActorTransform();
	TestTrue(TEXT("the flush spot fails the ordinary 1 cm rule"), ADataCenterDebugController::IsLayoutPlacementBlocked(Rack, Rack0));

	// (1) Move the rack away, then Undo / Redo / Undo.
	C->SelectLayoutAlongRay(Above(Rack->GetActorLocation()), Down);
	C->BeginLayoutDragAlongRay(Above(Rack->GetActorLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(FVector(0, 400, 0)), Down);
	C->EndLayoutDrag(true);
	const bool bMoved = !Rack->GetActorTransform().Equals(Rack0, 1e-6) && C->GetLayoutUndoCount() == 1;
	const bool bUndo = C->TryUndoLayout() && Rack->GetActorTransform().Equals(Rack0, 1e-6);
	const bool bRedo = C->TryRedoLayout() && !Rack->GetActorTransform().Equals(Rack0, 1e-6) && C->TryUndoLayout();
	TestTrue(TEXT("(1) a rack moved off a flush contact is moved, and Undo/Redo/Undo restores it"), bMoved && bUndo && bRedo);

	// (2) Group of the rack and its flush cooler: drag, Undo, quarter turn, Undo.
	C->SelectLayoutAlongRay(Above(Rack->GetActorLocation()), Down);
	C->SelectLayoutAlongRay(Above(Cooler->GetActorLocation()), Down, true);
	TestEqual(TEXT("(2) two selected"), C->GetSelectedLayoutActors().Num(), 2);
	C->BeginLayoutDragAlongRay(Above(Rack->GetActorLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(FVector(500, 0, 0)), Down);
	C->EndLayoutDrag(true);
	const FVector Shift = Rack->GetActorLocation() - Rack0.GetLocation();
	const bool bGroupMoved = !Shift.IsNearlyZero() && (Cooler->GetActorLocation() - Cooler0.GetLocation()).Equals(Shift, 1e-6);
	const bool bGroupUndo = C->TryUndoLayout() && Rack->GetActorTransform().Equals(Rack0, 1e-6) && Cooler->GetActorTransform().Equals(Cooler0, 1e-6);
	const bool bRotated = C->TryRotateLayoutSelection();
	const bool bRotUndo = bRotated && C->TryUndoLayout() && Rack->GetActorTransform().Equals(Rack0, 1e-6) && Cooler->GetActorTransform().Equals(Cooler0, 1e-6);
	TestTrue(TEXT("(2) the rigid group keeps its flush pair: drag and Undo"), bGroupMoved && bGroupUndo);
	TestTrue(TEXT("(2) group quarter turn and Undo"), bRotated && bRotUndo);

	// (3) A real overlap is still refused: move the rack away, park a blocker on its old spot, Undo must fail.
	C->SelectLayoutAlongRay(Above(Rack->GetActorLocation()), Down);
	C->BeginLayoutDragAlongRay(Above(Rack->GetActorLocation()), Down);
	C->UpdateLayoutDragAlongRay(Above(FVector(0, 400, 0)), Down);
	C->EndLayoutDrag(true);
	auto* Blocker = W->SpawnActor<ACrahUnitActor>(Rack0.GetLocation() + FVector(0, 20, 0), FRotator::ZeroRotator);
	Blocker->SizeM = FVector(.4, .2, 1.0);
	const FTransform Away = Rack->GetActorTransform();
	TestTrue(TEXT("(3) Undo into a real overlap is refused and changes nothing"), !C->TryUndoLayout() && Rack->GetActorTransform().Equals(Away, 1e-6));
	// A NEW neighbour half a centimetre from the old spot (not touching when recorded) keeps the 1 cm rule.
	Blocker->SetActorLocation(Rack0.GetLocation() + FVector(0, 50.0 * GjRackLayout::FrameWidthM + 0.5 + 10.0, 0));
	TestTrue(TEXT("(3) a new neighbour 0.5 cm away still refuses the Undo"), !C->TryUndoLayout() && Rack->GetActorTransform().Equals(Away, 1e-6));
	Blocker->Destroy();
	TestTrue(TEXT("(3) blocker gone: Undo back to the flush spot"), C->TryUndoLayout() && Rack->GetActorTransform().Equals(Rack0, 1e-6));

	// (4) Deleting a flush neighbour and undoing the deletion puts it back (cooler, then rack).
	const FName CoolerName = Cooler->GetFName();
	C->SelectLayoutAlongRay(Above(Cooler->GetActorLocation()), Down);
	const bool bCoolerDeleted = C->TryDeleteLayoutCrah() && FindObjectFast<ACrahUnitActor>(W->PersistentLevel, CoolerName) == nullptr;
	ACrahUnitActor* CoolerBack = nullptr;
	const bool bCoolerUndo = C->TryUndoLayout() && (CoolerBack = FindObjectFast<ACrahUnitActor>(W->PersistentLevel, CoolerName)) != nullptr
		&& CoolerBack->GetActorTransform().Equals(Cooler0, 1e-6);
	const FName RackName = Rack->GetFName();
	C->SelectLayoutAlongRay(Above(Rack->GetActorLocation()), Down);
	const bool bRackDeleted = C->TryDeleteLayoutRack() && FindObjectFast<ARackThermalActor>(W->PersistentLevel, RackName) == nullptr;
	ARackThermalActor* RackBack = nullptr;
	const bool bRackUndo = C->TryUndoLayout() && (RackBack = FindObjectFast<ARackThermalActor>(W->PersistentLevel, RackName)) != nullptr
		&& RackBack->GetActorTransform().Equals(Rack0, 1e-6);
	TestTrue(TEXT("(4) a flush cooler deleted and restored by Undo"), bCoolerDeleted && bCoolerUndo);
	TestTrue(TEXT("(4) a rack flush with a cooler deleted and restored by Undo"), bRackDeleted && bRackUndo);
	AddInfo(FString::Printf(TEXT("(1) 이동 %d·Undo %d·Redo %d, (2) 그룹 이동 %d·Undo %d·회전 %d·Undo %d, (4) 쿨러 삭제·Undo %d·%d, 랙 삭제·Undo %d·%d"),
		bMoved, bUndo, bRedo, bGroupMoved, bGroupUndo, bRotated, bRotUndo, bCoolerDeleted, bCoolerUndo, bRackDeleted, bRackUndo));

	W->DestroyWorld(false); GEngine->DestroyWorldContext(W);
	return true;
}

#endif
