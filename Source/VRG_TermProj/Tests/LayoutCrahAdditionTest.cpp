#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahAdditionTest,"VRG.Interaction.Layout.CrahAdditionUndoRedo",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahAdditionTest::RunTest(const FString& Parameters)
{
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>();
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator);
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); for(int I=0;I<25;++I) Sim->Tick(.04f);
 const auto Warm=Sim->GetSnapshot(); Sim->EnterLayoutMode();
 TArray<FCrahLayoutRecord> Automatic; Sim->CaptureAutomaticCrahLayout(Automatic);
 if(!TestTrue(TEXT("Automatic fixture has at least two units"),Automatic.Num()>=2)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
 const int32 AutoCount=Automatic.Num(); const FVector Down(0,0,-1);
 auto Count=[&]() { int32 N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N; };
 auto Preview=[&](FVector P) { C->UpdateRackAdditionAlongRay(P+FVector(0,0,500),Down); };
 auto WarmUnchanged=[&]() { const auto S=Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyStoredJ==Warm.EnergyStoredJ; };
 C->BeginInRowCrahAddition(); Preview(Automatic[0].Transform.GetLocation());
 TestTrue(TEXT("Automatic virtual box blocks new cooler"),C->IsRackAdditionBlocked());
 TestNull(TEXT("Virtual overlap never converts or creates"),C->ConfirmCrahAddition());
 // Synthetic distances at the shared total-clearance boundary, not equipment dimensions/defaults.
 C->PlaceSnapM=0;
 Preview(Automatic[0].Transform.GetLocation()-FVector(0,(Automatic[0].Settings.SizeM.Y+GetDefault<ACrahUnitActor>()->SizeM.Y)*50+.99,0));
 TestTrue(TEXT("Virtual cooler total clearance below 1cm rejects"),C->IsRackAdditionBlocked());
 Preview(Automatic[0].Transform.GetLocation()-FVector(0,(Automatic[0].Settings.SizeM.Y+GetDefault<ACrahUnitActor>()->SizeM.Y)*50+1.01,0));
 TestFalse(TEXT("Virtual cooler clearance above 1cm accepts"),C->IsRackAdditionBlocked()); C->PlaceSnapM=.05;
 Preview(FVector::ZeroVector); TestTrue(TEXT("Rack blocks destination"),C->IsRackAdditionBlocked());
 TestTrue(TEXT("Preview/rejection keeps automatic state and warm ledger/history"),Count()==0 && C->GetLayoutUndoCount()==0 && WarmUnchanged());
 Preview(FVector(182,83,0)); FTransform T; FVector Size;
 TestTrue(TEXT("Value preview uses actor defaults and 5cm snap"),C->GetRackAdditionPreview(T,Size) && Size==GetDefault<ACrahUnitActor>()->SizeM
  && T.GetLocation().Equals(FVector(180,85,0),1e-9));
 C->OnResetSim(); C->GetRackAdditionPreview(T,Size);
 TestTrue(TEXT("R changes only preview"),FMath::Abs(T.Rotator().Yaw-90)<1e-9 && Count()==0);
 C->CancelRackAddition(); TestNull(TEXT("Esc cannot confirm"),C->ConfirmCrahAddition());
 C->BeginInRowCrahAddition(); Preview(FVector(180,0,0)); C->SetPatchMapOpen(true);
 TestFalse(TEXT("Patch cancels and blocks"),C->IsAddingRack()); C->BeginInRowCrahAddition(); TestFalse(TEXT("Patch blocks button"),C->IsAddingRack()); C->SetPatchMapOpen(false);
 C->BeginLayoutMarquee(FVector2D(0,0),false); C->BeginInRowCrahAddition(); TestFalse(TEXT("Marquee blocks button"),C->IsAddingRack()); C->EndLayoutMarquee(false);
 C->BeginLayoutDragAlongRay(FVector(0,0,500),Down); C->BeginInRowCrahAddition(); TestFalse(TEXT("Drag blocks button"),C->IsAddingRack()); C->EndLayoutDrag(false);
 C->BeginInRowCrahAddition(); Preview(FVector(180,0,0)); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
 TestFalse(TEXT("Simulation transition cancels preview"),C->IsAddingRack()); C->BeginInRowCrahAddition(); TestFalse(TEXT("Simulation blocks button"),C->IsAddingRack());
 Sim->EnterLayoutMode(); Sim->CrahLayout=ECrahLayout::CeilingCoil; C->BeginInRowCrahAddition(); TestFalse(TEXT("No implicit ceiling model switch"),C->IsAddingRack()); Sim->CrahLayout=ECrahLayout::InRow;
 // Warm baseline is restored without integrating; failure must not rebuild the shared coupling.
 Sim->EnterSimulationMode(); Sim->Tick(.021f); for(int I=0;I<25;++I) Sim->Tick(.04f); Sim->EnterLayoutMode();
 const auto BeforeFailure=Sim->GetSnapshot();
 C->BeginInRowCrahAddition(); Preview(FVector(180,0,0));
 int32 SpawnNumber=0;
 const auto FailConversion=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* A) {
  if(Cast<ACrahUnitActor>(A) && ++SpawnNumber==2) A->Destroy(false,false);
 }));
 TestNull(TEXT("Mid conversion creation fails whole confirmation"),C->ConfirmCrahAddition()); W->RemoveOnActorSpawnedHandler(FailConversion);
 auto S=Sim->GetSnapshot();
 TestTrue(TEXT("Mid conversion returns automatic state, preview/history/clock/energy"),Count()==0 && C->IsAddingRack() && C->GetLayoutUndoCount()==0
  && S.SimTimeS==BeforeFailure.SimTimeS && S.EnergyInJ==BeforeFailure.EnergyInJ && S.EnergyStoredJ==BeforeFailure.EnergyStoredJ);
 const auto FailAdded=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* A) {
  if(Cast<ACrahUnitActor>(A) && A->GetName().StartsWith(TEXT("AddedCrah_"))) A->Destroy(false,false);
 }));
 TestNull(TEXT("Failure after all conversions rolls back them too"),C->ConfirmCrahAddition()); W->RemoveOnActorSpawnedHandler(FailAdded);
 TestTrue(TEXT("No partial actual units or history"),Count()==0 && C->GetLayoutUndoCount()==0 && C->IsAddingRack());
 auto* Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("One cooler confirmed with existing automatic units"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
 const FString Id=Added->LayoutId; const auto Config=Added->CaptureLayoutSettings(); const FTransform Initial=Added->GetActorTransform();
 TArray<FCrahLayoutRecord> Converted;
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah)
  Converted.Add({It->LayoutId,It->GetFName(),It->GetActorTransform(),It->CaptureLayoutSettings(),true,true});
 TestTrue(TEXT("Entire conversion plus one add is one record; original configs retained"),Count()==AutoCount+1 && Converted.Num()==AutoCount && C->GetLayoutUndoCount()==1
  && Config==GetDefault<ACrahUnitActor>()->CaptureLayoutSettings() && !Added->bConvertedAutomaticCrah && C->GetSelectedLayoutActor()==Added);
 for(const auto& R:Automatic) TestTrue(TEXT("Every original automatic box preserved"),Converted.ContainsByPredicate([&](const auto& M) {return M.Transform.Equals(R.Transform,1e-9) && M.Settings==R.Settings;}));
 TestNull(TEXT("Repeated click creates none"),C->ConfirmCrahAddition());
 auto* First=VrgCrahLifecycle::Find(W,Converted[0].Id); auto* Second=VrgCrahLifecycle::Find(W,Converted[1].Id);
 const auto FailUndo=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* A) { if(A==First) Second->SetRole(ROLE_SimulatedProxy); }));
 TestFalse(TEXT("Mid Undo remove failure rolls back full presence"),C->TryUndoLayout()); W->RemoveOnActorDestroyedHandler(FailUndo);
 TestTrue(TEXT("Failed Undo keeps stack/selection/all units; role restored"),Count()==AutoCount+1 && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0
  && C->GetSelectedLayoutActor()==Added && Second->GetLocalRole()==ROLE_Authority);
 TestTrue(TEXT("Undo removes added unit and entire conversion"),C->TryUndoLayout() && Count()==0);
 auto* Block=W->SpawnActor<ACoolingDistributionUnitActor>(Initial.GetLocation(),FRotator::ZeroRotator);
 TestFalse(TEXT("Redo collision is atomic"),C->TryRedoLayout()); Block->Destroy(false,false);
 TestTrue(TEXT("Collision retains redo record and automatic state"),Count()==0 && C->GetLayoutRedoCount()==1);
 SpawnNumber=0;
 const auto FailRedo=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* A) { if(Cast<ACrahUnitActor>(A) && ++SpawnNumber==2) A->Destroy(false,false); }));
 TestFalse(TEXT("Mid Redo restore failure rolls back all"),C->TryRedoLayout()); W->RemoveOnActorSpawnedHandler(FailRedo);
 TestTrue(TEXT("Mid Redo preserves history and no actual actors"),Count()==0 && C->GetLayoutRedoCount()==1);
 // Even grandfathered automatic contacts are rejected if their original neighbour changes.
 Rack->SetActorLocation(FVector(0,5,0)); TestFalse(TEXT("Changed automatic plan/contact refuses stale Redo"),C->TryRedoLayout()); Rack->SetActorLocation(FVector::ZeroVector);
 TestTrue(TEXT("Redo restores the same IDs/configs/transforms without GC"),C->TryRedoLayout()); Added=VrgCrahLifecycle::Find(W,Id);
 TestTrue(TEXT("Added identity/settings/transform and count restored"),Added && Added->CaptureLayoutSettings()==Config && Added->GetActorTransform().Equals(Initial,1e-9) && Count()==AutoCount+1);
 for(const auto& R:Converted) {auto* A=VrgCrahLifecycle::Find(W,R.Id); TestTrue(TEXT("Converted stable IDs and geometry"),A && A->GetFName()==R.Name && A->PreservesAutomaticAirBounds());}
 C->SelectLayoutAlongRay(Initial.GetLocation()+FVector(0,0,500),Down);
 C->BeginLayoutDragAlongRay(Initial.GetLocation()+FVector(0,0,500),Down);
 C->UpdateLayoutDragAlongRay(Initial.GetLocation()+FVector(200,0,500),Down,true); C->EndLayoutDrag(true); C->OnResetSim();
 const FTransform Final=Added->GetActorTransform();
 TestTrue(TEXT("Undo R/move/add; Redo reconnects cooler transform records"),C->TryUndoLayout() && C->TryUndoLayout() && C->TryUndoLayout()
  && C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout());
 Added=VrgCrahLifecycle::Find(W,Id); TestTrue(TEXT("Transform ref rebound"),Added && Added->GetActorTransform().Equals(Final,1e-9));
 C->BeginInRowCrahAddition(); Preview(FVector(180,180,0)); auto* Next=C->ConfirmCrahAddition();
 TestTrue(TEXT("Already actual world adds only one"),Next && Count()==AutoCount+2);
 TestTrue(TEXT("Actual-world Undo/Redo affects new cooler only"),C->TryUndoLayout() && Count()==AutoCount+1 && C->TryRedoLayout() && Count()==AutoCount+2 && IsValid(Added));
 AddInfo(TEXT("CRAH addition: value-only/virtual collision/5cm/1cm/R/Esc/patch/mode guards; auto conversion+one add transaction; actual-world add only; mid conversion/add/Undo/Redo failure rollback; stable IDs/config/full transforms; previous move/R reference reconnection. No simulation state is in command snapshots."));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
