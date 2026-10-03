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
	void Rebuild(UWorld* World);
}
