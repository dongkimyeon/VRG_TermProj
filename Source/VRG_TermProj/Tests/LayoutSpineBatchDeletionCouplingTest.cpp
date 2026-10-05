#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgSpineLayoutState.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineBatchDeletionCouplingTest, "VRG.Interaction.Layout.SpineBatchDeletionPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineBatchDeletionCouplingTest::RunTest(const FString& Parameters)
{
	auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
	UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
	Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
	auto* A=W->SpawnActor<ASpineCabinetActor>(FVector(0,180,0),FRotator::ZeroRotator);
	auto Config=GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings(); Config.NumSwitches=2; Config.FirstSwitchSlot=5;
	// Synthetic nondefault authored configuration; no new physical defaults or fitting.
	auto* B=VrgSpineLifecycle::Spawn(W,TEXT("RuntimeSpineBatchPhysics"),FGuid::NewGuid().ToString(),FTransform(FVector(0,-180,0)),Config);
	const FString Ids[]={VrgSpineLifecycle::Identity(A),VrgSpineLifecycle::Identity(B)};
	const FName Names[]={A->GetFName(),B->GetFName()};
	const auto ConfigA=A->CaptureLayoutSettings(); const FSpineLayoutSettings Configs[]={ConfigA,Config};
	const FTransform Transforms[]={A->GetActorTransform(),B->GetActorTransform()};
	double ExpectedW=0; int32 Switches=0;
	for(auto* Spine : {A,B}) {
		Spine->EnsureThermalModel(); FRackThermalModel Ref; Ref.Initialize(Spine->GetMutableModel().GetParams(),FRackThermalState());
		Ref.Step(Spine->GetMutableInput(),Sim->FixedStepS); ExpectedW+=Ref.GetLastTotalHeatW(); Switches+=Spine->NumSwitches;
	}
	// Existing estimated electronics/fan split and public QM9700 input envelope; not a new fit.
	// https://networking-docs.nvidia.com/qm97x0hw/specifications
	TestTrue(TEXT("Combined spine IT plausibility"),ExpectedW>Switches*597.0 && ExpectedW<=Switches*747.0);
	Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	const int32 Pipes=Room->GetMutableNetwork().GetNumPipes(); double MaxPower=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto Physics=[&](bool Present,const TCHAR* Stage) {
		TestTrue(TEXT("Rack/CDU/hydraulic inventory unchanged"),Room->GetRoutedRacks().Num()==1 && Room->GetRowCdu(0)==Cdu && Room->GetMutableNetwork().GetNumPipes()==Pipes);
		TestEqual(TEXT("Only live patch devices"),Room->GetPatchDevices().Num(),Present?3:1);
		TestTrue(TEXT("Cable restoration or retained unconnected diagnostic"),Present
			? (!Room->GetPatchPlan().Cables.IsEmpty() && Room->GetPatchPlan().Unconnected.IsEmpty() && Room->GetPatchPlan().RejectedClaims==0)
			: (Room->GetPatchPlan().Cables.IsEmpty() && !Room->GetPatchPlan().Unconnected.IsEmpty()));
		Sim->EnterSimulationMode(); Sim->Tick(.021f); const double Decrease=BaselineW-Sim->GetSnapshot().TotalItHeatW;
		const double Power=FMath::Abs(Decrease-(Present?0:ExpectedW))/ExpectedW;
		TestTrue(TEXT("Same initial step whole group IT decrease"),Power<1e-9); MaxPower=FMath::Max(MaxPower,Power);
		for(int32 I=0;I<20;++I) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Only live heat/air/duct targets"),S.RackHeatToAirW.Num()==(Present?3:1)
			&& S.RackAirMassFlowKgs.Num()==(Present?3:1) && S.RackDuctMassFlowKgs.Num()==(Present?3:1));
		double Duct=0;
		for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) {
			TestTrue(TEXT("Positive airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
			if(I>0) TestEqual(TEXT("Spines remain air only"),S.RackCoolantMassFlowKgs[I],0.0);
		}
		TestTrue(TEXT("Global conservation and duct bounds"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && Duct<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected);
		TestTrue(TEXT("Pressure converges within divergence bound"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,Duct); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Spine batch deletion %s: decrease %.6fW expected %.6fW power %.3e global %.3e duct %.3e divergence %.3e t%.2fs"),Stage,Decrease,ExpectedW,Power,S.NormalizedEnergyResidual,Duct,AS.NormalizedDivergence,S.SimTimeS));
		Sim->EnterLayoutMode();
	};
	const FString Before=TEXT("automation_spine_batch_before_")+FGuid::NewGuid().ToString();
	const FString After=TEXT("automation_spine_batch_after_")+FGuid::NewGuid().ToString(); FString Summary;
	TestTrue(TEXT("Save before whole deletion"),VrgLayoutFile::Save(W,Before,Summary));
	const FVector Down(0,0,-1); C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down);
	C->SelectLayoutAlongRay(B->GetActorLocation()+FVector(0,0,500),Down,true);
	TestTrue(TEXT("Delete placed and runtime together"),C->TryDeleteLayoutSpine()); Physics(false,TEXT("delete"));
	TestTrue(TEXT("Only placed explicit deletion and one command"),W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Num()==1 && C->GetLayoutUndoCount()==1);
	TestTrue(TEXT("Save deletion"),VrgLayoutFile::Save(W,After,Summary));
	TestTrue(TEXT("Whole Undo"),C->TryUndoLayout()); Physics(true,TEXT("undo"));
	TestTrue(TEXT("Whole Redo"),C->TryRedoLayout()); Physics(false,TEXT("redo"));
	TestTrue(TEXT("Load original then deleted inventory"),VrgLayoutFile::Load(W,Before,Summary) && VrgLayoutFile::Load(W,After,Summary));
	TestTrue(TEXT("Both deleted and records cleared"),!VrgSpineLifecycle::Find(W,Ids[0]) && !VrgSpineLifecycle::Find(W,Ids[1]) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Repeat deletion load no duplicate changes"),VrgLayoutFile::Load(W,After,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거"))); Physics(false,TEXT("deleted-load"));
	TestTrue(TEXT("Load whole original and repeat"),VrgLayoutFile::Load(W,Before,Summary) && VrgLayoutFile::Load(W,Before,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
	for(int32 I=0;I<2;++I) {
		auto* Spine=VrgSpineLifecycle::Find(W,Ids[I]); TestTrue(TEXT("Loaded config/identity/provenance/fresh state"),Spine && Spine->GetFName()==Names[I]
			&& Spine->CaptureLayoutSettings()==Configs[I] && Spine->bRuntimeLayoutSpine==(I==1)
			&& Spine->GetActorTransform().Equals(Transforms[I],1e-9) && Spine->GetMutableModel().GetEnergyInJ()==0);
	}
	Physics(true,TEXT("original-load"));
	UWorld* Fresh=NewWorld(); FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
	P.Name=Cdu->GetFName(); Fresh->SpawnActor<ACoolingDistributionUnitActor>(P); P.Name=Names[0]; Fresh->SpawnActor<ASpineCabinetActor>(P);
	TestTrue(TEXT("Fresh world deletion/repeat"),VrgLayoutFile::Load(Fresh,After,Summary) && VrgLayoutFile::Load(Fresh,After,Summary)
		&& !VrgSpineLifecycle::Find(Fresh,Ids[0]) && !VrgSpineLifecycle::Find(Fresh,Ids[1]));
	TestTrue(TEXT("Fresh world restore/repeat"),VrgLayoutFile::Load(Fresh,Before,Summary) && VrgLayoutFile::Load(Fresh,Before,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
	for(int32 I=0;I<2;++I) { const auto* Spine=VrgSpineLifecycle::Find(Fresh,Ids[I]);
		TestTrue(TEXT("Fresh restored provenance/settings/transform"),Spine && Spine->GetFName()==Names[I] && Spine->bRuntimeLayoutSpine==(I==1)
			&& Spine->CaptureLayoutSettings()==Configs[I] && Spine->GetActorTransform().Equals(Transforms[I],1e-9)); }
	AddInfo(FString::Printf(TEXT("Spine batch deletion coupling maxima: reference %.6fW power %.3e energy %.3e duct %.3e divergence %.3e; placed4 + runtime2 switches, delete/Undo/Redo/Load, fresh/repeat load; hydraulics unchanged"),ExpectedW,MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Before)); IFileManager::Get().Delete(*VrgLayoutFile::PathFor(After));
	Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
