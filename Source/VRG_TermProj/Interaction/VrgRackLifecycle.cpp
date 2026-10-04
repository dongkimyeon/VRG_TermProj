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
	if (Rack->Destroy(false, false)) return true;
	Rack->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	return false;
}

bool VrgRackLifecycle::RemoveBatch(const TArray<ARackThermalActor*>& Racks)
{
	if (Racks.IsEmpty()) return false;
	UWorld* World = IsValid(Racks[0]) ? Racks[0]->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld()) return false;
	TSet<ARackThermalActor*> Seen;
	TSet<FString> Ids;
	for (auto* Rack : Racks)
	{
		if (!IsValid(Rack) || Rack->GetWorld() != World || Rack->IsActorBeingDestroyed()
			|| Rack->IsActorBeginningPlay() || Rack->GetLocalRole() != ROLE_Authority
			|| Seen.Contains(Rack) || Ids.Contains(Identity(Rack))) return false;
		Seen.Add(Rack); Ids.Add(Identity(Rack));
	}
	struct FBackup
	{
		ARackThermalActor* Actor; FName Name; FString Id, PreviousId; UClass* Class;
		FTransform Transform; FRackLayoutSettings Settings;
		FRackThermalModel Model; FRackThermalInput Input; FRackThermalState Initial;
		double TimeS; bool bExternal; ENetRole Role;
	};
	TArray<FBackup> Backups;
	for (auto* Rack : Racks)
		Backups.Add({Rack, Rack->GetFName(), Identity(Rack), Rack->LayoutId, Rack->GetClass(),
			Rack->GetActorTransform(), Rack->CaptureLayoutSettings(), Rack->GetMutableModel(),
			Rack->Input, Rack->InitialState, Rack->GetSnapshot().SimTimeS, Rack->bDrivenExternally, Rack->GetLocalRole()});
	auto Rollback = [&]()
	{
		for (const auto& B : Backups)
			if (IsValid(B.Actor))
			{
				verify(B.Actor->Rename(*B.Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional));
				B.Actor->SetRole(B.Role);
			}
		for (const auto& B : Backups) if (!IsValid(B.Actor))
		{
			auto* Restored = Spawn(World, B.Name, B.Id, B.Class, B.Transform, B.Settings);
			checkf(Restored, TEXT("Layout deletion rollback could not restore %s"), *B.Name.ToString());
			// Failed transaction only: retain live state, not a successful Undo snapshot.
			Restored->GetMutableModel() = B.Model; Restored->Input = B.Input; Restored->InitialState = B.Initial;
			Restored->bDrivenExternally = B.bExternal; Restored->SetExternalSimTimeS(B.TimeS);
			for (TActorIterator<ADataCenterDebugController> It(World); It; ++It) It->NotifyLayoutRackRecreated(Restored, B.Actor);
			Restored->LayoutId = B.PreviousId;
		}
		Rebuild(World, true);
	};
	// Release every name before destruction. No history or coupling changes are committed here.
	for (const auto& B : Backups)
	{
		const FName Retired = MakeUniqueObjectName(B.Actor->GetOuter(), B.Class, TEXT("RetiredLayoutRack"));
		if (!B.Actor->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) { Rollback(); return false; }
	}
	for (const auto& B : Backups)
		if (!B.Actor->Destroy(false, false)) { Rollback(); return false; }
	return true;
}

void VrgRackLifecycle::Rebuild(UWorld* World, bool bKeepSelection)
{
	for (TActorIterator<ADataCenterDebugController> It(World); It; ++It) It->RefreshLayoutEquipmentReferences(bKeepSelection);
	for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It) { It->RebuildRoom(); It->RebuildRoomNow(); }
	for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) It->RebuildCoupling();
}
