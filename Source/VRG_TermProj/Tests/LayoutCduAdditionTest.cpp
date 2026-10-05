#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduAdditionTest,"VRG.Interaction.Layout.CduAdditionUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduAdditionTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>();
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Rack=W->SpawnActor<ARackThermalActor>();
	Sim->EnterLayoutMode();
	const FVector Down(0,0,-1);
	auto Preview=[&](FVector P) { C->UpdateRackAdditionAlongRay(P+FVector(0,0,500),Down); };
	auto Count=[&]() { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(W);It;++It) ++N; return N; };
	C->BeginCduAddition(); Preview(FVector(0,0,0));
	TestTrue(TEXT("Overlap marked blocked"),C->IsRackAdditionBlocked());
	TestNull(TEXT("Overlap never creates CDU"),C->ConfirmCduAddition());
	TestEqual(TEXT("Value-only preview count"),Count(),0);
	TestEqual(TEXT("Rejected addition no history"),C->GetLayoutUndoCount(),0);
	FTransform T; FVector SizeM;
	C->GetRackAdditionPreview(T,SizeM);
	TestTrue(TEXT("Total 1cm clearance boundary reused"),C->IsLayoutBoxBlocked(W,SizeM,FTransform(FVector(0,(SizeM.Y+Rack->RackSizeM.Y)*50.0+.99,0)))
		&& !C->IsLayoutBoxBlocked(W,SizeM,FTransform(FVector(0,(SizeM.Y+Rack->RackSizeM.Y)*50.0+1,0))));
	Preview(FVector(302,83,0)); C->GetRackAdditionPreview(T,SizeM);
	TestTrue(TEXT("5cm snap"),T.GetLocation().Equals(FVector(300,85,0),1e-9));
	C->OnResetSim(); C->GetRackAdditionPreview(T,SizeM);
	TestTrue(TEXT("R rotates only preview"),FMath::Abs(T.Rotator().Yaw-90)<1e-10 && Rack->GetActorRotation().IsZero());
	C->CancelRackAddition(); TestNull(TEXT("Esc shared cancel prevents confirm"),C->ConfirmCduAddition());
	C->BeginCduAddition(); Preview(FVector(300,85,0)); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels shared preview"),C->IsAddingRack());
	C->BeginCduAddition(); TestFalse(TEXT("Patch blocks button"),C->IsAddingRack()); C->SetPatchMapOpen(false);
	C->BeginCduAddition(); Preview(FVector(300,85,0)); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestFalse(TEXT("Mode switch cancels preview"),C->IsAddingRack());
	C->BeginCduAddition(); TestFalse(TEXT("Simulation blocks palette"),C->IsAddingRack()); Sim->EnterLayoutMode();
	C->BeginCduAddition(); Preview(FVector(300,85,0)); C->OnResetSim();
	auto* Added=C->ConfirmCduAddition();
	if (!TestNotNull(TEXT("One CDU created"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	const FString Id=VrgCduLifecycle::Identity(Added); const FName Name=Added->GetFName();
	const FTransform Initial=Added->GetActorTransform(); const auto Config=Added->CaptureLayoutSettings();
	TestTrue(TEXT("Code defaults and independent runtime identity"),Config==GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings() && Config.IsValid() && Added->bRuntimeLayoutCdu && !Id.IsEmpty());
	TestNull(TEXT("Repeated click creates none"),C->ConfirmCduAddition()); TestEqual(TEXT("One creation record"),C->GetLayoutUndoCount(),1);
	TestEqual(TEXT("Only one CDU"),Count(),1);
	TestFalse(TEXT("CDU deletion stays excluded"),C->TryDeleteLayoutRack());
	TestFalse(TEXT("CDU duplication stays excluded"),C->BeginRackDuplication());
	TestTrue(TEXT("Undo removes palette CDU"),C->TryUndoLayout()); TestEqual(TEXT("Undo count zero actors"),Count(),0);
	auto* Block=W->SpawnActor<ACrahUnitActor>(Initial.GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("Redo collision refuses without stack loss"),C->TryRedoLayout());
	TestTrue(TEXT("Rejected Redo state invariant"),Count()==0 && C->GetLayoutRedoCount()==1 && C->GetLayoutUndoCount()==0);
	Block->Destroy(false,false);
 FActorSpawnParameters Occupancy; Occupancy.Name=Name; auto* Occupant=W->SpawnActor<AActor>(Occupancy);
 TestFalse(TEXT("Redo occupied name refuses without history loss"),C->TryRedoLayout());
 TestTrue(TEXT("Occupied name leaves state and record intact"),Count()==0 && C->GetLayoutRedoCount()==1);
 Occupant->Rename(*MakeUniqueObjectName(W->PersistentLevel,AActor::StaticClass(),TEXT("RetiredTestOccupant")).ToString()); Occupant->Destroy();
	TestTrue(TEXT("Redo creates fresh CDU"),C->TryRedoLayout()); Added=VrgCduLifecycle::Find(W,Id);
	TestTrue(TEXT("ID/name/config/transform restored; live state cleared"),Added && Added->GetFName()==Name
		&& Added->CaptureLayoutSettings()==Config && Added->GetActorTransform().Equals(Initial,1e-9));
	// A move and R after addition remain reachable after the addition itself is undone/recreated.
	C->SelectLayoutAlongRay(Added->GetActorLocation()+FVector(0,0,500),Down);
	C->BeginLayoutDragAlongRay(Added->GetActorLocation()+FVector(0,0,500),Down);
	C->UpdateLayoutDragAlongRay(Added->GetActorLocation()+FVector(200,0,500),Down,true); C->EndLayoutDrag(true);
	C->OnResetSim(); const FTransform Final=Added->GetActorTransform();
	TestTrue(TEXT("Undo R/move/add"),C->TryUndoLayout() && C->TryUndoLayout() && C->TryUndoLayout());
	TestTrue(TEXT("Redo add/move/R rebinds references"),C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout());
	Added=VrgCduLifecycle::Find(W,Id);
	TestTrue(TEXT("Final transform restored"),Added && Added->GetActorTransform().Equals(Final,1e-9));
	TestEqual(TEXT("Layout clock never advances"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("CDU preview: no actor before click, 5cm snap/1cm clearance, R/Esc/patch/mode, one identity-preserving Undo/Redo, collision stack invariant, move/R reference reconnection and fresh coupling state passed."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
