#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgSpineLifecycle.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineBatchDuplicationCouplingTest, "VRG.Interaction.Layout.SpineBatchDuplicationPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineBatchDuplicationCouplingTest::RunTest(const FString& Parameters)
{
	auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
	UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
	Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
	auto* A=W->SpawnActor<ASpineCabinetActor>(FVector(0,180,0),FRotator::ZeroRotator);
	// Synthetic authored configuration and placement, not new hardware physical values.
	auto Config=GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings(); Config.NumSwitches=2; Config.FirstSwitchSlot=5;
	auto* B=VrgSpineLifecycle::Spawn(W,TEXT("RuntimeGroupSpineOriginal"),FGuid::NewGuid().ToString(),FTransform(FVector(0,-180,0)),Config);
	const FString SourceIds[]={VrgSpineLifecycle::Identity(A),VrgSpineLifecycle::Identity(B)};
	const FName SourceNames[]={A->GetFName(),B->GetFName()};
	const FSpineLayoutSettings Configs[]={A->CaptureLayoutSettings(),B->CaptureLayoutSettings()};
	const FTransform Originals[]={A->GetActorTransform(),B->GetActorTransform()}; double ExpectedW=0; int32 Switches=0;
	for(auto* Spine : {A,B}) { Spine->EnsureThermalModel(); FRackThermalModel Ref;
		Ref.Initialize(Spine->GetMutableModel().GetParams(),FRackThermalState()); Ref.Step(Spine->GetMutableInput(),Sim->FixedStepS);
		ExpectedW+=Ref.GetLastTotalHeatW(); Switches+=Spine->NumSwitches; }
	// Reuse the existing estimated electronics/fan split and QM9700 public input reference.
	// https://networking-docs.nvidia.com/qm97x0hw/specifications -- no new fit/defaults.
	TestTrue(TEXT("Spine group IT plausibility"),ExpectedW>Switches*597.0 && ExpectedW<=Switches*747.0);
	Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	const int32 Pipes=Room->GetMutableNetwork().GetNumPipes(); const FVector Down(0,0,-1);
	C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); C->SelectLayoutAlongRay(B->GetActorLocation()+FVector(0,0,500),Down,true);
	TestTrue(TEXT("Begin whole group"),C->BeginSpineDuplication());
	// Put clones ahead of originals in the actual patch row axis, with no overlaps.
	const auto Rows=Room->GetRackRowsForPlacement(nullptr);
	C->UpdateRackAdditionAlongRay(A->GetActorLocation()-Rows[0].AlongDir*450.0+FVector(0,0,500),Down);
	if(!TestNotNull(TEXT("Confirm whole group"),C->ConfirmSpineAddition())) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	FString Ids[2]; FName Names[2]; FTransform Transforms[2]; int32 Index=0;
	for(auto* Actor : C->GetSelectedLayoutActors()) { auto* Spine=CastChecked<ASpineCabinetActor>(Actor);
		Ids[Index]=VrgSpineLifecycle::Identity(Spine); Names[Index]=Spine->GetFName(); Transforms[Index]=Spine->GetActorTransform();
		TestTrue(TEXT("Clone fresh config and independent ID"),Spine->CaptureLayoutSettings()==Configs[Index] && Spine->bRuntimeLayoutSpine
			&& Ids[Index]!=SourceIds[0] && Ids[Index]!=SourceIds[1] && Spine->GetMutableModel().GetEnergyInJ()==0); ++Index; }
	TestTrue(TEXT("Both independent clones"),Index==2 && Ids[0]!=Ids[1]);
	double MaxPower=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto Physics=[&](bool Present,const TCHAR* Stage) {
		TestTrue(TEXT("Rack/CDU/hydraulics unchanged"),Room->GetRoutedRacks().Num()==1 && Room->GetRowCdu(0)==Cdu && Room->GetMutableNetwork().GetNumPipes()==Pipes);
		TestEqual(TEXT("Only live patch devices"),Room->GetPatchDevices().Num(),Present?5:3);
		TestTrue(TEXT("Existing row remains assigned"),Room->GetPatchPlan().Unconnected.IsEmpty() && Room->GetPatchPlan().RejectedClaims==0);
		if(Present) { const auto* First=VrgSpineLifecycle::Find(W,Ids[0]); int32 Device=INDEX_NONE;
			for(int32 I=0;I<Room->GetPatchDevices().Num();++I) if(Room->GetPatchDevices()[I].Label==First->GetActorNameOrLabel()) Device=I;
			TestTrue(TEXT("Clone gets cable by existing assignment order"),Device!=INDEX_NONE && Room->GetPatchPlan().Cables.ContainsByPredicate([&](const auto& Cable) { return Cable.FromDevice==Device || Cable.ToDevice==Device; })); }
		Sim->EnterSimulationMode(); Sim->Tick(.021f); const double Increase=Sim->GetSnapshot().TotalItHeatW-BaselineW;
		const double Power=FMath::Abs(Increase-(Present?ExpectedW:0))/ExpectedW; TestTrue(TEXT("Whole clone IT increment"),Power<1e-9); MaxPower=FMath::Max(MaxPower,Power);
		for(int32 I=0;I<20;++I) Sim->Tick(.04f); const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Only live heat/air/duct targets"),S.RackHeatToAirW.Num()==(Present?5:3) && S.RackAirMassFlowKgs.Num()==(Present?5:3) && S.RackDuctMassFlowKgs.Num()==(Present?5:3));
		double Duct=0; for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) {
			TestTrue(TEXT("Positive airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
			if(I>0) TestEqual(TEXT("Air-only spines"),S.RackCoolantMassFlowKgs[I],0.0); }
		TestTrue(TEXT("Global ledger and duct bounds"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && Duct<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected);
		TestTrue(TEXT("Pressure converged"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,Duct); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Spine group clone %s: increase %.6fW expected %.6fW power %.3e global %.3e duct %.3e divergence %.3e t%.2fs"),Stage,Increase,ExpectedW,Power,S.NormalizedEnergyResidual,Duct,AS.NormalizedDivergence,S.SimTimeS)); Sim->EnterLayoutMode();
	};
	Physics(true,TEXT("confirm")); TestTrue(TEXT("Whole Undo"),C->TryUndoLayout()); Physics(false,TEXT("undo"));
	TestTrue(TEXT("Whole Redo"),C->TryRedoLayout()); Physics(true,TEXT("redo"));
	const FString File=TEXT("automation_spine_group_clone_")+FGuid::NewGuid().ToString(); FString Summary;
	TestTrue(TEXT("Existing v2 save"),VrgLayoutFile::Save(W,File,Summary));
	TestTrue(TEXT("Undo then load both clones"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
	TestTrue(TEXT("Repeated load no duplicates/history"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	for(int32 I=0;I<2;++I) { auto* Clone=VrgSpineLifecycle::Find(W,Ids[I]); const auto* Source=VrgSpineLifecycle::Find(W,SourceIds[I]);
		TestTrue(TEXT("Reload clone identity/config/transform/fresh state and unchanged source"),Clone && Source && Clone->GetFName()==Names[I]
			&& Clone->CaptureLayoutSettings()==Configs[I] && Clone->GetActorTransform().Equals(Transforms[I],1e-9) && Clone->GetMutableModel().GetEnergyInJ()==0
			&& Source->GetFName()==SourceNames[I] && Source->CaptureLayoutSettings()==Configs[I] && Source->GetActorTransform().Equals(Originals[I],1e-9)); }
	Physics(true,TEXT("reload"));
	UWorld* Fresh=NewWorld(); FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
	P.Name=Cdu->GetFName(); Fresh->SpawnActor<ACoolingDistributionUnitActor>(P); P.Name=SourceNames[0]; Fresh->SpawnActor<ASpineCabinetActor>(P);
	TestTrue(TEXT("Fresh load/repeat"),VrgLayoutFile::Load(Fresh,File,Summary) && VrgLayoutFile::Load(Fresh,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
	int32 Count=0; for(TActorIterator<ASpineCabinetActor> It(Fresh);It;++It) ++Count; TestEqual(TEXT("Fresh inventory exactly four"),Count,4);
	for(int32 I=0;I<2;++I) { const auto* Clone=VrgSpineLifecycle::Find(Fresh,Ids[I]); const auto* Source=VrgSpineLifecycle::Find(Fresh,SourceIds[I]);
		TestTrue(TEXT("Fresh provenance/config/transform/identity"),Clone && Source && Clone->bRuntimeLayoutSpine && Source->bRuntimeLayoutSpine==(I==1)
			&& Clone->GetFName()==Names[I] && Clone->CaptureLayoutSettings()==Configs[I] && Clone->GetActorTransform().Equals(Transforms[I],1e-9)
			&& Source->GetFName()==SourceNames[I] && Source->CaptureLayoutSettings()==Configs[I] && Source->GetActorTransform().Equals(Originals[I],1e-9)); }
	AddInfo(FString::Printf(TEXT("Spine group clone maxima: reference %.6fW power %.3e energy %.3e duct %.3e divergence %.3e; placed4/runtime2 sources, confirm/Undo/Redo/Load, fresh/repeat load; no hydraulic change"),ExpectedW,MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File)); Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
