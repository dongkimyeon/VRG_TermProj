#pragma once
#include "Actors/CrahUnitActor.h"
#include "Subsystems/WorldSubsystem.h"
#include "VrgCrahLayoutState.generated.h"

// Explicit placed-CRAH deletion intent survives history clearing/eviction and mode changes.
// Authored configuration only; actor transform is UE cm, size m; no live thermal state.
UCLASS()
class UVrgCrahLayoutState : public UWorldSubsystem
{
 GENERATED_BODY()
public:
 TMap<FString, FCrahLayoutRecord> DeletedPlaced;
};
