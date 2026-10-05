#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

FString VrgCrahLifecycle::Identity(const ACrahUnitActor* Crah)
{
 return Crah->LayoutId.IsEmpty() ? TEXT("placed/") + Crah->GetName() : Crah->LayoutId;
}
ACrahUnitActor* VrgCrahLifecycle::Find(UWorld* World, const FString& Id)
{
 for (TActorIterator<ACrahUnitActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
 return nullptr;
}
ACrahUnitActor* VrgCrahLifecycle::Spawn(UWorld* World, const FCrahLayoutRecord& R)
{
 const auto& Name=R.Name; const auto& Id=R.Id; const auto& Transform=R.Transform; const auto& Settings=R.Settings;
 if (!World || !World->IsGameWorld() || !Settings.IsValid() || Id.IsEmpty() || Name.IsNone() || Transform.ContainsNaN()
  || Transform.GetScale3D().GetAbsMin()==0 || Find(World,Id) || FindObjectFast<UObject>(World->PersistentLevel,Name)) return nullptr;
 FActorSpawnParameters P; P.Name = Name;
 P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
 P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; P.bDeferConstruction = true;
 auto* Crah = World->SpawnActor<ACrahUnitActor>(ACrahUnitActor::StaticClass(), Transform, P);
 // Only retire our failed instance: a destroyed deferred actor can retain the requested name until GC.
 auto RetireFailedSpawn = [&]() {
  if (Crah && Crah->GetFName() == Name)
   Crah->Rename(*MakeUniqueObjectName(Crah->GetOuter(),Crah->GetClass(),TEXT("FailedLayoutCrah")).ToString(),
    nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
 };
 if (!IsValid(Crah)) { RetireFailedSpawn(); return nullptr; }
 Crah->LayoutId = Id; Crah->bRuntimeLayoutCrah = true; Crah->ApplyLayoutSettings(Settings);
 Crah->bConvertedAutomaticCrah = R.bConvertedAutomatic;
 Crah->AutomaticLayoutTransform = Transform; Crah->AutomaticLayoutSizeM = Settings.SizeM;
 Crah->FinishSpawning(Transform);
 // CRAH thermal/time state lives in the shared coupling, rebuilt fresh after a successful transaction.
 if (!IsValid(Crah)) { RetireFailedSpawn(); return nullptr; }
 return Crah;
}
static bool RemoveCrah(ACrahUnitActor* Crah)
{
 if (!IsValid(Crah) || !Crah->GetWorld()->IsGameWorld()
  || Crah->IsActorBeingDestroyed() || Crah->IsActorBeginningPlay()
  || Crah->GetClass() != ACrahUnitActor::StaticClass() || Crah->GetLocalRole() != ROLE_Authority) return false;
 const FName Name = Crah->GetFName();
 const FName Retired = MakeUniqueObjectName(Crah->GetOuter(), Crah->GetClass(), TEXT("RetiredLayoutCrah"));
 if (!Crah->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
 if (Crah->Destroy(false, false))
 {
  return true;
 }
 Crah->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional); return false;
}

bool VrgCrahLifecycle::RemoveBatch(const TArray<ACrahUnitActor*>& Crahs)
{
 if (Crahs.IsEmpty()) return false;
 UWorld* World = IsValid(Crahs[0]) ? Crahs[0]->GetWorld() : nullptr;
 if (!World || !World->IsGameWorld()) return false;
 TSet<ACrahUnitActor*> Seen; TSet<FString> Ids;
 for (auto* Crah : Crahs)
 {
  if (!IsValid(Crah) || Crah->GetWorld() != World || Crah->IsActorBeingDestroyed() || Crah->IsActorBeginningPlay()
   || Crah->GetClass() != ACrahUnitActor::StaticClass() || Crah->GetLocalRole() != ROLE_Authority
   || Seen.Contains(Crah) || Ids.Contains(Identity(Crah))) return false;
  Seen.Add(Crah); Ids.Add(Identity(Crah));
 }
 struct FBackup { ACrahUnitActor* Actor; FCrahLayoutRecord Record; FString PreviousId; ENetRole Role; bool bRuntime; FTransform AutomaticTransform; FVector AutomaticSizeM; };
 TArray<FBackup> Backups;
 for (auto* Crah : Crahs) Backups.Add({Crah,{Identity(Crah),Crah->GetFName(),Crah->GetActorTransform(),Crah->CaptureLayoutSettings(),Crah->bConvertedAutomaticCrah},
  Crah->LayoutId,Crah->GetLocalRole(),Crah->bRuntimeLayoutCrah,Crah->AutomaticLayoutTransform,Crah->AutomaticLayoutSizeM});
 for (auto* Crah : Crahs) if (!RemoveCrah(Crah))
 {
  for (const auto& B : Backups) if (IsValid(B.Actor)) B.Actor->SetRole(B.Role);
  for (const auto& B : Backups) if (!IsValid(B.Actor))
  {
   const auto& R = B.Record;
   auto* Restored = Spawn(World,R);
   checkf(Restored,TEXT("CRAH deletion rollback could not restore %s"),*R.Name.ToString());
   Restored->SetRole(B.Role);
   for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->NotifyLayoutCrahRecreated(Restored,B.Actor);
   Restored->AutomaticLayoutTransform=B.AutomaticTransform; Restored->AutomaticLayoutSizeM=B.AutomaticSizeM;
   Restored->LayoutId = B.PreviousId; Restored->bRuntimeLayoutCrah=B.bRuntime;
  }
  for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->RefreshLayoutEquipmentReferences(true);
  // Only actor links were replaced; the shared air/thermal bridge and clock stay intact on failure.
  // Successful edits use the normal full rebuild; Undo never copies thermal/integral state.
  return false;
 }
 return true;
}
