#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgSpineLayoutState.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

FString VrgSpineLifecycle::Identity(const ASpineCabinetActor* Spine)
{
	return Spine->LayoutId.IsEmpty() ? TEXT("placed/") + Spine->GetName() : Spine->LayoutId;
}

ASpineCabinetActor* VrgSpineLifecycle::Find(UWorld* World, const FString& Id)
{
	for (TActorIterator<ASpineCabinetActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
	return nullptr;
}

ASpineCabinetActor* VrgSpineLifecycle::Spawn(UWorld* World, FName Name, const FString& Id,
	const FTransform& Transform, const FSpineLayoutSettings& Settings, bool bRuntime)
{
	if (!World || !World->IsGameWorld() || !Settings.IsValid() || Id.IsEmpty()
		|| Find(World, Id) || FindObjectFast<UObject>(World->PersistentLevel, Name)) return nullptr;
	FActorSpawnParameters P;
	P.Name = Name; P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	auto* Spine = World->SpawnActor<ASpineCabinetActor>(ASpineCabinetActor::StaticClass(), Transform, P);
	if (!IsValid(Spine)) return nullptr;
	Spine->LayoutId = Id; Spine->bRuntimeLayoutSpine = bRuntime;
	Spine->ApplyLayoutSettings(Settings);
	Spine->FinishSpawning(Transform);
	if (!IsValid(Spine)) return nullptr;
	World->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Remove(Id);
	return Spine;
}

bool VrgSpineLifecycle::Remove(ASpineCabinetActor* Spine)
{
	if (!IsValid(Spine) || !Spine->GetWorld() || !Spine->GetWorld()->IsGameWorld()
		|| Spine->IsActorBeingDestroyed() || Spine->IsActorBeginningPlay()
		|| Spine->GetClass() != ASpineCabinetActor::StaticClass() || Spine->GetLocalRole() != ROLE_Authority) return false;
	const FName Name = Spine->GetFName();
	const FSpineLayoutRecord Record{Identity(Spine), Name, Spine->GetActorTransform(),
		Spine->CaptureLayoutSettings(), Spine->bRuntimeLayoutSpine};
	const FName Retired = MakeUniqueObjectName(Spine->GetOuter(), Spine->GetClass(), TEXT("RetiredLayoutSpine"));
	if (!Spine->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
	if (Spine->Destroy(false, false))
	{
		if (!Record.bRuntime) Spine->GetWorld()->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced.Add(Record.Id, Record);
		return true;
	}
	Spine->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	return false;
}

bool VrgSpineLifecycle::RemoveBatch(const TArray<ASpineCabinetActor*>& Spines)
{
	if (Spines.IsEmpty()) return false;
	UWorld* World = IsValid(Spines[0]) ? Spines[0]->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld()) return false;
	TSet<ASpineCabinetActor*> Seen; TSet<FString> Ids;
	for (auto* Spine : Spines)
	{
		if (!IsValid(Spine) || Spine->GetWorld() != World || Spine->IsActorBeingDestroyed()
			|| Spine->IsActorBeginningPlay() || Spine->GetLocalRole() != ROLE_Authority
			|| Spine->GetClass() != ASpineCabinetActor::StaticClass()
			|| Seen.Contains(Spine) || Ids.Contains(Identity(Spine))) return false;
		Seen.Add(Spine); Ids.Add(Identity(Spine));
	}
	struct FBackup
	{
		ASpineCabinetActor* Actor; FSpineLayoutRecord Record; FString PreviousId;
		FRackThermalModel Model; FRackThermalInput Input; ENetRole Role;
	};
	TArray<FBackup> Backups;
	for (auto* Spine : Spines)
		Backups.Add({Spine, {Identity(Spine), Spine->GetFName(), Spine->GetActorTransform(),
			Spine->CaptureLayoutSettings(), Spine->bRuntimeLayoutSpine}, Spine->LayoutId,
			Spine->GetMutableModel(), Spine->GetMutableInput(), Spine->GetLocalRole()});
	const auto DeletedBefore = World->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced;
	for (auto* Spine : Spines) if (!Remove(Spine))
	{
		// Failed transaction only: preserve live state and references. Successful Undo
		// continues to recreate fresh thermals from authored configuration alone.
		for (const auto& B : Backups) if (IsValid(B.Actor)) B.Actor->SetRole(B.Role);
		for (const auto& B : Backups) if (!IsValid(B.Actor))
		{
			const auto& R = B.Record;
			auto* Restored = Spawn(World, R.Name, R.Id, R.Transform, R.Settings, R.bRuntime);
			checkf(Restored, TEXT("Spine deletion rollback could not restore %s"), *R.Name.ToString());
			Restored->GetMutableModel() = B.Model; Restored->GetMutableInput() = B.Input;
			Restored->SetRole(B.Role);
			for (TActorIterator<ADataCenterDebugController> It(World); It; ++It) It->NotifyLayoutSpineRecreated(Restored, B.Actor);
			Restored->LayoutId = B.PreviousId;
		}
		World->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced = DeletedBefore;
		VrgRackLifecycle::Rebuild(World, true);
		return false;
	}
	return true;
}
