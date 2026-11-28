#pragma once

#include "Simulation/Acoustics/NoiseSource.h"

class FUniformAirGrid;

namespace VrgNoise
{
inline constexpr int32 IndoorOctaves = 8;
inline constexpr int32 MaxIndoorReceivers = 256;
inline constexpr double DefaultEarHeightM = 1.5;

enum class EIndoorSourceKind : uint8 { Rack, Spine, Crah, Cdu };

struct FIndoorAbsorption
{
	// 2026-11-28 field calibration (ESTIMATE from one measured site): room-average absorption back-calculated with
	// Sabine from the early decay times measured in a working colocation server room (BYU acoustic report for C7 Data
	// Centers, 28 x 31.7 x 4.1 m, EDT 1.5..2.1 s at 1 kHz, 1.1..3.2 s near 100 Hz, 0.47..0.59 s at 8 kHz; 8 kHz with the
	// room's air absorption removed, 4 kHz interpolated). It replaces the bare concrete/sheet-metal references
	// (ASHRAE 2023 Applications ch. 49 table 12: shell .01..03, equipment .04..07), which gave several-second
	// reverberation and put the ear-height map 15..25 dB above field data-hall levels (docs/기록/10_소음.md, 2026-11-28).
	// The measured value already contains the racks, so shell and equipment use the same room average.
	double Shell[IndoorOctaves] = {.13, .13, .14, .15, .14, .15, .22, .33};
	double Equipment[IndoorOctaves] = {.13, .13, .14, .15, .14, .15, .22, .33};
	bool IsValid() const;
};

struct FIndoorZone
{
	double SurfaceM2 = 0.0;
	double EquipmentSurfaceM2 = 0.0;
};

struct FIndoorGeometry
{
	FVector OriginM = FVector::ZeroVector;
	TArray<int32> CellZone;
	TArray<FIndoorZone> Zones;

	// Equipment owner indices are [0, EquipmentOwners); negative IDs are architectural solids.
	void Build(const FUniformAirGrid& Grid, const FVector& InOriginM, TConstArrayView<int32> Owners = {}, int32 EquipmentOwners = 0);
	int32 ZoneAt(const FUniformAirGrid& Grid, const FVector& PositionM) const;
	bool HasDirectPath(const FUniformAirGrid& Grid, const FVector& FromM, const FVector& ToM) const;
};

// A-weighted effective 4/R per square metre. A broadband rating uses the 1 kHz
// absorption as an estimate; for a spectrum the energy-weighted factors are used.
TOptional<double> IndoorDiffuseGain(const FIndoorZone& Zone, const FIndoorAbsorption& Absorption, const FSourceProfile* Spectrum = nullptr);
double IndoorDirectGain(double DistanceM); // spherical 1/(4 pi r^2), callers screen the near field

bool SampleIndoorMap(const FBox& RoomM, double SpacingM, double HeightM, TArray<FVector>& Out);
FSourceProfile SanAceIndoorFan(bool bCounterRotating);
FSourceProfile Pcw035IndoorCrah();

struct FIndoorSourceLevel
{
	FString Id;
	FVector PositionM = FVector::ZeroVector;
	EIndoorSourceKind Kind = EIndoorSourceKind::Rack;
	int32 Zone = INDEX_NONE;
	FLevel Power;
};

struct FIndoorReceiverLevel
{
	FVector PositionM = FVector::ZeroVector;
	int32 Zone = INDEX_NONE;
	FLevel Pressure;
	int32 NearSources = 0;
	int32 BlockedDirectSources = 0;
};

struct FIndoorSnapshot
{
	double SimTimeS = 0.0;
	TArray<FIndoorReceiverLevel> Receivers;
	TArray<FIndoorSourceLevel> Sources;
	TArray<FIndoorZone> Zones;
	int32 UnlocatedRunningSources = 0;
};
}
