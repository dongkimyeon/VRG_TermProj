// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FUniformAirGrid;

// Streamlines of the air velocity field (2026-10-27, the professor's "streamtraces" view).
// A streamline follows the direction of the velocity field from a seed: dx/ds = u / |u|, integrated with RK4 in arc
// length, the velocity trilinearly interpolated from the MAC faces (FUniformAirGrid::SampleVelocityMs). It stops in a
// solid cell, outside the domain, where the air is (almost) still, or at the length limit. Each point carries the speed
// and the temperature there, for colouring. A streamline is a picture of the field at one moment: it is not the path
// of a particle over time (that would be a pathline) unless the flow is steady.
// Plain C++ on a grid passed in (rule 5: the caller gives a consistent field); positions in metres, domain-local.

struct VRG_TERMPROJ_API FAirStreamline
{
	TArray<FVector> PointsM;
	TArray<float> SpeedMs;
	TArray<float> TemperatureK;
	enum class EStop : uint8 { Length, Solid, Outside, Still, Loop } Stop = EStop::Length;
};

struct VRG_TERMPROJ_API FAirStreamlineSettings
{
	double StepM = 0.05;          // arc-length step
	double MaxLengthM = 40.0;
	double MinSpeedMs = 0.02;     // below this the direction is noise: stop
	bool bBackward = false;       // trace against the flow (where the air at the seed came from)
	// Plan view: follow only the horizontal part of the velocity, at the seed's height (the in-plane flow of a slice;
	// the vertical part is dropped, so this is the flow seen from above, not a 3D path).
	bool bPlanar = false;
};

namespace AirStreamlines
{
	VRG_TERMPROJ_API FAirStreamline Trace(const FUniformAirGrid& Grid, const FVector& SeedM, const FAirStreamlineSettings& Settings);
	VRG_TERMPROJ_API TArray<FAirStreamline> TraceAll(const FUniformAirGrid& Grid, const TArray<FVector>& SeedsM, const FAirStreamlineSettings& Settings);
}
