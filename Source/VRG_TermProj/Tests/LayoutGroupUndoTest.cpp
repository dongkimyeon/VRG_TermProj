#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGroupMoveUndoTest, "VRG.Interaction.Layout.GroupMoveUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutGroupMoveUndoTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* C = World->SpawnActor<ADataCenterDebugController>();
	Sim->EnterLayoutMode();
	TArray<AActor*> Members = {World->SpawnActor<ARackThermalActor>(FVector(0,0,7), FRotator(0,17,0)),
		World->SpawnActor<ASpineCabinetActor>(FVector(0,500,13), FRotator(0,31,0))};
	Members[1]->SetActorScale3D(FVector(1.2,.8,1.1));
	const FVector Down(0,0,-1);
	for (int32 I=0; I<Members.Num(); ++I) C->SelectLayoutAlongRay(Members[I]->GetActorLocation()+FVector(0,0,500), Down, I>0);
	TArray<FTransform> Before, After;
	for (AActor* A:Members) Before.Add(A->GetActorTransform());
	auto Drag = [&](FVector Delta, bool bCommit) {
		FVector Eye=Members[0]->GetActorLocation()+FVector(0,0,500);
		C->BeginLayoutDragAlongRay(Eye,Down); C->UpdateLayoutDragAlongRay(Eye+Delta,Down,true); C->EndLayoutDrag(bCommit);
	};
	double MaxCm=0, MaxDeg=0;
	auto Check = [&](const TArray<FTransform>& Expected) {
		for (int32 I=0; I<Members.Num(); ++I) {
			MaxCm=FMath::Max(MaxCm,FVector::Distance(Members[I]->GetActorLocation(),Expected[I].GetLocation()));
			MaxDeg=FMath::Max(MaxDeg,FMath::Abs(FMath::FindDeltaAngleDegrees(Members[I]->GetActorRotation().Yaw,Expected[I].Rotator().Yaw)));
			TestTrue(TEXT("All transforms restored"),Members[I]->GetActorTransform().Equals(Expected[I],1e-9));
		}
	};
	Drag(FVector(-200,0,0),true);
	for (AActor* A:Members) After.Add(A->GetActorTransform());
	TestEqual(TEXT("Group move is one command"),C->GetLayoutUndoCount(),1);
	auto* Block=World->SpawnActor<ACrahUnitActor>(Before[1].GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("One blocked member rejects whole Undo"),C->TryUndoLayout()); Check(After);
	TestEqual(TEXT("Rejected Undo retains stack"),C->GetLayoutUndoCount(),1);
	Block->SetActorLocation(FVector(5000,5000,0));
	TestTrue(TEXT("Undo entire group"),C->TryUndoLayout()); Check(Before);
	Block->SetActorLocation(After[1].GetLocation());
	TestFalse(TEXT("One blocked member rejects whole Redo"),C->TryRedoLayout()); Check(Before);
	TestEqual(TEXT("Rejected Redo retains stack"),C->GetLayoutRedoCount(),1);
	Block->SetActorLocation(FVector(5000,5000,0));
	Drag(FVector(-50,0,0),false); Drag(FVector::ZeroVector,true);
	TestEqual(TEXT("Cancel/no-op retain Redo"),C->GetLayoutRedoCount(),1);
	TestTrue(TEXT("Redo entire group"),C->TryRedoLayout()); Check(After);
	Members[1]->AddActorWorldOffset(FVector(1,0,0));
	TestFalse(TEXT("Stale member rejects entire command"),C->TryUndoLayout());
	TestTrue(TEXT("First member untouched on stale rejection"),Members[0]->GetActorTransform().Equals(After[0],0));
	TestEqual(TEXT("Stale rejection keeps stack"),C->GetLayoutUndoCount(),1);
	TestEqual(TEXT("Clock unchanged"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(FString::Printf(TEXT("Group move restoration: max position %.3e cm, yaw delta %.3e deg; height/scale retained, whole-command collision/stale rejection and cancel/no-op tested."),MaxCm,MaxDeg));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
