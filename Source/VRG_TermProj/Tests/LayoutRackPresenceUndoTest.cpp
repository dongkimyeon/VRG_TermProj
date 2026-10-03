#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackPresenceUndoTest, "VRG.Interaction.Layout.RackPresenceUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackPresenceUndoTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); auto* C=W->SpawnActor<ADataCenterDebugController>();
	Sim->EnterLayoutMode();
	auto Count=[&]() { int32 N=0; for(TActorIterator<ARackThermalActor> It(W);It;++It) ++N; return N; };
	const FVector Down(0,0,-1);
	auto Select=[&](AActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
	C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,0,500),Down);
	auto* A=C->ConfirmRackAddition(); if(!TestNotNull(TEXT("Added rack"),A)) return false;
	const FString Id=VrgRackLifecycle::Identity(A); const FName Name=A->GetFName(); const FTransform Original=A->GetActorTransform();
	TestEqual(TEXT("Addition one command"),C->GetLayoutUndoCount(),1);
	TestTrue(TEXT("Undo addition"),C->TryUndoLayout()); TestEqual(TEXT("Addition removed"),Count(),0);
	auto* Block=W->SpawnActor<ACrahUnitActor>();
	TestFalse(TEXT("Occupied addition Redo rejected"),C->TryRedoLayout()); TestEqual(TEXT("Count unchanged"),Count(),0);
	TestEqual(TEXT("Presence collision retains Redo"),C->GetLayoutRedoCount(),1);
	Block->SetActorLocation(FVector(5000,5000,0));
	TestTrue(TEXT("Redo addition"),C->TryRedoLayout()); A=VrgRackLifecycle::Find(W,Id);
	if(!TestNotNull(TEXT("Stable ID recreated"),A)) return false;
	TestEqual(TEXT("Same actor name recreated"),A->GetFName(),Name);
	// Synthetic workload configuration: not a new physical default. No temperature/clock snapshot is transferred.
	A->Input.Utilization=.63; A->Input.SlotUtilization.Init(.42,A->Params.Slots.Num()); A->Input.SledDocked[2]=false;
	A->Input.ComponentHeatW[0]*=1.07;
	C->ClearLayoutHistory(); Select(A);
	const FRackLayoutSettings Config=A->CaptureLayoutSettings();
	FVector Eye=A->GetActorLocation()+FVector(0,0,500);
	C->BeginLayoutDragAlongRay(Eye,Down); C->UpdateLayoutDragAlongRay(Eye+FVector(-200,0,0),Down,true); C->EndLayoutDrag(true);
	const FTransform Moved=A->GetActorTransform(); Select(A);
	TestTrue(TEXT("Delete existing rack"),C->TryDeleteLayoutRack()); TestEqual(TEXT("Deletion removed actor"),Count(),0);
	TestTrue(TEXT("Undo delete"),C->TryUndoLayout()); A=VrgRackLifecycle::Find(W,Id);
	if(!TestNotNull(TEXT("Deleted rack restored"),A)) return false;
	const FRackLayoutSettings Restored=A->CaptureLayoutSettings();
	TestTrue(TEXT("Non-default configuration restored"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&Restored,0));
	TestTrue(TEXT("Deletion restores transform"),A->GetActorTransform().Equals(Moved,1e-9));
	TestEqual(TEXT("No old clock"),A->GetSnapshot().SimTimeS,0.0);
	TestTrue(TEXT("Undo earlier move after recreation"),C->TryUndoLayout());
	TestTrue(TEXT("Earlier command rebinds to restored rack"),A->GetActorTransform().Equals(Original,1e-9));
	TestTrue(TEXT("Redo earlier move"),C->TryRedoLayout()); TestTrue(TEXT("Redo deletion"),C->TryRedoLayout());
	TestEqual(TEXT("Deleted again"),Count(),0); TestTrue(TEXT("Undo deletion again"),C->TryUndoLayout());
	A=VrgRackLifecycle::Find(W,Id); Select(A);
	TestTrue(TEXT("Begin duplicate"),C->BeginRackDuplication()); C->UpdateRackAdditionAlongRay(FVector(-500,-500,500),Down);
	auto* B=C->ConfirmRackAddition(); if(!TestNotNull(TEXT("Duplicate"),B)) return false;
	const FString DuplicateId=VrgRackLifecycle::Identity(B);
	TestTrue(TEXT("Independent identity"),DuplicateId!=Id);
	TestTrue(TEXT("Undo duplicate"),C->TryUndoLayout()); TestEqual(TEXT("Source retained"),Count(),1);
	TestTrue(TEXT("Redo duplicate"),C->TryRedoLayout()); B=VrgRackLifecycle::Find(W,DuplicateId);
	TestNotNull(TEXT("Duplicate restored"),B);
	const FRackLayoutSettings DuplicateConfig=B->CaptureLayoutSettings();
	TestTrue(TEXT("Duplicate settings independent and equal"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&DuplicateConfig,0));
	B->Input.Utilization=.2;
	TestFalse(TEXT("External config change blocks destructive Undo"),C->TryUndoLayout());
	TestEqual(TEXT("Rejected removal preserves count"),Count(),2);
	TestEqual(TEXT("Source load unchanged"),A->Input.Utilization,.63);
	TestEqual(TEXT("Layout clock unchanged"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("Addition/delete/duplicate Undo/Redo: rack counts exact, stable identity/name and non-default settings restored; earlier move rebind, collision/config rejection and fresh state tested."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
