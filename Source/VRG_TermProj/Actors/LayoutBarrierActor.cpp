// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/LayoutBarrierActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

ALayoutBarrierActor::ALayoutBarrierActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Referenced from the CDO so the cooker packages them.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Body->SetStaticMesh(Cube.Object);
	if (Material.Succeeded()) Body->SetMaterial(0, Material.Object);
	// The engine cube is 100 cm centred; lift it so the actor origin is the bottom centre. The actor scale gives metres.
	Body->SetRelativeLocation(FVector(0.0, 0.0, 50.0));
}

void ALayoutBarrierActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisuals();
}

void ALayoutBarrierActor::RefreshVisuals()
{
	if (!Body || HasAnyFlags(RF_ClassDefaultObject)) return;
	if (!BodyMaterial)
	{
		BodyMaterial = Body->CreateDynamicMaterialInstance(0);
	}
	// Display colours only: grey wall, pale blue curtain.
	if (BodyMaterial)
	{
		BodyMaterial->SetVectorParameterValue(TEXT("Color"),
			Kind == EBarrierKind::Wall ? FLinearColor(.55f, .55f, .52f) : FLinearColor(.45f, .7f, .95f));
	}
}

FVector ALayoutBarrierActor::DefaultSizeM(EBarrierKind Kind)
{
	// UI starting sizes for a new barrier [m]; the user resizes with the scale gizmo.
	return Kind == EBarrierKind::Wall ? FVector(0.2, 3.0, 3.0) : FVector(0.02, 2.4, 2.4);
}

FString VrgBarrierLifecycle::Identity(const ALayoutBarrierActor* Barrier)
{
	return Barrier->LayoutId.IsEmpty() ? TEXT("placed/") + Barrier->GetName() : Barrier->LayoutId;
}

ALayoutBarrierActor* VrgBarrierLifecycle::Find(UWorld* World, const FString& Id)
{
	if (!World) return nullptr;
	for (TActorIterator<ALayoutBarrierActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
	return nullptr;
}

bool VrgBarrierLifecycle::IsValidTransform(const FTransform& Transform)
{
	const FVector S = Transform.GetScale3D();
	return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized() && S.X > 0.0 && S.Y > 0.0 && S.Z > 0.0;
}

ALayoutBarrierActor* VrgBarrierLifecycle::Spawn(UWorld* World, const FLayoutBarrierRecord& R)
{
	if (!World || !World->IsGameWorld() || R.Id.IsEmpty() || R.Name.IsNone() || !IsValidTransform(R.Transform)
		|| (R.Kind != EBarrierKind::Wall && R.Kind != EBarrierKind::Curtain)
		|| Find(World, R.Id) || FindObjectFast<UObject>(World->PersistentLevel, R.Name)) return nullptr;
	FActorSpawnParameters P;
	P.Name = R.Name;
	P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	auto* Barrier = World->SpawnActor<ALayoutBarrierActor>(ALayoutBarrierActor::StaticClass(), R.Transform, P);
	auto RetireFailed = [&]()
	{
		if (Barrier && Barrier->GetFName() == R.Name)
			Barrier->Rename(*MakeUniqueObjectName(Barrier->GetOuter(), Barrier->GetClass(), TEXT("FailedLayoutBarrier")).ToString(),
				nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	};
	if (!IsValid(Barrier)) { RetireFailed(); return nullptr; }
	Barrier->LayoutId = R.Id;
	Barrier->Kind = R.Kind;
	Barrier->FinishSpawning(R.Transform);
	if (!IsValid(Barrier)) { RetireFailed(); return nullptr; }
	Barrier->RefreshVisuals();
	return Barrier;
}

bool VrgBarrierLifecycle::RemoveBatch(const TArray<ALayoutBarrierActor*>& Barriers)
{
	if (Barriers.IsEmpty()) return false;
	UWorld* World = IsValid(Barriers[0]) ? Barriers[0]->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld()) return false;
	TSet<ALayoutBarrierActor*> Seen;
	for (auto* B : Barriers)
	{
		if (!IsValid(B) || B->GetWorld() != World || B->IsActorBeingDestroyed() || B->GetLocalRole() != ROLE_Authority
			|| Seen.Contains(B)) return false;
		Seen.Add(B);
	}
	TArray<FLayoutBarrierRecord> Removed;
	for (auto* B : Barriers)
	{
		const FLayoutBarrierRecord Record{Identity(B), B->GetFName(), B->GetActorTransform(), B->Kind};
		const FString PreviousId = B->LayoutId;
		// Release the name first: Destroy keeps the UObject until GC and Undo must recreate the same name at once.
		const FName Retired = MakeUniqueObjectName(B->GetOuter(), B->GetClass(), TEXT("RetiredLayoutBarrier"));
		if (B->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional) && B->Destroy(false, false))
		{
			Removed.Add(Record);
			continue;
		}
		if (IsValid(B)) B->Rename(*Record.Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
		for (const auto& R : Removed)
		{
			auto* Restored = Spawn(World, R);
			checkf(Restored, TEXT("Barrier deletion rollback could not restore %s"), *R.Name.ToString());
		}
		return false;
	}
	return true;
}
