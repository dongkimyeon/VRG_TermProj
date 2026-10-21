// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/PlantEquipmentActor.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Visualization/PlantDisplay.h"
#include "Visualization/PlantVisual.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

APlantEquipmentActor::APlantEquipmentActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Movable);
	// Referenced from the CDO so the cooker packages the shapes the parts are drawn with.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CookedShapes = { Cube.Object, Cylinder.Object };
	CookedMaterial = Material.Object;
}

void APlantEquipmentActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisuals();
}

void APlantEquipmentActor::RefreshVisuals()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || !RootComponent) return;
	const FVector Size = SizeM.ComponentMax(FVector(0.01));
	const FLinearColor Accent = StatusColor.A > 0.f ? StatusColor : IdentityColor(Kind);
	bool bSetsAlive = true;
	for (const auto& Pair : PartSets) bSetsAlive &= IsValid(Pair.Value);
	if (bDrawn && bSetsAlive && DrawnSizeM.Equals(Size, 0.0) && DrawnKind == Kind && DrawnColor.Equals(Accent, 0.f)) return;
	// Parts in actor space [cm]: the actor origin is the box's bottom centre, like the parts' own origin.
	const bool bChiller = Kind == EPlantUnitKind::Chiller || Kind == EPlantUnitKind::TrimChiller;
	TArray<FPlantVisualPart> Parts;
	PlantVisual::BuildParts(bChiller ? EPlantVisualKind::Chiller : EPlantVisualKind::DryCooler, Size, Parts);
	TArray<FPlantInstance> List;
	for (const FPlantVisualPart& P : Parts)
		List.Add({ P.bCylinder ? PlantInstances::Cylinder() : PlantInstances::Cube(), PlantVisual::ToneColor(P.Tone, Accent), PlantVisual::PartTransformCm(P) });
	PlantInstances::Apply(this, RootComponent, PartSets, List, true);
	DrawnSizeM = Size; DrawnKind = Kind; DrawnColor = Accent; bDrawn = true;
}

void APlantEquipmentActor::SetStatusColor(const FLinearColor& Color)
{
	if (StatusColor.Equals(Color, 0.f)) return;
	StatusColor = Color;
	RefreshVisuals();
}

FLinearColor APlantEquipmentActor::IdentityColor(EPlantUnitKind Kind)
{
	// The yard's kind colours (FWS orange, CW green, chillers blue, trim violet).
	switch (Kind)
	{
	case EPlantUnitKind::FwsDryCooler: return PlantDisplay::KindColor(EPlantLoopId::Fws, EPlantEquipmentKind::DryCooler);
	case EPlantUnitKind::CwDryCooler: return PlantDisplay::KindColor(EPlantLoopId::Cw, EPlantEquipmentKind::DryCooler);
	case EPlantUnitKind::Chiller: return PlantDisplay::KindColor(EPlantLoopId::Chw, EPlantEquipmentKind::Chiller);
	default: return PlantDisplay::KindColor(EPlantLoopId::Fws, EPlantEquipmentKind::TrimChiller);
	}
}

FVector APlantEquipmentActor::DefaultSizeM(EPlantUnitKind Kind)
{
	return Kind == EPlantUnitKind::FwsDryCooler || Kind == EPlantUnitKind::CwDryCooler ? FVector(2.4, 1.2, 1.3) : FVector(2.4, 1.2, 1.8);
}

const TCHAR* APlantEquipmentActor::KindName(EPlantUnitKind Kind)
{
	switch (Kind)
	{
	case EPlantUnitKind::FwsDryCooler: return TEXT("FWS 드라이쿨러");
	case EPlantUnitKind::CwDryCooler: return TEXT("CW 드라이쿨러");
	case EPlantUnitKind::Chiller: return TEXT("냉수 칠러");
	default: return TEXT("트림 칠러");
	}
}

FString VrgPlantEquipment::Identity(const APlantEquipmentActor* Unit)
{
	return Unit->LayoutId.IsEmpty() ? TEXT("placed/") + Unit->GetName() : Unit->LayoutId;
}

APlantEquipmentActor* VrgPlantEquipment::Find(UWorld* World, const FString& Id)
{
	if (!World) return nullptr;
	for (TActorIterator<APlantEquipmentActor> It(World); It; ++It) if (Identity(*It) == Id) return *It;
	return nullptr;
}

bool VrgPlantEquipment::IsValidRecord(const FPlantEquipmentRecord& R)
{
	return !R.Transform.ContainsNaN() && R.Transform.GetRotation().IsNormalized() && R.Transform.GetScale3D().Equals(FVector::OneVector, 0.0)
		&& !R.SizeM.ContainsNaN() && R.SizeM.GetMin() > 0.0 && uint8(R.Kind) <= uint8(EPlantUnitKind::TrimChiller);
}

APlantEquipmentActor* VrgPlantEquipment::Spawn(UWorld* World, const FPlantEquipmentRecord& R)
{
	if (!World || !World->IsGameWorld() || R.Id.IsEmpty() || R.Name.IsNone() || !IsValidRecord(R)
		|| Find(World, R.Id) || FindObjectFast<UObject>(World->PersistentLevel, R.Name)) return nullptr;
	FActorSpawnParameters P;
	P.Name = R.Name;
	P.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.bDeferConstruction = true;
	auto* Unit = World->SpawnActor<APlantEquipmentActor>(APlantEquipmentActor::StaticClass(), R.Transform, P);
	auto RetireFailed = [&]()
	{
		if (Unit && Unit->GetFName() == R.Name)
			Unit->Rename(*MakeUniqueObjectName(Unit->GetOuter(), Unit->GetClass(), TEXT("FailedPlantEquipment")).ToString(),
				nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	};
	if (!IsValid(Unit)) { RetireFailed(); return nullptr; }
	Unit->LayoutId = R.Id;
	Unit->Kind = R.Kind;
	Unit->SizeM = R.SizeM;
	Unit->FinishSpawning(R.Transform);
	if (!IsValid(Unit)) { RetireFailed(); return nullptr; }
	Unit->RefreshVisuals();
	return Unit;
}

FPlantEquipmentRecord VrgPlantEquipment::Capture(const APlantEquipmentActor* Unit)
{
	return { Identity(Unit), Unit->GetFName(), Unit->GetActorTransform(), Unit->Kind, Unit->SizeM };
}

bool VrgPlantEquipment::RemoveBatch(const TArray<APlantEquipmentActor*>& Units)
{
	if (Units.IsEmpty()) return false;
	UWorld* World = IsValid(Units[0]) ? Units[0]->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld()) return false;
	TSet<APlantEquipmentActor*> Seen;
	for (auto* U : Units)
	{
		if (!IsValid(U) || U->GetWorld() != World || U->IsActorBeingDestroyed() || U->GetLocalRole() != ROLE_Authority
			|| Seen.Contains(U)) return false;
		Seen.Add(U);
	}
	TArray<FPlantEquipmentRecord> Removed;
	for (auto* U : Units)
	{
		const FPlantEquipmentRecord Record = Capture(U);
		// Release the name first: Destroy keeps the UObject until GC and a rollback must recreate the same name at once.
		const FName Retired = MakeUniqueObjectName(U->GetOuter(), U->GetClass(), TEXT("RetiredPlantEquipment"));
		if (U->Rename(*Retired.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional) && U->Destroy(false, false))
		{
			Removed.Add(Record);
			continue;
		}
		if (IsValid(U)) U->Rename(*Record.Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
		for (const auto& R : Removed)
		{
			auto* Restored = Spawn(World, R);
			checkf(Restored, TEXT("Plant equipment deletion rollback could not restore %s"), *R.Name.ToString());
		}
		return false;
	}
	return true;
}

TArray<APlantEquipmentActor*> VrgPlantEquipment::Gather(UWorld* World, EPlantUnitKind Kind)
{
	TArray<APlantEquipmentActor*> Out;
	if (!World) return Out;
	for (TActorIterator<APlantEquipmentActor> It(World); It; ++It)
		if (IsValid(*It) && !It->IsActorBeingDestroyed() && It->Kind == Kind) Out.Add(*It);
	Out.Sort([](const APlantEquipmentActor& A, const APlantEquipmentActor& B) { return Identity(&A) < Identity(&B); });
	return Out;
}

FPlantEquipmentCounts VrgPlantEquipment::CountInWorld(UWorld* World)
{
	FPlantEquipmentCounts C;
	C.FwsDryCoolers = Gather(World, EPlantUnitKind::FwsDryCooler).Num();
	C.CwDryCoolers = Gather(World, EPlantUnitKind::CwDryCooler).Num();
	C.Chillers = Gather(World, EPlantUnitKind::Chiller).Num();
	C.TrimChillers = Gather(World, EPlantUnitKind::TrimChiller).Num();
	return C;
}

bool VrgPlantEquipment::RoomFootprintM(UWorld* World, FBox2D& Out)
{
	if (!World) return false;
	for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
	{
		const FRoomAirDomainSettings& Room = It->GetRoomAirDomain();
		if (Room.bManual) { Out = FBox2D(Room.MinXYM, Room.MaxXYM); return Out.bIsValid; }
		break;
	}
	for (TActorIterator<AAirFieldDebugActor> It(World); It; ++It)
	{
		const FBox B = It->GetDomainBoundsM();
		Out = FBox2D(FVector2D(B.Min.X, B.Min.Y), FVector2D(B.Max.X, B.Max.Y));
		return B.IsValid && Out.GetArea() > 0.0;
	}
	return false;
}

bool VrgPlantEquipment::OverlapsRoom(UWorld* World, const FVector& SizeM, const FTransform& Transform)
{
	FBox2D Room(ForceInit);
	return RoomFootprintM(World, Room) && OverlapsBox(Room, SizeM, Transform);
}

bool VrgPlantEquipment::OverlapsBox(const FBox2D& Room, const FVector& SizeM, const FTransform& Transform)
{
	if (!Room.bIsValid) return false;
	FBox2D Box(ForceInit);
	for (int32 C = 0; C < 8; ++C)
	{
		const FVector Local((C & 1 ? .5 : -.5) * SizeM.X, (C & 2 ? .5 : -.5) * SizeM.Y, C & 4 ? SizeM.Z : 0.0);
		const FVector P = SimUnits::UnrealToMeters(Transform.TransformPosition(SimUnits::MetersToUnreal(Local)));
		Box += FVector2D(P.X, P.Y);
	}
	return Box.Min.X < Room.Max.X && Box.Max.X > Room.Min.X && Box.Min.Y < Room.Max.Y && Box.Max.Y > Room.Min.Y;
}
