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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomHeightHandleTest, "VRG.Interaction.Layout.RoomHeightHandlePreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomHeightHandleTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>(); Rack->ApplyGb200Nvl72Preset();
	auto* Pawn=W->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C);
	Sim->EnterLayoutMode(); C->RefreshInteractionInputMode(); FString Message;
	TestFalse(TEXT("Automatic fit has no height handle edit"),C->SetRoomBoundsEditing(true));
	// Synthetic room/ray geometry, matching existing room tests; not facility dimensions or physical constants.
	FRoomAirDomainSettings A; A.bManual=true; A.MinXYM=FVector2D(-5.5,-4.5); A.MaxXYM=FVector2D(5.1,4.6); A.CeilingHeightM=3.1;
	TestTrue(TEXT("Manual room activates existing edit mode"),Sim->TrySetRoomAirDomain(A,Message) && C->SetRoomBoundsEditing(true));
	const FVector Eye(980,5,300), Side(-1,0,0), Down(0,0,-1);
	const auto Grid=Air->GridParams; const auto Origin=Air->GetActorLocation(); const auto Pose=Rack->GetActorTransform();
	C->SetRoomBoundsEditing(false); C->SelectLayoutAlongRay(FVector(0,0,500),Down); C->SetRoomBoundsEditing(true);
	TestFalse(TEXT("Parallel and nearly parallel rays never arm"),C->BeginRoomHeightDrag(FVector(-20,5,1000),Down)
		|| C->BeginRoomHeightDrag(FVector(-20,5,1000),FVector(.001,0,-1)));
	TestTrue(TEXT("Indeterminate height explains refusal without gesture/history"),!C->IsLayoutDragging() && !C->GetRoomBoundsMessage().IsEmpty() && C->GetLayoutUndoCount()==0);
	TestFalse(TEXT("Backward ray cannot grab an axis behind camera"),C->BeginRoomHeightDrag(Eye,FVector(1,0,0)));
	TestTrue(TEXT("Side ray arms height handle with original grab offset"),C->BeginRoomHeightDrag(Eye,Side));
	FRoomAirDomainSettings Preview; bool bValid=false;
	C->UpdateRoomCornerDrag(Eye,Side);
	TestTrue(TEXT("Initial update does not jump to axis hit height"),C->GetRoomBoundsPreview(Preview,bValid) && bValid && Preview==A);
	C->UpdateRoomCornerDrag(Eye+FVector(0,0,63),Side);
	TestTrue(TEXT("63cm delta snaps original3.1m height to3.75m while XY unchanged"),C->GetRoomBoundsPreview(Preview,bValid) && bValid
		&& Preview.MinXYM==A.MinXYM && Preview.MaxXYM==A.MaxXYM && Preview.CeilingHeightM==3.75 && C->GetRoomBoundsMessage().Contains(TEXT("43x37x15")));
	const auto Camera=Pawn->GetActorLocation(); C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true);
	C->BeginLayoutMarquee(FVector2D(0,0),false); C->BeginRackAddition(); C->OnResetSim();
	TestFalse(TEXT("Shared editor gates numeric window, addition, marquee and global history"),C->OpenRoomSettings() || C->IsAddingRack()
		|| C->IsLayoutMarqueeActive() || C->TryUndoLayout() || C->TryRedoLayout());
	TestFalse(TEXT("Height preview cannot start XY drag or equipment rotation/duplication/deletion"),C->BeginRoomCornerDrag(0,FVector(-550,-450,500),Down)
		|| C->TryRotateLayoutSelection() || C->BeginRackDuplication() || C->TryDeleteLayoutRack());
	TestTrue(TEXT("Preview preserves actual setting/equipment/grid/clock/history and camera"),Sim->GetRoomAirDomain()==A && Rack->GetActorTransform().Equals(Pose,0)
		&& Air->GetActorLocation()==Origin && Air->GridParams.NumCellsZ==Grid.NumCellsZ && Sim->GetSnapshot().SimTimeS==0
		&& C->GetLayoutUndoCount()==0 && Pawn->GetActorLocation()==Camera);
	C->UpdateRoomCornerDrag(FVector(-20,5,1000),FVector(.001,0,-1));
	TestTrue(TEXT("Near parallel during gesture preserves last value but blocks stale commit"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid && Preview.CeilingHeightM==3.75);
	TestFalse(TEXT("Indeterminate release cannot commit last valid height"),C->EndRoomCornerDrag(true));
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(FVector(-20,5,1000),Down); C->UpdateRoomCornerDrag(Eye+FVector(0,0,63),Side);
	TestTrue(TEXT("Stable ray recovery revalidates preview"),C->GetRoomBoundsPreview(Preview,bValid) && bValid && Preview.CeilingHeightM==3.75);
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc height cancellation keeps editor and original setting/history"),C->IsRoomBoundsEditing() && !C->IsLayoutDragging() && Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0);
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,-200),Side);
	TestTrue(TEXT("Too low for equipment/automatic CRAH/ducts is red with reason and allocatable plan"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid
		&& !C->GetRoomBoundsMessage().IsEmpty() && C->GetRoomBoundsMessage().Contains(TEXT("실제 범위")));
	TestFalse(TEXT("Too low release rejected"),C->EndRoomCornerDrag(true));
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,3700),Side);
	TestTrue(TEXT("Axis128 resource overflow is rejected without truncated plan"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid && !C->GetRoomBoundsMessage().Contains(TEXT("실제 범위")));
	TestFalse(TEXT("Resource overflow cannot commit"),C->EndRoomCornerDrag(true));
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,-310),Side);
	TestTrue(TEXT("Non-positive height refused by existing API"),C->GetRoomBoundsPreview(Preview,bValid) && !bValid); C->EndRoomCornerDrag(false);
	// Oblique and non-unit rays must locate the same vertical-axis height instead of using a fixed screen scale.
	TestTrue(TEXT("Oblique view ray arms same shared height drag"),C->BeginRoomHeightDrag(FVector(980,5,800),FVector(-1000,0,-500)));
	C->UpdateRoomCornerDrag(FVector(980,5,863),FVector(-1000,0,-500));
	TestTrue(TEXT("Oblique view preserves same5cm height snap and XY"),C->GetRoomBoundsPreview(Preview,bValid) && bValid
		&& Preview.CeilingHeightM==3.75 && Preview.MinXYM==A.MinXYM && Preview.MaxXYM==A.MaxXYM); C->EndRoomCornerDrag(false);
	C->BeginRoomHeightDrag(Eye,Side); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch closes/cancels height editor without record"),!C->IsRoomBoundsEditing() && !C->IsLayoutDragging() && Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0);
	TestFalse(TEXT("Patch blocks height edit"),C->SetRoomBoundsEditing(true)); C->SetPatchMapOpen(false);
	C->SetRoomBoundsEditing(true); TestTrue(TEXT("Numeric window exits idle height editor"),C->OpenRoomSettings() && !C->IsRoomBoundsEditing()); C->CloseRoomSettings();
	AddInfo(TEXT("Ceiling preview:3.1m +63cm ->3.75m, plan43x37x15=23865cells; XY/grid/pose/clock/history unchanged. Parallel/near-parallel/backward rays blocked, oblique/non-unit rays consistent; no actual picking/render/OS-focus claim."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
