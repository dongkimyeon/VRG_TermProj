#pragma once
#include "Actors/CoolingDistributionUnitActor.h"
#include "Subsystems/WorldSubsystem.h"
#include "VrgCduLayoutState.generated.h"

// Configuration only at the actor boundary (transform in UE cm). No live thermal state or references.
struct FCduLayoutRecord
{
	FString Id;
	FName Name;
	FTransform Transform;
	FCduLayoutSettings Settings;
	bool bRuntime = false;
};

// World lifetime keeps explicit placed-CDU deletion intent across history eviction/clear and mode changes.
// No level asset is touched. Palette CDUs remain reconciled by the existing v2 inventory.
UCLASS()
class UVrgCduLayoutState : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	TMap<FString, FCduLayoutRecord> DeletedPlaced;
};
