#pragma once
#include "CoreMinimal.h"
#include "Actors/CrahUnitActor.h"
namespace VrgCrahLifecycle
{
 FString Identity(const ACrahUnitActor* Crah);
 ACrahUnitActor* Find(UWorld* World, const FString& Id);
 ACrahUnitActor* Spawn(UWorld* World, const FCrahLayoutRecord& Record);
 ACrahUnitActor* Spawn(UWorld* World, FName Name, const FString& Id,
  const FTransform& Transform, const FCrahLayoutSettings& Settings, bool bRuntime = true);
 bool Remove(ACrahUnitActor* Crah);
 bool RemoveBatch(const TArray<ACrahUnitActor*>& Units);
}
