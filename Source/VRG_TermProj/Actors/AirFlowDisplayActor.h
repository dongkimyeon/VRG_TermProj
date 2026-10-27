// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Visualization/PlantVisual.h"
#include "AirFlowDisplayActor.generated.h"

class UInstancedStaticMeshComponent;

// Draws the air view's tubes and plan slice as instanced shapes grouped by colour band (PlantInstances), spawned by the
// integrated actor in simulation mode and cleared in layout mode. Display only: the instance list is built on the game
// thread from a copy of the averaged field (rule 5); this actor never reads the solver.
UCLASS()
class VRG_TERMPROJ_API AAirFlowDisplayActor : public AActor
{
	GENERATED_BODY()

public:
	AAirFlowDisplayActor();
	void Show(const TArray<FPlantInstance>& List);
	void Clear();
	int32 GetNumShown() const { return NumShown; }

private:
	UPROPERTY(Transient) TMap<uint64, TObjectPtr<UInstancedStaticMeshComponent>> Sets;
	int32 NumShown = 0;
};
