#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "SpineCabinetActor.generated.h"

class UStaticMeshComponent;

// Authored layout configuration only. Thermal state/input samples are deliberately absent.
USTRUCT()
struct FSpineLayoutSettings
{
	GENERATED_BODY()
	UPROPERTY() int32 NumSwitches = 0;
	UPROPERTY() int32 FirstSwitchSlot = 0;
	bool IsValid() const { return NumSwitches >= 1 && NumSwitches <= 8 && FirstSwitchSlot >= 0 && FirstSwitchSlot <= 36; }
	bool operator==(const FSpineLayoutSettings& Other) const { return NumSwitches == Other.NumSwitches && FirstSwitchSlot == Other.FirstSwitchSlot; }
};

// Row-end network cabinet: the patch panel / spine switches that rack uplinks terminate on.
// The room actor's patch manager discovers these, assigns their ports and routes cables to them.
//
// Air-only thermal target, stepped exclusively by DataCenterSimulationActor (one clock).
// No hydraulic branch; visual port count is photo-based, not the QM9700 thermal reference.
//
// Same 48U frame and the same reversed Quantum switch as the GB200 rack, so ports face the rear (-X)
// and share the hot-aisle cable tray. Empty U positions are closed with 1U blanks.
UCLASS()
class VRG_TERMPROJ_API ASpineCabinetActor : public AActor
{
	GENERATED_BODY()

public:
	ASpineCabinetActor();

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine", meta = (ClampMin = "1", ClampMax = "8"))
	int32 NumSwitches = 4;

	// Bottom U index (counted from the compute rack's first node slot) of the lowest switch.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine", meta = (ClampMin = "0", ClampMax = "36"))
	int32 FirstSwitchSlot = 16;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Spine")
	void RefreshVisuals();

	int32 GetNumPorts() const;
	// Port centre on the switch cage face, UE world metres. Port = Switch*36 + (upper ? 18 : 0) + Column-1.
	FVector GetPortWorldM(int32 Port) const;
	// Unit vector pointing out of the port (the cabinet's rear, world).
	FVector GetPortOutwardWorld() const { return -GetActorForwardVector(); }
	double GetSwitchZM(int32 Switch) const;
	void EnsureThermalModel();
	FSpineLayoutSettings CaptureLayoutSettings() const;
	void ApplyLayoutSettings(const FSpineLayoutSettings& Settings);
	UPROPERTY(Transient) FString LayoutId;
	// Only runtime palette equipment participates in authoritative inventory reconciliation.
	UPROPERTY(Transient) bool bRuntimeLayoutSpine = false;
	FRackThermalModel& GetMutableModel() { return Model; }
	FRackThermalInput& GetMutableInput() { return Input; }
	double GetDesignHeatToAirW() const { return FRackThermalModel::DesignHeatToAirW(Model.GetParams(), Input); }

private:
	FRackThermalModel Model;
	FRackThermalInput Input;
	int32 ThermalSwitchCount = INDEX_NONE, ThermalFirstSlot = INDEX_NONE;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Meshes;
};
