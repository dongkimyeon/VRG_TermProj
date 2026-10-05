#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgSpineLayoutState.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "InputKeyEventArgs.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineBatchDuplicationTest, "VRG.Interaction.Layout.SpineBatchDuplicationUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineBatchDuplicationTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	auto* A=W->SpawnActor<ASpineCabinetActor>(FVector(300,0,20),FRotator::ZeroRotator);
	// Synthetic nondefault authored config/transform; no new hardware physical values.
	auto Config=GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings(); Config.NumSwitches=2; Config.FirstSwitchSlot=5;
	auto* B=VrgSpineLifecycle::Spawn(W,TEXT("RuntimeBatchCloneSource"),FGuid::NewGuid().ToString(),
		FTransform(FRotator(0,90,0),FVector(700,0,35),FVector(.9,1.1,1.2)),Config);
	Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
	auto Select=[&](AActor* Actor,bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto Both=[&]() { Select(A); Select(B,true); };
	auto Count=[&]() { int32 N=0; for(TActorIterator<ASpineCabinetActor> It(W);It;++It) ++N; return N; };
	auto Refused=[&]() { const int32 N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
		TestFalse(TEXT("Duplication blocked"),C->BeginSpineDuplication());
		TestTrue(TEXT("Rejection preserves count/history"),Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R); };
	Refused(); Both(); Select(Rack,true); Refused(); Both();
	C->BeginLayoutDragAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
	C->BeginLayoutMarquee(FVector2D::ZeroVector,false); Refused(); C->EndLayoutMarquee(false);
	C->BeginSpineAddition(); Refused(); C->CancelRackAddition(); Both();
	C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false);
	Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Both();
	A->GetMutableModel().Step(A->GetMutableInput(),.02); B->GetMutableModel().Step(B->GetMutableInput(),.02);
	const FTransform Originals[]={A->GetActorTransform(),B->GetActorTransform()};
	const FSpineLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	const FString SourceIds[]={VrgSpineLifecycle::Identity(A),VrgSpineLifecycle::Identity(B)};
	const double Energies[]={A->GetMutableModel().GetEnergyInJ(),B->GetMutableModel().GetEnergyInJ()};
	auto Key=[&](FKey Code,EInputEvent Event) { FInputKeyEventArgs Args; Args.Key=Code; Args.Event=Event; Args.AmountDepressed=1; C->InputKey(Args); };
	Key(EKeys::LeftControl,IE_Pressed); Key(EKeys::D,IE_Pressed); Key(EKeys::D,IE_Released); Key(EKeys::LeftControl,IE_Released);
	TestEqual(TEXT("Ctrl+D gives two value previews"),C->GetRackAdditionPreviewCount(),2); Refused();
	C->UpdateRackAdditionAlongRay(A->GetActorLocation()+FVector(0,0,500),Down);
	TestTrue(TEXT("Source overlap blocks entire group"),C->IsRackAdditionBlocked()); TestNull(TEXT("No overlap creation"),C->ConfirmSpineAddition());
	C->CancelRackAddition(); TestTrue(TEXT("Cancel preserves count/history"),Count()==2 && C->GetLayoutUndoCount()==0);
	Both(); TestTrue(TEXT("Restart"),C->BeginSpineDuplication());
	C->UpdateRackAdditionAlongRay(FVector(302,203,500),Down); FTransform Preview[2]; FVector Size;
	for(int32 I=0;I<2;++I) { TestTrue(TEXT("Member preview"),C->GetRackAdditionPreview(Preview[I],Size,I));
		TestTrue(TEXT("Anchor5cm translation retains full member transform"),Preview[I].GetLocation().Equals(Originals[I].GetLocation()+FVector(0,205,0),1e-9)
			&& Preview[I].GetRotation().Equals(Originals[I].GetRotation(),1e-9) && Preview[I].GetScale3D().Equals(Originals[I].GetScale3D(),1e-9)); }
	TestFalse(TEXT("Group R is blocked"),C->TryRotateRackAddition()); C->OnResetSim();
	FTransform AfterR; C->GetRackAdditionPreview(AfterR,Size); TestTrue(TEXT("R leaves group preview intact"),AfterR.Equals(Preview[0],0));
	auto* Block=W->SpawnActor<ACrahUnitActor>(Preview[1].GetLocation(),FRotator::ZeroRotator);
	TestNull(TEXT("Second preview collision rejects all"),C->ConfirmSpineAddition());
	TestTrue(TEXT("Collision keeps count/history and preview"),Count()==2 && C->GetLayoutUndoCount()==0 && C->IsAddingRack()); Block->Destroy(false,false);
	int32 Spawned=0;
	const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Cast<ASpineCabinetActor>(Actor) && Actor->GetName().StartsWith(TEXT("DuplicatedSpine_")) && ++Spawned==2) Actor->Destroy(false,false);
	}));
	TestNull(TEXT("Second actual spawn failure removes first"),C->ConfirmSpineAddition()); W->RemoveOnActorSpawnedHandler(Handler);
	TestTrue(TEXT("Failed creation restores inventory/selection/history/deleted intent"),Count()==2 && C->GetSelectedLayoutActors().Num()==2
		&& C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0 && C->IsAddingRack() && W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.IsEmpty());
	auto* Anchor=C->ConfirmSpineAddition(); if(!TestNotNull(TEXT("Whole clone confirm"),Anchor)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	TestTrue(TEXT("Two clones, one command, both selected"),Count()==4 && C->GetLayoutUndoCount()==1 && C->GetSelectedLayoutActors().Num()==2);
	FString CloneIds[2]; FName CloneNames[2]; int32 I=0;
	for(auto* Actor : C->GetSelectedLayoutActors()) {
		auto* Clone=CastChecked<ASpineCabinetActor>(Actor); CloneIds[I]=VrgSpineLifecycle::Identity(Clone); CloneNames[I]=Clone->GetFName();
		TestTrue(TEXT("Unique runtime identity, config/transform and fresh ledger"),CloneIds[I]!=SourceIds[0] && CloneIds[I]!=SourceIds[1]
			&& Clone->bRuntimeLayoutSpine && Clone->CaptureLayoutSettings()==Configs[I] && Clone->GetActorTransform().Equals(Preview[I],1e-9) && Clone->GetMutableModel().GetEnergyInJ()==0);
		++I;
	}
	TestTrue(TEXT("Clones independent"),CloneIds[0]!=CloneIds[1]);
	auto* First=VrgSpineLifecycle::Find(W,CloneIds[0]); First->GetMutableInput().ComponentHeatW[0]+=1;
	TestTrue(TEXT("Originals unchanged"),A->GetActorTransform().Equals(Originals[0],0) && B->GetActorTransform().Equals(Originals[1],0)
		&& A->CaptureLayoutSettings()==Configs[0] && B->CaptureLayoutSettings()==Configs[1]
		&& A->GetMutableModel().GetEnergyInJ()==Energies[0] && B->GetMutableModel().GetEnergyInJ()==Energies[1]
		&& First->GetMutableInput().ComponentHeatW[0]!=A->GetMutableInput().ComponentHeatW[0]);
	TestNull(TEXT("Repeated confirm adds none"),C->ConfirmSpineAddition());
	TestTrue(TEXT("Whole Undo"),C->TryUndoLayout() && Count()==2);
	Block=W->SpawnActor<ACrahUnitActor>(Preview[1].GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("Second destination blocks whole Redo"),C->TryRedoLayout());
	TestTrue(TEXT("Redo rejection maintains inventory/stacks"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1); Block->Destroy(false,false);
	TestTrue(TEXT("Whole Redo"),C->TryRedoLayout()); TestEqual(TEXT("Redo count"),Count(),4);
	for(int32 J=0;J<2;++J) { const auto* Clone=VrgSpineLifecycle::Find(W,CloneIds[J]);
		TestTrue(TEXT("Redo stable identity/config/transform"),Clone && Clone->GetFName()==CloneNames[J] && Clone->CaptureLayoutSettings()==Configs[J] && Clone->GetActorTransform().Equals(Preview[J],1e-9)); }
	TestTrue(TEXT("Undo cleanup"),C->TryUndoLayout()); Both(); C->BeginSpineDuplication(); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels whole preview"),C->IsAddingRack()); C->SetPatchMapOpen(false); Both(); C->BeginSpineDuplication();
	Sim->EnterSimulationMode(); C->RefreshInteractionInputMode(); TestFalse(TEXT("Mode cancels whole preview"),C->IsAddingRack()); Sim->EnterLayoutMode();
	TestEqual(TEXT("Canceled count"),Count(),2); TestEqual(TEXT("Layout clock"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("Spine batch duplicate: guards/Ctrl+D/value previews, anchor5cm, full transforms/source invariance, total1cm group collision, R blocked/Esc/mode/patch cancel, mid-spawn rollback, unique IDs/fresh ledger, one Undo/Redo; Equals1e-9 actor cm boundary."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
