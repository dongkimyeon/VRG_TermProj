#pragma once
#include "Actors/CoolingDistributionUnitActor.h"

// Runtime native CDU addition/recreation. Shared room/simulation rebuild remains VrgRackLifecycle::Rebuild.
namespace VrgCduLifecycle
{
 FString Identity(const ACoolingDistributionUnitActor* Cdu);
 ACoolingDistributionUnitActor* Find(UWorld* World, const FString& Id);
 ACoolingDistributionUnitActor* Spawn(UWorld* World, FName Name, const FString& Id,
  const FTransform& Transform, const FCduLayoutSettings& Settings);
 bool Remove(ACoolingDistributionUnitActor* Cdu);
}
