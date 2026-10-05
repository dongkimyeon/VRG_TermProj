#pragma once
#include "Actors/CrahUnitActor.h"

// Runtime native CRAH addition/recreation. Shared room/simulation rebuild remains VrgRackLifecycle::Rebuild.
namespace VrgCrahLifecycle
{
 FString Identity(const ACrahUnitActor* Crah);
 ACrahUnitActor* Find(UWorld* World, const FString& Id);
 ACrahUnitActor* Spawn(UWorld* World, FName Name, const FString& Id,
  const FTransform& Transform, const FCrahLayoutSettings& Settings, bool bRuntime = true);
 bool Remove(ACrahUnitActor* Crah);
 bool RemoveBatch(const TArray<ACrahUnitActor*>& Crahs);
}
