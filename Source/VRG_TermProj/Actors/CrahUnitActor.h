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

// Authored placement settings only; coil/fan thermal state belongs to the coupling bridge.
USTRUCT()
struct FCrahLayoutSettings
{
	GENERATED_BODY()
	UPROPERTY() ECrahUnitKind Kind = ECrahUnitKind::InRow;
	UPROPERTY() FVector SizeM = FVector::ZeroVector;
	UPROPERTY() double VolumeFlowM3s = 0.0;
	bool IsValid() const { return (Kind == ECrahUnitKind::InRow || Kind == ECrahUnitKind::Perimeter)
		&& !SizeM.ContainsNaN() && SizeM.GetMin() > 0 && FMath::IsFinite(VolumeFlowM3s) && VolumeFlowM3s >= 0; }
	bool operator==(const FCrahLayoutSettings& R) const { return Kind == R.Kind && SizeM == R.SizeM && VolumeFlowM3s == R.VolumeFlowM3s; }
};
struct FCrahLayoutRecord
{
	FString Id; FName Name; FTransform Transform; FCrahLayoutSettings Settings; bool bRuntime = true;
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
	UPROPERTY(Transient) FString LayoutId;
	UPROPERTY(Transient) bool bRuntimeLayoutCrah = false;
	FCrahLayoutSettings CaptureLayoutSettings() const;
	void ApplyLayoutSettings(const FCrahLayoutSettings& Settings);
	void RefreshVisuals();

	virtual void OnConstruction(const FTransform& Transform) override;

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
