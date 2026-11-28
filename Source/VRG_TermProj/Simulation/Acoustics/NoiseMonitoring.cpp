#include "Simulation/Acoustics/NoiseMonitoring.h"

#include <cmath>

namespace VrgNoise
{
const TCHAR* ReceiverSetName(EReceiverSet Set)
{
	switch (Set)
	{
	case EReceiverSet::Indoor: return TEXT("indoor");
	case EReceiverSet::Probe: return TEXT("probe");
	case EReceiverSet::Boundary: return TEXT("boundary");
	default: return TEXT("unknown");
	}
}

void FTimeAverage::Add(const FLevel& Pressure, double Duration)
{
	if (!FMath::IsFinite(Duration) || Duration <= 0.0 || !FMath::IsFinite(DurationS + Duration)) return;
	DurationS += Duration;
	const bool bKnown = Pressure.State == ELevelState::Known && Pressure.Db.IsSet() && FMath::IsFinite(*Pressure.Db);
	if ((Pressure.State != ELevelState::Silent && !bKnown) || Pressure.MissingSources > 0) IncompleteS += Duration;
	if (!bKnown) return;
	const double Db = *Pressure.Db;
	if (ScaledEnergyS == 0.0)
	{
		PeakDb = Db;
		ScaledEnergyS = Duration;
	}
	else if (Db > PeakDb)
	{
		ScaledEnergyS = ScaledEnergyS * std::pow(10.0, (PeakDb - Db) / 10.0) + Duration;
		PeakDb = Db;
	}
	else ScaledEnergyS += Duration * std::pow(10.0, (Db - PeakDb) / 10.0);
}

TOptional<double> FTimeAverage::GetDb() const
{
	if (ScaledEnergyS <= 0.0 || DurationS <= 0.0) return {};
	const double Db = PeakDb + 10.0 * (std::log10(ScaledEnergyS) - std::log10(DurationS));
	return FMath::IsFinite(Db) ? TOptional<double>(Db) : TOptional<double>();
}

const TCHAR* FTimeAverage::GetState() const
{
	if (DurationS == 0.0) return TEXT("unavailable");
	if (GetDb().IsSet()) return IncompleteS > 0.0 ? TEXT("partial") : TEXT("known");
	return IncompleteS > 0.0 ? TEXT("missing") : TEXT("silent");
}

void FNoiseReceiverGroup::Observe(TConstArrayView<FNoiseReading> Readings, double Start, double End, double SourceTime,
	const FNoiseOperatingPoint& Operating)
{
	const double Duration = End - Start;
	if (Readings.IsEmpty() || !FMath::IsFinite(Duration) || Duration <= 0.0) return;
	if (!Tracks.IsEmpty() && End <= EndS) return;
	if (Tracks.Num() != Readings.Num() || (!Tracks.IsEmpty() && !FMath::IsNearlyEqual(EndS, Start, 1e-9)))
		*this = FNoiseReceiverGroup();
	if (Tracks.IsEmpty())
	{
		Tracks.SetNum(Readings.Num());
		StartS = Start;
	}
	EndS = End;
	LatestSourceS = SourceTime;
	const double Weight = Duration / (EndS - StartS);
	MeanItW += (Operating.ItW - MeanItW) * Weight;
	MeanCoolingW += (Operating.CoolingW - MeanCoolingW) * Weight;
	if (Operating.MaxInletK.IsSet())
	{
		InletCoveredS += Duration;
		const double Prior = MeanMaxInletK.Get(*Operating.MaxInletK);
		MeanMaxInletK = FMath::Lerp(Prior, *Operating.MaxInletK, Duration / InletCoveredS);
	}
	const bool bRecord = End + 1e-9 >= NextTraceS;
	if (bRecord) NextTraceS = End + NoiseTraceIntervalS;
	for (int32 I = 0; I < Readings.Num(); ++I)
	{
		auto& Track = Tracks[I];
		Track.Latest = Readings[I];
		const auto& Pressure = Track.Latest.Pressure;
		Track.Average.Add(Pressure, Duration);
		if (!bRecord) continue;
		const FNoiseTraceSample Sample{End, Pressure.Db, Pressure.State, Pressure.MissingSources > 0};
		if (Track.Trace.Num() < NoiseTraceLimit) Track.Trace.Add(Sample);
		else Track.Trace[Track.NextTrace] = Sample;
		Track.NextTrace = (Track.NextTrace + 1) % NoiseTraceLimit;
	}
}

const TCHAR* NoiseLevelState(const FLevel& Level)
{
	switch (Level.State)
	{
	case ELevelState::Known: return Level.MissingSources > 0 ? TEXT("partial") : TEXT("known");
	case ELevelState::Silent: return TEXT("silent");
	case ELevelState::MissingData: return TEXT("missing");
	default: return TEXT("unavailable");
	}
}

FColor NoiseLevelColor(const FLevel& Level)
{
	if (Level.Db.IsSet())
	{
		const double T = FMath::Clamp((*Level.Db - NoiseLegendMinDb) / (NoiseLegendMaxDb - NoiseLegendMinDb), 0.0, 1.0);
		return FMath::Lerp(T < .5 ? FLinearColor(.12f, .64f, .95f) : FLinearColor(.98f, .77f, .22f),
			T < .5 ? FLinearColor(.98f, .77f, .22f) : FLinearColor(.91f, .14f, .15f), 2.0 * (T < .5 ? T : T - .5)).ToFColorSRGB();
	}
	if (Level.State == ELevelState::Silent) return FColor(128, 145, 158);
	if (Level.State == ELevelState::MissingData) return FColor(190, 125, 238);
	return FColor(77, 90, 108);
}
}
