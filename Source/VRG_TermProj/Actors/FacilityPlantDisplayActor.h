// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Visualization/PlantDisplay.h"
#include "FacilityPlantDisplayActor.generated.h"

class UStaticMeshComponent;

// S8 F7: draws the facility plant from a PlantDisplay snapshot (the integrated actor spawns it while the plant runs in
// simulation mode and removes it in layout mode). Display only: it never reads or changes the plant (rule 5), the
// snapshot is a copy made on the game thread. Pipes recolour by flow (or temperature with vrg.Pipes.Temperature 1, the
// same switch as the room's pipes); meshes are rebuilt only when the snapshot's structure changes.
UCLASS()
class VRG_TERMPROJ_API AFacilityPlantDisplayActor : public AActor
{
	GENERATED_BODY()

public:
	AFacilityPlantDisplayActor();

	void ShowSnapshot(const FPlantDisplaySnapshot& Snapshot);
	const FPlantDisplaySnapshot& GetSnapshot() const { return Shown; }
	// Components by role (tests count these, not every component of the actor).
	int32 GetNumPipeMeshes() const { return PipeMeshes.Num(); }
	int32 GetNumEquipmentMeshes() const { return EquipmentMeshes.Num(); }
	int32 GetNumLeakMeshes() const { return LeakMeshes.Num(); }
	int32 GetNumRebuilds() const { return Rebuilds; }
	// Pipe diameters on screen [m] (schematic).
	double HeaderDiameterM = 0.20, BranchDiameterM = 0.10;

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> PipeMeshes;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> EquipmentMeshes;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> LeakMeshes;
	TArray<int32> PipeMeshOwner;      // display pipe index of every pipe mesh (a polyline has several)
	FPlantDisplaySnapshot Shown;
	uint32 BuiltHash = 0;
	bool bBuilt = false;
	int32 Rebuilds = 0;
	void Rebuild();
	void Recolor();
};
