#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CrahUnitActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGroupRowSnapPersistenceTest,"VRG.Interaction.Layout.RackGroupRowSnapPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutGroupRowSnapPersistenceTest::RunTest(const FString& Parameters)
{
	UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>();
	TArray<ARackThermalActor*> Racks;
	const FVector Positions[]={FVector(0,0,0),FVector(0,80,0),FVector(400,200,7),FVector(400,280,13)};
	for(const FVector& Position:Positions) {
		auto* Rack=W->SpawnActor<ARackThermalActor>(Position,FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
		Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
		Racks.Add(Rack);
	}
	// Synthetic control variation only; hardware parameters and preset sources stay unchanged.
	Racks[3]->Input.Utilization=.7; Racks[3]->Input.SledDocked[2]=false;
	Racks[3]->SetActorScale3D(FVector(1,.9,1.1));
	Room->RackPitchM=.66; Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	const FVector Down(0,0,-1);
	auto* M=Racks[2]; auto* N=Racks[3];
	const FTransform Starts[]={M->GetActorTransform(),N->GetActorTransform()};
	const FRackLayoutSettings Settings[]={M->CaptureLayoutSettings(),N->CaptureLayoutSettings()};
	auto RowCount=[&]() { TSet<int32> Rows; for(int32 Index:Room->GetRoutedRackRows()) Rows.Add(Index); return Rows.Num(); };
	auto CheckTopology=[&](int32 ExpectedRows) {
		int32 Count=0; for(TActorIterator<ARackThermalActor> It(W);It;++It) ++Count;
		TestTrue(TEXT("All four racks retain routing/patch membership"),Count==4 && Room->GetRoutedRacks().Num()==4 && Room->GetRackBranchPipeIndices().Num()==4 && Room->GetPatchDevices().Num()==4 && RowCount()==ExpectedRows);
		TSet<int32> Branches;
		for(int32 Index:Room->GetRackBranchPipeIndices()) {
			TestTrue(TEXT("Four independent valid coolant branches"),Index>=0 && Index<Room->GetMutableNetwork().GetNumPipes() && !Branches.Contains(Index)); Branches.Add(Index);
		}
		for(int32 Row:Room->GetRoutedRackRows()) TestTrue(TEXT("Reference CDU assignment retained"),Room->GetRowCdu(Row)==Cdu);
	};
	CheckTopology(2); const int32 BasePipes=Room->GetMutableNetwork().GetNumPipes();
	Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineW=Sim->GetSnapshot().TotalItHeatW;
	// Three full-load racks use the existing ~120kW +/-5% envelope; fourth rack is positive reduced load.
	TestTrue(TEXT("Existing rack power plausibility bound"),BaselineW>3*1.14e5 && BaselineW<4*1.26e5);
	Sim->EnterLayoutMode(); C->ClearLayoutHistory();
	auto Begin=[&]() {
		C->SelectLayoutAlongRay(M->GetActorLocation()+FVector(0,0,500),Down);
		C->SelectLayoutAlongRay(N->GetActorLocation()+FVector(0,0,500),Down,true);
		C->BeginLayoutDragAlongRay(M->GetActorLocation()+FVector(0,0,500),Down);
		C->UpdateLayoutDragAlongRay(FVector(9,151,507),Down);
	};
	Begin(); C->EndLayoutDrag(false); CheckTopology(2);
	TestEqual(TEXT("Cancel creates no record"),C->GetLayoutUndoCount(),0);
	Begin(); C->EndLayoutDrag(true);
	// This fixture does not tick the world's room/mesh debounce. Exercise its existing explicit rebuild path.
	Room->RebuildRoomNow(); CheckTopology(1);
	TestEqual(TEXT("Group attachment is one command"),C->GetLayoutUndoCount(),1);
	const FTransform Attached[]={M->GetActorTransform(),N->GetActorTransform()};
	TestTrue(TEXT("Attachment uses observed .80m pitch, preserves height and scale"),Attached[0].GetLocation().Equals(FVector(0,160,7),1e-9)
		&& Attached[1].GetLocation().Equals(FVector(0,240,13),1e-9) && Attached[1].GetScale3D()==Starts[1].GetScale3D());
	const int32 JoinedPipes=Room->GetMutableNetwork().GetNumPipes();
	double MaxCm=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0,MaxPower=0;
	auto CheckTransforms=[&](const FTransform* Expected) {
		for(int32 I=0;I<2;++I) {
			auto* Rack=I==0?M:N; MaxCm=FMath::Max(MaxCm,FVector::Distance(Rack->GetActorLocation(),Expected[I].GetLocation()));
			TestTrue(TEXT("Full group transform restored"),Rack->GetActorTransform().Equals(Expected[I],1e-9));
		}
	};
	auto RunPhysics=[&](const TCHAR* Stage) {
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		const double PowerError=FMath::Abs(Sim->GetSnapshot().TotalItHeatW-BaselineW)/BaselineW;
		TestTrue(TEXT("Same initial step IT unchanged by row attachment"),PowerError<1e-9); MaxPower=FMath::Max(MaxPower,PowerError);
		for(int32 Frame=0;Frame<20;++Frame) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("All live racks coupled to heat/coolant/air/duct"),S.RackCoolantMassFlowKgs.Num()==4 && S.RackHeatToAirW.Num()==4 && S.RackAirMassFlowKgs.Num()==4 && S.RackDuctMassFlowKgs.Num()==4);
		double DuctError=0;
		for(int32 I=0;I<FMath::Min(S.RackAirMassFlowKgs.Num(),S.RackDuctMassFlowKgs.Num());++I) {
			TestTrue(TEXT("Positive rack airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) DuctError=FMath::Max(DuctError,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Existing global conservation/duct criteria"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected && DuctError<1e-9);
		TestTrue(TEXT("Existing pressure/divergence criteria"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,DuctError); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Group row %s: t%.2fs IT error %.3e global %.3e duct %.3e divergence %.3e"),Stage,S.SimTimeS,PowerError,S.NormalizedEnergyResidual,DuctError,AS.NormalizedDivergence));
		Sim->EnterLayoutMode();
	};
	RunPhysics(TEXT("attach"));
	TestTrue(TEXT("Undo restores whole source row"),C->TryUndoLayout()); CheckTransforms(Starts); CheckTopology(2);
	TestEqual(TEXT("Undo restores original pipe inventory"),Room->GetMutableNetwork().GetNumPipes(),BasePipes);
	auto* Block=W->SpawnActor<ACrahUnitActor>(Attached[1].GetLocation(),FRotator::ZeroRotator);
	TestFalse(TEXT("Second destination conflict refuses whole Redo"),C->TryRedoLayout()); CheckTransforms(Starts);
	TestTrue(TEXT("Rejected Redo keeps command and topology"),C->GetLayoutRedoCount()==1 && C->GetLayoutUndoCount()==0 && RowCount()==2);
	Block->Destroy(false,false); RunPhysics(TEXT("undo"));
	TestTrue(TEXT("Redo restores whole attachment"),C->TryRedoLayout()); CheckTransforms(Attached); CheckTopology(1);
	TestEqual(TEXT("Redo restores joined pipe inventory"),Room->GetMutableNetwork().GetNumPipes(),JoinedPipes); RunPhysics(TEXT("redo"));
	const FString File=TEXT("automation_group_row_")+FGuid::NewGuid().ToString(); FString Summary;
	const FString Ids[]={VrgRackLifecycle::Identity(M),VrgRackLifecycle::Identity(N)};
	TestTrue(TEXT("Save attached group using v2"),VrgLayoutFile::Save(W,File,Summary));
	TestTrue(TEXT("Undo before reload"),C->TryUndoLayout()); CheckTopology(2);
	TestTrue(TEXT("Load restores attached group"),VrgLayoutFile::Load(W,File,Summary)); CheckTopology(1);
	M=VrgRackLifecycle::Find(W,Ids[0]); N=VrgRackLifecycle::Find(W,Ids[1]);
	if(!TestNotNull(TEXT("Loaded first group member"),M) || !TestNotNull(TEXT("Loaded second group member"),N)) { IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File)); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	CheckTransforms(Attached);
	for(int32 I=0;I<2;++I) {
		auto* Rack=I==0?M:N; const auto Config=Rack->CaptureLayoutSettings();
		TestTrue(TEXT("Load preserves settings and clears live clock"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Config,&Settings[I],0) && Rack->GetSnapshot().SimTimeS==0);
	}
	TestTrue(TEXT("Load clears both command stacks"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Repeated Load creates/removes none"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("랙0 생성/0 제거"))); CheckTopology(1); RunPhysics(TEXT("reload"));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	AddInfo(FString::Printf(TEXT("Group row persistence: rows2->1->2->1, pipes%d->%d->%d->%d | max transform %.3e cm | IT %.6fW relative %.3e | global %.3e duct %.3e divergence %.3e | v2 duplicate count4/histories0"),BasePipes,JoinedPipes,BasePipes,JoinedPipes,MaxCm,BaselineW,MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
