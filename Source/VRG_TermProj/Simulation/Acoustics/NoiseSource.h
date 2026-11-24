#pragma once

#include "CoreMinimal.h"

// N1 source calculations. Linear sound power is W (reference 1 pW), while linear
// sound pressure here is mean-square Pa^2 (reference (20 uPa)^2), never RMS Pa.
// A-weighted linear power is an equivalent quantity, not heat-ledger power.
namespace VrgNoise
{
enum class EQuantity : uint8 { SoundPower, SoundPressure };
enum class EWeighting : uint8 { Unweighted, A };
enum class ELevelState : uint8 { Known, Silent, MissingData, InvalidInput };

struct FLevel
{
	ELevelState State = ELevelState::InvalidInput;
	EQuantity Quantity = EQuantity::SoundPower;
	EWeighting Weighting = EWeighting::A;
	TOptional<double> Db;
	int32 KnownSources = 0;
	int32 SilentSources = 0;
	int32 MissingSources = 0;
	bool bEstimated = false;
};

// One complete acoustic rating, e.g. one drycooler unit including all its fans.
// Empty frequencies means one broadband level. Otherwise levels are sorted,
// distinct nominal octave centres from 63 to 8000 Hz with matching levels.
struct FSourceProfile
{
	FString Id;
	FString Model;
	FString SourceUrl;
	FString SourceLocator;
	FString RatingCondition;
	EQuantity Quantity = EQuantity::SoundPower;
	EWeighting Weighting = EWeighting::A;
	TArray<double> FrequenciesHz;
	TArray<double> LevelsDb;
	double ReferenceSpeedRatio = 1.0;
	bool bFanSpeedEstimate = false;
	bool bEstimatedRating = false;

	bool IsValid() const;
};

// On failure the caller's output is unchanged. Zero linear input has no finite dB.
VRG_TERMPROJ_API bool TryDbToLinear(double Db, EQuantity Quantity, double& OutLinear);
VRG_TERMPROJ_API bool TryLinearToDb(double Linear, EQuantity Quantity, double& OutDb);
VRG_TERMPROJ_API bool TryOctaveACorrection(double FrequencyHz, double& OutDb);

// A null profile on a running source is missing data. A stopped source is silent.
// This evaluates one unit, not NumFans additional copies of a whole-unit rating.
VRG_TERMPROJ_API FLevel Evaluate(const FSourceProfile* Profile, bool bRunning, double SpeedRatio,
	EWeighting OutputWeighting = EWeighting::A);

// Incoherent energy sum, accepting previously combined levels as well. Mixing
// quantities/weightings is rejected. Missing source counts survive partial sums.
VRG_TERMPROJ_API FLevel Combine(const TArray<FLevel>& Levels, EQuantity Quantity, EWeighting Weighting);
VRG_TERMPROJ_API FSourceProfile Liebert940Standard60Hz();
}
