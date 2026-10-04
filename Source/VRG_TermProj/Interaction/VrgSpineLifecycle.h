#pragma once
#include "Actors/SpineCabinetActor.h"

// Game-thread runtime layout lifecycle; uses the rack lifecycle's shared rebuild path.
namespace VrgSpineLifecycle
{
	FString Identity(const ASpineCabinetActor* Spine);
	ASpineCabinetActor* Find(UWorld* World, const FString& Id);
	ASpineCabinetActor* Spawn(UWorld* World, FName Name, const FString& Id,
		const FTransform& Transform, const FSpineLayoutSettings& Settings, bool bRuntime = true);
	bool Remove(ASpineCabinetActor* Spine);
}
