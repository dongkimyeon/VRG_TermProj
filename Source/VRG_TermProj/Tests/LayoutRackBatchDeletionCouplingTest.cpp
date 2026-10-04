#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackBatchDeletionCouplingTest, "VRG.Interaction.Layout.RackBatchDeletionCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackBatchDeletionCouplingTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>();
	auto MakeExisting=[&](FVector Position) {
		auto* Rack=W->SpawnActor<ARackThermalActor>(Position,FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
		Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
		return Rack;
	};
	auto* Keep=MakeExisting(FVector::ZeroVector);
	auto* Existing=MakeExisting(FVector(0,85,0));
	Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,170,500),FVector(0,0,-1));
	auto* Added=C->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Runtime rack"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	// Synthetic reduced workload, no new hardware values. All defaults come from the existing preset.
	Added->Input.Utilization=.7; Added->Input.SledDocked[2]=false; Added->Input.ComponentHeatW[0]*=1.05;
	const FString Ids[]={VrgRackLifecycle::Identity(Existing),VrgRackLifecycle::Identity(Added)};
	const auto AddedConfig=Added->CaptureLayoutSettings();
	auto ReferenceW=[&](const FRackLayoutSettings& Config) {
		FRackThermalModel Reference; Reference.Initialize(Config.Params,FRackThermalState());
		Reference.Step(Config.Input,Sim->FixedStepS); return Reference.GetLastTotalHeatW();
	};
	const double FullW=ReferenceW(Existing->CaptureLayoutSettings());
	const double ReducedW=ReferenceW(AddedConfig); const double ExpectedRemovedW=FullW+ReducedW;
	// Existing NVIDIA ~120 kW +/-5% safety envelope; reduced workload must stay below full load.
	// Source/default provenance remains in MakeGb200Nvl72Params/HeatW and CLAUDE.md; no new estimate here.
	TestTrue(TEXT("Full rack remains in existing 114..126 kW range"),FullW>1.14e5 && FullW<1.26e5);
	TestTrue(TEXT("Reduced test load has plausible IT power"),ReducedW>0 && ReducedW<FullW && ReducedW<1.26e5);
	auto Count=[&]() { int32 N=0; for (TActorIterator<ARackThermalActor> It(W);It;++It) ++N; return N; };
	auto Select=[&](AActor* Actor,bool bToggle=false) { C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1),bToggle); };
	auto CheckTopology=[&](int32 Expected) {
		TestTrue(TEXT("Rack/row branch/patch inventory updated together"),Count()==Expected && Room->GetRoutedRacks().Num()==Expected
			&& Room->GetRackBranchPipeIndices().Num()==Expected && Room->GetPatchDevices().Num()==Expected && Room->GetRowCdu(0)==Cdu);
		for (int32 Index:Room->GetRackBranchPipeIndices()) TestTrue(TEXT("Each surviving rack has a valid coolant branch"),
			Index>=0 && Index<Room->GetMutableNetwork().GetNumPipes());
	};
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double BeforeW=Sim->GetSnapshot().TotalItHeatW;
	const int32 BeforePipes=Room->GetMutableNetwork().GetNumPipes(); CheckTopology(3);
	Sim->EnterLayoutMode(); C->ClearLayoutHistory(); Select(Existing); Select(Added,true);
	TestTrue(TEXT("Delete original/runtime racks as one batch"),C->TryDeleteLayoutRack()); CheckTopology(1);
	const int32 AfterPipes=Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Removed racks absent from routing and patch"),Room->GetRoutedRacks()[0]==Keep && AfterPipes<BeforePipes
		&& !VrgRackLifecycle::Find(W,Ids[0]) && !VrgRackLifecycle::Find(W,Ids[1]));
	TestTrue(TEXT("Selection and old air/coolant targets invalidated immediately"),C->GetSelectedLayoutActors().IsEmpty()
		&& Sim->GetSnapshot().RackAirMassFlowKgs.IsEmpty() && C->GetLayoutUndoCount()==1);
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double RemovedW=BeforeW-Sim->GetSnapshot().TotalItHeatW;
	const double PowerError=FMath::Abs(RemovedW-ExpectedRemovedW)/ExpectedRemovedW;
	TestTrue(TEXT("IT drops by exactly both rack references at equal initial step/load"),PowerError<1e-9);
	double MaxResidual=0,MaxDuct=0,MaxDivergence=0;
	auto CheckPhysics=[&](int32 Expected,const TCHAR* Stage) {
		for (int32 Frame=0;Frame<20;++Frame) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Only present racks coupled to coolant/heat/air/duct"),S.RackCoolantMassFlowKgs.Num()==Expected
			&& S.RackHeatToAirW.Num()==Expected && S.RackAirMassFlowKgs.Num()==Expected && S.RackDuctMassFlowKgs.Num()==Expected);
		double DuctError=0;
		for (int32 I=0;I<FMath::Min(S.RackAirMassFlowKgs.Num(),S.RackDuctMassFlowKgs.Num());++I) {
			TestTrue(TEXT("Positive surviving rack airflow"),S.RackAirMassFlowKgs[I]>0);
			if (S.RackAirMassFlowKgs[I]>0) DuctError=FMath::Max(DuctError,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Global heat ledger meets existing normalized energy criterion"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9);
		TestTrue(TEXT("Duct closure meets existing per-rack mass flow criterion"),!S.bAirDuctsRejected && DuctError<1e-9);
		TestTrue(TEXT("Pressure converges within existing normalized divergence criterion"),S.bAirPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxResidual=FMath::Max(MaxResidual,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,DuctError);
		MaxDivergence=FMath::Max(MaxDivergence,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Batch %s: racks %d t %.2f s | global residual %.3e | duct relative %.3e | divergence %.3e"),Stage,Expected,S.SimTimeS,S.NormalizedEnergyResidual,DuctError,AS.NormalizedDivergence));
	};
	CheckPhysics(1,TEXT("delete"));
	Sim->EnterLayoutMode();
	TestTrue(TEXT("One Undo restores both racks"),C->TryUndoLayout()); CheckTopology(3);
	const int32 RestoredPipes=Room->GetMutableNetwork().GetNumPipes();
	TestEqual(TEXT("Undo restores the complete pipe inventory"),RestoredPipes,BeforePipes);
	for (const FString& Id:Ids) {
		auto* Restored=VrgRackLifecycle::Find(W,Id);
		if (TestNotNull(TEXT("Identity restored"),Restored)) TestEqual(TEXT("Undo does not copy simulated time"),Restored->GetSnapshot().SimTimeS,0.0);
	}
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double RestoreError=FMath::Abs(Sim->GetSnapshot().TotalItHeatW-BeforeW)/BeforeW;
	TestTrue(TEXT("Restored inventory matches baseline IT at same initial conditions"),RestoreError<1e-9);
	CheckPhysics(3,TEXT("undo"));
	Sim->EnterLayoutMode(); TestTrue(TEXT("One Redo deletes both racks again"),C->TryRedoLayout()); CheckTopology(1);
	TestEqual(TEXT("Redo removes the same pipe branches"),Room->GetMutableNetwork().GetNumPipes(),AfterPipes);
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double RedoError=FMath::Abs((BeforeW-Sim->GetSnapshot().TotalItHeatW)-ExpectedRemovedW)/ExpectedRemovedW;
	TestTrue(TEXT("Redo power removal matches both references"),RedoError<1e-9); CheckPhysics(1,TEXT("redo"));
	AddInfo(FString::Printf(TEXT("Batch coupling: pipes %d->%d->%d->%d | IT removed %.6f W = full %.6f + reduced %.6f W | removal/restore/redo relative %.3e/%.3e/%.3e | max energy %.3e duct %.3e divergence %.3e"),
		BeforePipes,AfterPipes,RestoredPipes,Room->GetMutableNetwork().GetNumPipes(),RemovedW,FullW,ReducedW,PowerError,RestoreError,RedoError,MaxResidual,MaxDuct,MaxDivergence));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
