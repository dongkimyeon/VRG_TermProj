#pragma once

#include "Simulation/Acoustics/NoiseSource.h"

namespace VrgNoise
{
enum class EReceiverSet : uint8 { Indoor, Probe, Boundary, Count };
const TCHAR* ReceiverSetName(EReceiverSet Set);

struct FTimeAverage
{
	double DurationS = 0.0;
	double IncompleteS = 0.0;

	void Add(const FLevel& Pressure, double Duration);
	TOptional<double> GetDb() const;
	const TCHAR* GetState() const;

private:
	double PeakDb = 0.0;
	double ScaledEnergyS = 0.0; // Seconds of energy relative to PeakDb.
};

struct FNoiseReading
{
	FVector PositionM = FVector::ZeroVector;
	FLevel Pressure;
	int32 NearSources = 0;
};

struct FNoiseTraceSample
{
	double TimeS = 0.0;
	TOptional<double> Db;
	ELevelState State = ELevelState::InvalidInput;
	bool bPartial = false;
};

struct FNoiseTrack
{
	FNoiseReading Latest;
	FTimeAverage Average;
	TArray<FNoiseTraceSample> Trace;
	int32 NextTrace = 0;
};

struct FNoiseOperatingPoint
{
	double ItW = 0.0;
	double CoolingW = 0.0;
	TOptional<double> MaxInletK;
};

inline constexpr int32 NoiseTraceLimit = 256;
inline constexpr double NoiseTraceIntervalS = .2;
inline constexpr double NoiseLegendMinDb = 40.0;
inline constexpr double NoiseLegendMaxDb = 100.0;

struct FNoiseReceiverGroup
{
	double StartS = 0.0;
	double EndS = 0.0;
	double LatestSourceS = 0.0;
	double MeanItW = 0.0;
	double MeanCoolingW = 0.0;
	TOptional<double> MeanMaxInletK;
	TArray<FNoiseTrack> Tracks;

	// Time is SimTime. One completed step for a stationary set of receiver positions.
	void Observe(TConstArrayView<FNoiseReading> Readings, double Start, double End, double SourceTime,
		const FNoiseOperatingPoint& Operating);

private:
	double NextTraceS = 0.0;
	double InletCoveredS = 0.0;
};

const TCHAR* NoiseLevelState(const FLevel& Level);
FColor NoiseLevelColor(const FLevel& Level);
}
