#include "Actors/DataCenterSimulationActor.h"

bool ADataCenterSimulationActor::SetOutdoorNoiseEnvironment(const VrgNoise::FOutdoorEnvironment& Environment)
{
	if (!Environment.IsValid() || (!OutdoorBoundaryPoints.IsEmpty() && !FMath::IsFinite(Environment.GroundZM + OutdoorBoundaryHeightM)))
		return false;
	OutdoorNoiseEnvironment = Environment;
	ResetBoundaryNoisePaths();
	return true;
}

bool ADataCenterSimulationActor::SetOutdoorNoiseBoundary(const TArray<FVector2D>& VerticesM, double SpacingM, double HeightM)
{
	if (!FMath::IsFinite(HeightM) || HeightM <= 0.0 || !FMath::IsFinite(OutdoorNoiseEnvironment.GroundZM + HeightM)
		|| !VrgNoise::SampleOutdoorBoundary(VerticesM, SpacingM, OutdoorBoundaryPoints)) return false;
	OutdoorBoundaryHeightM = HeightM;
	ResetBoundaryNoisePaths();
	return true;
}

void ADataCenterSimulationActor::ClearOutdoorNoiseBoundary()
{
	OutdoorBoundaryPoints.Reset();
	ResetBoundaryNoisePaths();
	if (!OutdoorNoiseReceiver.IsSet()) ResetOutdoorNoiseRun();
}

void ADataCenterSimulationActor::ResetBoundaryNoisePaths()
{
	OutdoorBoundaryPaths.Reset();
	OutdoorBoundaryPathTemperatureK.Reset();
	OutdoorBoundarySnapshot.Reset();
}

const ADataCenterSimulationActor::FBoundaryNoisePath* ADataCenterSimulationActor::GetOutdoorBoundaryPath(int32 Receiver, int32 Source) const
{
	if (!OutdoorBoundaryPoints.IsValidIndex(Receiver) || !OutdoorNoiseSources.IsValidIndex(Source)) return nullptr;
	const int32 Index = Receiver * OutdoorNoiseSources.Num() + Source;
	return OutdoorBoundaryPaths.IsValidIndex(Index) ? &OutdoorBoundaryPaths[Index] : nullptr;
}

void ADataCenterSimulationActor::BuildOutdoorBoundaryPaths(double TemperatureK)
{
	using namespace VrgNoise;
	OutdoorBoundaryPathTemperatureK = TemperatureK;
	OutdoorBoundaryPaths.Reset();
	OutdoorBoundaryPaths.Reserve(OutdoorBoundaryPoints.Num() * OutdoorNoiseSources.Num());
	const auto Air = OutdoorAir(TemperatureK, OutdoorNoiseEnvironment.RelativeHumidity, OutdoorNoiseEnvironment.PressurePa);
	const double ReceiverZ = OutdoorNoiseEnvironment.GroundZM + OutdoorBoundaryHeightM;
	static const FSourceProfile EstimatedBands = Liebert940EstimatedOctaves();
	for (const FVector2D& Point : OutdoorBoundaryPoints)
	{
		FOutdoorReceiver Receiver{ FVector(Point, ReceiverZ) };
		for (const FOutdoorNoiseSource& Source : OutdoorNoiseSources)
		{
			FBoundaryNoisePath& Path = OutdoorBoundaryPaths.AddDefaulted_GetRef();
			Path.bTooClose = !OutdoorTransferDb(Receiver, Source.CenterM, Source.MinDistanceM).IsSet();
			if (Path.bTooClose || !Air.IsSet()) continue;
			Path.Loss = OutdoorPath(Source.CenterM, Receiver.PositionM, OutdoorNoiseEnvironment, *Air);
			if (Path.Loss.IsSet()) Path.TransferDb = OutdoorOctaveTransferDb(*Path.Loss, EstimatedBands);
		}
	}
}

void ADataCenterSimulationActor::UpdateOutdoorNoiseBoundary(const VrgNoise::FOutdoorSample& Sample, const TArray<VrgNoise::FLevel>& Powers)
{
	if (OutdoorBoundaryPoints.IsEmpty()) return;
	using namespace VrgNoise;
	const double TemperatureK = Plant.GetThermalSpec().AmbientK;
	if (Sample.bPlantRunning && (!OutdoorBoundaryPathTemperatureK.IsSet() || *OutdoorBoundaryPathTemperatureK != TemperatureK))
		BuildOutdoorBoundaryPaths(TemperatureK);
	if (!OutdoorBoundarySnapshot.IsSet()) OutdoorBoundarySnapshot.Emplace();
	FOutdoorBoundarySnapshot& Snapshot = *OutdoorBoundarySnapshot;
	Snapshot.SimTimeS = Sample.SimTimeS;
	Snapshot.TemperatureK = TemperatureK;
	Snapshot.bPlantRunning = Sample.bPlantRunning;
	Snapshot.Receivers.SetNum(OutdoorBoundaryPoints.Num());

	TArray<FLevel> Contributions;
	Contributions.Reserve(Powers.Num());
	for (int32 ReceiverIndex = 0; ReceiverIndex < OutdoorBoundaryPoints.Num(); ++ReceiverIndex)
	{
		FBoundaryReceiverLevel& Receiver = Snapshot.Receivers[ReceiverIndex];
		Receiver.PositionM = FVector(OutdoorBoundaryPoints[ReceiverIndex], OutdoorNoiseEnvironment.GroundZM + OutdoorBoundaryHeightM);
		Receiver.NearSources = 0;
		Contributions.Reset();
		for (int32 SourceIndex = 0; SourceIndex < Powers.Num(); ++SourceIndex)
		{
			const FBoundaryNoisePath& Path = OutdoorBoundaryPaths[ReceiverIndex * Powers.Num() + SourceIndex];
			const FLevel& Power = Powers[SourceIndex];
			if (Path.bTooClose && Power.State != ELevelState::Silent) ++Receiver.NearSources;
			FLevel Contribution = OutdoorContribution(Power, Path.TransferDb);
			if (Contribution.State == ELevelState::Known) Contribution.bEstimated = true; // Surrogate octave spectrum.
			Contributions.Add(MoveTemp(Contribution));
		}
		Receiver.Pressure = Combine(Contributions, EQuantity::SoundPressure, EWeighting::A);
	}
}
