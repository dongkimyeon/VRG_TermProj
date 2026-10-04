#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackGroupDuplicationTest, "VRG.Interaction.Layout.RackGroupDuplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackGroupDuplicationTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>();
	auto* C = W->SpawnActor<ADataCenterDebugController>(); Sim->EnterLayoutMode();
	auto* A = W->SpawnActor<ARackThermalActor>(FVector(0,0,7), FRotator(0,17,0));
	auto* B = W->SpawnActor<ARackThermalActor>(FVector(0,250,13), FRotator(0,-11,0));
	A->ApplyGb200Nvl72Preset(); B->ApplyGb200Nvl72Preset();
	A->bShowReadout = false; B->bShowReadout = false;
	B->SetActorScale3D(FVector(.8,1.2,1.1));
	B->Input.Utilization = .62; B->Input.SledDocked[2] = false;
	const FRackLayoutSettings Configs[] = {A->CaptureLayoutSettings(), B->CaptureLayoutSettings()};
	const FTransform Starts[] = {A->GetActorTransform(), B->GetActorTransform()};
	const FVector Down(0,0,-1);
	auto Select = [&](AActor* Actor, bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto SelectBoth = [&]() { Select(A); Select(B,true); };
	auto Count = [&]() { int32 N=0; for (TActorIterator<ARackThermalActor> It(W); It; ++It) ++N; return N; };
	auto* Block = W->SpawnActor<ACrahUnitActor>(FVector(3000,3000,0),FRotator::ZeroRotator);
	SelectBoth(); Select(Block,true);
	TestFalse(TEXT("Mixed selection rejects whole duplicate"),C->BeginRackDuplication());
	TestTrue(TEXT("No mutation on mixed selection"),Count()==2 && C->GetLayoutUndoCount()==0);
	SelectBoth();
	// Exercise the actual event-time chord with a group selection.
	for (auto Pair : {TPair<FKey,EInputEvent>(EKeys::LeftControl,IE_Pressed), {EKeys::D,IE_Pressed},
		{EKeys::D,IE_Released}, {EKeys::LeftControl,IE_Released}})
	{
		FInputKeyEventArgs Args; Args.Key=Pair.Key; Args.Event=Pair.Value; Args.AmountDepressed=1; C->InputKey(Args);
	}
	TestEqual(TEXT("Two value-only previews"),C->GetRackAdditionPreviewCount(),2);
	C->UpdateRackAdditionAlongRay(FVector(503,754,500),Down);
	FTransform Before[2], Targets[2]; FVector Size;
	for (int32 I=0; I<2; ++I) TestTrue(TEXT("Member preview available"),C->GetRackAdditionPreview(Before[I],Size,I));
	TestTrue(TEXT("Translation preserves relative position"),(Before[1].GetLocation()-Before[0].GetLocation()).Equals(Starts[1].GetLocation()-Starts[0].GetLocation(),1e-9));
	C->OnResetSim();
	const FQuat Quarter = FRotator(0,90,0).Quaternion();
	double MaxCm=0;
	for (int32 I=0; I<2; ++I)
	{
		C->GetRackAdditionPreview(Targets[I],Size,I);
		TestTrue(TEXT("R preserves height/scale and rotates each orientation"),Targets[I].GetLocation().Z==Starts[I].GetLocation().Z
			&& Targets[I].GetScale3D()==Starts[I].GetScale3D() && Targets[I].GetRotation().Equals(Quarter*Starts[I].GetRotation(),1e-12));
	}
	MaxCm=FVector::Distance(Targets[1].GetLocation()-Targets[0].GetLocation(),Quarter.RotateVector(Starts[1].GetLocation()-Starts[0].GetLocation()));
	TestTrue(TEXT("Group R preserves rotated relative offset"),MaxCm<1e-9);
	for (int32 I=0; I<3; ++I) C->OnResetSim();
	for (int32 I=0; I<2; ++I) { FTransform T; C->GetRackAdditionPreview(T,Size,I); TestTrue(TEXT("Four R restores complete preview"),T.Equals(Before[I],1e-9)); }
	C->OnResetSim();
	Block->SetActorLocation(Targets[1].GetLocation());
	TestTrue(TEXT("Second member collision blocks entire group"),C->IsRackAdditionBlocked());
	TestNull(TEXT("Collision creates no partial group"),C->ConfirmRackAddition());
	TestTrue(TEXT("Collision keeps preview/selection/history"),Count()==2 && C->IsAddingRack() && C->GetSelectedLayoutActors().Num()==2 && C->GetLayoutUndoCount()==0);
	Block->SetActorLocation(FVector(3000,3000,0));
	C->CancelRackAddition(); TestEqual(TEXT("Cancel drops all value snapshots"),C->GetRackAdditionPreviewCount(),0);
	TestNull(TEXT("Cancel cannot spawn"),C->ConfirmRackAddition());
	C->BeginRackDuplication(); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels whole group"),C->IsAddingRack()); C->SetPatchMapOpen(false);
	SelectBoth(); C->BeginRackDuplication(); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestFalse(TEXT("Mode transition cancels whole group"),C->IsAddingRack()); Sim->EnterLayoutMode();
	// A malformed source arrangement must not allow internally overlapping copies.
	SelectBoth(); B->SetActorLocation(A->GetActorLocation()); C->BeginRackDuplication();
	C->UpdateRackAdditionAlongRay(FVector(1500,1500,500),Down);
	TestTrue(TEXT("Member-to-member collision rejects group"),C->IsRackAdditionBlocked());
	TestNull(TEXT("Internal collision creates nothing"),C->ConfirmRackAddition()); C->CancelRackAddition(); B->SetActorTransform(Starts[1]);
	SelectBoth(); TestTrue(TEXT("Begin final group"),C->BeginRackDuplication());
	C->UpdateRackAdditionAlongRay(FVector(503,754,500),Down); C->OnResetSim();
	// Mutation after capture proves value ownership. The source live state must also survive confirmation.
	B->Input.Utilization=.4; A->bDrivenExternally=true; A->SetExternalSimTimeS(12);
	A->GetMutableModel().Step(A->Input,.02);
	const auto State=A->GetSnapshot(); const double Energy=A->GetMutableModel().GetEnergyInJ();
	auto* First=C->ConfirmRackAddition();
	TestNotNull(TEXT("Confirm entire group"),First);
	TestTrue(TEXT("Two copies, one command, copies selected"),Count()==4 && C->GetLayoutUndoCount()==1 && C->GetSelectedLayoutActors().Num()==2);
	TestNull(TEXT("Second click cannot repeat creation"),C->ConfirmRackAddition());
	TArray<FString> Ids; TArray<FName> Names;
	const auto Copies=C->GetSelectedLayoutActors();
	for (int32 I=0; I<Copies.Num(); ++I)
	{
		auto* Rack=CastChecked<ARackThermalActor>(Copies[I]);
		Ids.Add(VrgRackLifecycle::Identity(Rack)); Names.Add(Rack->GetFName());
		const auto Config=Rack->CaptureLayoutSettings();
		TestTrue(TEXT("Captured settings and target transform"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&Configs[I],0)
			&& Rack->GetActorTransform().Equals(Targets[I],1e-9));
		FRackThermalModel Fresh; Fresh.Initialize(Configs[I].Params,FRackThermalState());
		TestTrue(TEXT("Fresh clock, temperatures, energy and independent arrays"),Rack->bDrivenExternally && Rack->GetSnapshot().SimTimeS==0
			&& Rack->GetMutableModel().GetEnergyInJ()==0 && Rack->GetMutableModel().GetState().ComponentTempK==Fresh.GetState().ComponentTempK
			&& Rack->Input.ComponentHeatW.GetData()!=A->Input.ComponentHeatW.GetData() && Rack->Input.ComponentHeatW.GetData()!=B->Input.ComponentHeatW.GetData());
	}
	TestTrue(TEXT("Source transforms/live thermal state unchanged"),A->GetActorTransform().Equals(Starts[0],0) && B->GetActorTransform().Equals(Starts[1],0)
		&& A->GetSnapshot().SimTimeS==State.SimTimeS && A->GetSnapshot().ComponentTempK==State.ComponentTempK && A->GetMutableModel().GetEnergyInJ()==Energy);
	if (Ids.Num()==2)
	{
		TestTrue(TEXT("Unique identities/names"),Ids[0]!=Ids[1] && Names[0]!=Names[1] && Ids[0]!=VrgRackLifecycle::Identity(A) && Ids[1]!=VrgRackLifecycle::Identity(B));
		TestTrue(TEXT("One Undo removes both copies"),C->TryUndoLayout()); TestEqual(TEXT("Originals remain"),Count(),2);
		Block->SetActorLocation(Targets[1].GetLocation());
		TestFalse(TEXT("Blocked second Redo rejects all"),C->TryRedoLayout());
		TestTrue(TEXT("Rejected Redo preserves both stacks"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
		Block->SetActorLocation(FVector(3000,3000,0));
		USceneComponent* Occupant=nullptr;
		const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
			if (Actor->GetFName()==Names[0]) Occupant=NewObject<USceneComponent>(W->PersistentLevel,Names[1]);
		}));
		TestFalse(TEXT("Second real spawn failure rolls back first"),C->TryRedoLayout()); W->RemoveOnActorSpawnedHandler(Handler);
		TestTrue(TEXT("Failed batch leaves originals and Redo"),Count()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
		if (Occupant) Occupant->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors|REN_NonTransactional);
		TestTrue(TEXT("Redo restores both stable identities"),C->TryRedoLayout());
		for (int32 I=0; I<2; ++I) { auto* Rack=VrgRackLifecycle::Find(W,Ids[I]); TestTrue(TEXT("Restored name and full transform"),Rack && Rack->GetFName()==Names[I] && Rack->GetActorTransform().Equals(Targets[I],1e-9)); }
		TestEqual(TEXT("Four racks after Redo"),Count(),4);
		TestTrue(TEXT("Undo again"),C->TryUndoLayout());
		SelectBoth(); C->BeginRackDuplication(); C->UpdateRackAdditionAlongRay(FVector(-1000,1000,500),Down);
		TestNotNull(TEXT("New group edit"),C->ConfirmRackAddition()); TestEqual(TEXT("New edit clears Redo"),C->GetLayoutRedoCount(),0);
	}
	TestEqual(TEXT("Layout clock stays zero"),Sim->GetSnapshot().SimTimeS,0.0);
	AddInfo(FString::Printf(TEXT("Group duplicate: 2->4->2->4; one command; relative rotation error %.3e cm; value settings, fresh state, atomic collision/spawn failure."),MaxCm));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
