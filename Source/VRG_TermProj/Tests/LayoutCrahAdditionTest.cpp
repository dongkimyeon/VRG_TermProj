#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahAdditionTest,"VRG.Interaction.Layout.CrahAdditionUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCrahAdditionTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	W->SpawnActor<AAirFieldDebugActor>();
	TArray<ARackThermalActor*> Racks;
	for(int32 I=0;I<4;++I) Racks.Add(W->SpawnActor<ARackThermalActor>(FVector(I*500,0,0),FRotator::ZeroRotator));
	Sim->EnterLayoutMode();
	TArray<FCrahLayoutRecord> Auto; TestTrue(TEXT("Value automatic plan"),Sim->GetAutomaticCrahLayout(Auto));
	TestEqual(TEXT("Four rows plan eight coolers"),Auto.Num(),8);
	auto Count=[&]() { int32 N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N; };
	auto Preview=[&](FVector P) { C->UpdateRackAdditionAlongRay(P+FVector(0,0,500),FVector(0,0,-1)); };
	C->BeginCrahAddition(); Preview(Auto[0].Transform.GetLocation());
	TestTrue(TEXT("Automatic cooler blocks preview before any actor exists"),C->IsRackAdditionBlocked());
	TestNull(TEXT("No overlapping creation"),C->ConfirmCrahAddition());
	Preview(FVector(3002,83,0)); FTransform T; FVector Size; C->GetRackAdditionPreview(T,Size);
	TestTrue(TEXT("Snap and authored dimensions"),T.GetLocation().Equals(FVector(3000,85,0),1e-9) && Size==GetDefault<ACrahUnitActor>()->SizeM);
	C->OnResetSim(); C->GetRackAdditionPreview(T,Size); TestTrue(TEXT("R rotates preview"),FMath::Abs(T.Rotator().Yaw-90)<1e-10);
	C->CancelRackAddition(); TestTrue(TEXT("Cancel keeps automatic mode, count and history"),Count()==0 && C->GetLayoutUndoCount()==0);
	C->BeginCrahAddition(); C->SetPatchMapOpen(true); TestFalse(TEXT("Patch cancels preview"),C->IsAddingRack());
	C->BeginCrahAddition(); TestFalse(TEXT("Patch blocks palette"),C->IsAddingRack()); C->SetPatchMapOpen(false);
	Sim->CrahLayout=ECrahLayout::CeilingCoil; C->BeginCrahAddition(); TestFalse(TEXT("Ceiling comparison mode rejects duct palette"),C->IsAddingRack()); Sim->CrahLayout=ECrahLayout::InRow;
	C->BeginCrahAddition(); Preview(FVector(3000,85,0));
	// Fail during the automatic batch; no partial materialization, history or selection changes.
	int32 SpawnCount=0;
	const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Cast<ACrahUnitActor>(Actor) && ++SpawnCount==2) Actor->Destroy(false,false);
	}));
	TestNull(TEXT("Mid-conversion spawn failure rejects all"),C->ConfirmCrahAddition()); W->RemoveOnActorSpawnedHandler(Handler);
	TestTrue(TEXT("Failed conversion preserves automatic state and preview"),Count()==0 && C->GetLayoutUndoCount()==0 && C->IsAddingRack());
	auto* Added=C->ConfirmCrahAddition();
	if(!TestNotNull(TEXT("Eight converted plus new cooler"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	const FString Id=VrgCrahLifecycle::Identity(Added); const FName Name=Added->GetFName(); const FTransform Initial=Added->GetActorTransform();
	TestTrue(TEXT("Nine actors and one command"),Count()==9 && C->GetLayoutUndoCount()==1 && C->GetSelectedLayoutActor()==Added);
	for(const auto& Planned:Auto) {
		bool Found=false; for(TActorIterator<ACrahUnitActor> It(W);It;++It) Found|=It->GetActorTransform().Equals(Planned.Transform,1e-9) && It->CaptureLayoutSettings()==Planned.Settings;
		TestTrue(TEXT("Original automatic geometry and kind preserved"),Found);
	}
	TestNull(TEXT("Repeated confirm does not add"),C->ConfirmCrahAddition());
	C->BeginCrahAddition(); Preview(FVector(3500,85,0)); TestNotNull(TEXT("Second add keeps converted inventory"),C->ConfirmCrahAddition());
	TestEqual(TEXT("Only one extra actor"),Count(),10); TestTrue(TEXT("Undo second add"),C->TryUndoLayout()); TestEqual(TEXT("Original nine remain"),Count(),9);
	// Fail after the first destruction during conversion Undo and require every identity back.
	TArray<ACrahUnitActor*> Units; for(TActorIterator<ACrahUnitActor> It(W);It;++It) Units.Add(*It);
	TArray<FString> Ids; for(auto* U:Units) Ids.Add(VrgCrahLifecycle::Identity(U));
	const auto DestroyHandler=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Actor==Units[0]) Units[1]->SetRole(ROLE_SimulatedProxy);
	}));
	TestFalse(TEXT("Mid-removal failure preserves batch"),C->TryUndoLayout()); W->RemoveOnActorDestroyedHandler(DestroyHandler);
	for(const auto& UnitId:Ids) TestNotNull(TEXT("Failed removal restores ID"),VrgCrahLifecycle::Find(W,UnitId));
	TestTrue(TEXT("Undo restores automatic regime"),C->TryUndoLayout()); TestEqual(TEXT("No placed coolers after conversion Undo"),Count(),0);
	auto* Block=W->SpawnActor<ACoolingDistributionUnitActor>(Initial.GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("New destination conflict keeps Redo"),C->TryRedoLayout()); Block->SetActorLocation(FVector(5000,5000,0));
	Racks[0]->AddActorWorldOffset(FVector(0,100,0)); TestFalse(TEXT("Changed automatic plan rejects stale Redo"),C->TryRedoLayout()); Racks[0]->AddActorWorldOffset(FVector(0,-100,0));
	TestTrue(TEXT("Redo same conversion"),C->TryRedoLayout()); Added=VrgCrahLifecycle::Find(W,Id);
	TestTrue(TEXT("Name and transform preserved"),Added && Added->GetFName()==Name && Added->GetActorTransform().Equals(Initial,1e-9));
	C->SelectLayoutAlongRay(Added->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1));
	C->BeginLayoutDragAlongRay(Added->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1));
	C->UpdateLayoutDragAlongRay(Added->GetActorLocation()+FVector(200,0,500),FVector(0,0,-1),true); C->EndLayoutDrag(true); C->OnResetSim();
	const FTransform Final=Added->GetActorTransform();
	TestTrue(TEXT("Undo R, move, conversion"),C->TryUndoLayout() && C->TryUndoLayout() && C->TryUndoLayout());
	TestTrue(TEXT("Redo conversion, move, R rebinds ID"),C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout());
	Added=VrgCrahLifecycle::Find(W,Id); TestTrue(TEXT("Full transform restored"),Added && Added->GetActorTransform().Equals(Final,1e-9));
	TestEqual(TEXT("Layout time remains zero"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("CRAH: auto8 -> placed9 -> 10 -> 9 -> auto8 -> placed9; one conversion command, snapshot geometry/identity, failure rollback and move/R references passed."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
