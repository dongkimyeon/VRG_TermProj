// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// On-screen debug text (2026-09-27). Off by default: the screen shows only the key help. Every summary and label is also
// in the log (UE_LOG) or can be turned back on with the console `vrg.Hud.Debug 1`.
namespace VrgHud
{
	VRG_TERMPROJ_API bool ShowDebugText();
}
