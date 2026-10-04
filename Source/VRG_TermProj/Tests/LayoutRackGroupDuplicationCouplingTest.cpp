#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackGroupDuplicationCouplingTest, "VRG.Interaction.Layout.RackGroupDuplicationCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackGroupDuplicationCouplingTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>();
	auto* A=W->SpawnActor<ARackThermalActor>();
	A->ApplyGb200Nvl72Preset(); A->bUseGjModels=true; A->bShowReadout=false;
	A->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
	Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	const FVector Down(0,0,-1);
	C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,85,500),Down);
	auto* B=C->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Runtime source rack"),B)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	// Synthetic workload only; equipment parameters retain the existing preset.
	B->Input.Utilization=.7; B->Input.SledDocked[2]=false; B->Input.ComponentHeatW[0]*=1.05;
	const FRackLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	double ReferenceW[2];
	for (int32 I=0; I<2; ++I) { FRackThermalModel M; M.Initialize(Configs[I].Params,FRackThermalState()); M.Step(Configs[I].Input,Sim->FixedStepS); ReferenceW[I]=M.GetLastTotalHeatW(); }
	const double ExpectedAddedW=ReferenceW[0]+ReferenceW[1];
	TestTrue(TEXT("Existing full-load plausibility bound"),ReferenceW[0]>1.14e5 && ReferenceW[0]<1.26e5);
	TestTrue(TEXT("Reduced load positive and below full load"),ReferenceW[1]>0 && ReferenceW[1]<ReferenceW[0]);
	auto Select=[&](AActor* Actor,bool Toggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle); };
	auto CheckTopology=[&](int32 Expected) {
		int32 Count=0; for (TActorIterator<ARackThermalActor> It(W);It;++It) ++Count;
		TestTrue(TEXT("Rack, branch and patch counts agree"),Count==Expected && Room->GetRoutedRacks().Num()==Expected
			&& Room->GetRackBranchPipeIndices().Num()==Expected && Room->GetPatchDevices().Num()==Expected);
		TSet<int32> Branches;
		for (int32 Index:Room->GetRackBranchPipeIndices()) { TestTrue(TEXT("Distinct valid branch"),Index>=0 && Index<Room->GetMutableNetwork().GetNumPipes() && !Branches.Contains(Index)); Branches.Add(Index); }
		for (int32 Row:Room->GetRoutedRackRows()) TestTrue(TEXT("CDU assigned"),Room->GetRowCdu(Row)==Cdu);
	};
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double BaselineW=Sim->GetSnapshot().TotalItHeatW;
	const int32 BaselinePipes=Room->GetMutableNetwork().GetNumPipes(); CheckTopology(2);
	Sim->EnterLayoutMode(); C->ClearLayoutHistory(); Select(A); Select(B,true);
	TestTrue(TEXT("Duplicate existing and runtime source together"),C->BeginRackDuplication());
	C->UpdateRackAdditionAlongRay(FVector(0,215,500),Down);
	TestNotNull(TEXT("Confirm both copies"),C->ConfirmRackAddition()); CheckTopology(4);
	TArray<FString> Ids; TArray<FName> Names; TArray<FTransform> Transforms;
	for (AActor* Actor:C->GetSelectedLayoutActors()) { auto* Rack=CastChecked<ARackThermalActor>(Actor); Ids.Add(VrgRackLifecycle::Identity(Rack)); Names.Add(Rack->GetFName()); Transforms.Add(Rack->GetActorTransform()); }
	if (!TestEqual(TEXT("Both clones selected"),Ids.Num(),2)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	const int32 AddedPipes=Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Additional branches created"),AddedPipes>BaselinePipes);
	double MaxEnergy=0,MaxDuct=0,MaxDivergence=0,MaxPower=0;
	auto CheckPhysics=[&](int32 Expected,const TCHAR* Stage) {
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		const double TargetW=BaselineW+(Expected==4?ExpectedAddedW:0);
		const double PowerError=FMath::Abs(Sim->GetSnapshot().TotalItHeatW-TargetW)/TargetW;
		TestTrue(TEXT("IT equals independent reference sum at same initial step"),PowerError<1e-9); MaxPower=FMath::Max(MaxPower,PowerError);
		for (int32 Frame=0;Frame<20;++Frame) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Every rack coupled to coolant, heat, air and duct"),S.RackCoolantMassFlowKgs.Num()==Expected && S.RackHeatToAirW.Num()==Expected
			&& S.RackAirMassFlowKgs.Num()==Expected && S.RackDuctMassFlowKgs.Num()==Expected);
		double DuctError=0;
		for (int32 I=0;I<FMath::Min(S.RackAirMassFlowKgs.Num(),S.RackDuctMassFlowKgs.Num());++I) {
			TestTrue(TEXT("Positive rack airflow"),S.RackAirMassFlowKgs[I]>0);
			if (S.RackAirMassFlowKgs[I]>0) DuctError=FMath::Max(DuctError,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Global heat conservation"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9);
		TestTrue(TEXT("Duct flow closure"),!S.bAirDuctsRejected && !AS.bDuctsRejected && DuctError<1e-9);
		TestTrue(TEXT("Pressure and normalized divergence"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,DuctError); MaxDivergence=FMath::Max(MaxDivergence,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Group copy %s: racks %d t %.2f s | power relative %.3e | energy %.3e duct %.3e divergence %.3e"),Stage,Expected,S.SimTimeS,PowerError,S.NormalizedEnergyResidual,DuctError,AS.NormalizedDivergence));
		Sim->EnterLayoutMode();
	};
	CheckPhysics(4,TEXT("confirm"));
	TestTrue(TEXT("One Undo removes group"),C->TryUndoLayout()); CheckTopology(2);
	TestEqual(TEXT("Undo restores original pipes"),Room->GetMutableNetwork().GetNumPipes(),BaselinePipes);
	CheckPhysics(2,TEXT("undo"));
	TestTrue(TEXT("One Redo restores group"),C->TryRedoLayout()); CheckTopology(4);
	TestEqual(TEXT("Redo restores copied pipes"),Room->GetMutableNetwork().GetNumPipes(),AddedPipes);
	CheckPhysics(4,TEXT("redo"));
	const FString File=TEXT("AutomationGroupDuplication"); FString Summary;
	TestTrue(TEXT("Save v2 group inventory"),VrgLayoutFile::Save(W,File,Summary));
	Select(VrgRackLifecycle::Find(W,Ids[0])); Select(VrgRackLifecycle::Find(W,Ids[1]),true);
	TestTrue(TEXT("Delete both copies before reload"),C->TryDeleteLayoutRack()); CheckTopology(2);
	TestTrue(TEXT("Reload restores copied group"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(4);
	TestTrue(TEXT("Load clears both histories"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Repeated Load does not duplicate group"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(4);
	for (int32 I=0;I<2;++I) {
		auto* Rack=VrgRackLifecycle::Find(W,Ids[I]);
		if (!TestNotNull(TEXT("Loaded clone ID"),Rack)) continue;
		const auto Settings=Rack->CaptureLayoutSettings();
		TestTrue(TEXT("Loaded name, settings and full transform match"),Rack->GetFName()==Names[I] && Rack->GetActorTransform().Equals(Transforms[I],1e-9)
			&& FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Settings,&Configs[I],0));
		TestEqual(TEXT("Loaded clone fresh clock"),Rack->GetSnapshot().SimTimeS,0.0);
	}
	CheckPhysics(4,TEXT("reload"));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	AddInfo(FString::Printf(TEXT("Group copy coupling: pipes %d->%d->%d->%d | IT added %.6f W = %.6f + %.6f | max power %.3e energy %.3e duct %.3e divergence %.3e"),
		BaselinePipes,AddedPipes,BaselinePipes,Room->GetMutableNetwork().GetNumPipes(),ExpectedAddedW,ReferenceW[0],ReferenceW[1],MaxPower,MaxEnergy,MaxDuct,MaxDivergence));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
