// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FUniformAirGrid;

// Distance from every fluid cell centre to the nearest wall [m] (2026-09-27, for the Chen-Xu turbulence length).
// Walls are the domain boundary and the solid cells. It is the exact Euclidean distance between cell centres on the
// grid padded with a ring of wall cells (Felzenszwalb & Huttenlocher separable transform), minus half a cell, so
// a cell next to a wall gets h/2 - exact for axis-aligned walls, within h/2 for corners and diagonals.
// Solid cells get 0. Topology only: recompute when solids change, not every step (it does not read the fields).
namespace AirWallDistance
{
	VRG_TERMPROJ_API void Compute(const FUniformAirGrid& Grid, TArray<double>& OutDistanceM);
}
