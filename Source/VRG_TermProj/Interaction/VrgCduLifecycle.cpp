#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCduLayoutState.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterRoomActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

FString VrgCduLifecycle::Identity(const ACoolingDistributionUnitActor* Cdu)
{
 return Cdu->LayoutId.IsEmpty() ? TEXT("placed/") + Cdu->GetName() : Cdu->LayoutId;
}
ACoolingDistributionUnitActor* VrgCduLifecycle::Find(UWorld* World, const FString& Id)
{
 for (TActorIterator<ACoolingDistributionUnitActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
 return nullptr;
}
ACoolingDistributionUnitActor* VrgCduLifecycle::Spawn(UWorld* World, FName Name, const FString& Id,
 const FTransform& Transform, const FCduLayoutSettings& Settings, bool bRuntime)
{
 if (!World || !World->IsGameWorld() || !Settings.IsValid() || Id.IsEmpty() || Transform.ContainsNaN()
  || Transform.GetScale3D().GetAbsMin() == 0 || Find(World, Id) || FindObjectFast<UObject>(World->PersistentLevel, Name)) return nullptr;
 FActorSpawnParameters P; P.Name = Name;
 P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
 P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn; P.bDeferConstruction = true;
 auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(), Transform, P);
 // Only retire our failed instance: a destroyed deferred actor can retain the requested name until GC.
 auto RetireFailedSpawn = [&]() {
  if (Cdu && Cdu->GetFName() == Name)
   Cdu->Rename(*MakeUniqueObjectName(Cdu->GetOuter(),Cdu->GetClass(),TEXT("FailedLayoutCdu")).ToString(),
    nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
 };
 if (!IsValid(Cdu)) { RetireFailedSpawn(); return nullptr; }
 Cdu->LayoutId = Id; Cdu->bRuntimeLayoutCdu = bRuntime; Cdu->ApplyLayoutSettings(Settings);
 Cdu->FinishSpawning(Transform);
 // CDU thermal/time state lives in the shared coupling, rebuilt fresh after a successful transaction.
 if (!IsValid(Cdu)) { RetireFailedSpawn(); return nullptr; }
 World->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Remove(Id);
 return Cdu;
}
bool VrgCduLifecycle::Remove(ACoolingDistributionUnitActor* Cdu)
{
 if (!IsValid(Cdu) || !Cdu->GetWorld()->IsGameWorld()
  || Cdu->IsActorBeingDestroyed() || Cdu->IsActorBeginningPlay()
  || Cdu->GetClass() != ACoolingDistributionUnitActor::StaticClass() || Cdu->GetLocalRole() != ROLE_Authority) return false;
 const FName Name = Cdu->GetFName();
 const FCduLayoutRecord Record{Identity(Cdu), Name, Cdu->GetActorTransform(), Cdu->CaptureLayoutSettings(), Cdu->bRuntimeLayoutCdu};
 const FName Retired = MakeUniqueObjectName(Cdu->GetOuter(), Cdu->GetClass(), TEXT("RetiredLayoutCdu"));
 if (!Cdu->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
 if (Cdu->Destroy(false, false))
 {
  if (!Record.bRuntime) Cdu->GetWorld()->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Add(Record.Id, Record);
  return true;
 }
 Cdu->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional); return false;
}

bool VrgCduLifecycle::RemoveBatch(const TArray<ACoolingDistributionUnitActor*>& Cdus)
{
 if (Cdus.IsEmpty()) return false;
 UWorld* World = IsValid(Cdus[0]) ? Cdus[0]->GetWorld() : nullptr;
 if (!World || !World->IsGameWorld()) return false;
 TSet<ACoolingDistributionUnitActor*> Seen; TSet<FString> Ids;
 for (auto* Cdu : Cdus)
 {
  if (!IsValid(Cdu) || Cdu->GetWorld() != World || Cdu->IsActorBeingDestroyed() || Cdu->IsActorBeginningPlay()
   || Cdu->GetClass() != ACoolingDistributionUnitActor::StaticClass() || Cdu->GetLocalRole() != ROLE_Authority
   || Seen.Contains(Cdu) || Ids.Contains(Identity(Cdu))) return false;
  Seen.Add(Cdu); Ids.Add(Identity(Cdu));
 }
 struct FBackup { ACoolingDistributionUnitActor* Actor; FCduLayoutRecord Record; FString PreviousId; ENetRole Role; };
 TArray<FBackup> Backups;
 for (auto* Cdu : Cdus) Backups.Add({Cdu,{Identity(Cdu),Cdu->GetFName(),Cdu->GetActorTransform(),Cdu->CaptureLayoutSettings(),Cdu->bRuntimeLayoutCdu},
  Cdu->LayoutId,Cdu->GetLocalRole()});
 const auto DeletedBefore = World->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced;
 for (auto* Cdu : Cdus) if (!Remove(Cdu))
 {
  for (const auto& B : Backups) if (IsValid(B.Actor)) B.Actor->SetRole(B.Role);
  for (const auto& B : Backups) if (!IsValid(B.Actor))
  {
   const auto& R = B.Record;
   auto* Restored = Spawn(World,R.Name,R.Id,R.Transform,R.Settings,R.bRuntime);
   checkf(Restored,TEXT("CDU deletion rollback could not restore %s"),*R.Name.ToString());
   Restored->SetRole(B.Role);
   for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->NotifyLayoutCduRecreated(Restored,B.Actor);
   Restored->LayoutId = B.PreviousId;
  }
  World->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced = DeletedBefore;
  for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->RefreshLayoutEquipmentReferences(true);
  for (TActorIterator<ADataCenterRoomActor> It(World);It;++It) It->RebuildRoomNow();
  // Only CDU actor links were replaced. The shared thermal bridge/clock stay intact on failed removal.
  // Successful edits use the normal full rebuild; Undo never copies thermal/integral state.
  return false;
 }
 return true;
}
