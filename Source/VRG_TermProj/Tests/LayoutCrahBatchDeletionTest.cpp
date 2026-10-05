#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahBatchDeletionTest, "VRG.Interaction.Layout.CrahBatchDeletionUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCrahBatchDeletionTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	auto* A=W->SpawnActor<ACrahUnitActor>(FVector(300,0,20),FRotator::ZeroRotator);
	// Synthetic nondefault authored settings/transform, not new hardware reference values.
	auto Config=GetDefault<ACrahUnitActor>()->CaptureLayoutSettings(); Config.Kind=ECrahUnitKind::Perimeter; Config.VolumeFlowM3s=.25; Config.SizeM*=1.1;
	auto* B=VrgCrahLifecycle::Spawn(W,TEXT("RuntimeBatchCrah"),FGuid::NewGuid().ToString(),
		FTransform(FRotator::ZeroRotator,FVector(700,0,35),FVector(.9,1.1,1.2)),Config);
	Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
	auto Select=[&](AActor* Actor,bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto Both=[&]() { Select(A); Select(B,true); };
	auto Count=[&]() { int32 N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N; };
	auto Refused=[&]() {
		const int32 N=Count(),U=C->GetLayoutUndoCount(),R=C->GetLayoutRedoCount();
		TestFalse(TEXT("Batch deletion blocked"),C->TryDeleteLayoutCrah());
		TestTrue(TEXT("Rejected batch count/history invariant"),Count()==N && C->GetLayoutUndoCount()==U && C->GetLayoutRedoCount()==R);
	};
	Refused(); Both(); Select(Rack,true); Refused();
	Both(); C->BeginLayoutDragAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); Refused(); C->EndLayoutDrag(false);
	C->BeginLayoutMarquee(FVector2D::ZeroVector,false); Refused(); C->EndLayoutMarquee(false);
	C->BeginCrahAddition(); Refused(); C->CancelRackAddition(); Both();
	C->SetPatchMapOpen(true); Refused(); C->SetPatchMapOpen(false);
	Sim->EnterSimulationMode(); Refused(); Sim->EnterLayoutMode(); Both();
	const FString Ids[]={VrgCrahLifecycle::Identity(A),VrgCrahLifecycle::Identity(B)};
	const FName Names[]={A->GetFName(),B->GetFName()};
	const FCrahLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	const FTransform Initial[]={A->GetActorTransform(),B->GetActorTransform()};
	C->BeginLayoutDragAlongRay(A->GetActorLocation()+FVector(0,0,500),Down);
	C->UpdateLayoutDragAlongRay(A->GetActorLocation()+FVector(0,200,500),Down,true); C->EndLayoutDrag(true);
	C->OnResetSim(); const FTransform Final[]={A->GetActorTransform(),B->GetActorTransform()};
	TestEqual(TEXT("Group move and R are earlier records"),C->GetLayoutUndoCount(),2);
	const double ClockBefore=Sim->GetSnapshot().SimTimeS;
	const auto Handler=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Actor==A) B->SetRole(ROLE_SimulatedProxy);
	}));
	TestFalse(TEXT("Second destruction rejection rolls back first"),C->TryDeleteLayoutCrah()); W->RemoveOnActorDestroyedHandler(Handler);
	A=VrgCrahLifecycle::Find(W,Ids[0]); B=VrgCrahLifecycle::Find(W,Ids[1]);
	if(!TestNotNull(TEXT("Rollback placed"),A) || !TestNotNull(TEXT("Rollback runtime"),B)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	TestTrue(TEXT("Rollback counts/selection/history/deletion intent"),Count()==2 && C->GetSelectedLayoutActors().Num()==2
		&& C->GetLayoutUndoCount()==2 && C->GetLayoutRedoCount()==0 && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.IsEmpty());
	TestTrue(TEXT("Failed transaction preserves settings/transform/clock"),A->CaptureLayoutSettings()==Configs[0]
  && B->CaptureLayoutSettings()==Configs[1] && A->GetActorTransform().Equals(Final[0],1e-9)
  && B->GetActorTransform().Equals(Final[1],1e-9) && Sim->GetSnapshot().SimTimeS==ClockBefore);
	TestTrue(TEXT("Batch deletion commits"),C->TryDeleteLayoutCrah());
	TestTrue(TEXT("One history record removes all and selection"),Count()==0 && C->GetLayoutUndoCount()==3 && C->GetSelectedLayoutActors().IsEmpty()
		&& W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
	auto* Block=W->SpawnActor<ARackThermalActor>(Final[1].GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("Second destination collision refuses whole Undo"),C->TryUndoLayout());
	TestTrue(TEXT("Collision state/history invariant"),Count()==0 && C->GetLayoutUndoCount()==3 && C->GetLayoutRedoCount()==0);
	FVector BlockSize; C->PlaceableBoxM(Block,BlockSize); const FVector CrahSize=Configs[1].SizeM;
	// Group R is +90 degrees, so local CRAH depth is the world Y footprint.
	Block->SetActorLocation(Final[1].GetLocation()+FVector(0,(CrahSize.X*Final[1].GetScale3D().X+BlockSize.Y)*50.0+.5,0));
	TestFalse(TEXT("Half cm gap violates total one cm clearance"),C->TryUndoLayout()); TestEqual(TEXT("Clearance rejection atomic"),Count(),0);
	Block->Destroy(false,false);
	auto* Occupant=NewObject<USceneComponent>(W->PersistentLevel,Names[1]);
	TestFalse(TEXT("Occupied second name preflight rejects all"),C->TryUndoLayout()); TestEqual(TEXT("No partial spawn"),Count(),0);
	Occupant->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
	USceneComponent* Late=nullptr;
	const auto SpawnHandler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if(Actor->GetFName()==Names[0]) Late=NewObject<USceneComponent>(W->PersistentLevel,Names[1]);
	}));
	TestFalse(TEXT("Late second spawn failure rolls back first"),C->TryUndoLayout()); W->RemoveOnActorSpawnedHandler(SpawnHandler);
	TestTrue(TEXT("Spawn rollback count/history/deletion intent unchanged"),Count()==0 && C->GetLayoutUndoCount()==3
		&& C->GetLayoutRedoCount()==0 && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
	if(Late) Late->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
	TestTrue(TEXT("Whole Undo"),C->TryUndoLayout());
	A=VrgCrahLifecycle::Find(W,Ids[0]); B=VrgCrahLifecycle::Find(W,Ids[1]);
	for(int32 I=0;I<2;++I) {
		auto* Restored=VrgCrahLifecycle::Find(W,Ids[I]); if(!TestNotNull(TEXT("Restored identity"),Restored)) continue;
		TestTrue(TEXT("Config/transform/provenance and fresh state"),Restored->GetFName()==Names[I] && Restored->CaptureLayoutSettings()==Configs[I]
			&& Restored->bRuntimeLayoutCrah==(I==1) && Restored->GetActorTransform().Equals(Final[I],1e-9));
	}
	TestTrue(TEXT("Earlier group R/move references rebound"),C->TryUndoLayout() && C->TryUndoLayout()
		&& A->GetActorTransform().Equals(Initial[0],1e-9) && B->GetActorTransform().Equals(Initial[1],1e-9));
	TestTrue(TEXT("Redo prior move/R"),C->TryRedoLayout() && C->TryRedoLayout());
	B->VolumeFlowM3s=.7; TestFalse(TEXT("Second stale config rejects whole Redo"),C->TryRedoLayout());
	TestTrue(TEXT("Stale destructive Redo preserves both/stacks"),Count()==2 && C->GetLayoutUndoCount()==2 && C->GetLayoutRedoCount()==1);
	B->ApplyLayoutSettings(Configs[1]); TestTrue(TEXT("Whole Redo deletes all"),C->TryRedoLayout()); TestEqual(TEXT("Redo count"),Count(),0);
	TestTrue(TEXT("Repeat whole Undo"),C->TryUndoLayout()); TestEqual(TEXT("Undo count"),Count(),2);
	 // Synthetic original automatic pair contacts, using the authored dimensions; not hardware measurements.
 A=VrgCrahLifecycle::Find(W,Ids[0]); B=VrgCrahLifecycle::Find(W,Ids[1]);
 A->Destroy(false,false); B->Destroy(false,false); Rack->SetActorLocation(FVector(-1000,0,0)); C->ClearLayoutHistory();
 FCrahLayoutRecord OriginalA{TEXT("converted-batch-A"),TEXT("ConvertedBatchA"),FTransform(FVector(300,0,0)),
  GetDefault<ACrahUnitActor>()->CaptureLayoutSettings(),true,true};
 FCrahLayoutRecord OriginalB=OriginalA; OriginalB.Id=TEXT("converted-batch-B"); OriginalB.Name=TEXT("ConvertedBatchB");
 OriginalB.Transform.SetLocation(FVector(300,OriginalA.Settings.SizeM.Y*100,0));
 A=VrgCrahLifecycle::Spawn(W,OriginalA); B=VrgCrahLifecycle::Spawn(W,OriginalB); Both();
 TestTrue(TEXT("Delete original touching converted pair"),C->TryDeleteLayoutCrah() && Count()==0);
 TestTrue(TEXT("Restore unchanged automatic pair contact"),C->TryUndoLayout() && Count()==2);
 A=VrgCrahLifecycle::Find(W,OriginalA.Id); B=VrgCrahLifecycle::Find(W,OriginalB.Id);
 TestTrue(TEXT("Both original boundaries and provenance restored"),A && B && A->PreservesAutomaticAirBounds() && B->PreservesAutomaticAirBounds());
 // Move one converted unit to a new touching location. Its origin flag is not a clearance exemption.
 B->SetActorLocation(FVector(300,OriginalA.Settings.SizeM.Y*100,20)); C->ClearLayoutHistory(); Both();
 TestTrue(TEXT("Delete moved converted pair"),C->TryDeleteLayoutCrah() && Count()==0);
 TestFalse(TEXT("Moved converted pair cannot bypass one cm on Undo"),C->TryUndoLayout());
 TestTrue(TEXT("Pair refusal atomic and history retained"),Count()==0 && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0);
 TestEqual(TEXT("Layout clock unchanged"),Sim->GetSnapshot().SimTimeS,0.0);
 AddInfo(TEXT("CRAH batch: placed/runtime/converted; input guards, one record, total1cm, mid-delete/spawn rollback, deletion intent and selection/stacks, earlier group move/R rebind, original contact only; full transform Equals1e-9 actor cm/scale dimensionless. No live state copied."));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif