// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CrahUnitActor.generated.h"

class UArrowComponent;
class UStaticMeshComponent;

// How a placed CRAH unit takes in and supplies air (2026-09-27).
UENUM(BlueprintType)
enum class ECrahUnitKind : uint8
{
	// In-row cooler: air in through the rear face, cold air out through the front face (like a rack, reversed).
	InRow,
	// Perimeter unit: return air in through the top face, supply out of the lower half of the front face.
	Perimeter,
};

// Authored layout input only; zero flow retains the existing design-sharing policy.
struct VRG_TERMPROJ_API FCrahLayoutSettings
{
 ECrahUnitKind Kind; FVector SizeM; double VolumeFlowM3s;
 bool IsValid() const;
 bool operator==(const FCrahLayoutSettings& Other) const;
};

// A CRAH / in-row cooler placed by hand (2026-09-27). The actor's forward (+X, the arrow) is the supply direction.
// When the level has any of these, ADataCenterSimulationActor uses them instead of its automatic layout
// (unless it runs the lumped ceiling coil). Rotate in 90 degree steps: the air grid is axis-aligned and the unit's
// airflow axis snaps to the nearest grid axis. Moving it takes effect at the next RebuildCoupling / Play.
// Visual and placement only: the physics (duct faces, coil, fan) lives in the integrated actor and the bridge.
UCLASS()
class VRG_TERMPROJ_API ACrahUnitActor : public AActor
{
	GENERATED_BODY()

public:
	ACrahUnitActor();

	virtual void OnConstruction(const FTransform& Transform) override;
 FCrahLayoutSettings CaptureLayoutSettings() const { return {Kind,SizeM,VolumeFlowM3s}; }
 void ApplyLayoutSettings(const FCrahLayoutSettings& Settings);
 UPROPERTY(VisibleAnywhere, Category = "CRAH|Layout") FString LayoutId;
 UPROPERTY(VisibleAnywhere, Category = "CRAH|Layout") bool bRuntimeLayoutCrah = false;
 // Preserve the automatic grid extent only while this converted box remains unchanged.
 UPROPERTY(VisibleAnywhere, Category = "CRAH|Layout") bool bConvertedAutomaticCrah = false;
 FTransform AutomaticLayoutTransform = FTransform::Identity;
 FVector AutomaticLayoutSizeM = FVector::ZeroVector;
 bool PreservesAutomaticAirBounds() const { return bConvertedAutomaticCrah
  && GetActorTransform().Equals(AutomaticLayoutTransform,1e-9) && SizeM.Equals(AutomaticLayoutSizeM,1e-9); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CRAH")
	ECrahUnitKind Kind = ECrahUnitKind::InRow;

	// Box [m]: X depth (along the supply direction), Y width, Z height. The actor origin is the bottom centre.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CRAH")
	FVector SizeM = FVector(1.068, 0.6, 2.236);

	// Airflow [m^3/s]. 0 = an equal share of the design airflow among the units left at 0.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CRAH", meta = (ClampMin = "0.0"))
	double VolumeFlowM3s = 0.0;

private:
	UPROPERTY(VisibleAnywhere, Category = "CRAH")
	TObjectPtr<UStaticMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, Category = "CRAH")
	TObjectPtr<UArrowComponent> SupplyArrow;
};
