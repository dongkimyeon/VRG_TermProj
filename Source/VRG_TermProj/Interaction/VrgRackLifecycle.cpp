#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

FString VrgRackLifecycle::Identity(const ARackThermalActor* Rack)
{
	return Rack->LayoutId.IsEmpty() ? TEXT("placed/") + Rack->GetName() : Rack->LayoutId;
}

ARackThermalActor* VrgRackLifecycle::Find(UWorld* World, const FString& Id)
{
	for (TActorIterator<ARackThermalActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
	return nullptr;
}

ARackThermalActor* VrgRackLifecycle::Spawn(UWorld* World, FName Name, const FString& Id, UClass* Class,
	const FTransform& Transform, const FRackLayoutSettings& Settings)
{
	if (!World || !World->IsGameWorld() || !Class || !Class->IsChildOf(ARackThermalActor::StaticClass())
		|| Id.IsEmpty() || Find(World, Id) || FindObjectFast<UObject>(World->PersistentLevel, Name)) return nullptr;
	FActorSpawnParameters P;
	P.Name = Name;
	P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	auto* Rack = World->SpawnActor<ARackThermalActor>(Class, Transform, P);
	if (!Rack) return nullptr;
	Rack->LayoutId = Id;
	Rack->bDrivenExternally = true;
	Rack->ApplyLayoutSettings(Settings);
	Rack->FinishSpawning(Transform);
	Rack->PauseSim(); Rack->RefreshVisuals();
	return Rack;
}

bool VrgRackLifecycle::Remove(ARackThermalActor* Rack)
{
	if (!IsValid(Rack) || !Rack->GetWorld()->IsGameWorld()) return false;
	const FName Name = Rack->GetFName();
	const FName Retired = MakeUniqueObjectName(Rack->GetOuter(), Rack->GetClass(), TEXT("RetiredLayoutRack"));
	// Destroy leaves its UObject until GC; release the name so Undo can recreate the same actor name immediately.
	if (!Rack->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
	if (Rack->Destroy()) return true;
	Rack->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	return false;
}

void VrgRackLifecycle::Rebuild(UWorld* World)
{
	for (TActorIterator<ADataCenterDebugController> It(World); It; ++It) It->RefreshLayoutEquipmentReferences();
	for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It) { It->RebuildRoom(); It->RebuildRoomNow(); }
	for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) It->RebuildCoupling();
}
