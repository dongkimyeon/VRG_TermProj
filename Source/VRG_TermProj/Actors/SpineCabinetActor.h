#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpineCabinetActor.generated.h"

class UStaticMeshComponent;

// Row-end network cabinet: the patch panel / spine switches that rack uplinks terminate on.
// The room actor's patch manager discovers these, assigns their ports and routes cables to them.
//
// --- Display only ---
// This cabinet is NOT a heat source, is NOT solid in the air grid and has no hydraulic branch.
// Real spine switches dissipate roughly 1-2 kW each; that is outside the current thermal model
// (same as the NVLink switch trays and power shelves missing from the GB200 racks).
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

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> Meshes;
};
