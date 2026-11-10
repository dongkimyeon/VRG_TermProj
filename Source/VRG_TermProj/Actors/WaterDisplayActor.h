// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Visualization/WaterDisplay.h"
#include "WaterDisplayActor.generated.h"

class UInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class UMaterialInterface;

// W6: draws a FWaterDisplaySnapshot (Visualization/WaterDisplay.h). The floor surface and the dry-now wet marks are a
// procedural mesh; drops (spheres), last-step paths (cylinders), films and drip trays (thin boxes) and wet skins (boxes)
// are instanced engine shapes. Materials: /Game/Water/Materials/M_VrgWater and M_VrgWetSurface
// (Tools/install_water_materials.py); without them a tinted engine material stands in. Display only: the snapshot is
// built on the game thread from the models' state (rule 5); this actor never reads a solver. Metres -> cm here (rule 1).
UCLASS()
class VRG_TERMPROJ_API AWaterDisplayActor : public AActor
{
	GENERATED_BODY()

public:
	AWaterDisplayActor();
	// Floor meshes are rebuilt only when bFloor (the caller throttles them); the instances every call.
	void Show(const FWaterDisplaySnapshot& Snapshot, bool bFloor);
	void Clear();
	bool UsesWaterMaterial() const { return bWaterMaterial; }
	int32 GetNumInstances() const { return NumInstances; }

private:
	UPROPERTY(Transient) TObjectPtr<UProceduralMeshComponent> Surface;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Drops;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Streaks;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Sheets;
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Skins;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> WaterMaterial;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> WetMaterial;
	bool bWaterMaterial = false;
	int32 NumInstances = 0;
	bool bSurfaceShown = false, bWetShown = false;
	void EnsureComponents();
};
