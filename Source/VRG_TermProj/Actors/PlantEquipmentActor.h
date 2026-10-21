// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "PlantEquipmentActor.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

// S8 F8 (Tools/설비냉각_계획.md): one facility plant unit placed by hand in the plant yard outside the room.
UENUM(BlueprintType)
enum class EPlantUnitKind : uint8
{
	FwsDryCooler,   // FWS bank (CDU primaries, separate topology)
	CwDryCooler,    // condenser water bank (chiller condensers)
	Chiller,        // chilled water chillers (duty + standby)
	TrimChiller,    // FWS trim chillers (duty + standby)
};

struct FPlantEquipmentRecord
{
	FString Id; FName Name; FTransform Transform; EPlantUnitKind Kind = EPlantUnitKind::FwsDryCooler; FVector SizeM = FVector::ZeroVector;
};

// A box (SizeM: depth, width, height; origin at the bottom centre, actor scale 1) like the other layout equipment, so
// selection, drag, rotation, the marquee, the 1 cm overlap rule and F framing use the shared layout code. It is drawn
// as the unit's parts (PlantVisual: fans and coil of a dry cooler, shells and panel of a chiller) inside that box;
// the accent parts show the plant's status for this unit (SetStatusColor from the yard display).
//
// What it changes in the model: when at least one actor of a kind exists, the plant has exactly that many units of the
// kind (FacilityPlantLayout::ApplyEquipmentCounts); with none, the automatic sizing rule of F2~F4 stays. Its location
// is where the plant yard draws that unit and routes its pipes (the yard stays a schematic: no plant piping geometry,
// so moving a unit changes no pressure drop). The plant sits outside the air domain: a unit may not be placed over the
// room (the air grid has no plant equipment, so inside it would show something the model does not compute).
UCLASS()
class VRG_TERMPROJ_API APlantEquipmentActor : public AActor
{
	GENERATED_BODY()

public:
	APlantEquipmentActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Visualization")
	void RefreshVisuals();
	// Accent colour (identity, or running / stopped / failed while the plant is live). Redraws only on a change.
	void SetStatusColor(const FLinearColor& Color);
	static FLinearColor IdentityColor(EPlantUnitKind Kind);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plant")
	EPlantUnitKind Kind = EPlantUnitKind::FwsDryCooler;

	// Box size [m]. A new unit takes DefaultSizeM(Kind).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plant")
	FVector SizeM = FVector(2.4, 1.2, 1.3);

	UPROPERTY(VisibleAnywhere, Category = "Plant|Layout")
	FString LayoutId;

	// Schematic box sizes [m] (the same as the F7 yard; not catalog dimensions).
	static FVector DefaultSizeM(EPlantUnitKind Kind);
	static const TCHAR* KindName(EPlantUnitKind Kind);

private:
	UPROPERTY(Transient)
	TMap<uint64, TObjectPtr<UInstancedStaticMeshComponent>> PartSets;
	// Hard references so the cooker packages the engine shapes and material the parts use.
	UPROPERTY() TArray<TObjectPtr<UStaticMesh>> CookedShapes;
	UPROPERTY() TObjectPtr<UMaterialInterface> CookedMaterial;
	FLinearColor StatusColor = FLinearColor::Transparent;   // transparent = identity colour
	FVector DrawnSizeM = FVector::ZeroVector;
	FLinearColor DrawnColor = FLinearColor::Transparent;
	EPlantUnitKind DrawnKind = EPlantUnitKind::FwsDryCooler;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> DrawnMesh;
	bool bDrawn = false;
};

namespace VrgPlantEquipment
{
	VRG_TERMPROJ_API FString Identity(const APlantEquipmentActor* Unit);
	VRG_TERMPROJ_API APlantEquipmentActor* Find(UWorld* World, const FString& Id);
	// Positive size, unit scale, normalised rotation, finite values.
	VRG_TERMPROJ_API bool IsValidRecord(const FPlantEquipmentRecord& Record);
	// Runtime world only; fails (and leaves nothing behind) on a name/id clash or an invalid record.
	VRG_TERMPROJ_API APlantEquipmentActor* Spawn(UWorld* World, const FPlantEquipmentRecord& Record);
	VRG_TERMPROJ_API FPlantEquipmentRecord Capture(const APlantEquipmentActor* Unit);
	// Destroys every unit or none: on a failure the already removed ones are spawned back.
	VRG_TERMPROJ_API bool RemoveBatch(const TArray<APlantEquipmentActor*>& Units);
	// Units of one kind in a stable order (by identity): unit K of the plant model is the K-th actor.
	VRG_TERMPROJ_API TArray<APlantEquipmentActor*> Gather(UWorld* World, EPlantUnitKind Kind);
	VRG_TERMPROJ_API FPlantEquipmentCounts CountInWorld(UWorld* World);
	// XY footprint of the air domain the layout currently has: the manual room, else the air grid actor's domain.
	VRG_TERMPROJ_API bool RoomFootprintM(UWorld* World, FBox2D& Out);
	// True when the box (UE transform, size in m) overlaps the room footprint in XY (touching is allowed).
	VRG_TERMPROJ_API bool OverlapsBox(const FBox2D& RoomM, const FVector& SizeM, const FTransform& Transform);
	VRG_TERMPROJ_API bool OverlapsRoom(UWorld* World, const FVector& SizeM, const FTransform& Transform);
}
