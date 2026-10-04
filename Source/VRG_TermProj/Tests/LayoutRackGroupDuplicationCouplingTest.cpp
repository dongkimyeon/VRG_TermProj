#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackGroupDuplicationCouplingTest,"VRG.Interaction.Layout.RackGroupDuplicationCoupling",
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
	Sim->EnterLayoutMode();
	TArray<ARackThermalActor*> Sources;
	for(int32 I=0;I<2;++I) {
		ARackThermalActor* Rack=nullptr;
		if(I==0) Rack=W->SpawnActor<ARackThermalActor>();
		else {
			C->BeginRackAddition(); C->UpdateRackAdditionAlongRay(FVector(0,85,500),FVector(0,0,-1));
			Rack=C->ConfirmRackAddition();
		}
		if(!TestNotNull(TEXT("Existing/runtime group source"),Rack)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
		Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
		Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
		Sources.Add(Rack);
	}
	// Synthetic reduced load; no hardware defaults or physical calibration introduced.
	Sources[1]->Input.Utilization=.7; Sources[1]->Input.SledDocked[2]=false; Sources[1]->Input.ComponentHeatW[0]*=1.05;
	const FRackLayoutSettings Configs[]={Sources[0]->CaptureLayoutSettings(),Sources[1]->CaptureLayoutSettings()};
	double ReferenceW[2];
	for(int32 I=0;I<2;++I) {
		FRackThermalModel Model; Model.Initialize(Configs[I].Params,FRackThermalState());
		Model.Step(Configs[I].Input,Sim->FixedStepS); ReferenceW[I]=Model.GetLastTotalHeatW();
	}
	const double ExpectedW=ReferenceW[0]+ReferenceW[1];
	// Existing NVIDIA ~120 kW +/-5% envelope, not a new physical estimate. Preset source remains unchanged.
	TestTrue(TEXT("Full/reduced rack plausibility"),ReferenceW[0]>1.14e5 && ReferenceW[0]<1.26e5
		&& ReferenceW[1]>0 && ReferenceW[1]<ReferenceW[0]);
	auto Count=[&]() { int32 N=0; for(TActorIterator<ARackThermalActor> It(W);It;++It) ++N; return N; };
	auto RowCount=[&]() { TSet<int32> Rows; for(int32 Index:Room->GetRoutedRackRows()) Rows.Add(Index); return Rows.Num(); };
	auto CheckTopology=[&](int32 Expected) {
		TestTrue(TEXT("Inventory/routed branches/patch agree"),Count()==Expected && Room->GetRoutedRacks().Num()==Expected
			&& Room->GetRackBranchPipeIndices().Num()==Expected && Room->GetPatchDevices().Num()==Expected);
		TSet<int32> Branches;
		for(int32 Index:Room->GetRackBranchPipeIndices()) {
			TestTrue(TEXT("Distinct valid coolant branch"),Index>=0 && Index<Room->GetMutableNetwork().GetNumPipes() && !Branches.Contains(Index));
			Branches.Add(Index);
		}
		for(int32 Row:Room->GetRoutedRackRows()) TestTrue(TEXT("Every rack row assigned to the existing CDU"),Row>=0 && Room->GetRowCdu(Row)==Cdu);
	};
	Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double BaselineW=Sim->GetSnapshot().TotalItHeatW;
	const int32 BasePipes=Room->GetMutableNetwork().GetNumPipes(); CheckTopology(2);
	Sim->EnterLayoutMode(); C->ClearLayoutHistory();
	for(int32 I=0;I<2;++I) C->SelectLayoutAlongRay(Sources[I]->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1),I>0);
	TestTrue(TEXT("Begin group duplication"),C->BeginRackDuplication());
	C->UpdateRackAdditionAlongRay(FVector(-250,0,500),FVector(0,0,-1));
	TestNotNull(TEXT("Confirm both racks together"),C->ConfirmRackAddition()); CheckTopology(4);
	const int32 ClonePipes=Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("New row and pipe branches included"),RowCount()==2 && ClonePipes>BasePipes);
	TArray<FString> CloneIds; TArray<FName> CloneNames; TArray<FTransform> CloneTransforms;
	for(AActor* Actor:C->GetSelectedLayoutActors()) {
		auto* Rack=CastChecked<ARackThermalActor>(Actor); CloneIds.Add(VrgRackLifecycle::Identity(Rack)); CloneNames.Add(Rack->GetFName()); CloneTransforms.Add(Rack->GetActorTransform());
	}
	TestTrue(TEXT("All clones share one history command"),CloneIds.Num()==2 && C->GetLayoutUndoCount()==1);
	if(CloneIds.Num()!=2) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	double MaxPowerError=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto RunPhysics=[&](int32 Expected,const TCHAR* Stage) {
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		const double TargetW=Expected==4 ? BaselineW+ExpectedW:BaselineW;
		const double PowerError=FMath::Abs(Sim->GetSnapshot().TotalItHeatW-TargetW)/ExpectedW;
		MaxPowerError=FMath::Max(MaxPowerError,PowerError);
		TestTrue(TEXT("IT inventory delta matches two independent references at same initial step"),PowerError<1e-9);
		for(int32 Frame=0;Frame<20;++Frame) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Only live racks coupled to heat/coolant/air/duct"),S.RackCoolantMassFlowKgs.Num()==Expected
			&& S.RackHeatToAirW.Num()==Expected && S.RackAirMassFlowKgs.Num()==Expected && S.RackDuctMassFlowKgs.Num()==Expected);
		double DuctError=0;
		for(int32 I=0;I<FMath::Min(S.RackAirMassFlowKgs.Num(),S.RackDuctMassFlowKgs.Num());++I) {
			TestTrue(TEXT("Positive rack airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) DuctError=FMath::Max(DuctError,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Existing global energy criterion"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9);
		TestTrue(TEXT("Existing duct closure criterion"),!S.bAirDuctsRejected && !AS.bDuctsRejected && DuctError<1e-9);
		TestTrue(TEXT("Existing pressure/divergence criterion"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,DuctError); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Group %s: racks%d t%.2fs power relative %.3e | global %.3e duct %.3e divergence %.3e"),Stage,Expected,S.SimTimeS,PowerError,S.NormalizedEnergyResidual,DuctError,AS.NormalizedDivergence));
		Sim->EnterLayoutMode();
	};
	RunPhysics(4,TEXT("confirm"));
	TestTrue(TEXT("Undo entire duplicate"),C->TryUndoLayout()); CheckTopology(2);
	TestEqual(TEXT("Undo removes both pipe branches/row"),Room->GetMutableNetwork().GetNumPipes(),BasePipes);
	for(const auto& Id:CloneIds) TestNull(TEXT("Clone removed by Undo"),VrgRackLifecycle::Find(W,Id));
	RunPhysics(2,TEXT("undo"));
	TestTrue(TEXT("Redo entire duplicate"),C->TryRedoLayout()); CheckTopology(4);
	TestEqual(TEXT("Redo restores pipe inventory"),Room->GetMutableNetwork().GetNumPipes(),ClonePipes);
	RunPhysics(4,TEXT("redo"));
	FString Summary; const FString File=TEXT("automation_group_duplicate_")+FGuid::NewGuid().ToString();
	TestTrue(TEXT("Save existing v2 with both clones"),VrgLayoutFile::Save(W,File,Summary));
	TestTrue(TEXT("Undo before reload"),C->TryUndoLayout()); CheckTopology(2);
	TestTrue(TEXT("Load recreates both clones"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(4);
	TestTrue(TEXT("Load clears both history stacks"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	for(int32 I=0;I<CloneIds.Num();++I) {
		auto* Rack=VrgRackLifecycle::Find(W,CloneIds[I]);
		if(!TestNotNull(TEXT("Saved clone identity restored"),Rack)) continue;
		const auto Config=Rack->CaptureLayoutSettings();
		TestTrue(TEXT("Saved name/settings/full transform restored"),Rack->GetFName()==CloneNames[I] && FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&Configs[I],0)
			&& Rack->GetActorTransform().Equals(CloneTransforms[I],1e-9) && Rack->GetSnapshot().SimTimeS==0 && Rack->GetMutableModel().GetEnergyInJ()==0);
	}
	// Preserve the remote Save -> group Delete -> Load path alongside the Undo -> Load path.
	for(int32 I=0;I<CloneIds.Num();++I) C->SelectLayoutAlongRay(VrgRackLifecycle::Find(W,CloneIds[I])->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1),I>0);
	TestTrue(TEXT("Delete saved clone group before reload"),C->TryDeleteLayoutRack()); CheckTopology(2);
	TestTrue(TEXT("Load restores group after Delete"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(4);
	TestTrue(TEXT("Load clears deletion history"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Repeated Load succeeds"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(4);
	TestTrue(TEXT("Repeated Load creates/removes none"),Summary.Contains(TEXT("랙0 생성/0 제거")));
	RunPhysics(4,TEXT("reload"));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	AddInfo(FString::Printf(TEXT("Group coupling/save: pipes %d->%d->%d->%d | IT pair %.6f W=%.6f+%.6f W | max power relative %.3e global %.3e duct %.3e divergence %.3e | v2 repeat count4, histories0."),
		BasePipes,ClonePipes,BasePipes,ClonePipes,ExpectedW,ReferenceW[0],ReferenceW[1],MaxPowerError,MaxEnergy,MaxDuct,MaxDiv));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
