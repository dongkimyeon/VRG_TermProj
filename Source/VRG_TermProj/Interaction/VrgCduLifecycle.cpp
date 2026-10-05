#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCduLayoutState.h"
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
 if (!IsValid(Cdu)) return nullptr;
 Cdu->LayoutId = Id; Cdu->bRuntimeLayoutCdu = bRuntime; Cdu->ApplyLayoutSettings(Settings);
 Cdu->FinishSpawning(Transform);
 // CDU thermal/time state lives in the shared coupling, rebuilt fresh after a successful transaction.
 if (!IsValid(Cdu)) return nullptr;
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
