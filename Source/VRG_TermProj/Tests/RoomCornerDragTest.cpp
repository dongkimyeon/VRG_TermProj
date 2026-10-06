#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCornerPreviewTest, "VRG.Interaction.Layout.RoomCornerPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCornerPreviewTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart = false;
	auto* Air = W->SpawnActor<AAirFieldDebugActor>(); auto* Rack = W->SpawnActor<ARackThermalActor>(); Rack->ApplyGb200Nvl72Preset();
	auto* Pawn = W->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput = NewObject<UPlayerInput>(C);
	Sim->EnterLayoutMode(); C->RefreshInteractionInputMode();
	const FVector Down(0,0,-1); FString Message;
	TestFalse(TEXT("Automatic mode cannot activate handles"), C->SetRoomBoundsEditing(true));
	TestTrue(TEXT("Automatic refusal explains manual prerequisite"), !C->GetRoomBoundsMessage().IsEmpty() && !C->IsRoomBoundsEditing());
	// Synthetic room fixture geometry, not sourced hardware/physical constants.
	FRoomAirDomainSettings A; A.bManual=true; A.MinXYM=FVector2D(-5.5,-4.5); A.MaxXYM=FVector2D(5.1,4.6); A.CeilingHeightM=3.1;
	TestTrue(TEXT("Existing room API accepts fixture"), Sim->TrySetRoomAirDomain(A, Message));
	const auto Grid = Air->GridParams; const auto Origin = Air->GetActorLocation(); const auto Pose = Rack->GetActorTransform();
	C->SelectLayoutAlongRay(FVector(0,0,500), Down);
	TestTrue(TEXT("Manual mode activates exclusive handles"), C->SetRoomBoundsEditing(true));
	FRoomAirDomainSettings Preview; bool bValid=false;
	TestTrue(TEXT("Range and planned cells available"), C->GetRoomBoundsPreview(Preview,bValid) && bValid && Preview==A && C->GetRoomBoundsMessage().Contains(TEXT("20683")));
	C->BeginRackAddition(); C->BeginLayoutDragAlongRay(FVector(0,0,500),Down); C->BeginLayoutMarquee(FVector2D(0,0),false);
	TestFalse(TEXT("Room edit owns equipment drag/add/marquee"), C->IsAddingRack() || C->IsLayoutDragging() || C->IsLayoutMarqueeActive());
	TestFalse(TEXT("Rotation/move/duplicate/delete are blocked"), C->TryRotateLayoutSelection() || C->TryLayoutMove(Rack,FVector(0,100,0)) || C->BeginRackDuplication() || C->TryDeleteLayoutRack());
	const auto Selection=C->GetSelectedLayoutActors(); C->SelectLayoutAlongRay(FVector(2000,0,500),Down);
	TestTrue(TEXT("Room clicks cannot clear equipment selection underneath"), C->GetSelectedLayoutActors()==Selection);
	const auto CameraBefore=Pawn->GetActorLocation(); C->ApplyLayoutCameraInput(1,FVector(1,0,0),0,true);
	TestTrue(TEXT("Idle room editor preserves camera navigation"), Pawn->GetActorLocation()!=CameraBefore);
	TestFalse(TEXT("Invalid handle/ray cannot arm"), C->BeginRoomCornerDrag(-1,FVector(510,460,500),Down) || C->BeginRoomCornerDrag(3,FVector(510,460,500),FVector(1,0,0)));
	TestTrue(TEXT("Max corner arms"), C->BeginRoomCornerDrag(3,FVector(510,460,500),Down));
	C->UpdateRoomCornerDrag(FVector(613,573,500),Down);
	TestTrue(TEXT("Absolute corner snaps to 5cm, opposite corner and ceiling fixed"), C->GetRoomBoundsPreview(Preview,bValid) && bValid
		&& Preview.MinXYM==A.MinXYM && Preview.MaxXYM.Equals(FVector2D(6.15,5.75),1e-12) && Preview.CeilingHeightM==A.CeilingHeightM);
	const auto DragCamera=Pawn->GetActorLocation(); C->ApplyLayoutCameraInput(1,FVector(1,0,0),1,true);
	TestTrue(TEXT("Preview cannot change actual domain/equipment/clock/history/camera"), Sim->GetRoomAirDomain()==A
		&& Air->GetActorLocation()==Origin && Air->GridParams.NumCellsX==Grid.NumCellsX && Rack->GetActorTransform().Equals(Pose,0)
		&& Sim->GetSnapshot().SimTimeS==0 && C->GetLayoutUndoCount()==0 && Pawn->GetActorLocation()==DragCamera);
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc cancels entire preview, leaves editor active and history unchanged"), !C->IsLayoutDragging() && C->IsRoomBoundsEditing() && Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0);
	for(int32 Corner=0;Corner<4;++Corner)
	{
		const FVector Start(Corner&1?510:-550,Corner&2?460:-450,500);
		TestTrue(TEXT("Each corner can arm"),C->BeginRoomCornerDrag(Corner,Start,Down));
		C->UpdateRoomCornerDrag(Start+FVector(Corner&1?50:-50,Corner&2?50:-50,0),Down);
		C->GetRoomBoundsPreview(Preview,bValid);
		TestTrue(TEXT("Only selected X/Y edges change"),bValid && (Corner&1?Preview.MinXYM.X==A.MinXYM.X:Preview.MaxXYM.X==A.MaxXYM.X)
			&& (Corner&2?Preview.MinXYM.Y==A.MinXYM.Y:Preview.MaxXYM.Y==A.MaxXYM.Y) && Preview.CeilingHeightM==A.CeilingHeightM);
		C->EndRoomCornerDrag(false);
	}
	C->BeginRoomCornerDrag(3,FVector(510,460,500),Down); C->UpdateRoomCornerDrag(FVector(20,20,500),Down);
	TestTrue(TEXT("Equipment/automatic CRAH/duct inclusion rejection displayed with grid plan"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid && !C->GetRoomBoundsMessage().IsEmpty() && C->GetRoomBoundsMessage().Contains(TEXT("셀")));
	TestFalse(TEXT("Invalid inclusion cannot commit"),C->EndRoomCornerDrag(true));
	C->BeginRoomCornerDrag(3,FVector(510,460,500),Down); C->UpdateRoomCornerDrag(FVector(5000,5000,500),Down);
	TestTrue(TEXT("Resource limit rejection is not a clipped plan"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid && !C->GetRoomBoundsMessage().Contains(TEXT("실제 범위")));
	C->EndRoomCornerDrag(false);
	C->BeginRoomCornerDrag(3,FVector(510,460,500),Down); C->UpdateRoomCornerDrag(FVector(-600,-600,500),Down);
	TestTrue(TEXT("Crossing opposite corner is rejected, never sorted into a different room"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid); C->EndRoomCornerDrag(false);
	C->BeginRoomCornerDrag(3,FVector(510,460,500),Down); C->UpdateRoomCornerDrag(FVector(510,460,500),FVector(1,0,0));
	TestFalse(TEXT("No floor intersection cannot commit stale preview"),C->EndRoomCornerDrag(true));
	TestTrue(TEXT("All canceled/rejected previews preserve settings and history"),Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0);
	C->BeginRoomCornerDrag(3,FVector(510,460,500),Down); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch cancels preview and disables edit"),!C->IsRoomBoundsEditing() && !C->IsLayoutDragging() && Sim->GetRoomAirDomain()==A);
	TestFalse(TEXT("Patch blocks activation"),C->SetRoomBoundsEditing(true)); C->SetPatchMapOpen(false);
	C->SetRoomBoundsEditing(true); TestTrue(TEXT("Numeric modal exits idle handle editor"),C->OpenRoomSettings() && !C->IsRoomBoundsEditing()); C->CloseRoomSettings();
	C->SetRoomBoundsEditing(true); C->InputKey(Esc); TestFalse(TEXT("Idle Esc exits handles"),C->IsRoomBoundsEditing());
	AddInfo(TEXT("Room corners: four corners, 5cm snap, opposite/height unchanged; preview/rejection/cancel settings/pose/clock/history unchanged; exclusive equipment/marquee and idle camera preserved. No actual viewport picking/focus/render claim."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
