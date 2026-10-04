#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGroupRowSnapTest,"VRG.Interaction.Layout.RackGroupRowSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutGroupRowSnapTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>();
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Room=W->SpawnActor<ADataCenterRoomActor>();
	W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-1000,0,0),FRotator::ZeroRotator);
	auto* A=W->SpawnActor<ARackThermalActor>();
	auto* B=W->SpawnActor<ARackThermalActor>(FVector(0,80,0),FRotator::ZeroRotator);
	auto* M=W->SpawnActor<ARackThermalActor>(FVector(300,200,7),FRotator::ZeroRotator);
	auto* N=W->SpawnActor<ARackThermalActor>(FVector(300,280,13),FRotator::ZeroRotator);
	auto* Block=W->SpawnActor<ACrahUnitActor>(FVector(3000,3000,0),FRotator::ZeroRotator);
	// Synthetic placement geometry tests UI decisions, not equipment dimensions or new physical inputs.
	A->RackSizeM=B->RackSizeM=M->RackSizeM=N->RackSizeM=FVector(.4,.4,1);
	M->SetActorScale3D(FVector(2,1,1.1)); N->SetActorScale3D(FVector(2,1.2,.9));
	Block->SizeM=FVector(.2,.2,1);
	Room->RowGroupToleranceM=.5; Room->RackPitchM=.66; // Observed row pitch .80 m must win.
	Sim->EnterLayoutMode();
	const FTransform Starts[]={M->GetActorTransform(),N->GetActorTransform()};
	const FVector Down(0,0,-1);
	auto Select=[&](AActor* Actor,bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto Begin=[&](bool LastAnchor=false) {
		Select(M); Select(N,true);
		C->BeginLayoutDragAlongRay((LastAnchor?N:M)->GetActorLocation()+FVector(0,0,500),Down);
	};
	auto Preview=[&](FVector Position,bool Alt=false) { C->UpdateLayoutDragAlongRay(Position+FVector(0,0,500),Down,Alt); };
	auto Guide=[&]() { FVector Start,End; return C->GetLayoutRowSnapGuide(Start,End); };
	auto CheckOriginal=[&]() { TestTrue(TEXT("Preview/cancel/rejection preserves both live transforms"),M->GetActorTransform().Equals(Starts[0],0) && N->GetActorTransform().Equals(Starts[1],0)); };
	Begin(); Preview(Starts[0].GetLocation()); C->EndLayoutDrag(true);
	CheckOriginal(); TestEqual(TEXT("Click records no edit"),C->GetLayoutUndoCount(),0);
	Begin(); Preview(FVector(29,151,7));
	FTransform Targets[2];
	C->GetLayoutDragPreviewTransform(M,Targets[0]); C->GetLayoutDragPreviewTransform(N,Targets[1]);
	double MaxCm=0;
	for(int32 I=0;I<2;++I) {
		FTransform Expected=Starts[I]; Expected.SetLocation(FVector(20,160+80*I,Starts[I].GetLocation().Z));
		MaxCm=FMath::Max(MaxCm,FVector::Distance(Targets[I].GetLocation(),Expected.GetLocation()));
		TestTrue(TEXT("Observed pitch/scaled rear align without reshaping group"),Targets[I].Equals(Expected,1e-9));
	}
	TestTrue(TEXT("Group relative position stays intact"),(Targets[1].GetLocation()-Targets[0].GetLocation()).Equals(Starts[1].GetLocation()-Starts[0].GetLocation(),1e-9));
	FVector GuideStart,GuideEnd;
	TestTrue(TEXT("Guide spans every destination and existing rear line"),C->GetLayoutRowSnapGuide(GuideStart,GuideEnd)
		&& GuideStart.Equals(FVector(-20,240,7),1e-9) && GuideEnd.Equals(FVector(-20,0,7),1e-9));
	CheckOriginal();
	Preview(FVector(29,151,7),true);
	FTransform Grid; C->GetLayoutDragPreviewTransform(M,Grid);
	TestTrue(TEXT("Alt hides guide and restores anchor grid"),!Guide() && Grid.GetLocation()==FVector(30,150,7) && Grid.GetRotation().Equals(Starts[0].GetRotation(),0));
	C->GetLayoutDragPreviewTransform(N,Grid);
	TestTrue(TEXT("Alt retains whole group's heights and spacing"),Grid.GetLocation()==FVector(30,230,13) && Grid.GetScale3D()==Starts[1].GetScale3D());
	Preview(FVector(29,151,7)); TestTrue(TEXT("Alt release restores snapping"),Guide());
	C->EndLayoutDrag(false); CheckOriginal();
	TestTrue(TEXT("Cancel clears guide/drag and leaves history"),!Guide() && !C->IsLayoutDragging() && C->GetLayoutUndoCount()==0);
	Begin(true); Preview(FVector(29,231,13));
	C->GetLayoutDragPreviewTransform(M,Grid);
	TestTrue(TEXT("Clicked second member anchors same rigid placement"),Guide() && Grid.Equals(Targets[0],1e-9));
	C->EndLayoutDrag(false);
	Begin(); Preview(FVector(70,151,7)); TestTrue(TEXT("Inclusive rear tolerance attaches group"),Guide());
	Preview(FVector(70.01,151,7)); TestFalse(TEXT("Outside rear tolerance remains free drag"),Guide());
	Preview(FVector(29,160,7)); TestTrue(TEXT("One observed interval boundary attaches"),Guide());
	Preview(FVector(29,160.01,7)); TestFalse(TEXT("No member near row stays free drag"),Guide());
	C->EndLayoutDrag(false);
	// Every incompatible arrangement falls back, never auto-rotates or changes the spacing.
	N->SetActorRotation(FRotator(0,90,0)); Begin(); Preview(FVector(29,151,7));
	C->GetLayoutDragPreviewTransform(N,Grid);
	TestTrue(TEXT("Different facing retains group grid drag"),!Guide() && Grid.Rotator().Equals(N->GetActorRotation(),1e-9));
	C->EndLayoutDrag(false); N->SetActorTransform(Starts[1]);
	N->SetActorLocation(FVector(300,275,13)); Begin(); Preview(FVector(29,151,7));
	TestFalse(TEXT("Incompatible spacing does not rearrange group"),Guide()); C->EndLayoutDrag(false); N->SetActorTransform(Starts[1]);
	N->SetActorLocation(FVector(310,280,13)); Begin(); Preview(FVector(29,151,7));
	TestFalse(TEXT("Different rear lines do not reshape group"),Guide()); C->EndLayoutDrag(false); N->SetActorTransform(Starts[1]);
	Select(M); Select(N,true); Select(Block,true); C->BeginLayoutDragAlongRay(M->GetActorLocation()+FVector(0,0,500),Down);
	Preview(FVector(29,151,7)); TestFalse(TEXT("Mixed selection has no rack row guide"),Guide()); C->EndLayoutDrag(false);
	B->SetActorLocation(FVector(2000,80,0)); Begin(); Preview(FVector(329,151,7));
	TestFalse(TEXT("Whole dragged group excluded, no pitch from lone neighbours or self"),Guide()); C->EndLayoutDrag(false); B->SetActorLocation(FVector(0,80,0));
	Begin(); Preview(FVector(29,151,7));
	Block->SetActorLocation(FVector(70.5,240,13)); // Second rack has only .5 cm clearance: total 1 cm rule rejects all.
	TestTrue(TEXT("One member violates clearance, entire group blocked"),Guide() && C->IsLayoutDragPreviewBlocked());
	C->EndLayoutDrag(true); CheckOriginal(); TestEqual(TEXT("Rejected group records no edit"),C->GetLayoutUndoCount(),0);
	Block->SetActorLocation(FVector(3000,3000,0));
	Begin(); Preview(FVector(29,151,7)); B->SetActorLocation(FVector(0,100,0));
	C->EndLayoutDrag(true);
	TestTrue(TEXT("Release refresh drops incompatible changed pitch"),M->GetActorLocation()==FVector(30,150,7) && N->GetActorLocation()==FVector(30,230,13));
	M->SetActorTransform(Starts[0]); N->SetActorTransform(Starts[1]); B->SetActorLocation(FVector(0,80,0)); C->ClearLayoutHistory();
	Begin(); Preview(FVector(29,151,7)); C->EndLayoutDrag(true);
	TestTrue(TEXT("Entire valid group commits as one edit"),M->GetActorTransform().Equals(Targets[0],1e-9) && N->GetActorTransform().Equals(Targets[1],1e-9) && C->GetLayoutUndoCount()==1);
	Room->RebuildRoomNow(); const auto Routed=Room->GetRoutedRacks(); const auto Rows=Room->GetRoutedRackRows();
	const int32 Ai=Routed.IndexOfByKey(A),Mi=Routed.IndexOfByKey(M),Ni=Routed.IndexOfByKey(N);
	TestTrue(TEXT("Committed members join actual reference row"),Ai!=INDEX_NONE && Mi!=INDEX_NONE && Ni!=INDEX_NONE && Rows[Ai]==Rows[Mi] && Rows[Ai]==Rows[Ni]);
	TestEqual(TEXT("Layout clock stays zero"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(FString::Printf(TEXT("Group row snap: max position %.3e cm; observed .80 m, configured .66 m; scaled rear, clicked anchor, guide extent, Alt/cancel, incompatibility, self exclusion, clearance and release refresh tested; SimTime 0 s. Actual OS group drag unverified."),MaxCm));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
