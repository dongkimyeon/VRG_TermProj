#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahDeletionTest,"VRG.Interaction.Layout.CrahDeletionUndoRedo",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahDeletionTest::RunTest(const FString& Parameters)
{
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 auto* Placed=W->SpawnActor<ACrahUnitActor>(FVector(300,0,20),FRotator::ZeroRotator);
 auto* Other=W->SpawnActor<ACrahUnitActor>(FVector(700,0,0),FRotator::ZeroRotator);
 Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
 auto Select=[&](AActor* A,bool Toggle=false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
 auto Count=[&]() {int N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N;};
 auto Refused=[&]() {const int N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
  TestFalse(TEXT("Deletion guarded"),C->TryDeleteLayoutCrah());
  TestTrue(TEXT("Refusal leaves counts/history"),Count()==N && U==C->GetLayoutUndoCount() && R==C->GetLayoutRedoCount());};
 Refused(); Select(Rack); Refused(); Select(Placed); Select(Other,true); Refused(); Select(Rack,true); Refused();
 Select(Placed); C->BeginLayoutDragAlongRay(Placed->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
 C->BeginLayoutMarquee(FVector2D(0,0),false); Refused(); C->EndLayoutMarquee(false);
 C->BeginInRowCrahAddition(); Refused(); C->CancelRackAddition(); Select(Placed);
 C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false); Select(Placed);
 Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Select(Placed);
 // Synthetic settings/scale/positions to exercise snapshots, not new hardware defaults.
 Placed->VolumeFlowM3s=.3; Placed->SetActorScale3D(FVector(.9,1.1,1));
 const auto Config=Placed->CaptureLayoutSettings(); const auto Initial=Placed->GetActorTransform();
 const auto Id=VrgCrahLifecycle::Identity(Placed); const auto Name=Placed->GetFName();
 Select(Placed); C->BeginLayoutDragAlongRay(Placed->GetActorLocation()+FVector(0,0,500),Down);
 C->UpdateLayoutDragAlongRay(Placed->GetActorLocation()+FVector(0,200,500),Down,true); C->EndLayoutDrag(true);
 C->OnResetSim(); const auto Final=Placed->GetActorTransform();
 TestTrue(TEXT("Placed delete records one command"),C->TryDeleteLayoutCrah() && Count()==1 && C->GetLayoutUndoCount()==3);
 TestTrue(TEXT("Selection/reference and persistent deletion intent"),C->GetSelectedLayoutActors().IsEmpty()
  && !VrgCrahLifecycle::Find(W,Id) && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Contains(Id));
 auto* Block=W->SpawnActor<ACrahUnitActor>(Final.GetLocation(),FRotator::ZeroRotator);
 TestFalse(TEXT("Overlap refuses Undo"),C->TryUndoLayout()); Block->Destroy(false,false);
 FActorSpawnParameters P; P.Name=Name; auto* Occupant=W->SpawnActor<AActor>(P);
 TestFalse(TEXT("Name occupancy refuses Undo"),C->TryUndoLayout());
 Occupant->Rename(*MakeUniqueObjectName(W->PersistentLevel,AActor::StaticClass(),TEXT("RetiredCrahTest")).ToString()); Occupant->Destroy();
 const auto Fail=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* A){if(Cast<ACrahUnitActor>(A)) A->Destroy(false,false);}));
 TestFalse(TEXT("Failed creation retains delete state/history"),C->TryUndoLayout()); W->RemoveOnActorSpawnedHandler(Fail);
 TestTrue(TEXT("Rejected Undo leaves intent/count/history"),Count()==1 && C->GetLayoutUndoCount()==3 && C->GetLayoutRedoCount()==0
  && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Contains(Id));
 TestTrue(TEXT("Undo delete"),C->TryUndoLayout()); auto* Restored=VrgCrahLifecycle::Find(W,Id);
 if(TestNotNull(TEXT("Restored"),Restored)) {
  TestTrue(TEXT("Identity/settings/provenance/full transform"),Restored->GetFName()==Name && !Restored->bRuntimeLayoutCrah
   && Restored->CaptureLayoutSettings()==Config && Restored->GetActorTransform().Equals(Final,1e-9));
  TestTrue(TEXT("Earlier move/R reference rebound"),C->TryUndoLayout() && C->TryUndoLayout() && Restored->GetActorTransform().Equals(Initial,1e-9));
  TestTrue(TEXT("Redo move/R/delete"),C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout() && Count()==1);
 }
 C->ClearLayoutHistory();
 FCrahLayoutRecord R{TEXT("runtime-delete-test"),TEXT("RuntimeCrahDelete"),FTransform(FVector(300,0,0)),Other->CaptureLayoutSettings(),true,true};
 auto* Runtime=VrgCrahLifecycle::Spawn(W,R);
 if(TestNotNull(TEXT("Runtime converted cooler"),Runtime)) {
  const auto Baseline=Runtime->AutomaticLayoutTransform; const auto Size=Runtime->AutomaticLayoutSizeM;
  // Original automatic contact is synthetic but uses the exact actor box, without modifying collision rules.
  Rack->SetActorLocation(FVector(300-(Rack->RackSizeM.X+Runtime->SizeM.X)*50,0,0));
  Select(Runtime); TestTrue(TEXT("Delete converted original contact"),C->TryDeleteLayoutCrah());
  TestTrue(TEXT("Undo preserves unchanged original contact"),C->TryUndoLayout()); Runtime=VrgCrahLifecycle::Find(W,R.Id);
  if(TestNotNull(TEXT("Restored converted"),Runtime)) {
   TestTrue(TEXT("Original boundary/provenance retained"),Runtime->bRuntimeLayoutCrah && Runtime->bConvertedAutomaticCrah
    && Runtime->AutomaticLayoutTransform.Equals(Baseline,1e-9) && Runtime->AutomaticLayoutSizeM.Equals(Size,1e-9));
   Runtime->VolumeFlowM3s+=.1; TestFalse(TEXT("Changed settings blocks Redo"),C->TryRedoLayout());
   Runtime->VolumeFlowM3s=R.Settings.VolumeFlowM3s;
   Runtime->SetRole(ROLE_SimulatedProxy); TestFalse(TEXT("Removal failure leaves history"),C->TryRedoLayout()); Runtime->SetRole(ROLE_Authority);
   TestTrue(TEXT("Redo converted deletion"),C->TryRedoLayout());
   Rack->AddActorWorldOffset(FVector(.5,0,0)); TestFalse(TEXT("Changed contacting neighbour blocks Undo"),C->TryUndoLayout());
  }
 }
 TestEqual(TEXT("Layout clock remains zero"),Sim->GetSnapshot().SimTimeS,0.0);
 AddInfo(TEXT("CRAH single placed/runtime/converted delete, guards, history/ref restoration, overlap/name/spawn/remove refusal, original contact and boundary preservation; no live state snapshot."));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
