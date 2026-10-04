#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackGroupDuplicationTest,"VRG.Interaction.Layout.RackGroupDuplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackGroupDuplicationTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>();
	auto* C=W->SpawnActor<ADataCenterDebugController>(); Sim->EnterLayoutMode();
	auto* A=W->SpawnActor<ARackThermalActor>(FVector(0,0,7),FRotator(0,17,0)); A->ApplyGb200Nvl72Preset();
	C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,500,500),FVector(0,0,-1));
	auto* B=C->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Runtime source"),B)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	B->SetActorLocation(FVector(0,500,13)); B->SetActorRotation(FRotator(0,31,0));
	A->SetActorScale3D(FVector(.8,1.2,1)); B->SetActorScale3D(FVector(1.2,.8,1.1));
	// Synthetic configuration/live state used to detect accidental copying, not hardware defaults.
	A->Input.Utilization=.63; B->Input.Utilization=.42; B->Input.SledDocked[2]=false;
	A->bDrivenExternally=true; A->SetExternalSimTimeS(42); A->GetMutableModel().Step(A->Input,.02);
	const FRackLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	const FTransform Original[]={A->GetActorTransform(),B->GetActorTransform()};
	const auto OriginalState=A->GetSnapshot(); const double OriginalEnergy=A->GetMutableModel().GetEnergyInJ();
	const FString SourceIds[]={VrgRackLifecycle::Identity(A),VrgRackLifecycle::Identity(B)};
	auto Count=[&]() { int32 N=0; for(TActorIterator<ARackThermalActor> It(W);It;++It) ++N; return N; };
	auto Select=[&](AActor* Actor,bool bToggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1),bToggle); };
	auto Key=[&](FKey Code,EInputEvent Event) { FInputKeyEventArgs Args; Args.Key=Code; Args.Event=Event; Args.AmountDepressed=1; C->InputKey(Args); };
	C->ClearLayoutHistory(); Select(A); Select(B,true);
	auto* Block=W->SpawnActor<ACrahUnitActor>(FVector(3000,3000,0),FRotator::ZeroRotator);
	Select(Block,true); TestFalse(TEXT("Mixed selection rejects whole duplicate"),C->BeginRackDuplication());
	TestTrue(TEXT("Mixed rejection leaves world/history/selection"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetSelectedLayoutActors().Num()==3);
	Select(A); Select(B,true);
	Key(EKeys::LeftControl,IE_Pressed); Key(EKeys::D,IE_Pressed); Key(EKeys::D,IE_Released); Key(EKeys::LeftControl,IE_Released);
	TestEqual(TEXT("Short Ctrl+D starts whole group preview"),C->GetRackAdditionPreviewCount(),2);
	TestFalse(TEXT("Active group preview rejects another Ctrl+D"),C->BeginRackDuplication());
	C->UpdateRackAdditionAlongRay(FVector(0,0,500),FVector(0,0,-1));
	TestTrue(TEXT("Sources participate in collision check"),C->IsRackAdditionBlocked());
	TestNull(TEXT("Original overlap refuses whole confirmation"),C->ConfirmRackAddition());
	TestTrue(TEXT("Overlap leaves count/history"),Count()==2 && C->GetLayoutUndoCount()==0);
	C->UpdateRackAdditionAlongRay(FVector(-402,2,500),FVector(0,0,-1));
	FTransform Preview[2]; FVector Size;
	for(int32 I=0;I<2;++I) TestTrue(TEXT("Each member has a preview box"),C->GetRackAdditionPreview(Preview[I],Size,I));
	TestFalse(TEXT("Group preview R is blocked"),C->TryRotateRackAddition());
	double MaxError=0;
	for(int32 I=0;I<2;++I) {
		FTransform Expected=Original[I]; Expected.AddToTranslation(FVector(-400,0,0));
		TestTrue(TEXT("Anchor snap and full relative transforms retained"),Preview[I].Equals(Expected,1e-9));
		MaxError=FMath::Max(MaxError,FVector::Distance(Preview[I].GetLocation(),Expected.GetLocation()));
		FTransform Unchanged; C->GetRackAdditionPreview(Unchanged,Size,I);
		TestTrue(TEXT("Blocked rotation leaves each preview unchanged"),Unchanged.Equals(Preview[I],0));
	}
	Block->SetActorLocation(Preview[1].GetLocation());
	TestTrue(TEXT("Second destination collision blocks entire preview"),C->IsRackAdditionBlocked());
	TestNull(TEXT("Second collision cannot create first rack"),C->ConfirmRackAddition());
	TestEqual(TEXT("No partial creation"),Count(),2); Block->SetActorLocation(FVector(3000,3000,0));
	Key(EKeys::Escape,IE_Pressed); C->CancelRackAddition(); // Direct cancellation path, no EnhancedInput viewport in this fixture.
	TestTrue(TEXT("Cancel leaves count/history"),Count()==2 && C->GetLayoutUndoCount()==0 && !C->IsAddingRack());
	C->BeginRackDuplication(); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch entry cancels group preview"),C->IsAddingRack()); C->SetPatchMapOpen(false);
	C->BeginRackDuplication(); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestFalse(TEXT("Mode transition cancels group preview"),C->IsAddingRack());
	Sim->EnterLayoutMode(); Select(A); Select(B,true);
	// Reset original live state after mode transition and prove confirmation doesn't touch it.
	A->SetExternalSimTimeS(42); A->GetMutableModel().Step(A->Input,.02);
	const auto BeforeConfirm=A->GetSnapshot(); const double BeforeEnergy=A->GetMutableModel().GetEnergyInJ();
	C->BeginRackDuplication(); C->UpdateRackAdditionAlongRay(FVector(-400,0,500),FVector(0,0,-1));
	int32 Spawned=0;
	const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if (Cast<ARackThermalActor>(Actor) && ++Spawned==2) Actor->Destroy(false,false);
	}));
	TestNull(TEXT("Second deferred actor destruction rolls back first creation"),C->ConfirmRackAddition());
	W->RemoveOnActorSpawnedHandler(Handler);
	TestTrue(TEXT("Spawn failure preserves original selection/history and preview"),Count()==2 && C->GetSelectedLayoutActors().Num()==2
		&& C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0 && C->IsAddingRack());
	TestNotNull(TEXT("Confirm group after removing failure cause"),C->ConfirmRackAddition());
	TestTrue(TEXT("Two clones, one history record, selected together"),Count()==4 && C->GetLayoutUndoCount()==1 && C->GetSelectedLayoutActors().Num()==2);
	TArray<FString> CloneIds; TArray<FTransform> CloneTransforms;
	for(int32 I=0;I<C->GetSelectedLayoutActors().Num();++I) {
		auto* Clone=CastChecked<ARackThermalActor>(C->GetSelectedLayoutActors()[I]);
		const auto Settings=Clone->CaptureLayoutSettings();
		TestTrue(TEXT("Each source settings copied independently"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Settings,&Configs[I],0));
		const FString Id=VrgRackLifecycle::Identity(Clone);
		TestTrue(TEXT("Independent IDs"),Id!=SourceIds[0] && Id!=SourceIds[1] && !CloneIds.Contains(Id));
		CloneIds.Add(Id); CloneTransforms.Add(Clone->GetActorTransform());
		FRackThermalModel Fresh; Fresh.Initialize(Configs[I].Params,FRackThermalState());
		TestTrue(TEXT("Fresh temperatures/clock/integral ledger"),Clone->GetSnapshot().SimTimeS==0 && Clone->GetMutableModel().GetEnergyInJ()==0
			&& Clone->GetMutableModel().GetState().ComponentTempK==Fresh.GetState().ComponentTempK);
		Clone->Input.ComponentHeatW[0]+=11; auto* Source=I==0 ? A:B;
		TestEqual(TEXT("No shared array storage"),Source->Input.ComponentHeatW[0],Configs[I].Input.ComponentHeatW[0]);
		Clone->ApplyLayoutSettings(Configs[I]);
	}
	TestTrue(TEXT("Original transforms/live state/clock/ledger untouched"),A->GetActorTransform().Equals(Original[0],0) && B->GetActorTransform().Equals(Original[1],0)
		&& A->GetSnapshot().ComponentTempK==BeforeConfirm.ComponentTempK && A->GetSnapshot().SimTimeS==BeforeConfirm.SimTimeS && A->GetMutableModel().GetEnergyInJ()==BeforeEnergy);
	TestTrue(TEXT("One Undo removes both clones"),C->TryUndoLayout()); TestEqual(TEXT("Originals remain"),Count(),2);
	Block->SetActorLocation(CloneTransforms[1].GetLocation());
	TestFalse(TEXT("Second restoration collision rejects whole Redo"),C->TryRedoLayout());
	TestTrue(TEXT("Redo rejection retains count/stacks"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
	Block->SetActorLocation(FVector(3000,3000,0)); TestTrue(TEXT("One Redo restores both clones"),C->TryRedoLayout());
	for(int32 I=0;I<CloneIds.Num();++I) {
		auto* Clone=VrgRackLifecycle::Find(W,CloneIds[I]);
		if(TestNotNull(TEXT("Clone identity restored"),Clone)) TestTrue(TEXT("Transform restored"),Clone->GetActorTransform().Equals(CloneTransforms[I],1e-9));
	}
	Select(A); C->BeginRackDuplication(); TestTrue(TEXT("Single preview R retained"),C->TryRotateRackAddition()); C->CancelRackAddition();
	AddInfo(FString::Printf(TEXT("Group duplication: preview error %.3e cm | counts 2->4->2->4, one command | mixed/second collision/spawn failure atomic | source initial live %.2f s %.3f J; clone clocks/ledgers 0."),MaxError,OriginalState.SimTimeS,OriginalEnergy));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
