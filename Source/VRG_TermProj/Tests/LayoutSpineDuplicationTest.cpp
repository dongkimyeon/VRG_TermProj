#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "InputKeyEventArgs.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineDuplicationTest, "VRG.Interaction.Layout.SpineDuplicationUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineDuplicationTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	auto* Source=W->SpawnActor<ASpineCabinetActor>(FVector(300,0,25),FRotator(0,90,0));
	Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
	auto Select=[&](AActor* A,bool Toggle=false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto Count=[&]() { int32 N=0; for(TActorIterator<ASpineCabinetActor> It(W);It;++It) ++N; return N; };
	auto Refused=[&]() {
		const int32 N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
		TestFalse(TEXT("Duplication blocked"),C->BeginSpineDuplication());
		TestTrue(TEXT("Rejected duplication count/history invariant"),Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R);
	};
	Refused(); Select(Rack); Refused(); Select(Source); Select(Rack,true); Refused();
	auto* Other=W->SpawnActor<ASpineCabinetActor>(FVector(700,0,0),FRotator::ZeroRotator);
	Other->Destroy(false,false); // Native group selection is covered by SpineBatchDuplicationUndoRedo.
	Select(Source); C->BeginLayoutDragAlongRay(Source->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
	C->BeginLayoutMarquee(FVector2D(0,0),false); Refused(); C->EndLayoutMarquee(false);
	C->BeginSpineAddition(); Refused(); C->CancelRackAddition(); Select(Source);
	C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false); Select(Source);
	Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Select(Source);
	auto Key=[&](FKey Code,EInputEvent Event) { FInputKeyEventArgs A; A.Key=Code; A.Event=Event; A.AmountDepressed=1; C->InputKey(A); };
	for(int32 Case=0;Case<2;++Case)
	{
		if(Case==1) {
			auto Config=GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings(); Config.NumSwitches=2; Config.FirstSwitchSlot=5;
			// Synthetic authored settings/transform only, not new hardware physical values.
			Source=VrgSpineLifecycle::Spawn(W,TEXT("RuntimeSpineSource"),FGuid::NewGuid().ToString(),FTransform(FRotator(0,90,0),FVector(1000,0,25)),Config);
		}
		Source->SetActorScale3D(FVector(.9,1.1,1.2)); Source->EnsureThermalModel();
		Source->GetMutableModel().Step(Source->GetMutableInput(),.02);
		const FTransform Original=Source->GetActorTransform(); const auto Config=Source->CaptureLayoutSettings();
		const FString OriginalId=VrgSpineLifecycle::Identity(Source); const double Energy=Source->GetMutableModel().GetEnergyInJ();
		const int32 Before=Count(); C->ClearLayoutHistory(); Select(Source);
		Key(EKeys::LeftControl,IE_Pressed); Key(EKeys::D,IE_Pressed); Key(EKeys::D,IE_Released); Key(EKeys::LeftControl,IE_Released);
		TestTrue(TEXT("Ctrl+D event starts shared spine preview"),C->IsAddingRack() && C->GetRackAdditionPreviewCount()==1);
		Refused(); C->UpdateRackAdditionAlongRay(Original.GetLocation()+FVector(0,0,500),Down);
		TestTrue(TEXT("Overlap marks red/blocked"),C->IsRackAdditionBlocked()); TestNull(TEXT("Overlap refuses creation"),C->ConfirmSpineAddition());
		TestTrue(TEXT("Value-only preview and rejection"),Count()==Before && C->GetLayoutUndoCount()==0);
		C->CancelRackAddition(); TestNull(TEXT("Esc no creation"),C->ConfirmSpineAddition());
		TestTrue(TEXT("Restart duplicate"),C->BeginSpineDuplication());
		C->UpdateRackAdditionAlongRay(FVector(Original.GetLocation().X+2,203,500),Down);
		FTransform Preview; FVector SizeM; TestTrue(TEXT("Destination preview"),C->GetRackAdditionPreview(Preview,SizeM));
		TestTrue(TEXT("5cm XY snap preserves original Z/rotation/scale"),Preview.GetLocation().Equals(FVector(Original.GetLocation().X,205,25),1e-9)
			&& Preview.GetRotation().Equals(Original.GetRotation(),1e-9) && Preview.GetScale3D().Equals(Original.GetScale3D(),1e-9));
		C->OnResetSim(); C->GetRackAdditionPreview(Preview,SizeM);
		TestTrue(TEXT("R rotates preview only"),Preview.GetRotation().Equals(FRotator(0,90,0).Quaternion()*Original.GetRotation(),1e-9)
			&& Source->GetActorTransform().Equals(Original,0));
		auto* Clone=C->ConfirmSpineAddition();
		if(!TestNotNull(TEXT("Single spine clone"),Clone)) break;
		const FString CloneId=VrgSpineLifecycle::Identity(Clone); const FName CloneName=Clone->GetFName();
		TestTrue(TEXT("One creation and runtime independent identity"),Count()==Before+1 && C->GetLayoutUndoCount()==1
			&& Clone->bRuntimeLayoutSpine && CloneId!=OriginalId && Clone->GetFName()!=Source->GetFName());
		TestTrue(TEXT("Config/transform copied; original unchanged and no live thermal copy"),Clone->CaptureLayoutSettings()==Config
			&& Clone->GetActorTransform().Equals(Preview,1e-9) && Source->GetActorTransform().Equals(Original,0)
			&& Source->CaptureLayoutSettings()==Config && Source->GetMutableModel().GetEnergyInJ()==Energy && Clone->GetMutableModel().GetEnergyInJ()==0);
		const double OriginalHeat=Source->GetMutableInput().ComponentHeatW[0];
		Clone->GetMutableInput().ComponentHeatW[0]+=1;
		TestEqual(TEXT("Input arrays independent"),Source->GetMutableInput().ComponentHeatW[0],OriginalHeat);
		TestNull(TEXT("Repeated confirm creates none"),C->ConfirmSpineAddition());
		TestTrue(TEXT("Undo removes only clone"),C->TryUndoLayout() && Count()==Before && IsValid(Source));
		auto* Block=W->SpawnActor<ACrahUnitActor>(Preview.GetLocation(),FRotator::ZeroRotator);
		TestFalse(TEXT("Redo collision refuses"),C->TryRedoLayout());
		TestTrue(TEXT("Collision keeps inventory/stacks"),Count()==Before && C->GetLayoutRedoCount()==1 && C->GetLayoutUndoCount()==0); Block->Destroy(false,false);
		TestTrue(TEXT("Redo clone"),C->TryRedoLayout()); Clone=VrgSpineLifecycle::Find(W,CloneId);
		TestTrue(TEXT("Same clone identity/config and fresh model"),Clone && Clone->GetFName()==CloneName && Clone->CaptureLayoutSettings()==Config
			&& Clone->GetActorTransform().Equals(Preview,1e-9) && Clone->GetMutableModel().GetEnergyInJ()==0);
		TestTrue(TEXT("Remove clone again"),C->TryUndoLayout()); C->ClearLayoutHistory(); Select(Source);
		C->BeginSpineDuplication(); C->SetPatchMapOpen(true); TestFalse(TEXT("Patch cancels clone preview"),C->IsAddingRack()); C->SetPatchMapOpen(false);
		Select(Source); C->BeginSpineDuplication(); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
		TestFalse(TEXT("Simulation cancels clone preview"),C->IsAddingRack()); Sim->EnterLayoutMode();
		TestEqual(TEXT("Cancel inventory unchanged"),Count(),Before);
	}
	TestEqual(TEXT("Layout time stays zero"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("Spine duplicate: Ctrl+D, guards, placed/default and runtime/nondefault config, value preview/R/Esc/collision/patch/mode, independent runtime ID/model/input, one Undo/Redo; full transform Equals1e-9 at actor cm boundary."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
