// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class UWorld;

// S6 (2026-09-27): save / load an equipment layout for A/B comparison.
// A file holds the placement of every rack, spine, CDU and CRAH actor (by actor name; position in m, yaw in degrees)
// and the cooling settings (containment, CRAH layout, coolant heat rejection, air cell size). Loading moves the actors
// of the same name; it never spawns or deletes equipment, and reports the ones it could not match.
// Files live in Saved/Layouts/<name>.json. Loading switches the simulation to layout mode first (the next simulation
// entry rebuilds everything from the loaded layout).
namespace VrgLayoutFile
{
	VRG_TERMPROJ_API FString PathFor(const FString& Name);
	VRG_TERMPROJ_API bool Save(UWorld* World, const FString& Name, FString& OutSummary);
	VRG_TERMPROJ_API bool Load(UWorld* World, const FString& Name, FString& OutSummary);
}
