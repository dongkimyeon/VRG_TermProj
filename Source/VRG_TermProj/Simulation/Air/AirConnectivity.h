// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FUniformAirGrid;
struct FAirSolverInput;

// Connected air regions of the grid (6-neighbour, solid cells excluded) and the fan ducts that straddle them.
//
// Every duct carries equal intake and exhaust volume flow (ValidateDucts). If one duct's intake faces and
// exhaust faces sit in different air regions, the regions exchange air through that duct only, so each region has
// a net source: the Neumann pressure problem has no solution there and the projection reports a failure (D5).
// Walls, curtains and containment are what can split the air like that, so the integrated actor checks this after
// marking solids and reports it instead of letting the pressure solver fail silently later.
namespace VrgAirConnectivity
{
	// Label of every cell (INDEX_NONE for solid cells); returns the number of air regions.
	VRG_TERMPROJ_API int32 LabelAirRegions(const FUniformAirGrid& Grid, TArray<int32>& OutLabel);

	// Number of ducts whose faces are not all in one air region.
	VRG_TERMPROJ_API int32 CountSplitDucts(const FUniformAirGrid& Grid, const FAirSolverInput& Input);
}
