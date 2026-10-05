#pragma once
#include "CoreMinimal.h"
#include "Actors/CrahUnitActor.h"
struct FCrahLayoutRecord
{
 FString Id; FName Name; FTransform Transform; FCrahLayoutSettings Settings;
 bool bConvertedAutomatic = false;
};
namespace VrgCrahLifecycle
{
 FString Identity(const ACrahUnitActor* Crah);
 ACrahUnitActor* Find(UWorld* World, const FString& Id);
 ACrahUnitActor* Spawn(UWorld* World, const FCrahLayoutRecord& Record);
 bool RemoveBatch(const TArray<ACrahUnitActor*>& Units);
}
