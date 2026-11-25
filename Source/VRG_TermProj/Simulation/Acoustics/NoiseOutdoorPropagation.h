#pragma once

#include "Simulation/Acoustics/NoiseOutdoor.h"

namespace VrgNoise
{
inline constexpr int32 OutdoorOctaves = 8;
inline constexpr int32 MaxBoundaryReceivers = 64;
inline constexpr int32 MaxOutdoorScreens = 16;
inline constexpr double OutdoorNominalHz[OutdoorOctaves] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};

struct FOutdoorScreen
{
	FVector2D StartM = FVector2D::ZeroVector;
	FVector2D EndM = FVector2D::ZeroVector;
	double HeightM = 0.0;
	double ReflectedEnergy = 0.0;

	bool IsValid() const;
};

struct FOutdoorEnvironment
{
	double GroundZM = 0.0;
	double GroundFactor = 0.0;  // Uniform flat ground, G = 0 (hard) to 1 (porous).
	double RelativeHumidity = 0.7;
	double PressurePa = 101325.0;
	TArray<FOutdoorScreen> Screens;

	bool IsValid() const;
};

struct FOutdoorAir
{
	double TemperatureK = 0.0;
	double AbsorptionDbPerM[OutdoorOctaves] = {};
};

struct FOutdoorBandLoss
{
	double AtmosphereDb = 0.0;
	double GroundDb = 0.0;
	double BarrierDb = 0.0;
	double ReflectionGainDb = 0.0; // Energetic increase relative to the screened direct contribution.
};

struct FOutdoorPathLoss
{
	double DistanceM = 0.0;
	double DivergenceDb = 0.0;
	FOutdoorBandLoss Bands[OutdoorOctaves];
	int32 BlockingScreens = 0;
	int32 ReflectedPaths = 0;
};

TOptional<FOutdoorAir> OutdoorAir(double TemperatureK, double RelativeHumidity, double PressurePa);
TOptional<FOutdoorPathLoss> OutdoorPath(const FVector& SourceM, const FVector& ReceiverM,
	const FOutdoorEnvironment& Environment, const FOutdoorAir& Air);
FSourceProfile Liebert940EstimatedOctaves();
TOptional<double> OutdoorOctaveTransferDb(const FOutdoorPathLoss& Path, const FSourceProfile& Profile);

// Samples an open line or polyline. A repeated first vertex closes it without duplicating that receiver.
// Each segment is subdivided into equal intervals no larger than SpacingM. Failure leaves Out unchanged.
bool SampleOutdoorBoundary(const TArray<FVector2D>& VerticesM, double SpacingM, TArray<FVector2D>& Out);

struct FBoundaryReceiverLevel
{
	FVector PositionM = FVector::ZeroVector;
	FLevel Pressure;
	int32 NearSources = 0;
};

struct FOutdoorBoundarySnapshot
{
	double SimTimeS = 0.0;
	double TemperatureK = 0.0;
	bool bPlantRunning = false;
	TArray<FBoundaryReceiverLevel> Receivers;
};
}
