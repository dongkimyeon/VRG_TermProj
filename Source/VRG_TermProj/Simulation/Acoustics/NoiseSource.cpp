#include "Simulation/Acoustics/NoiseSource.h"

#include <cmath>
#include <limits>

namespace VrgNoise
{
namespace
{
	// Nominal octave-band corrections rounded to 0.1 dB, applied exactly once.
	// ASHRAE Fundamentals 2021, chapter 8, table 5:
	// https://handbook.ashrae.org/Handbooks/F21/SI/F21_Ch08/F21_Ch08_si.aspx
	constexpr double OctaveHz[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
	constexpr double OctaveADb[] = {-26.2, -16.1, -8.6, -3.2, 0, 1.2, 1.0, -1.1};

	bool IsWeighting(EWeighting Weighting)
	{
		return Weighting == EWeighting::Unweighted || Weighting == EWeighting::A;
	}

	bool LogReference(EQuantity Quantity, double& Out)
	{
		if (Quantity == EQuantity::SoundPower) Out = -12.0;
		else if (Quantity == EQuantity::SoundPressure) Out = std::log10(4e-10);
		else return false;
		return true;
	}

	FLevel EmptyLevel(EQuantity Quantity, EWeighting Weighting, ELevelState State)
	{
		FLevel Result;
		Result.Quantity = Quantity;
		Result.Weighting = Weighting;
		Result.State = State;
		return Result;
	}

	bool IsConsistent(const FLevel& Level)
	{
		if (Level.KnownSources < 0 || Level.SilentSources < 0 || Level.MissingSources < 0) return false;
		switch (Level.State)
		{
		case ELevelState::Known:
			return Level.KnownSources > 0 && Level.Db.IsSet() && std::isfinite(Level.Db.GetValue());
		case ELevelState::Silent:
			return Level.KnownSources == 0 && Level.MissingSources == 0 && !Level.Db.IsSet();
		case ELevelState::MissingData:
			return Level.KnownSources == 0 && Level.MissingSources > 0 && !Level.Db.IsSet();
		default:
			return false;
		}
	}
}

bool TryDbToLinear(double Db, EQuantity Quantity, double& OutLinear)
{
	double ReferenceLog;
	if (!std::isfinite(Db) || !LogReference(Quantity, ReferenceLog)) return false;
	// Move the reference inside the exponent to avoid premature pow overflow.
	const double Value = std::pow(10.0, Db / 10.0 + ReferenceLog);
	if (!std::isfinite(Value) || Value <= 0.0) return false;
	OutLinear = Value;
	return true;
}

bool TryLinearToDb(double Linear, EQuantity Quantity, double& OutDb)
{
	double ReferenceLog;
	if (!std::isfinite(Linear) || Linear <= 0.0 || !LogReference(Quantity, ReferenceLog)) return false;
	const double Value = 10.0 * (std::log10(Linear) - ReferenceLog);
	if (!std::isfinite(Value)) return false;
	OutDb = Value;
	return true;
}

bool TryOctaveACorrection(double FrequencyHz, double& OutDb)
{
	for (int32 Band = 0; Band < UE_ARRAY_COUNT(OctaveHz); ++Band)
	{
		if (FrequencyHz == OctaveHz[Band])
		{
			OutDb = OctaveADb[Band];
			return true;
		}
	}
	return false;
}

bool FSourceProfile::IsValid() const
{
	if (Id.IsEmpty() || Model.IsEmpty() || SourceUrl.IsEmpty() || SourceLocator.IsEmpty() || RatingCondition.IsEmpty()
		|| Quantity != EQuantity::SoundPower || !IsWeighting(Weighting)
		|| !std::isfinite(ReferenceSpeedRatio) || ReferenceSpeedRatio <= 0.0 || LevelsDb.IsEmpty()) return false;
	if (FrequenciesHz.IsEmpty())
	{
		return LevelsDb.Num() == 1 && std::isfinite(LevelsDb[0]);
	}
	if (FrequenciesHz.Num() != LevelsDb.Num()) return false;
	double PreviousHz = 0.0;
	for (int32 Band = 0; Band < FrequenciesHz.Num(); ++Band)
	{
		double Correction;
		if (FrequenciesHz[Band] <= PreviousHz || !TryOctaveACorrection(FrequenciesHz[Band], Correction)
			|| !std::isfinite(LevelsDb[Band])) return false;
		PreviousHz = FrequenciesHz[Band];
	}
	return true;
}

FLevel Combine(const TArray<FLevel>& Levels, EQuantity Quantity, EWeighting Weighting)
{
	const FLevel Invalid = EmptyLevel(Quantity, Weighting, ELevelState::InvalidInput);
	double ReferenceLog;
	if (!LogReference(Quantity, ReferenceLog) || !IsWeighting(Weighting)) return Invalid;
	FLevel Result = EmptyLevel(Quantity, Weighting, ELevelState::Silent);
	double MaxDb = -std::numeric_limits<double>::infinity();
	int64 Known = 0, Silent = 0, Missing = 0;
	for (const FLevel& Level : Levels)
	{
		if (Level.Quantity != Quantity || Level.Weighting != Weighting || !IsConsistent(Level)) return Invalid;
		Known += Level.KnownSources;
		Silent += Level.SilentSources;
		Missing += Level.MissingSources;
		Result.bEstimated |= Level.bEstimated;
		if (Level.Db.IsSet()) MaxDb = FMath::Max(MaxDb, Level.Db.GetValue());
	}
	if (Known > MAX_int32 || Silent > MAX_int32 || Missing > MAX_int32) return Invalid;
	Result.KnownSources = int32(Known);
	Result.SilentSources = int32(Silent);
	Result.MissingSources = int32(Missing);
	if (Known == 0)
	{
		Result.State = Missing > 0 ? ELevelState::MissingData : ELevelState::Silent;
		return Result;
	}
	double RelativeEnergy = 0.0;
	for (const FLevel& Level : Levels)
	{
		if (Level.Db.IsSet()) RelativeEnergy += std::pow(10.0, (Level.Db.GetValue() - MaxDb) / 10.0);
	}
	const double SumDb = MaxDb + 10.0 * std::log10(RelativeEnergy);
	if (!std::isfinite(SumDb)) return Invalid;
	Result.State = ELevelState::Known;
	Result.Db = SumDb;
	return Result;
}

FLevel Evaluate(const FSourceProfile* Profile, bool bRunning, double SpeedRatio, EWeighting OutputWeighting)
{
	FLevel Result = EmptyLevel(EQuantity::SoundPower, OutputWeighting, ELevelState::InvalidInput);
	if (!IsWeighting(OutputWeighting) || !std::isfinite(SpeedRatio) || SpeedRatio < 0.0
		|| (Profile && !Profile->IsValid())) return Result;
	if (!bRunning || SpeedRatio == 0.0)
	{
		Result.State = ELevelState::Silent;
		Result.SilentSources = 1;
		return Result;
	}
	if (!Profile)
	{
		Result.State = ELevelState::MissingData;
		Result.MissingSources = 1;
		return Result;
	}
	const bool bSpeedChanged = SpeedRatio != Profile->ReferenceSpeedRatio;
	const bool bNeedsA = Profile->Weighting == EWeighting::Unweighted && OutputWeighting == EWeighting::A;
	if ((bSpeedChanged && !Profile->bFanSpeedEstimate)
		|| (bNeedsA && Profile->FrequenciesHz.IsEmpty())
		|| (Profile->Weighting == EWeighting::A && OutputWeighting != EWeighting::A))
	{
		Result.State = ELevelState::MissingData;
		Result.MissingSources = 1;
		return Result;
	}
	// Same fan/system-curve approximation, AMCA Basics of Fan Noise, slide 56.
	// Subtract logs rather than forming a ratio that can overflow/underflow.
	const double SpeedDb = bSpeedChanged ? 50.0 * (std::log10(SpeedRatio) - std::log10(Profile->ReferenceSpeedRatio)) : 0.0;
	TArray<FLevel> Bands;
	for (int32 Band = 0; Band < Profile->LevelsDb.Num(); ++Band)
	{
		double Correction = 0.0;
		if (bNeedsA) TryOctaveACorrection(Profile->FrequenciesHz[Band], Correction);
		FLevel Value = EmptyLevel(EQuantity::SoundPower, OutputWeighting, ELevelState::Known);
		Value.Db = Profile->LevelsDb[Band] + SpeedDb + Correction;
		Value.KnownSources = 1;
		Value.bEstimated = Profile->bEstimatedRating || bSpeedChanged;
		Bands.Add(Value);
	}
	Result = Combine(Bands, EQuantity::SoundPower, OutputWeighting);
	if (Result.State == ELevelState::Known) Result.KnownSources = 1; // bands belong to one source
	return Result;
}

FSourceProfile Liebert940Standard60Hz()
{
	FSourceProfile Profile;
	Profile.Id = TEXT("liebert-940-60hz-standard-unit");
	Profile.Model = TEXT("Vertiv Liebert 940 60 Hz Standard; whole unit, 8 fans");
	Profile.SourceUrl = TEXT("https://www.vertiv.com/48ea08/globalassets/shared/liebert-air-cooled-direct-drive-drycoolers-technical-design-manual_00.pdf");
	Profile.SourceLocator = TEXT("Technical Design Manual 2023, table 2.1; PDF p11 / printed p7");
	Profile.RatingCondition = TEXT("60 Hz Standard; EAT 35 C, EFT 48.9 C, 40% EG; 43400 CFM; LwA re 1 pW");
	Profile.LevelsDb = {97.4};
	Profile.bFanSpeedEstimate = true;
	return Profile;
}
}
