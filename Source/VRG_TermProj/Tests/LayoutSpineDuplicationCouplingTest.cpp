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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineDuplicationCouplingTest, "VRG.Interaction.Layout.SpineDuplicationPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineDuplicationCouplingTest::RunTest(const FString& Parameters)
{
	double MaxPower=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
	for(int32 Case=0;Case<2;++Case)
	{
		UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
		auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
		auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
		auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
		Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
		Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
		Sim->EnterLayoutMode(); const FVector Down(0,0,-1);
		auto Select=[&](ASpineCabinetActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
		// Retain an unrelated explicit placed deletion across duplicate Undo/Redo and save/load.
		auto* Deleted=W->SpawnActor<ASpineCabinetActor>(FVector(500,500,0),FRotator::ZeroRotator);
		const FString DeletedId=VrgSpineLifecycle::Identity(Deleted); const FName DeletedName=Deleted->GetFName();
		Select(Deleted); TestTrue(TEXT("Existing deletion before duplication"),C->TryDeleteLayoutSpine()); C->ClearLayoutHistory();
		ASpineCabinetActor* Source=nullptr;
		if(Case==0) Source=W->SpawnActor<ASpineCabinetActor>(FVector(0,180,0),FRotator::ZeroRotator);
		else {
			auto Config=GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings(); Config.NumSwitches=2; Config.FirstSwitchSlot=5;
			// Synthetic nondefault authored configuration, no new hardware physical estimate.
			Source=VrgSpineLifecycle::Spawn(W,TEXT("RuntimeSpineOriginal"),FGuid::NewGuid().ToString(),FTransform(FVector(0,180,0)),Config);
		}
		Source->EnsureThermalModel(); const auto Config=Source->CaptureLayoutSettings(); const auto OriginalTransform=Source->GetActorTransform();
		const FString SourceId=VrgSpineLifecycle::Identity(Source); const FName SourceName=Source->GetFName();
		FRackThermalModel Ref; Ref.Initialize(Source->GetMutableModel().GetParams(),FRackThermalState());
		Ref.Step(Source->GetMutableInput(),Sim->FixedStepS); const double ExpectedW=Ref.GetLastTotalHeatW();
		// Existing estimated electronics/fan split and QM9700 passive-cable input reference, not a new fit.
		// https://networking-docs.nvidia.com/qm97x0hw/specifications
		TestTrue(TEXT("Spine IT plausibility"),ExpectedW>Config.NumSwitches*597.0 && ExpectedW<=Config.NumSwitches*747.0);
		Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaseW=Sim->GetSnapshot().TotalItHeatW;
		Sim->EnterLayoutMode(); const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
		Select(Source); TestTrue(TEXT("Duplicate source"),C->BeginSpineDuplication());
		// Existing patch assignment consumes ports in ascending row AlongDir order.
		// Put the clone before the original using the actual row axis, not an assumed +Y.
		const auto Rows=Room->GetRackRowsForPlacement(nullptr);
		const FVector CloneLocation=Source->GetActorLocation()-Rows[0].AlongDir*120.0;
		C->UpdateRackAdditionAlongRay(CloneLocation+FVector(0,0,500),Down); auto* Clone=C->ConfirmSpineAddition();
		if(!TestNotNull(TEXT("Duplicate confirmed"),Clone)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); break; }
		const FString CloneId=VrgSpineLifecycle::Identity(Clone); const FName CloneName=Clone->GetFName(); const FTransform CloneTransform=Clone->GetActorTransform();
		TestTrue(TEXT("Original unchanged and clone independent"),CloneId!=SourceId && Clone->bRuntimeLayoutSpine
			&& Clone->CaptureLayoutSettings()==Config && Source->CaptureLayoutSettings()==Config
			&& Source->GetActorTransform().Equals(OriginalTransform,0) && Clone->GetMutableModel().GetEnergyInJ()==0);
		auto Physics=[&](bool Present,const TCHAR* Stage) {
			TestTrue(TEXT("Rack row/CDU/hydraulics unchanged"),Room->GetRoutedRacks().Num()==1 && Room->GetRowCdu(0)==Cdu
				&& Room->GetMutableNetwork().GetNumPipes()==Pipes);
			TestEqual(TEXT("Patch device inventory"),Room->GetPatchDevices().Num(),Present?3:2);
			TestTrue(TEXT("Existing assigned row remains connected"),Room->GetPatchPlan().Unconnected.IsEmpty() && Room->GetPatchPlan().RejectedClaims==0);
			if(Present) {
				const auto* Live=VrgSpineLifecycle::Find(W,CloneId); int32 Device=INDEX_NONE;
				for(int32 I=0;I<Room->GetPatchDevices().Num();++I) if(Room->GetPatchDevices()[I].Label==Live->GetActorNameOrLabel()) Device=I;
				TestTrue(TEXT("Duplicate receives cables through existing row assignment"),Device!=INDEX_NONE
					&& Room->GetPatchPlan().Cables.ContainsByPredicate([&](const auto& Cable) { return Cable.FromDevice==Device || Cable.ToDevice==Device; }));
			}
			Sim->EnterSimulationMode(); Sim->Tick(.021f);
			const double Increase=Sim->GetSnapshot().TotalItHeatW-BaseW;
			const double Power=FMath::Abs(Increase-(Present?ExpectedW:0))/ExpectedW;
			TestTrue(TEXT("Same initial step one spine power increment"),Power<1e-9); MaxPower=FMath::Max(MaxPower,Power);
			for(int32 I=0;I<20;++I) Sim->Tick(.04f);
			const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
			TestTrue(TEXT("Only live equipment heat/air/duct targets"),S.RackHeatToAirW.Num()==(Present?3:2)
				&& S.RackAirMassFlowKgs.Num()==(Present?3:2) && S.RackDuctMassFlowKgs.Num()==(Present?3:2));
			double Duct=0;
			for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) {
				TestTrue(TEXT("Positive equipment airflow"),S.RackAirMassFlowKgs[I]>0);
				if(S.RackAirMassFlowKgs[I]>0) Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
				if(I>0) TestEqual(TEXT("Air-only spines have no coolant"),S.RackCoolantMassFlowKgs[I],0.0);
			}
			TestTrue(TEXT("Global ledger and duct bounds"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && Duct<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected);
			TestTrue(TEXT("Pressure converged and divergence bound"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
			MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,Duct); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
			AddInfo(FString::Printf(TEXT("Spine clone case%d %s: increase %.6fW reference %.6fW error %.3e global %.3e duct %.3e divergence %.3e t%.2fs"),Case,Stage,Increase,ExpectedW,Power,S.NormalizedEnergyResidual,Duct,AS.NormalizedDivergence,S.SimTimeS));
			Sim->EnterLayoutMode();
		};
		Physics(true,TEXT("confirm")); TestTrue(TEXT("Undo duplicate"),C->TryUndoLayout()); Physics(false,TEXT("undo"));
		TestTrue(TEXT("Redo duplicate"),C->TryRedoLayout()); Physics(true,TEXT("redo"));
		const FString File=TEXT("automation_spine_clone_")+FGuid::NewGuid().ToString(); FString Summary;
		TestTrue(TEXT("Save duplicate using existing v2"),VrgLayoutFile::Save(W,File,Summary));
		TestTrue(TEXT("Undo then reload duplicate"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
		Clone=VrgSpineLifecycle::Find(W,CloneId);
		TestTrue(TEXT("Reload identity/config/transform/fresh model/history"),Clone && Clone->GetFName()==CloneName
			&& Clone->CaptureLayoutSettings()==Config && Clone->GetActorTransform().Equals(CloneTransform,1e-9)
			&& Clone->GetMutableModel().GetEnergyInJ()==0 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
		TestTrue(TEXT("Repeated reload no duplicate"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
		TestTrue(TEXT("Unrelated deletion intent retained"),W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Contains(DeletedId)); Physics(true,TEXT("reload"));
		UWorld* Fresh=NewWorld();
		// Latest v2 room/ceiling settings require the same owner as the real level.
		Fresh->SpawnActor<ADataCenterSimulationActor>()->EnterLayoutMode();
		FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
		P.Name=Cdu->GetFName(); Fresh->SpawnActor<ACoolingDistributionUnitActor>(P);
		if(Case==0) { P.Name=SourceName; Fresh->SpawnActor<ASpineCabinetActor>(P); }
		P.Name=DeletedName; Fresh->SpawnActor<ASpineCabinetActor>(P);
		TestTrue(TEXT("Fresh-world load and repeated load"),VrgLayoutFile::Load(Fresh,File,Summary) && VrgLayoutFile::Load(Fresh,File,Summary));
		const auto* FreshClone=VrgSpineLifecycle::Find(Fresh,CloneId); const auto* FreshSource=VrgSpineLifecycle::Find(Fresh,SourceId);
		TestTrue(TEXT("Fresh original/clone provenance, identity, config and deletion"),FreshClone && FreshSource && FreshClone!=FreshSource
			&& FreshClone->bRuntimeLayoutSpine && FreshSource->bRuntimeLayoutSpine==(Case==1)
			&& FreshClone->CaptureLayoutSettings()==Config && FreshSource->CaptureLayoutSettings()==Config
			&& FreshClone->GetActorTransform().Equals(CloneTransform,1e-9) && !VrgSpineLifecycle::Find(Fresh,DeletedId));
		IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File)); Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh);
		W->DestroyWorld(false); GEngine->DestroyWorldContext(W);
	}
	AddInfo(FString::Printf(TEXT("Spine duplication coupling maxima: power %.3e energy %.3e duct %.3e divergence %.3e | placed/default + runtime/nondefault, confirm/Undo/Redo/Load; no hydraulic addition"),MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	return true;
}
#endif
