// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Visualization/PlantDisplay.h"
#include "Visualization/PlantVisual.h"
#include "FacilityPlantDisplayActor.generated.h"

class UInstancedStaticMeshComponent;

// S8 F7: draws the facility plant from a PlantDisplay snapshot (the integrated actor spawns it whenever a plant is built,
// in layout and simulation mode, with or without vrg.Plant.Enable). Display only: it never reads or changes the plant
// (rule 5), the snapshot is a copy made on the game thread. Pipes recolour by flow (or temperature with
// vrg.Pipes.Temperature 1, the same switch as the room's pipes) while the plant is live, loop colours otherwise.
// Everything is instanced shapes grouped by colour (PlantInstances); the upload is skipped when nothing changed.
// Units placed by hand (APlantEquipmentActor) draw themselves; the yard draws the automatic units.
UCLASS()
class VRG_TERMPROJ_API AFacilityPlantDisplayActor : public AActor
{
	GENERATED_BODY()

public:
	AFacilityPlantDisplayActor();

	void ShowSnapshot(const FPlantDisplaySnapshot& Snapshot);
	const FPlantDisplaySnapshot& GetSnapshot() const { return Shown; }
	// What was drawn (tests count these, not components).
	int32 GetNumPipeSegments() const { return PipeSegments; }
	int32 GetNumHeaderSegments() const { return HeaderSegments; }
	int32 GetNumEquipmentDrawn() const { return EquipmentDrawn; }
	int32 GetNumLeakMarkers() const { return LeakMarkers; }
	int32 GetNumUploads() const { return Uploads; }

private:
	UPROPERTY(Transient) TMap<uint64, TObjectPtr<UInstancedStaticMeshComponent>> Sets;
	FPlantDisplaySnapshot Shown;
	uint32 UploadedHash = 0;
	bool bUploaded = false;
	int32 PipeSegments = 0, HeaderSegments = 0, EquipmentDrawn = 0, LeakMarkers = 0, Uploads = 0;
};
