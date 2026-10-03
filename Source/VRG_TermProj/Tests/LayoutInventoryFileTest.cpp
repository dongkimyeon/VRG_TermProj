#include "Misc/AutomationTest.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutInventoryFileTest,"VRG.Interaction.Layout.InventorySaveLoad",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutInventoryFileTest::RunTest(const FString& Parameters)
{
	const FString File=TEXT("AutomationInventory"); FString Summary;
	auto NewWorld=[&]() { UWorld* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
	auto SpawnRack=[&](UWorld* W,const TCHAR* Name,FVector P) {
		FActorSpawnParameters S; S.Name=Name;
		auto* R=W->SpawnActor<ARackThermalActor>(P,FRotator::ZeroRotator,S); R->ApplyGb200Nvl72Preset(); return R;
	};
	auto Count=[&](UWorld* W) { int32 N=0; for(TActorIterator<ARackThermalActor> It(W);It;++It) ++N; return N; };
	UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); auto* C=W->SpawnActor<ADataCenterDebugController>(); Sim->EnterLayoutMode();
	auto* Deleted=SpawnRack(W,TEXT("BaseRackA"),FVector(0,0,0));
	auto* Base=SpawnRack(W,TEXT("BaseRackB"),FVector(0,1000,9));
	Base->Input.Utilization=.63; Base->Input.SledDocked[2]=false; Base->Input.SlotUtilization.Init(.4,Base->Params.Slots.Num());
	Base->SetActorTransform(FTransform(FRotator(2,31,1),FVector(0,1000,9),FVector(1.1,.9,1.2)));
	const FVector Down(0,0,-1);
	auto Select=[&](AActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
	Select(Base); TestTrue(TEXT("Duplicate for file"),C->BeginRackDuplication()); C->UpdateRackAdditionAlongRay(FVector(-500,1500,500),Down);
	auto* Clone=C->ConfirmRackAddition(); if(!TestNotNull(TEXT("Clone saved"),Clone)) return false;
	const FString BaseId=VrgRackLifecycle::Identity(Base),CloneId=VrgRackLifecycle::Identity(Clone);
	const FName CloneName=Clone->GetFName(); const FTransform BT=Base->GetActorTransform(),CT=Clone->GetActorTransform();
	const auto BC=Base->CaptureLayoutSettings(),CC=Clone->CaptureLayoutSettings();
	Select(Deleted); TestTrue(TEXT("Delete base A before save"),C->TryDeleteLayoutRack());
	TestTrue(TEXT("Save authoritative rack inventory"),VrgLayoutFile::Save(W,File,Summary));
	FString OriginalJson; FFileHelper::LoadFileToString(OriginalJson,*VrgLayoutFile::PathFor(File));
	Select(Clone); TestTrue(TEXT("Delete saved runtime rack"),C->TryDeleteLayoutRack());
	Base->SetActorLocation(FVector(500,1000,0)); Base->Input.Utilization=.1;
	TestTrue(TEXT("Reload reconciles runtime inventory"),VrgLayoutFile::Load(W,File,Summary));
	TestEqual(TEXT("Exactly saved rack count"),Count(W),2);
	TestNull(TEXT("Deleted base remains absent"),VrgRackLifecycle::Find(W,TEXT("placed/BaseRackA")));
	Clone=VrgRackLifecycle::Find(W,CloneId);
	TestNotNull(TEXT("Missing clone recreated"),Clone);
	TestTrue(TEXT("Existing full transform restored"),Base->GetActorTransform().Equals(BT,1e-9));
	TestTrue(TEXT("Runtime full transform restored"),Clone->GetActorTransform().Equals(CT,1e-9));
	const auto Loaded=Base->CaptureLayoutSettings();
	TestTrue(TEXT("Existing non-default settings restored"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&BC,&Loaded,0));
	TestEqual(TEXT("Successful load clears Undo"),C->GetLayoutUndoCount(),0);
	TestEqual(TEXT("Successful load clears Redo"),C->GetLayoutRedoCount(),0);
	TestTrue(TEXT("Repeat load has no duplicates"),VrgLayoutFile::Load(W,File,Summary)); TestEqual(TEXT("Repeat count"),Count(W),2);
	// Reopen into a separate world with the original level racks plus an extra rack.
	UWorld* W2=NewWorld(); auto* S2=W2->SpawnActor<ADataCenterSimulationActor>(); S2->EnterLayoutMode();
	SpawnRack(W2,TEXT("BaseRackA"),FVector(0,0,0)); SpawnRack(W2,TEXT("BaseRackB"),FVector(0,1000,0));
	SpawnRack(W2,TEXT("ExtraRack"),FVector(2000,0,0));
	TestTrue(TEXT("Fresh-world reload"),VrgLayoutFile::Load(W2,File,Summary));
	TestEqual(TEXT("Fresh-world saved count"),Count(W2),2);
	TestNull(TEXT("Fresh-world deleted base stays deleted"),VrgRackLifecycle::Find(W2,TEXT("placed/BaseRackA")));
	auto* Fresh=VrgRackLifecycle::Find(W2,CloneId); TestNotNull(TEXT("Runtime ID persists across worlds"),Fresh);
	const auto FreshConfig=Fresh->CaptureLayoutSettings();
	TestTrue(TEXT("Fresh-world configuration equal"),FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&CC,&FreshConfig,0));
	TestTrue(TEXT("Fresh-world transform equal"),Fresh->GetActorTransform().Equals(CT,1e-9));
	TestEqual(TEXT("Fresh solver clock"),Fresh->GetSnapshot().SimTimeS,0.0);
	// Failed imports must not clear valid history or partially move equipment.
	Select(Base); FVector Eye=Base->GetActorLocation()+FVector(0,0,500);
	C->BeginLayoutDragAlongRay(Eye,Down); C->UpdateLayoutDragAlongRay(Eye+FVector(-200,0,0),Down,true); C->EndLayoutDrag(true);
	const FTransform Changed=Base->GetActorTransform(); const int32 History=C->GetLayoutUndoCount();
	auto Write=[&](const TSharedPtr<FJsonObject>& Root) { FString T; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&T)); FFileHelper::SaveStringToFile(T,*VrgLayoutFile::PathFor(File)); };
	auto Parse=[&]() { TSharedPtr<FJsonObject> R; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OriginalJson),R); return R; };
	auto CheckUnchanged=[&]() { TestEqual(TEXT("Failed import keeps count"),Count(W),2); TestEqual(TEXT("Failed import keeps history"),C->GetLayoutUndoCount(),History); TestTrue(TEXT("Failed import keeps transform"),Base->GetActorTransform().Equals(Changed,0)); };
	auto Bad=Parse(); Bad->SetNumberField(TEXT("version"),999); Write(Bad); TestFalse(TEXT("Unknown version rejected"),VrgLayoutFile::Load(W,File,Summary)); CheckUnchanged();
	Bad=Parse(); const auto E=Bad->GetArrayField(TEXT("equipment")); E[1]->AsObject()->SetStringField(TEXT("rackId"),E[0]->AsObject()->GetStringField(TEXT("rackId")));
	Write(Bad); TestFalse(TEXT("Duplicate ID rejected"),VrgLayoutFile::Load(W,File,Summary)); CheckUnchanged();
	Bad=Parse(); Bad->GetArrayField(TEXT("equipment"))[0]->AsObject()->GetObjectField(TEXT("rackSettings"))->GetObjectField(TEXT("params"))->GetArrayField(TEXT("slots"))[0]->AsObject()->SetNumberField(TEXT("deviceType"),999);
	Write(Bad); TestFalse(TEXT("Invalid slot reference rejected"),VrgLayoutFile::Load(W,File,Summary)); CheckUnchanged();
	FFileHelper::SaveStringToFile(OriginalJson,*VrgLayoutFile::PathFor(File));
	// A non-rack object occupying a missing rack's saved name fails before any transforms/settings are touched.
	VrgRackLifecycle::Remove(Clone); FActorSpawnParameters P; P.Name=CloneName;
	auto* Occupant=W->SpawnActor<ACrahUnitActor>(FVector(5000,5000,0),FRotator::ZeroRotator,P);
	TestFalse(TEXT("Spawn name conflict rejected atomically"),VrgLayoutFile::Load(W,File,Summary));
	TestEqual(TEXT("Name conflict keeps one rack"),Count(W),1); TestEqual(TEXT("Name conflict retains history"),C->GetLayoutUndoCount(),History);
	TestTrue(TEXT("Name conflict keeps existing placement"),Base->GetActorTransform().Equals(Changed,0));
	TestNotNull(TEXT("Non-rack occupant retained"),Occupant);
	AddInfo(TEXT("Inventory v2: separate-world and repeated reload, added/duplicated/deleted racks, full transforms and non-default configuration restored; malformed version/ID/slot/name rejected without partial edits/history loss."));
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	W2->DestroyWorld(false); GEngine->DestroyWorldContext(W2); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
