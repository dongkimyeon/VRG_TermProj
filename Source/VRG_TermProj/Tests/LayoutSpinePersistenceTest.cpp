#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpinePersistenceTest,"VRG.Interaction.Layout.SpinePersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpinePersistenceTest::RunTest(const FString& Parameters)
{
	auto NewWorld=[]() { auto* World=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); return World; };
	UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
	Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
	Room->RebuildRoomNow(); Sim->EnterLayoutMode(); const int32 BasePipes=Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Unconnected row remains diagnosed"),!Room->GetPatchPlan().Unconnected.IsEmpty());
	Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaseW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	C->BeginSpineAddition(); C->UpdateRackAdditionAlongRay(FVector(0,180,500),FVector(0,0,-1));
	auto* Added=C->ConfirmSpineAddition();
	if (!TestNotNull(TEXT("Palette spine"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	const FString Id=VrgSpineLifecycle::Identity(Added); const FName Name=Added->GetFName(); const FTransform Transform=Added->GetActorTransform();
	FRackThermalModel Ref; Ref.Initialize(Added->GetMutableModel().GetParams(),FRackThermalState());
	FRackThermalInput RefInput=Added->GetMutableInput(); Ref.Step(RefInput,Sim->FixedStepS); const double ExpectedW=Ref.GetLastTotalHeatW();
	// Existing MakeQuantumSpineParams estimate: 597 W electronics + calculated fans, <=747 W/switch reference.
	// Public passive-cable typical input: https://networking-docs.nvidia.com/qm97x0hw/specifications (no new fitting).
	TestTrue(TEXT("Existing QM9700 power plausibility envelope"),ExpectedW>Added->NumSwitches*597.0 && ExpectedW<=Added->NumSwitches*747.0);
	double MaxPower=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto Physics=[&](bool bSpine,const TCHAR* Stage) {
		Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
		const double Error=FMath::Abs(First.TotalItHeatW-(BaseW+(bSpine?ExpectedW:0)))/ExpectedW;
		TestTrue(TEXT("Same initial step IT delta matches independent spine model"),Error<1e-9); MaxPower=FMath::Max(MaxPower,Error);
		for(int32 I=0;I<20;++I) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestEqual(TEXT("Heat/air target inventory"),S.RackHeatToAirW.Num(),bSpine?2:1);
		if(bSpine) TestEqual(TEXT("Spine has no coolant branch/flow"),S.RackCoolantMassFlowKgs.Last(),0.0);
		TestEqual(TEXT("Hydraulic topology unchanged by air-only equipment"),Room->GetMutableNetwork().GetNumPipes(),BasePipes);
		double Duct=0;
		for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) {
			TestTrue(TEXT("Each cabinet has positive fan airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Global ledger / ducts existing criteria"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && Duct<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected);
		TestTrue(TEXT("Pressure converged and existing divergence bound"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,Duct); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Spine %s IT error %.3e global %.3e duct %.3e divergence %.3e t%.2fs"),Stage,Error,S.NormalizedEnergyResidual,Duct,AS.NormalizedDivergence,S.SimTimeS));
		Sim->EnterLayoutMode();
	};
	TestTrue(TEXT("Aligned new spine participates in row patch assignment"),Room->GetPatchDevices().Num()==2 && Room->GetPatchPlan().Unconnected.IsEmpty() && !Room->GetPatchPlan().Cables.IsEmpty());
	Physics(true,TEXT("add"));
	TestTrue(TEXT("Undo removes spine"),C->TryUndoLayout());
	TestTrue(TEXT("Undo restores missing-spine diagnosis"),Room->GetPatchDevices().Num()==1 && !Room->GetPatchPlan().Unconnected.IsEmpty()); Physics(false,TEXT("undo"));
	TestTrue(TEXT("Redo restores spine"),C->TryRedoLayout()); Physics(true,TEXT("redo"));
	const FString File=TEXT("automation_spine_")+FGuid::NewGuid().ToString(); FString Summary;
	TestTrue(TEXT("Save additive v2 spine inventory"),VrgLayoutFile::Save(W,File,Summary));
	FString Original; FFileHelper::LoadFileToString(Original,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Undo then saved reload"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
	Added=VrgSpineLifecycle::Find(W,Id);
	TestTrue(TEXT("ID/name/transform and fresh thermal state restored"),Added && Added->GetFName()==Name && Added->GetActorTransform().Equals(Transform,1e-9) && Added->GetMutableModel().GetEnergyInJ()==0);
	TestTrue(TEXT("Successful load clears history"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Repeated load creates no duplicate"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거"))); Physics(true,TEXT("reload"));
	// Fresh-world reconstruction with nondefault authored settings; synthetic test values, not new hardware defaults.
	FSpineLayoutSettings NonDefault=Added->CaptureLayoutSettings(); NonDefault.NumSwitches=2; NonDefault.FirstSwitchSlot=5;
	Added->ApplyLayoutSettings(NonDefault); TestTrue(TEXT("Save nondefault configuration"),VrgLayoutFile::Save(W,File,Summary));
	UWorld* Fresh=NewWorld(); FActorSpawnParameters P; P.Name=Rack->GetFName(); auto* FreshRack=Fresh->SpawnActor<ARackThermalActor>(P);
	FreshRack->ApplyGb200Nvl72Preset();
	// Latest v2 stores authored room/ceiling settings; a fresh level must have their owner.
	TestFalse(TEXT("Fresh world without room settings owner rejects load"),VrgLayoutFile::Load(Fresh,File,Summary));
	TestTrue(TEXT("Missing-owner rejection explains the failure and preserves inventory"),Summary.Contains(TEXT("통합 액터 없음")) && !VrgSpineLifecycle::Find(Fresh,Id));
	Fresh->SpawnActor<ADataCenterSimulationActor>()->EnterLayoutMode();
	TestTrue(TEXT("Fresh world runtime restoration"),VrgLayoutFile::Load(Fresh,File,Summary));
	auto* FreshSpine=VrgSpineLifecycle::Find(Fresh,Id);
	TestTrue(TEXT("Fresh world config, identity and state"),FreshSpine && FreshSpine->CaptureLayoutSettings()==NonDefault && FreshSpine->GetFName()==Name && FreshSpine->GetMutableModel().GetEnergyInJ()==0);
	// Invalid settings fail before history/transforms/inventory are changed.
	C->SelectLayoutAlongRay(Added->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1)); C->OnResetSim();
	const FTransform Changed=Added->GetActorTransform(); const int32 History=C->GetLayoutUndoCount();
	TSharedPtr<FJsonObject> Root; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Original),Root);
	for(const auto& Item:Root->GetArrayField(TEXT("equipment"))) if(Item->AsObject()->HasField(TEXT("spineSettings")))
		Item->AsObject()->GetObjectField(TEXT("spineSettings"))->SetNumberField(TEXT("numSwitches"),1.5);
	FString Bad; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Bad)); FFileHelper::SaveStringToFile(Bad,*VrgLayoutFile::PathFor(File));
	TestFalse(TEXT("Fractional switch count refused"),VrgLayoutFile::Load(W,File,Summary));
	TestTrue(TEXT("Invalid import state invariant"),C->GetLayoutUndoCount()==History && Added->GetActorTransform().Equals(Changed,0));
	// Old v2 has no spine capability/IDs: retain transform-only behavior, no implicit runtime deletion.
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Original),Root); Root->RemoveField(TEXT("spineInventory")); Root->RemoveField(TEXT("deletedSpines"));
	for(const auto& Item:Root->GetArrayField(TEXT("equipment"))) { auto O=Item->AsObject(); O->RemoveField(TEXT("spineId")); O->RemoveField(TEXT("spineSettings")); O->RemoveField(TEXT("runtimeSpine")); }
	FString Legacy; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Old v2 spines remain name-matched and settings retained"),VrgLayoutFile::Load(W,File,Summary) && Added->CaptureLayoutSettings()==NonDefault);
	// New authoritative file omitting a palette spine removes only runtime spines; placed spines are retained.
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Original),Root);
	TArray<TSharedPtr<FJsonValue>> WithoutSpines;
	for(const auto& Item:Root->GetArrayField(TEXT("equipment"))) if(!Item->AsObject()->HasField(TEXT("spineId"))) WithoutSpines.Add(Item);
	Root->SetArrayField(TEXT("equipment"),WithoutSpines);
	FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Bad)); FFileHelper::SaveStringToFile(Bad,*VrgLayoutFile::PathFor(File));
	auto* Placed=W->SpawnActor<ASpineCabinetActor>(FVector(800,800,0),FRotator::ZeroRotator);
	TestTrue(TEXT("New inventory removes absent runtime only"),VrgLayoutFile::Load(W,File,Summary) && !VrgSpineLifecycle::Find(W,Id) && IsValid(Placed));
	AddInfo(FString::Printf(TEXT("Spine coupling: delta %.6fW relative max %.3e | global max %.3e duct %.3e divergence %.3e | no hydraulic addition; v2/fresh-world/nondefault/legacy/malformed/history verified"),ExpectedW,MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File)); Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh);
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
