#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "InputKeyEventArgs.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahBatchDuplicationTest, "VRG.Interaction.Layout.CrahBatchDuplicationUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCrahBatchDuplicationTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	auto* A=W->SpawnActor<ACrahUnitActor>(FVector(300,0,20),FRotator::ZeroRotator);
	// Synthetic nondefault authored config/transform; no new hardware physical values.
	auto Config=GetDefault<ACrahUnitActor>()->CaptureLayoutSettings(); Config.Kind=ECrahUnitKind::Perimeter; Config.VolumeFlowM3s=.25; Config.SizeM*=1.1;
	auto* B=VrgCrahLifecycle::Spawn(W,TEXT("RuntimeBatchCloneSource"),FGuid::NewGuid().ToString(),
		FTransform(FRotator(0,90,0),FVector(700,0,35),FVector(.9,1.1,1.2)),Config);
	// A is placed initially; then mark its authored baseline as a converted automatic source.
	A->bConvertedAutomaticCrah=true; A->AutomaticLayoutTransform=A->GetActorTransform(); A->AutomaticLayoutSizeM=A->SizeM;
	Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
	auto Select=[&](AActor* Actor,bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto Both=[&]() { Select(A); Select(B,true); };
	auto Count=[&]() { int32 N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N; };
	auto Refused=[&]() { const int32 N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
		TestFalse(TEXT("Duplication blocked"),C->BeginCrahDuplication());
		TestTrue(TEXT("Rejection preserves count/history"),Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R); };
	Refused(); Both(); Select(Rack,true); Refused(); Both();
	C->BeginLayoutDragAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
	C->BeginLayoutMarquee(FVector2D::ZeroVector,false); Refused(); C->EndLayoutMarquee(false);
	C->BeginCrahAddition(); Refused(); C->CancelRackAddition(); Both();
	C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false);
	Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Both();
	const FTransform Originals[]={A->GetActorTransform(),B->GetActorTransform()};
	const FCrahLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	const FString SourceIds[]={VrgCrahLifecycle::Identity(A),VrgCrahLifecycle::Identity(B)};
	auto Key=[&](FKey Code,EInputEvent Event) { FInputKeyEventArgs Args; Args.Key=Code; Args.Event=Event; Args.AmountDepressed=1; C->InputKey(Args); };
	Key(EKeys::LeftControl,IE_Pressed); Key(EKeys::D,IE_Pressed); Key(EKeys::D,IE_Released); Key(EKeys::LeftControl,IE_Released);
	TestEqual(TEXT("Ctrl+D gives two value previews"),C->GetRackAdditionPreviewCount(),2); Refused();
	C->UpdateRackAdditionAlongRay(A->GetActorLocation()+FVector(0,0,500),Down);
	TestTrue(TEXT("Source overlap blocks entire group"),C->IsRackAdditionBlocked()); TestNull(TEXT("No overlap creation"),C->ConfirmCrahAddition());
	C->CancelRackAddition(); TestTrue(TEXT("Cancel preserves count/history"),Count()==2 && C->GetLayoutUndoCount()==0);
	Both(); TestTrue(TEXT("Restart"),C->BeginCrahDuplication());
	C->UpdateRackAdditionAlongRay(FVector(302,203,500),Down); FTransform Preview[2]; FVector Size;
	for(int32 I=0;I<2;++I) { TestTrue(TEXT("Member preview"),C->GetRackAdditionPreview(Preview[I],Size,I));
		TestTrue(TEXT("Anchor5cm translation retains full member transform"),Preview[I].GetLocation().Equals(Originals[I].GetLocation()+FVector(0,205,0),1e-9)
			&& Preview[I].GetRotation().Equals(Originals[I].GetRotation(),1e-9) && Preview[I].GetScale3D().Equals(Originals[I].GetScale3D(),1e-9) && Size==Configs[I].SizeM); }
	TestFalse(TEXT("Group R is blocked"),C->TryRotateRackAddition()); C->OnResetSim();
	FTransform AfterR; C->GetRackAdditionPreview(AfterR,Size); TestTrue(TEXT("R leaves group preview intact"),AfterR.Equals(Preview[0],0));
	auto* Block=W->SpawnActor<ACoolingDistributionUnitActor>(Preview[1].GetLocation(),FRotator::ZeroRotator);
	TestNull(TEXT("Second preview collision rejects all"),C->ConfirmCrahAddition());
	TestTrue(TEXT("Collision keeps count/history and preview"),Count()==2 && C->GetLayoutUndoCount()==0 && C->IsAddingRack()); Block->Destroy(false,false);
	int32 Spawned=0;
	const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Cast<ACrahUnitActor>(Actor) && Actor->GetName().StartsWith(TEXT("DuplicatedCrah_")) && ++Spawned==2) Actor->Destroy(false,false);
	}));
	TestNull(TEXT("Second actual spawn failure removes first"),C->ConfirmCrahAddition()); W->RemoveOnActorSpawnedHandler(Handler);
	TestTrue(TEXT("Failed creation restores inventory/selection/history/deleted intent"),Count()==2 && C->GetSelectedLayoutActors().Num()==2
		&& C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0 && C->IsAddingRack() && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.IsEmpty());
	auto* Anchor=C->ConfirmCrahAddition(); if(!TestNotNull(TEXT("Whole clone confirm"),Anchor)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	TestTrue(TEXT("Two clones, one command, both selected"),Count()==4 && C->GetLayoutUndoCount()==1 && C->GetSelectedLayoutActors().Num()==2);
	FString CloneIds[2]; FName CloneNames[2]; int32 I=0;
	for(auto* Actor : C->GetSelectedLayoutActors()) {
		auto* Clone=CastChecked<ACrahUnitActor>(Actor); CloneIds[I]=VrgCrahLifecycle::Identity(Clone); CloneNames[I]=Clone->GetFName();
		TestTrue(TEXT("Unique runtime identity, config/transform"),CloneIds[I]!=SourceIds[0] && CloneIds[I]!=SourceIds[1]
			&& Clone->bRuntimeLayoutCrah && Clone->CaptureLayoutSettings()==Configs[I] && Clone->GetActorTransform().Equals(Preview[I],1e-9));
		TestFalse(TEXT("No copied automatic provenance or boundary exemption"),Clone->bConvertedAutomaticCrah || Clone->PreservesAutomaticAirBounds());
		++I;
	}
	TestTrue(TEXT("Clones independent"),CloneIds[0]!=CloneIds[1]);
	auto* First=VrgCrahLifecycle::Find(W,CloneIds[0]); First->VolumeFlowM3s+=.05;
	TestTrue(TEXT("Originals unchanged and settings independent"),A->GetActorTransform().Equals(Originals[0],0) && B->GetActorTransform().Equals(Originals[1],0)
		&& A->CaptureLayoutSettings()==Configs[0] && B->CaptureLayoutSettings()==Configs[1]
		&& VrgCrahLifecycle::Identity(A)==SourceIds[0] && VrgCrahLifecycle::Identity(B)==SourceIds[1]
		&& First->VolumeFlowM3s!=A->VolumeFlowM3s && A->PreservesAutomaticAirBounds() && !B->bConvertedAutomaticCrah);
	First->ApplyLayoutSettings(Configs[0]);
	auto* Second=VrgCrahLifecycle::Find(W,CloneIds[1]);
	const auto RemoveHandler=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Actor==First) Second->SetRole(ROLE_SimulatedProxy);
	}));
	TestFalse(TEXT("Second removal failure rolls back whole Undo"),C->TryUndoLayout()); W->RemoveOnActorDestroyedHandler(RemoveHandler);
	TestTrue(TEXT("Failed Undo preserves clones/selection/stacks and source inventory"),Count()==4 && C->GetSelectedLayoutActors().Num()==2
		&& C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0 && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.IsEmpty());
	TestNull(TEXT("Repeated confirm adds none"),C->ConfirmCrahAddition());
	TestTrue(TEXT("Whole Undo"),C->TryUndoLayout() && Count()==2);
	Block=W->SpawnActor<ACoolingDistributionUnitActor>(Preview[1].GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("Second destination blocks whole Redo"),C->TryRedoLayout());
	TestTrue(TEXT("Redo rejection maintains inventory/stacks"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1); Block->Destroy(false,false);
	const auto RedoHandler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Actor->GetFName()==CloneNames[1]) Actor->Destroy(false,false);
	}));
	TestFalse(TEXT("Second restoration failure rolls back first"),C->TryRedoLayout()); W->RemoveOnActorSpawnedHandler(RedoHandler);
	TestTrue(TEXT("Failed Redo preserves inventory/stacks/deletion intent"),Count()==2 && C->GetLayoutUndoCount()==0
		&& C->GetLayoutRedoCount()==1 && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.IsEmpty());
	TestNull(TEXT("Failed second restore name free without GC"),FindObjectFast<UObject>(W->PersistentLevel,CloneNames[1]));
	TestTrue(TEXT("Whole Redo"),C->TryRedoLayout()); TestEqual(TEXT("Redo count"),Count(),4);
	for(int32 J=0;J<2;++J) { const auto* Clone=VrgCrahLifecycle::Find(W,CloneIds[J]);
		TestTrue(TEXT("Redo stable identity/config/transform"),Clone && Clone->GetFName()==CloneNames[J] && Clone->CaptureLayoutSettings()==Configs[J] && Clone->GetActorTransform().Equals(Preview[J],1e-9)); }
	TestTrue(TEXT("Undo cleanup"),C->TryUndoLayout()); Both(); C->BeginCrahDuplication(); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels whole preview"),C->IsAddingRack()); C->SetPatchMapOpen(false); Both(); C->BeginCrahDuplication();
	Sim->EnterSimulationMode(); C->RefreshInteractionInputMode(); TestFalse(TEXT("Mode cancels whole preview"),C->IsAddingRack()); Sim->EnterLayoutMode();
	TestEqual(TEXT("Canceled count"),Count(),2); TestEqual(TEXT("Layout clock"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("Crah batch duplicate: guards/Ctrl+D/value previews, anchor5cm, full transforms/source invariance, total1cm group collision, R blocked/Esc/mode/patch cancel, mid-spawn/remove/restore rollback, unique IDs/no copied thermal state, one Undo/Redo; Equals1e-9 actor cm boundary."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
