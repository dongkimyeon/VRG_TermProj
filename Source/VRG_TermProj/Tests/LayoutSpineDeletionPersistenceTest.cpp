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
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSpineDeletionPersistenceTest, "VRG.Interaction.Layout.SpineDeletionPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSpineDeletionPersistenceTest::RunTest(const FString& Parameters)
{
	auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
	UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
	auto* C=W->SpawnActor<ADataCenterDebugController>();
	auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
	auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator);
	auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
	Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
	Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
	auto* Spine=W->SpawnActor<ASpineCabinetActor>(FVector(0,180,0),FRotator::ZeroRotator);
	const FString Id=VrgSpineLifecycle::Identity(Spine); const FName Name=Spine->GetFName();
	const auto Config=Spine->CaptureLayoutSettings(); const FTransform Transform=Spine->GetActorTransform();
	Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
	Spine->EnsureThermalModel(); // Placed actors initialize thermals on simulation entry, not OnConstruction.
	FRackThermalModel Reference; Reference.Initialize(Spine->GetMutableModel().GetParams(),FRackThermalState());
	Reference.Step(Spine->GetMutableInput(),Sim->FixedStepS); const double ExpectedW=Reference.GetLastTotalHeatW();
	// Reuse existing estimated electronics/fan split and public QM9700 passive-cable typical input reference.
	// https://networking-docs.nvidia.com/qm97x0hw/specifications -- no new hardware fitting/defaults.
	TestTrue(TEXT("Existing spine plausibility envelope"),ExpectedW>Config.NumSwitches*597.0 && ExpectedW<=Config.NumSwitches*747.0);
	Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	double MaxPower=0,MaxEnergy=0,MaxDuct=0,MaxDiv=0;
	auto Physics=[&](bool Present,const TCHAR* Stage) {
		TestTrue(TEXT("Rack/CDU/hydraulic inventory unchanged"),Room->GetRoutedRacks().Num()==1
			&& Room->GetMutableNetwork().GetNumPipes()==Pipes && Room->GetRowCdu(0)==Cdu);
		TestEqual(TEXT("Patch devices"),Room->GetPatchDevices().Num(),Present?2:1);
		TestTrue(TEXT("Cable assignment or retained unconnected diagnosis"),Present
			? (!Room->GetPatchPlan().Cables.IsEmpty() && Room->GetPatchPlan().Unconnected.IsEmpty())
			: (Room->GetPatchPlan().Cables.IsEmpty() && !Room->GetPatchPlan().Unconnected.IsEmpty()));
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		const double Delta=BaselineW-Sim->GetSnapshot().TotalItHeatW;
		const double Power=FMath::Abs(Delta-(Present?0:ExpectedW))/ExpectedW;
		TestTrue(TEXT("Same initial step removed spine IT"),Power<1e-9); MaxPower=FMath::Max(MaxPower,Power);
		for(int32 I=0;I<20;++I) Sim->Tick(.04f);
		const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		TestTrue(TEXT("Only live targets coupled"),S.RackHeatToAirW.Num()==(Present?2:1)
			&& S.RackAirMassFlowKgs.Num()==(Present?2:1) && S.RackDuctMassFlowKgs.Num()==(Present?2:1));
		if(Present) TestEqual(TEXT("Spine has no coolant branch"),S.RackCoolantMassFlowKgs.Last(),0.0);
		double Duct=0;
		for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) {
			TestTrue(TEXT("Positive fan airflow"),S.RackAirMassFlowKgs[I]>0);
			if(S.RackAirMassFlowKgs[I]>0) Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]);
		}
		TestTrue(TEXT("Global conservation and duct bounds"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && Duct<1e-9 && !S.bAirDuctsRejected && !AS.bDuctsRejected);
		TestTrue(TEXT("Pressure converges"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
		MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDuct=FMath::Max(MaxDuct,Duct); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
		AddInfo(FString::Printf(TEXT("Spine deletion %s: IT decrease %.6fW error %.3e global %.3e duct %.3e divergence %.3e t%.2fs"),Stage,Delta,Power,S.NormalizedEnergyResidual,Duct,AS.NormalizedDivergence,S.SimTimeS));
		Sim->EnterLayoutMode();
	};
	const FVector Down(0,0,-1);
	auto Select=[&](ASpineCabinetActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
	const FString File=TEXT("automation_spine_delete_")+FGuid::NewGuid().ToString(); FString Summary;
	TestTrue(TEXT("Save before deletion"),VrgLayoutFile::Save(W,File,Summary));
	FString Full; FFileHelper::LoadFileToString(Full,*VrgLayoutFile::PathFor(File));
	Select(Spine); TestTrue(TEXT("Delete placed"),C->TryDeleteLayoutSpine()); Physics(false,TEXT("placed-delete"));
	TestTrue(TEXT("Explicit placed deletion survives history clear"),W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Contains(Id));
	TestTrue(TEXT("Undo placed"),C->TryUndoLayout()); Physics(true,TEXT("placed-undo"));
	TestTrue(TEXT("Redo placed"),C->TryRedoLayout()); Physics(false,TEXT("placed-redo"));
	TestTrue(TEXT("Save deletion"),VrgLayoutFile::Save(W,File,Summary)); FString Deleted; FFileHelper::LoadFileToString(Deleted,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Undo then load deletes placed spine"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
	TestTrue(TEXT("Deletion reload counts/history"),!VrgSpineLifecycle::Find(W,Id) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Deleted intent remains after load clears history"),W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Contains(Id));
	TestTrue(TEXT("Repeated deleted file no duplicate removal"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
	Physics(false,TEXT("deleted-load"));
	FFileHelper::SaveStringToFile(Full,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Full save restores missing placed spine"),VrgLayoutFile::Load(W,File,Summary)); Spine=VrgSpineLifecycle::Find(W,Id);
	TestTrue(TEXT("Placed config/name/origin/transform fresh"),Spine && !Spine->bRuntimeLayoutSpine && Spine->GetFName()==Name
		&& Spine->CaptureLayoutSettings()==Config && Spine->GetActorTransform().Equals(Transform,1e-9) && Spine->GetMutableModel().GetEnergyInJ()==0);
	TestTrue(TEXT("Repeated full file no duplication"),VrgLayoutFile::Load(W,File,Summary) && Summary.Contains(TEXT("스파인0 생성/0 제거")));
	Physics(true,TEXT("full-load"));
	// A fresh level contains the original spine: explicit deletion must apply, then full save must reconstruct it.
	UWorld* Fresh=NewWorld();
	// Latest v2 room/ceiling settings require the same owner as the real level.
	Fresh->SpawnActor<ADataCenterSimulationActor>()->EnterLayoutMode();
	FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
	P.Name=Name; Fresh->SpawnActor<ASpineCabinetActor>(P);
	P.Name=Cdu->GetFName(); Fresh->SpawnActor<ACoolingDistributionUnitActor>(P);
	FFileHelper::SaveStringToFile(Deleted,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Fresh level applies explicit deletion"),VrgLayoutFile::Load(Fresh,File,Summary) && !VrgSpineLifecycle::Find(Fresh,Id));
	FFileHelper::SaveStringToFile(Full,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Fresh level full restore and repeat"),VrgLayoutFile::Load(Fresh,File,Summary) && VrgLayoutFile::Load(Fresh,File,Summary)
		&& VrgSpineLifecycle::Find(Fresh,Id) && !VrgSpineLifecycle::Find(Fresh,Id)->bRuntimeLayoutSpine);
	// Legacy v2 must not infer deletion from an omitted placed spine; old spineInventory true also stays safe.
	TSharedPtr<FJsonObject> Root; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Deleted),Root); Root->RemoveField(TEXT("deletedSpines"));
	FString Legacy; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Old spine-capable v2 omission preserves placed"),VrgLayoutFile::Load(W,File,Summary) && VrgSpineLifecycle::Find(W,Id));
	Root->RemoveField(TEXT("spineInventory")); FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("Old plain v2 omission preserves placed"),VrgLayoutFile::Load(W,File,Summary) && VrgSpineLifecycle::Find(W,Id));
	Root->SetNumberField(TEXT("version"),1); FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestTrue(TEXT("v1 omission preserves placed"),VrgLayoutFile::Load(W,File,Summary) && VrgSpineLifecycle::Find(W,Id));
	// Duplicate live/deleted IDs and fractional configs fail before inventory/history/transforms change.
	Spine=VrgSpineLifecycle::Find(W,Id); Select(Spine); C->OnResetSim(); const auto Changed=Spine->GetActorTransform();
	const int32 History=C->GetLayoutUndoCount();
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Full),Root); TSharedPtr<FJsonObject> SpineO;
	for(const auto& V:Root->GetArrayField(TEXT("equipment"))) if(V->AsObject()->HasField(TEXT("spineId"))) SpineO=V->AsObject();
	Root->SetArrayField(TEXT("deletedSpines"),{MakeShared<FJsonValueObject>(SpineO)});
	FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestFalse(TEXT("Live/deleted ID conflict refuses"),VrgLayoutFile::Load(W,File,Summary));
	TestTrue(TEXT("Failed load invariant"),C->GetLayoutUndoCount()==History && Spine->GetActorTransform().Equals(Changed,0)
		&& W->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.IsEmpty());
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Deleted),Root);
	Root->GetArrayField(TEXT("deletedSpines"))[0]->AsObject()->GetObjectField(TEXT("spineSettings"))->SetNumberField(TEXT("numSwitches"),1.5);
	FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Legacy)); FFileHelper::SaveStringToFile(Legacy,*VrgLayoutFile::PathFor(File));
	TestFalse(TEXT("Malformed deletion config refuses"),VrgLayoutFile::Load(W,File,Summary));
	TestTrue(TEXT("Malformed deletion no partial change"),C->GetLayoutUndoCount()==History && Spine->GetActorTransform().Equals(Changed,0));
	C->TryUndoLayout(); Select(Spine); TestTrue(TEXT("Remove original for palette test"),C->TryDeleteLayoutSpine());
	C->BeginSpineAddition(); C->UpdateRackAdditionAlongRay(FVector(0,180,500),Down); auto* Added=C->ConfirmSpineAddition();
	if(TestNotNull(TEXT("Palette spine for deletion coupling"),Added)) {
		const FString RuntimeId=VrgSpineLifecycle::Identity(Added);
		TestTrue(TEXT("Delete palette"),C->TryDeleteLayoutSpine()); Physics(false,TEXT("runtime-delete"));
		TestTrue(TEXT("Undo palette"),C->TryUndoLayout()); Physics(true,TEXT("runtime-undo"));
		TestTrue(TEXT("Runtime save"),VrgLayoutFile::Save(W,File,Summary));
		TestTrue(TEXT("Redo then runtime reload"),C->TryRedoLayout() && VrgLayoutFile::Load(W,File,Summary));
		TestTrue(TEXT("Runtime identity/history restored"),VrgSpineLifecycle::Find(W,RuntimeId)
			&& VrgSpineLifecycle::Find(W,RuntimeId)->bRuntimeLayoutSpine && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
		Physics(true,TEXT("runtime-load"));
	}
	AddInfo(FString::Printf(TEXT("Spine deletion coupling: reference %.6fW max power %.3e energy %.3e duct %.3e divergence %.3e | placed/runtime, explicit deletions, fresh/repeated/legacy loads; no hydraulic change"),ExpectedW,MaxPower,MaxEnergy,MaxDuct,MaxDiv));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File)); Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh);
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
