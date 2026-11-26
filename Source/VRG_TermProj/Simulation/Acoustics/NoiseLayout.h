#pragma once

#include "Simulation/Acoustics/NoiseIndoor.h"
#include "Simulation/Acoustics/NoiseOutdoorPropagation.h"

class FJsonObject;

namespace VrgNoise
{
struct FBoundaryRequest
{
	TArray<FVector2D> VerticesM;
	double SpacingM = 0.0;
	double HeightM = 0.0;
};

struct FNoiseLayout
{
	TOptional<FOutdoorReceiver> Probe;
	TOptional<FBoundaryRequest> Boundary;
	FOutdoorEnvironment Environment;
	TOptional<FVector2D> IndoorPositionM;
	TOptional<double> IndoorSpacingM;
	double IndoorHeightM = DefaultEarHeightM;
	FIndoorAbsorption Absorption;
	TOptional<double> CduLwA;

	bool IsValid(const FBox& FinalRoomM = FBox(ForceInit)) const;
};

TSharedRef<FJsonObject> WriteNoiseLayout(const FNoiseLayout& Layout);
bool ReadNoiseLayout(const FJsonObject& Json, FNoiseLayout& Out);
}
