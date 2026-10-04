#pragma once
#include "Actors/RackThermalActor.h"

// Shared game-thread runtime layout lifecycle. No solver state or level-asset mutation.
namespace VrgRackLifecycle
{
	FString Identity(const ARackThermalActor* Rack);
	ARackThermalActor* Find(UWorld* World, const FString& Id);
	ARackThermalActor* Spawn(UWorld* World, FName Name, const FString& Id, UClass* Class,
		const FTransform& Transform, const FRackLayoutSettings& Settings);
	bool Remove(ARackThermalActor* Rack);
	// Preflight the entire batch; a failed engine removal rolls back live state in memory.
	// Successful Undo uses configuration-only Spawn and never transfers this rollback state.
	bool RemoveBatch(const TArray<ARackThermalActor*>& Racks);
	void Rebuild(UWorld* World, bool bKeepSelection = false);
}
