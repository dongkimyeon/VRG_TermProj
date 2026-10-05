#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahDeletionPersistenceTest,"VRG.Interaction.Layout.CrahDeletionPersistence",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahDeletionPersistenceTest::RunTest(const FString& Parameters)
{
 struct FFixture {
  UWorld* W; ADataCenterDebugController* C; ADataCenterSimulationActor* Sim;
  FFixture() {
   W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
   Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; C=W->SpawnActor<ADataCenterDebugController>();
   for(int I=0;I<2;++I) { FActorSpawnParameters P; P.Name=I==0?TEXT("PlacedCrahA"):TEXT("PlacedCrahB");
    auto* A=W->SpawnActor<ACrahUnitActor>(FVector(300+400*I,0,10),FRotator(0,90*I,0),P);
    // Synthetic nondefault settings/scale; no hardware constants or live thermal snapshots.
    A->VolumeFlowM3s=.25; A->SetActorScale3D(FVector(.9,1.1,1)); }
   Sim->EnterLayoutMode();
  }
  ~FFixture(){W->DestroyWorld(false);GEngine->DestroyWorldContext(W);}
  int Count(){int N=0;for(TActorIterator<ACrahUnitActor> It(W);It;++It)++N;return N;}
 };
 FFixture F; FString Summary;
 const FString Before=TEXT("AutomationCrahDeleteBefore"),After=Before+TEXT("After"),Legacy=Before+TEXT("Legacy"),Bad=Before+TEXT("Bad");
 auto* A=VrgCrahLifecycle::Find(F.W,TEXT("placed/PlacedCrahA")); const auto Config=A->CaptureLayoutSettings(); const auto Transform=A->GetActorTransform();
 TestTrue(TEXT("Save placed inventory before deletion"),VrgLayoutFile::Save(F.W,Before,Summary));
 F.C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),FVector(0,0,-1));
 TestTrue(TEXT("Delete placed and Save intent"),F.C->TryDeleteLayoutCrah() && VrgLayoutFile::Save(F.W,After,Summary));
 F.C->ClearLayoutHistory(); TestTrue(TEXT("Deletion intent survives history clear"),F.W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
 TestTrue(TEXT("Load before restores missing placed"),VrgLayoutFile::Load(F.W,Before,Summary)); A=VrgCrahLifecycle::Find(F.W,TEXT("placed/PlacedCrahA"));
 TestTrue(TEXT("Placed config/full transform/source restored"),A && !A->bRuntimeLayoutCrah && A->CaptureLayoutSettings()==Config && A->GetActorTransform().Equals(Transform,1e-9));
 TestTrue(TEXT("Load after twice deletes only intended placed"),VrgLayoutFile::Load(F.W,After,Summary) && VrgLayoutFile::Load(F.W,After,Summary) && F.Count()==1);
 TestTrue(TEXT("Load clears both stacks but retains explicit intent"),F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0 && F.W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
 const auto Fail=F.W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* X){if(Cast<ACrahUnitActor>(X)) X->Destroy(false,false);}));
 TestFalse(TEXT("Load restore failure rejects whole transaction"),VrgLayoutFile::Load(F.W,Before,Summary)); F.W->RemoveOnActorSpawnedHandler(Fail);
 TestTrue(TEXT("Failed Load retains count/intent/history"),F.Count()==1 && F.W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1 && F.C->GetLayoutUndoCount()==0);
 TestTrue(TEXT("Can restore after failed deferred spawn without GC"),VrgLayoutFile::Load(F.W,Before,Summary));
 FString Text; FFileHelper::LoadFileToString(Text,*VrgLayoutFile::PathFor(After)); TSharedPtr<FJsonObject> Root;
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root); Root->RemoveField(TEXT("deletedCrahs"));
 auto Write=[&](const FString& Name){FString Out;FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Out));return FFileHelper::SaveStringToFile(Out,*VrgLayoutFile::PathFor(Name));};
 Write(Legacy); TestTrue(TEXT("Old v2 absent entry preserves placed"),VrgLayoutFile::Load(F.W,Legacy,Summary) && F.Count()==2);
 Root->SetNumberField(TEXT("version"),1); Write(Legacy); TestTrue(TEXT("v1 omission preserves placed"),VrgLayoutFile::Load(F.W,Legacy,Summary) && F.Count()==2);
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root);
 Root->GetArrayField(TEXT("deletedCrahs"))[0]->AsObject()->SetBoolField(TEXT("runtimeCrah"),true); Write(Bad);
 TestFalse(TEXT("Runtime actor in placed deletion list rejected"),VrgLayoutFile::Load(F.W,Bad,Summary)); TestEqual(TEXT("Invalid list has no partial deletion"),F.Count(),2);
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root);
 auto Entries=Root->GetArrayField(TEXT("deletedCrahs")); const auto Duplicate=Entries[0]; Entries.Add(Duplicate); Root->SetArrayField(TEXT("deletedCrahs"),Entries); Write(Bad);
 TestFalse(TEXT("Duplicate deletion ID/name rejected"),VrgLayoutFile::Load(F.W,Bad,Summary)); TestEqual(TEXT("Duplicate retains count"),F.Count(),2);
 {FFixture New;TestTrue(TEXT("Fresh world applies existing deletion twice"),VrgLayoutFile::Load(New.W,After,Summary) && VrgLayoutFile::Load(New.W,After,Summary) && New.Count()==1);
  TestTrue(TEXT("Fresh world Load before restores once"),VrgLayoutFile::Load(New.W,Before,Summary) && VrgLayoutFile::Load(New.W,Before,Summary) && New.Count()==2);
  auto* Restored=VrgCrahLifecycle::Find(New.W,TEXT("placed/PlacedCrahA"));
  TestTrue(TEXT("New world independent identity/config/transform"),Restored && !Restored->bRuntimeLayoutCrah && Restored->CaptureLayoutSettings()==Config && Restored->GetActorTransform().Equals(Transform,1e-9));}
 AddInfo(TEXT("deletedCrahs optional v2: explicit native placed intent, old v1/v2 preservation, repeated/new world restore/delete without duplicates, Load history clear, invalid list/failed spawn transaction preservation; no live state."));
 for(const auto& Name:{Before,After,Legacy,Bad}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
#endif
