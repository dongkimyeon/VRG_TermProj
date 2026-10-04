#include "Interaction/VrgSpineLifecycle.h"
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
	return IsValid(Spine) ? Spine : nullptr;
}

bool VrgSpineLifecycle::Remove(ASpineCabinetActor* Spine)
{
	if (!IsValid(Spine) || !Spine->GetWorld() || !Spine->GetWorld()->IsGameWorld()
		|| Spine->IsActorBeingDestroyed() || Spine->IsActorBeginningPlay()
		|| Spine->GetClass() != ASpineCabinetActor::StaticClass() || Spine->GetLocalRole() != ROLE_Authority) return false;
	const FName Name = Spine->GetFName();
	const FName Retired = MakeUniqueObjectName(Spine->GetOuter(), Spine->GetClass(), TEXT("RetiredLayoutSpine"));
	if (!Spine->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional)) return false;
	if (Spine->Destroy(false, false)) return true;
	Spine->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	return false;
}
