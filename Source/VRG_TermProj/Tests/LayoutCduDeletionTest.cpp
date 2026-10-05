#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduDeletionTest, "VRG.Interaction.Layout.CduDeletionUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduDeletionTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>();
	auto* Rack = W->SpawnActor<ARackThermalActor>();
	auto* Placed = W->SpawnActor<ACoolingDistributionUnitActor>(FVector(300,0,0), FRotator::ZeroRotator);
	auto* Other = W->SpawnActor<ACoolingDistributionUnitActor>(FVector(700,0,0), FRotator::ZeroRotator);
	Sim->EnterLayoutMode();
	const FVector Down(0,0,-1);
	auto Select = [&](AActor* A, bool bToggle = false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500), Down, bToggle); };
	auto Count = [&]() { int32 N=0; for (TActorIterator<ACoolingDistributionUnitActor> It(W); It; ++It) ++N; return N; };
	auto Refused = [&]() {
		const int32 N=Count(), U=C->GetLayoutUndoCount(), R=C->GetLayoutRedoCount();
		TestFalse(TEXT("Deletion blocked"), C->TryDeleteLayoutCdu());
		TestTrue(TEXT("Blocked count/history invariant"), Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R);
	};
	Refused(); Select(Rack); Refused(); Select(Placed); Select(Rack,true); Refused();
	Select(Placed); Select(Other,true); Refused();
	Select(Placed); C->BeginLayoutDragAlongRay(FVector(300,0,500),Down); Refused(); C->EndLayoutDrag(false);
	C->BeginLayoutMarquee(FVector2D(0,0),false); Refused(); C->EndLayoutMarquee(false);
	C->BeginCduAddition(); Refused(); C->CancelRackAddition(); Select(Placed);
	C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false); Select(Placed);
	Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Select(Placed);
	// Synthetic nondefault layout settings/transform, not new hardware physical values.
	FCduLayoutSettings Config=Placed->CaptureLayoutSettings(); Config.PumpSpeedRatio=.8; Config.PumpEfficiency=.65; Config.bUseReferenceModel=false;
	Placed->ApplyLayoutSettings(Config);
	Placed->SetActorScale3D(FVector(.9,1.1,1)); Placed->SetActorLocation(FVector(300,0,20));
	const FTransform Initial=Placed->GetActorTransform();
	const FString Id=VrgCduLifecycle::Identity(Placed); const FName Name=Placed->GetFName();
	Select(Placed); C->BeginLayoutDragAlongRay(Placed->GetActorLocation()+FVector(0,0,500),Down);
	C->UpdateLayoutDragAlongRay(Placed->GetActorLocation()+FVector(0,200,500),Down,true); C->EndLayoutDrag(true);
	C->OnResetSim(); const FTransform Final=Placed->GetActorTransform();
	TestTrue(TEXT("Placed CDU deletion"),C->TryDeleteLayoutCdu());
	TestTrue(TEXT("Actor/reference removed, one deletion record"),Count()==1 && !VrgCduLifecycle::Find(W,Id)
		&& C->GetSelectedLayoutActors().IsEmpty() && C->GetLayoutUndoCount()==3);
	auto* Block=W->SpawnActor<ACrahUnitActor>(Final.GetLocation(),FRotator::ZeroRotator);
	const int32 History=C->GetLayoutUndoCount();
	TestFalse(TEXT("Collision refuses undo"),C->TryUndoLayout());
	TestTrue(TEXT("Collision keeps actor count and stacks"),Count()==1 && C->GetLayoutUndoCount()==History && C->GetLayoutRedoCount()==0);
	Block->Destroy(false,false);
 FActorSpawnParameters O; O.Name=Name; auto* Occupant=W->SpawnActor<AActor>(O);
 TestFalse(TEXT("Occupied name refuses Undo with record retained"),C->TryUndoLayout());
 TestTrue(TEXT("Failed restore leaves count/history"),Count()==1 && C->GetLayoutUndoCount()==History);
 Occupant->Rename(*MakeUniqueObjectName(W->PersistentLevel,AActor::StaticClass(),TEXT("RetiredCduTest")).ToString()); Occupant->Destroy();
	TestTrue(TEXT("Delete undo"),C->TryUndoLayout()); auto* Restored=VrgCduLifecycle::Find(W,Id);
	TestTrue(TEXT("Identity/config/provenance/transform; authored settings"),Restored && Restored->GetFName()==Name
		&& !Restored->bRuntimeLayoutCdu && Restored->CaptureLayoutSettings()==Config
		&& Restored->GetActorTransform().Equals(Final,1e-9));
	TestTrue(TEXT("Earlier R/move references rebound"),C->TryUndoLayout() && C->TryUndoLayout()
		&& Restored->GetActorTransform().Equals(Initial,1e-9));
	TestTrue(TEXT("Redo move/R/delete"),C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout());
	TestTrue(TEXT("Redo removes one again"),Count()==1 && !VrgCduLifecycle::Find(W,Id));
	C->ClearLayoutHistory(); C->BeginCduAddition();
	C->UpdateRackAdditionAlongRay(FVector(300,200,500),Down); auto* Added=C->ConfirmCduAddition();
	if (TestNotNull(TEXT("Runtime CDU"),Added))
	{
		const FString RuntimeId=VrgCduLifecycle::Identity(Added);
		TestTrue(TEXT("Delete runtime"),C->TryDeleteLayoutCdu());
		TestTrue(TEXT("Undo runtime deletion"),C->TryUndoLayout()); Added=VrgCduLifecycle::Find(W,RuntimeId);
		TestTrue(TEXT("Runtime provenance retained"),Added && Added->bRuntimeLayoutCdu);
		// A changed restored configuration must not be silently deleted by Redo.
		Added->PumpEfficiency=.5;
		TestFalse(TEXT("Changed config refuses Redo"),C->TryRedoLayout());
		TestTrue(TEXT("Rejected Redo stack preserved"),Count()==2 && C->GetLayoutRedoCount()==1);
		Added->PumpEfficiency=GetDefault<ACoolingDistributionUnitActor>()->PumpEfficiency;
		TestTrue(TEXT("Redo runtime deletion"),C->TryRedoLayout());
	}
	TestEqual(TEXT("Layout clock unchanged"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(TEXT("Placed/runtime single CDU delete: guards/count/history, fresh origin-preserving Undo/Redo, collision refusal, previous move/R reference restoration; full transform Equals1e-9 at actor cm boundary."));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
