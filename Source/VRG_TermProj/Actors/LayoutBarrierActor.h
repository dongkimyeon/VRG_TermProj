// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LayoutBarrierActor.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

// Layout wall / air curtain (2026-10-06). Both are impermeable, adiabatic solids in the air grid:
// no heat capacity, no conduction, no leakage. The two kinds differ only in their default size and colour.
UENUM(BlueprintType)
enum class EBarrierKind : uint8
{
	Wall,
	Curtain,
};

struct FLayoutBarrierRecord
{
	FString Id; FName Name; FTransform Transform; EBarrierKind Kind = EBarrierKind::Wall;
};

// A box placed by hand in layout mode. The actor scale IS the box size in metres (X thickness, Y width, Z height),
// the origin is the bottom centre, so the scale gizmo resizes the box and the existing transform Undo/Redo and
// layout file v2 (full transform) carry the size with no separate size field.
//
// Air grid: cells whose centre lies in the box become solid. Each half extent is widened to at least 0.51 cell
// so a barrier thinner than one cell still blocks every face path (calculation thickness >= one cell; the mesh
// shows the authored thickness). Cells next to equipment duct faces are left open (see MarkRackSolidCells).
UCLASS()
class VRG_TERMPROJ_API ALayoutBarrierActor : public AActor
{
	GENERATED_BODY()

public:
	ALayoutBarrierActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	void RefreshVisuals();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Barrier")
	EBarrierKind Kind = EBarrierKind::Wall;

	UPROPERTY(VisibleAnywhere, Category = "Barrier|Layout")
	FString LayoutId;

	// UI defaults for a new barrier [m] (thickness, width, height). Placement choices, not material or room data.
	static FVector DefaultSizeM(EBarrierKind Kind);

	// Size [m] = actor scale. Only meaningful for positive scale.
	FVector GetSizeM() const { return GetActorScale3D(); }

	// Local box [-0.5, 0.5] x [-0.5, 0.5] x [0, 1] in actor space (scaled to metres by the actor scale).
	static FVector UnitBoxM() { return FVector(1.0, 1.0, 1.0); }

private:
	UPROPERTY(VisibleAnywhere, Category = "Barrier")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMaterial;
};

namespace VrgBarrierLifecycle
{
	VRG_TERMPROJ_API FString Identity(const ALayoutBarrierActor* Barrier);
	VRG_TERMPROJ_API ALayoutBarrierActor* Find(UWorld* World, const FString& Id);
	// Runtime world only; fails (and leaves nothing behind) on a name/id clash or an invalid transform.
	VRG_TERMPROJ_API ALayoutBarrierActor* Spawn(UWorld* World, const FLayoutBarrierRecord& Record);
	VRG_TERMPROJ_API bool IsValidTransform(const FTransform& Transform);
	// Destroys every barrier or none: on a failure the already removed ones are spawned back.
	VRG_TERMPROJ_API bool RemoveBatch(const TArray<ALayoutBarrierActor*>& Barriers);
}
