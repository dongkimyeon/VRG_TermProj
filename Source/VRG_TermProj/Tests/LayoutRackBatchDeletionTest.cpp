#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackBatchDeletionTest, "VRG.Interaction.Layout.RackBatchDeletionUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackBatchDeletionTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>();
	auto* C = W->SpawnActor<ADataCenterDebugController>(); Sim->EnterLayoutMode();
	auto* A = W->SpawnActor<ARackThermalActor>(FVector(0,0,7), FRotator(0,17,0));
	A->ApplyGb200Nvl72Preset(); A->bShowReadout = false;
	C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,500,500), FVector(0,0,-1));
	auto* B = C->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Runtime rack"), B)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	B->SetActorScale3D(FVector(1.2,.8,1.1)); B->SetActorLocation(FVector(0,500,13));
	// Synthetic non-default workload, not physical reference values or new defaults.
	A->Input.Utilization = .63; B->Input.Utilization = .42; B->Input.SledDocked[2] = false;
	const FString Ids[] = {VrgRackLifecycle::Identity(A), VrgRackLifecycle::Identity(B)};
	const FName Names[] = {A->GetFName(), B->GetFName()};
	const FRackLayoutSettings Configs[] = {A->CaptureLayoutSettings(), B->CaptureLayoutSettings()};
	const FTransform Before[] = {A->GetActorTransform(), B->GetActorTransform()};
	auto Count = [&]() { int32 N=0; for (TActorIterator<ARackThermalActor> It(W); It; ++It) ++N; return N; };
	auto Select = [&](AActor* Actor, bool bToggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500), FVector(0,0,-1), bToggle); };
	C->ClearLayoutHistory(); Select(A); Select(B, true);
	auto* Block = W->SpawnActor<ACrahUnitActor>(FVector(3000,3000,0), FRotator::ZeroRotator);
	Select(Block, true);
	TestFalse(TEXT("Any non-rack rejects whole selection"), C->TryDeleteLayoutRack());
	TestTrue(TEXT("Mixed rejection keeps count/selection/history"), Count()==2 && C->GetSelectedLayoutActors().Num()==3 && C->GetLayoutUndoCount()==0);
	Select(A); Select(B, true);
	const FVector Eye = A->GetActorLocation()+FVector(0,0,500);
	C->BeginLayoutDragAlongRay(Eye, FVector(0,0,-1));
	C->UpdateLayoutDragAlongRay(Eye+FVector(-200,0,0), FVector(0,0,-1), true); C->EndLayoutDrag(true);
	const FTransform Moved[] = {A->GetActorTransform(), B->GetActorTransform()};
	TestEqual(TEXT("Earlier group move is one command"), C->GetLayoutUndoCount(), 1);
	// Fault injection after the first real destruction: the second engine Destroy must fail.
	A->bDrivenExternally = true;
	A->GetMutableModel().Step(A->Input, .02); A->SetExternalSimTimeS(12.0);
	const auto LiveState = A->GetMutableModel().GetState();
	const auto Handler = W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor) {
		if (Actor == A) B->SetRole(ROLE_SimulatedProxy);
	}));
	TestFalse(TEXT("Mid-batch engine failure rejects deletion"), C->TryDeleteLayoutRack());
	W->RemoveOnActorDestroyedHandler(Handler);
	A = VrgRackLifecycle::Find(W, Ids[0]); B = VrgRackLifecycle::Find(W, Ids[1]);
	TestTrue(TEXT("Engine failure rolls back both identities, selection and history"), IsValid(A) && IsValid(B)
		&& Count()==2 && C->GetSelectedLayoutActors().Num()==2 && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0);
	if (!A || !B) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	TestTrue(TEXT("Failed transaction retains live thermal state/clock"), A->GetMutableModel().GetState().ComponentTempK==LiveState.ComponentTempK
		&& A->GetSnapshot().SimTimeS==12.0 && B->GetLocalRole()==ROLE_Authority);
	TestTrue(TEXT("Delete original and runtime rack together"), C->TryDeleteLayoutRack());
	TestTrue(TEXT("Deletion one command and no selected stale actors"), Count()==0 && C->GetLayoutUndoCount()==2 && C->GetSelectedLayoutActors().IsEmpty());
	Block->SetActorLocation(Moved[1].GetLocation());
	TestFalse(TEXT("Second restoration collision rejects entire Undo"), C->TryUndoLayout());
	TestTrue(TEXT("Blocked Undo leaves count and both stacks unchanged"), Count()==0 && C->GetLayoutUndoCount()==2 && C->GetLayoutRedoCount()==0);
	FVector BlockSizeM; C->PlaceableBoxM(Block, BlockSizeM);
	const double TouchXcm = (Configs[1].RackSizeM.X * Moved[1].GetScale3D().X + BlockSizeM.X) * 50.0;
	Block->SetActorLocation(Moved[1].GetLocation()+FVector(TouchXcm+.5,0,0));
	TestFalse(TEXT("Half cm gap still violates total one cm clearance"), C->TryUndoLayout());
	TestEqual(TEXT("Clearance rejection is atomic"), Count(), 0);
	Block->SetActorLocation(FVector(3000,3000,0));
	// Names are preflighted as a batch even when the first name is free.
	auto* Occupant = NewObject<USceneComponent>(W->PersistentLevel, Names[1]);
	TestFalse(TEXT("Second occupied name rejects entire Undo"), C->TryUndoLayout());
	TestEqual(TEXT("Name failure cannot create first member"), Count(), 0);
	Occupant->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	USceneComponent* LateOccupant = nullptr;
	const auto SpawnHandler = W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
		if (Actor->GetFName()==Names[0]) LateOccupant=NewObject<USceneComponent>(W->PersistentLevel,Names[1]);
	}));
	TestFalse(TEXT("Second spawn failure rolls back the first restoration"), C->TryUndoLayout());
	W->RemoveOnActorSpawnedHandler(SpawnHandler);
	TestTrue(TEXT("Spawn failure leaves count/history/selection unchanged"), Count()==0 && C->GetLayoutUndoCount()==2
		&& C->GetLayoutRedoCount()==0 && C->GetSelectedLayoutActors().IsEmpty());
	if (LateOccupant) LateOccupant->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	TestTrue(TEXT("Undo restores both racks"), C->TryUndoLayout());
	A=VrgRackLifecycle::Find(W,Ids[0]); B=VrgRackLifecycle::Find(W,Ids[1]);
	double MaxCm=0;
	for (int32 I=0; I<2; ++I)
	{
		auto* Rack = VrgRackLifecycle::Find(W,Ids[I]);
		if (!TestNotNull(TEXT("Stable identity"), Rack)) continue;
		const auto Config=Rack->CaptureLayoutSettings();
		TestTrue(TEXT("Name/config/height/scale/direction restored"), Rack->GetFName()==Names[I]
			&& FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&Configs[I],0) && Rack->GetActorTransform().Equals(Moved[I],1e-9));
		TestEqual(TEXT("Successful Undo starts fresh clock"), Rack->GetSnapshot().SimTimeS, 0.0);
		FRackThermalModel Fresh;
		Fresh.Initialize(Configs[I].Params, FRackThermalState());
		TestTrue(TEXT("Successful Undo starts fresh temperatures and integral state"),
			Rack->GetMutableModel().GetState().ComponentTempK==Fresh.GetState().ComponentTempK
			&& Rack->GetMutableModel().GetEnergyResidualJ()==Fresh.GetEnergyResidualJ());
		MaxCm=FMath::Max(MaxCm,FVector::Distance(Rack->GetActorLocation(),Moved[I].GetLocation()));
	}
	TestTrue(TEXT("Undo earlier group move rebinds both actors"), C->TryUndoLayout());
	TestTrue(TEXT("Both earlier transforms restored"), A->GetActorTransform().Equals(Before[0],1e-9) && B->GetActorTransform().Equals(Before[1],1e-9));
	TestTrue(TEXT("Redo earlier group move"), C->TryRedoLayout());
	B->Input.Utilization=.1;
	TestFalse(TEXT("Second stale config rejects whole destructive Redo"), C->TryRedoLayout());
	TestTrue(TEXT("Stale Redo leaves both actors/records"), Count()==2 && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==1);
	B->ApplyLayoutSettings(Configs[1]);
	TestTrue(TEXT("Redo deletes both together"), C->TryRedoLayout()); TestEqual(TEXT("Redo count"),Count(),0);
	TestTrue(TEXT("Repeat Undo recreates whole command"), C->TryUndoLayout()); TestEqual(TEXT("Restored count"),Count(),2);
	TestEqual(TEXT("Simulation clock does not advance in layout"), Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(FString::Printf(TEXT("Batch deletion: counts 2->0->2->0->2, one command; mixed/collision/name/stale/engine-failure rejection; prior move refs rebound; max transform error %.3e cm; successful Undo clocks 0 s."),MaxCm));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
