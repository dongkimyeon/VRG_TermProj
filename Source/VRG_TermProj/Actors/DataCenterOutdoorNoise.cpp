#include "Actors/DataCenterSimulationActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Visualization/PlantDisplay.h"

#include <cmath>
#include <cstdlib>

namespace
{
	constexpr int32 OutdoorNoiseHistoryLimit = 512;

	// The thermal catalog has no SKU field. Require the defining rating fields of the
	// default 940 before attaching its acoustic profile to either bank.
	bool UsesLiebert940Rating(const FDryCoolerCatalog& Catalog)
	{
		const FDryCoolerCatalog Default;
		return Catalog.Fans == Default.Fans && Catalog.RatedHeatW == Default.RatedHeatW
			&& Catalog.RatedItdK == Default.RatedItdK && Catalog.RatedAirInK == Default.RatedAirInK
			&& Catalog.RatedAirFlowM3s == Default.RatedAirFlowM3s && Catalog.RatedFluidFlowM3s == Default.RatedFluidFlowM3s
			&& Catalog.FanRatedMotorOutputW == Default.FanRatedMotorOutputW;
	}

	const TCHAR* NoiseLoopName(EPlantLoopId Loop)
	{
		return Loop == EPlantLoopId::Fws ? TEXT("fws") : TEXT("cw");
	}

	bool ParseNoiseCoordinate(const FString& Text, double& Out)
	{
		const FTCHARToUTF8 Utf8(*Text);
		char* End = nullptr;
		Out = std::strtod(Utf8.Get(), &End);
		return End == Utf8.Get() + Utf8.Length() && End != Utf8.Get() && std::isfinite(Out);
	}
}

bool ADataCenterSimulationActor::SetOutdoorNoiseReceiver(const VrgNoise::FOutdoorReceiver& Receiver)
{
	if (!Receiver.IsValid()) return false;
	OutdoorNoiseReceiver = Receiver;
	ResetOutdoorNoiseRun();
	return true;
}

void ADataCenterSimulationActor::ClearOutdoorNoiseReceiver()
{
	OutdoorNoiseReceiver.Reset();
	ResetOutdoorNoiseRun();
}

void ADataCenterSimulationActor::ResetOutdoorNoiseRun()
{
	bOutdoorNoiseSourcesReady = false;
	OutdoorNoiseSources.Reset();
	OutdoorNoiseHistory.Reset();
	OutdoorNoiseNextSample = 0;
}

void ADataCenterSimulationActor::BuildOutdoorNoiseSources()
{
	// Build once per run or changed probe, independent of display visibility.
	const FPlantDisplaySnapshot Yard = PlantDisplay::Build(Plant, MakePlantDisplayInput());
	const auto FwsPlaced = VrgPlantEquipment::Gather(GetWorld(), EPlantUnitKind::FwsDryCooler);
	const auto CwPlaced = VrgPlantEquipment::Gather(GetWorld(), EPlantUnitKind::CwDryCooler);
	for (const FPlantDisplayEquipment& Unit : Yard.Equipment)
	{
		if (Unit.Kind != EPlantEquipmentKind::DryCooler) continue;
		const auto& Placed = Unit.Loop == EPlantLoopId::Fws ? FwsPlaced : CwPlaced;
		FOutdoorNoiseSource& Source = OutdoorNoiseSources.AddDefaulted_GetRef();
		Source.Loop = Unit.Loop;
		Source.UnitIndex = Unit.Index;
		Source.CenterM = Unit.CenterM;
		Source.bPlaced = Unit.bPlaced && Placed.IsValidIndex(Unit.Index);
		Source.Id = Source.bPlaced ? TEXT("plant/") + VrgPlantEquipment::Identity(Placed[Unit.Index])
			: FString::Printf(TEXT("auto/plant/%s/drycooler/%d"), NoiseLoopName(Unit.Loop), Unit.Index);
		// Screen nearby receivers using the modeled enclosure diagonal. This is a
		// geometry heuristic for the point-source approximation.
		Source.MinDistanceM = 2.0 * Unit.SizeM.Size();
		Source.TransferDb = VrgNoise::OutdoorTransferDb(*OutdoorNoiseReceiver, Source.CenterM, Source.MinDistanceM);
	}
	bOutdoorNoiseSourcesReady = true;
}

void ADataCenterSimulationActor::OutdoorNoiseAfterStep()
{
	if (!OutdoorNoiseReceiver.IsSet()) return;
	using namespace VrgNoise;
	FOutdoorSample Sample;
	Sample.SimTimeS = Clock.GetSimTimeS();
	Sample.bPlantRunning = bPlantActive && Plant.IsThermalActive();
	TArray<FLevel> Contributions;
	if (Sample.bPlantRunning)
	{
		if (!bOutdoorNoiseSourcesReady) BuildOutdoorNoiseSources();
		static const FSourceProfile LiebertProfile = Liebert940Standard60Hz();
		const FSourceProfile* Profile = UsesLiebert940Rating(Plant.GetThermalSpec().DryCooler) ? &LiebertProfile : nullptr;
		Contributions.Reserve(OutdoorNoiseSources.Num());
		for (const FOutdoorNoiseSource& Source : OutdoorNoiseSources)
		{
			const FPlantUnitTruth State = Plant.GetDryCoolerTruth(Source.Loop, Source.UnitIndex);
			const bool bRunning = State.bRunning && !State.bFailed;
			const FLevel Power = Evaluate(Profile, bRunning, State.FanRatio);
			if (bRunning && !Source.TransferDb.IsSet()) ++Sample.NearSources;
			Contributions.Add(OutdoorContribution(Power, Source.TransferDb));
		}
		const FFacilityPlantState& State = Plant.GetState();
		Sample.FwsSupplyK = State.SupplyK;
		Sample.CwSupplyK = State.CwSupplyK;
		Sample.DryCoolerFanW = Plant.GetFanElectricW() + Plant.GetCwFanElectricW();
		Sample.DryCoolerRejectedW = State.DryCoolerHeatW + State.CwDryCoolerHeatW;
	}
	Sample.Pressure = Combine(Contributions, EQuantity::SoundPressure, EWeighting::A);

	if (OutdoorNoiseHistory.Num() < OutdoorNoiseHistoryLimit) OutdoorNoiseHistory.Add(MoveTemp(Sample));
	else OutdoorNoiseHistory[OutdoorNoiseNextSample] = MoveTemp(Sample);
	OutdoorNoiseNextSample = (OutdoorNoiseNextSample + 1) % OutdoorNoiseHistoryLimit;
}

TArray<VrgNoise::FOutdoorSample> ADataCenterSimulationActor::GetOutdoorNoiseHistory() const
{
	TArray<VrgNoise::FOutdoorSample> Result;
	const int32 Count = OutdoorNoiseHistory.Num();
	if (Count == 0) return Result;
	Result.Reserve(Count);
	const int32 Start = OutdoorNoiseNextSample % Count;
	for (int32 Index = 0; Index < Count; ++Index) Result.Add(OutdoorNoiseHistory[(Start + Index) % Count]);
	return Result;
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseProbe(
		TEXT("vrg.Noise.Probe"), TEXT("Record drycooler noise at a world position in metres: x y z [sphere|hemisphere], or clear."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			const bool bClear = Args.Num() == 1 && Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase);
			VrgNoise::FOutdoorReceiver Receiver;
			if (!bClear)
			{
				if (Args.Num() < 3 || Args.Num() > 4 || !ParseNoiseCoordinate(Args[0], Receiver.PositionM.X)
					|| !ParseNoiseCoordinate(Args[1], Receiver.PositionM.Y) || !ParseNoiseCoordinate(Args[2], Receiver.PositionM.Z)
					|| (Args.Num() == 4 && !Args[3].Equals(TEXT("sphere"), ESearchCase::IgnoreCase)
						&& !Args[3].Equals(TEXT("hemisphere"), ESearchCase::IgnoreCase)))
				{
					UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.Probe x y z [sphere|hemisphere] or clear"));
					return;
				}
				if (Args.Num() == 4 && Args[3].Equals(TEXT("hemisphere"), ESearchCase::IgnoreCase))
					Receiver.Radiation = VrgNoise::EOutdoorRadiation::Hemisphere;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				if (bClear) It->ClearOutdoorNoiseReceiver();
				else It->SetOutdoorNoiseReceiver(Receiver);
				UE_LOG(LogTemp, Display, TEXT("[Noise] %s"), bClear ? TEXT("Probe cleared") : TEXT("Probe set; samples start on the next simulation step"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseStatus(
		TEXT("vrg.Noise.Status"), TEXT("Print drycooler-only LpA and cooling samples on the shared clock. Optional sample count (1..512)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			int32 Count = 1;
			TCHAR* End = nullptr;
			if (Args.Num() == 1) Count = FCString::Strtoi(*Args[0], &End, 10);
			if (Args.Num() > 1 || (End && *End != 0) || Count < 1 || Count > OutdoorNoiseHistoryLimit)
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.Status [1..512]"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const auto Samples = It->GetOutdoorNoiseHistory();
				if (Samples.IsEmpty()) UE_LOG(LogTemp, Display, TEXT("[Noise] No samples. Set vrg.Noise.Probe and advance the simulation."));
				for (int32 Index = FMath::Max(0, Samples.Num() - Count); Index < Samples.Num(); ++Index)
				{
					const auto& Sample = Samples[Index];
					const auto& L = Sample.Pressure;
					const FString Level = L.Db.IsSet() ? FString::Printf(TEXT("%.3f dBA%s"), *L.Db,
						L.MissingSources > 0 ? TEXT(" (partial)") : TEXT(""))
						: L.State == VrgNoise::ELevelState::InvalidInput ? TEXT("invalid")
						: L.State == VrgNoise::ELevelState::MissingData ? TEXT("missing") : TEXT("silent");
					UE_LOG(LogTemp, Display, TEXT("[Noise] t=%.3f s drycooler LpA=%s plant=%d known=%d stopped=%d missing=%d near=%d speedEstimate=%d fanW=%.3f rejectedW=%.3f FWS_K=%.3f CW_K=%.3f"),
						Sample.SimTimeS, *Level, int32(Sample.bPlantRunning), L.KnownSources, L.SilentSources, L.MissingSources,
						Sample.NearSources, int32(L.bEstimated), Sample.DryCoolerFanW, Sample.DryCoolerRejectedW, Sample.FwsSupplyK, Sample.CwSupplyK);
				}
			}
		}));
}
