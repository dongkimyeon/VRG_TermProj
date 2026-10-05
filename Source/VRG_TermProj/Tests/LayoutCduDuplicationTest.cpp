#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCduLayoutState.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "InputKeyEventArgs.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduDuplicationTest,"VRG.Interaction.Layout.CduDuplicationUndoRedo",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduDuplicationTest::RunTest(const FString& Parameters)
{
 UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 auto* Source=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(300,0,25),FRotator(0,90,0));
 auto* Other=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(700,0,0),FRotator::ZeroRotator);
 Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
 auto Select=[&](AActor* A,bool Toggle=false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
 auto Count=[&]() { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(W);It;++It) ++N; return N; };
 auto Refused=[&]() {
  const int32 N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
  TestFalse(TEXT("CDU duplication blocked"),C->BeginCduDuplication());
  TestTrue(TEXT("Rejected input keeps actors/history"),Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R);
 };
 Refused(); Select(Rack); Refused(); Select(Source); Select(Rack,true); Refused(); Select(Source); Select(Other,true); Refused();
 Select(Source); C->BeginLayoutDragAlongRay(Source->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
 C->BeginLayoutMarquee(FVector2D(0,0),false); Refused(); C->EndLayoutMarquee(false);
 C->BeginCduAddition(); Refused(); C->CancelRackAddition(); Select(Source);
 C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false); Select(Source);
 Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Select(Source);
 auto Key=[&](FKey Code,EInputEvent Event) { FInputKeyEventArgs A; A.Key=Code; A.Event=Event; A.AmountDepressed=1; C->InputKey(A); };
 for(int32 Case=0;Case<2;++Case)
 {
  if(Case==1) {
   auto Config=GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings();
   // Synthetic authored values/transform for copy isolation; not new hardware defaults or measured parameters.
   Config.PumpSpeedRatio=.8; Config.PumpEfficiency=.65; Config.SizeM*=1.1; Config.bUseReferenceModel=false;
   Config.FacilitySupplyTempK+=2; Config.FacilityMassFlowKgs=3; Config.HxUAWK=5000;
   Config.ConnectionHeightM=.3; Config.ConnectionSpacingM=.4;
   Source=VrgCduLifecycle::Spawn(W,TEXT("RuntimeCduCopySource"),FGuid::NewGuid().ToString(),FTransform(FRotator(0,90,0),FVector(1000,0,25),FVector(.9,1.1,1.2)),Config);
  }
  const auto Config=Source->CaptureLayoutSettings(); const FTransform Original=Source->GetActorTransform();
  const FString SourceId=VrgCduLifecycle::Identity(Source); const FName SourceName=Source->GetFName();
  const int32 Before=Count(); C->ClearLayoutHistory(); Select(Source);
  Key(EKeys::LeftControl,IE_Pressed); Key(EKeys::D,IE_Pressed); Key(EKeys::D,IE_Released); Key(EKeys::LeftControl,IE_Released);
  TestTrue(TEXT("Ctrl+D event starts value-only CDU preview"),C->IsAddingRack() && C->GetRackAdditionPreviewCount()==1 && Count()==Before);
  Refused(); C->UpdateRackAdditionAlongRay(Original.GetLocation()+FVector(0,0,500),Down);
  TestTrue(TEXT("Original overlap blocked using copied dimensions"),C->IsRackAdditionBlocked());
  TestNull(TEXT("Overlap never creates clone"),C->ConfirmCduAddition());
  TestTrue(TEXT("Overlap keeps original and empty history"),Count()==Before && C->GetLayoutUndoCount()==0 && C->IsAddingRack());
  C->UpdateRackAdditionAlongRay(FVector(1602+Case*500,83,500),Down);
  FTransform Preview; FVector Size; TestTrue(TEXT("Valid copied preview"),C->GetRackAdditionPreview(Preview,Size));
  TestTrue(TEXT("5cm XY snap, original height/rotation/scale/size"),Preview.GetLocation().Equals(FVector(1600+Case*500,85,25),1e-9)
   && Preview.GetRotation().Equals(Original.GetRotation(),1e-9) && Preview.GetScale3D().Equals(Original.GetScale3D(),1e-9) && Size==Config.SizeM);
  C->OnResetSim(); C->GetRackAdditionPreview(Preview,Size);
  TestTrue(TEXT("R rotates preview only"),Preview.GetRotation().Equals(FRotator(0,90,0).Quaternion()*Original.GetRotation(),1e-9) && Source->GetActorTransform().Equals(Original,0));
  C->CancelRackAddition(); TestNull(TEXT("Esc cancel cannot confirm"),C->ConfirmCduAddition());
  TestTrue(TEXT("Cancel preserves count/history"),Count()==Before && C->GetLayoutUndoCount()==0);
  Select(Source); C->BeginCduDuplication(); C->UpdateRackAdditionAlongRay(FVector(1602+Case*500,83,500),Down);
  C->OnResetSim(); C->GetRackAdditionPreview(Preview,Size);
  const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor) {
   if(Cast<ACoolingDistributionUnitActor>(Actor) && Actor->GetName().StartsWith(TEXT("DuplicatedCdu_"))) Actor->Destroy(false,false);
  }));
  TestNull(TEXT("Actual failed spawn rejects confirmation"),C->ConfirmCduAddition()); W->RemoveOnActorSpawnedHandler(Handler);
  TestTrue(TEXT("Failed confirmation preserves source, selection, preview and history"),Count()==Before && C->GetSelectedLayoutActor()==Source
   && C->IsAddingRack() && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0 && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.IsEmpty());
  auto* Clone=C->ConfirmCduAddition();
  if(!TestNotNull(TEXT("One clone confirmed"),Clone)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
  const FString Id=VrgCduLifecycle::Identity(Clone); const FName Name=Clone->GetFName();
  TestTrue(TEXT("Independent runtime identity and one record"),Count()==Before+1 && C->GetLayoutUndoCount()==1
   && Clone->bRuntimeLayoutCdu && Id!=SourceId && Name!=SourceName);
  TestTrue(TEXT("Settings/transform copied; original unchanged"),Clone->CaptureLayoutSettings()==Config && Clone->GetActorTransform().Equals(Preview,1e-9)
   && Source->CaptureLayoutSettings()==Config && Source->GetActorTransform().Equals(Original,0) && VrgCduLifecycle::Identity(Source)==SourceId);
  Clone->PumpSpeedRatio+=.05; TestEqual(TEXT("Copied settings are independent"),Source->PumpSpeedRatio,Config.PumpSpeedRatio); Clone->ApplyLayoutSettings(Config);
  TestNull(TEXT("Repeated confirmation creates none"),C->ConfirmCduAddition());
  TestTrue(TEXT("Undo removes clone only"),C->TryUndoLayout() && Count()==Before && IsValid(Source));
  auto* Block=W->SpawnActor<ACrahUnitActor>(Preview.GetLocation(),FRotator::ZeroRotator);
  TestFalse(TEXT("Redo collision rejects"),C->TryRedoLayout()); Block->Destroy(false,false);
  TestTrue(TEXT("Collision preserves stacks"),Count()==Before && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
  FActorSpawnParameters P; P.Name=Name; auto* Occupant=W->SpawnActor<AActor>(P);
  TestFalse(TEXT("Redo occupied name rejects"),C->TryRedoLayout());
  Occupant->Rename(*MakeUniqueObjectName(W->PersistentLevel,AActor::StaticClass(),TEXT("RetiredCduCopyOccupant")).ToString()); Occupant->Destroy();
  TestTrue(TEXT("Name failure preserves inventory/history"),Count()==Before && C->GetLayoutRedoCount()==1);
  const auto FailRedo=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
   if(Actor->GetFName()==Name) Actor->Destroy(false,false);
  }));
  TestFalse(TEXT("Actual failed Redo spawn"),C->TryRedoLayout()); W->RemoveOnActorSpawnedHandler(FailRedo);
  TestTrue(TEXT("Failed restore keeps history and actors"),Count()==Before && C->GetLayoutRedoCount()==1 && C->GetLayoutUndoCount()==0);
  TestNull(TEXT("Failed deferred actor releases restore name without GC"),FindObjectFast<UObject>(W->PersistentLevel,Name));
  TestTrue(TEXT("Redo creates same clone"),C->TryRedoLayout()); Clone=VrgCduLifecycle::Find(W,Id);
  TestTrue(TEXT("ID/name/config/transform restored"),Clone && Clone->GetFName()==Name && Clone->CaptureLayoutSettings()==Config && Clone->GetActorTransform().Equals(Preview,1e-9));
  TestTrue(TEXT("Undo clone before cancel cases"),C->TryUndoLayout()); C->ClearLayoutHistory(); Select(Source);
  C->BeginCduDuplication(); C->SetPatchMapOpen(true); TestFalse(TEXT("Patch cancels preview"),C->IsAddingRack()); C->SetPatchMapOpen(false);
  Select(Source); C->BeginCduDuplication(); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
  TestFalse(TEXT("Mode switch cancels preview"),C->IsAddingRack()); Sim->EnterLayoutMode();
  TestEqual(TEXT("Cancel never creates clone"),Count(),Before);
 }
 TestEqual(TEXT("Layout/snapshot never imports simulation time"),Sim->GetSnapshot().SimTimeS,0.0);
 AddInfo(TEXT("CDU duplicate: Ctrl+D guards; placed/default and runtime/synthetic config; value-only 5cm/1cm/R/Esc/patch/mode; original unchanged, independent ID/settings; one Undo/Redo; collision/name/actual spawn failure invariants. Transform Equals1e-9 at actor cm boundary, scale dimensionless."));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
