#pragma once
#include "Actors/SpineCabinetActor.h"
#include "Subsystems/WorldSubsystem.h"
#include "VrgSpineLayoutState.generated.h"

// Configuration only at the actor boundary (transform in UE cm). No live thermal state or references.
struct FSpineLayoutRecord
{
	FString Id;
	FName Name;
	FTransform Transform;
	FSpineLayoutSettings Settings;
	bool bRuntime = false;
};

// World lifetime keeps explicit placed-spine deletion intent across history eviction/clear and mode changes.
// No level asset is touched. Palette spines remain reconciled by the existing v2 inventory.
UCLASS()
class UVrgSpineLayoutState : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	TMap<FString, FSpineLayoutRecord> DeletedPlaced;
};
