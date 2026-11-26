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
	ResetNoiseReceiverGroup(VrgNoise::EReceiverSet::Probe);
	ResetOutdoorNoiseRun();
	return true;
}

void ADataCenterSimulationActor::ClearOutdoorNoiseReceiver()
{
	OutdoorNoiseReceiver.Reset();
	ResetNoiseReceiverGroup(VrgNoise::EReceiverSet::Probe);
	ResetOutdoorNoiseRun();
}

void ADataCenterSimulationActor::ResetOutdoorNoiseRun()
{
	bOutdoorNoiseSourcesReady = false;
	OutdoorNoiseSources.Reset();
	OutdoorNoiseHistory.Reset();
	OutdoorNoiseNextSample = 0;
	ResetBoundaryNoisePaths();
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
		if (OutdoorNoiseReceiver.IsSet())
			Source.TransferDb = VrgNoise::OutdoorTransferDb(*OutdoorNoiseReceiver, Source.CenterM, Source.MinDistanceM);
	}
	bOutdoorNoiseSourcesReady = true;
}

void ADataCenterSimulationActor::OutdoorNoiseAfterStep()
{
	if (!OutdoorNoiseReceiver.IsSet() && OutdoorBoundaryPoints.IsEmpty()) return;
	using namespace VrgNoise;
	FOutdoorSample Sample;
	Sample.SimTimeS = Clock.GetSimTimeS();
	Sample.bPlantRunning = bPlantActive && Plant.IsThermalActive();
	TArray<FLevel> Contributions, Powers;
	if (Sample.bPlantRunning)
	{
		if (!bOutdoorNoiseSourcesReady) BuildOutdoorNoiseSources();
		static const FSourceProfile LiebertProfile = Liebert940Standard60Hz();
		const FSourceProfile* Profile = UsesLiebert940Rating(Plant.GetThermalSpec().DryCooler) ? &LiebertProfile : nullptr;
		if (OutdoorNoiseReceiver.IsSet()) Contributions.Reserve(OutdoorNoiseSources.Num());
		if (!OutdoorBoundaryPoints.IsEmpty()) Powers.Reserve(OutdoorNoiseSources.Num());
		for (const FOutdoorNoiseSource& Source : OutdoorNoiseSources)
		{
			const FPlantUnitTruth State = Plant.GetDryCoolerTruth(Source.Loop, Source.UnitIndex);
			const bool bRunning = State.bRunning && !State.bFailed;
			const FLevel Power = Evaluate(Profile, bRunning, State.FanRatio);
			if (OutdoorNoiseReceiver.IsSet())
			{
				if (bRunning && !Source.TransferDb.IsSet()) ++Sample.NearSources;
				Contributions.Add(OutdoorContribution(Power, Source.TransferDb));
			}
			if (!OutdoorBoundaryPoints.IsEmpty()) Powers.Add(Power);
		}
		const FFacilityPlantState& State = Plant.GetState();
		Sample.FwsSupplyK = State.SupplyK;
		Sample.CwSupplyK = State.CwSupplyK;
		Sample.DryCoolerFanW = Plant.GetFanElectricW() + Plant.GetCwFanElectricW();
		Sample.DryCoolerRejectedW = State.DryCoolerHeatW + State.CwDryCoolerHeatW;
	}
	UpdateOutdoorNoiseBoundary(Sample, Powers);
	if (!OutdoorNoiseReceiver.IsSet()) return;
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

namespace
{
	bool ParseCoordinates(const TArray<FString>& Args, int32 Start, TArray<double>& Values)
	{
		for (int32 Index = Start; Index < Args.Num(); ++Index)
		{
			double Number;
			if (!ParseNoiseCoordinate(Args[Index], Number)) return false;
			Values.Add(Number);
		}
		return true;
	}

	bool ParseNoiseIndex(const FString& Token, int32& Index)
	{
		TCHAR* End = nullptr;
		Index = FCString::Strtoi(*Token, &End, 10);
		return End != *Token && *End == 0 && Index >= 0;
	}

	const TCHAR* BoundaryLevelState(const VrgNoise::FLevel& Pressure)
	{
		using VrgNoise::ELevelState;
		if (Pressure.Db.IsSet()) return Pressure.MissingSources > 0 ? TEXT("partial") : TEXT("estimated");
		if (Pressure.State == ELevelState::Silent) return TEXT("silent");
		return Pressure.State == ELevelState::MissingData ? TEXT("missing") : TEXT("invalid");
	}

	const TCHAR* BoundaryUsage = TEXT("vrg.Noise.Boundary spacing_m height_m x1 y1 x2 y2 [x3 y3 ...] or clear");
	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseBoundary(
		TEXT("vrg.Noise.Boundary"), BoundaryUsage,
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			if (Args.Num() == 1 && Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase))
			{
				for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) It->ClearOutdoorNoiseBoundary();
				return;
			}
			TArray<double> Values;
			if (Args.Num() < 6 || Args.Num() > 2 + 2 * (VrgNoise::MaxBoundaryReceivers + 1) || Args.Num() % 2 != 0 || !ParseCoordinates(Args, 0, Values))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] %s"), BoundaryUsage);
				return;
			}
			TArray<FVector2D> Vertices;
			for (int32 Index = 2; Index < Values.Num(); Index += 2) Vertices.Emplace(Values[Index], Values[Index + 1]);
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const bool bOk = It->SetOutdoorNoiseBoundary(Vertices, Values[0], Values[1]);
				UE_LOG(LogTemp, Display, TEXT("[Noise] %s"), bOk ? TEXT("Boundary set; advance the simulation")
					: TEXT("Boundary rejected: positive height and spacing, distinct adjacent vertices, up to 64 receivers."));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseEnvironment(
		TEXT("vrg.Noise.Environment"), TEXT("Set propagation inputs: ground_z_m ground_factor relative_humidity_percent pressure_kPa."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			TArray<double> Values;
			if (Args.Num() != 4 || !ParseCoordinates(Args, 0, Values))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.Environment ground_z_m G(0..1) RH(0..100) pressure_kPa"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				auto Environment = It->GetOutdoorNoiseEnvironment();
				Environment.GroundZM = Values[0];
				Environment.GroundFactor = Values[1];
				Environment.RelativeHumidity = Values[2] / 100.0;
				Environment.PressurePa = Values[3] * 1000.0;
				UE_LOG(LogTemp, Display, TEXT("[Noise] Environment %s"), It->SetOutdoorNoiseEnvironment(Environment) ? TEXT("set") : TEXT("rejected"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseBarrier(
		TEXT("vrg.Noise.Barrier"), TEXT("Define a vertical acoustic screen: add x1 y1 x2 y2 height_m [reflected_energy_fraction], or clear."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			const bool bClear = Args.Num() == 1 && Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase);
			TArray<double> Values;
			if (!bClear && ((Args.Num() != 6 && Args.Num() != 7) || !Args[0].Equals(TEXT("add"), ESearchCase::IgnoreCase) || !ParseCoordinates(Args, 1, Values)))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.Barrier add x1 y1 x2 y2 height_m [rho(0..1)] or clear"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				auto Environment = It->GetOutdoorNoiseEnvironment();
				if (bClear) Environment.Screens.Reset();
				else Environment.Screens.Add({{Values[0], Values[1]}, {Values[2], Values[3]}, Values[4], Values.Num() > 5 ? Values[5] : 0.0});
				UE_LOG(LogTemp, Display, TEXT("[Noise] Barrier change %s"), It->SetOutdoorNoiseEnvironment(Environment) ? TEXT("applied") : TEXT("rejected"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoiseBoundaryStatus(
		TEXT("vrg.Noise.BoundaryStatus"), TEXT("Print latest estimated boundary levels, or one zero-based receiver index."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			int32 Selected = INDEX_NONE;
			if (Args.Num() > 1 || (Args.Num() == 1 && !ParseNoiseIndex(Args[0], Selected)))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.BoundaryStatus [receiver_index]"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const auto& Snapshot = It->GetOutdoorBoundarySnapshot();
				if (!Snapshot.IsSet())
				{
					UE_LOG(LogTemp, Display, TEXT("[Noise] No boundary results. Set vrg.Noise.Boundary and advance the simulation."));
					continue;
				}
				if (Selected != INDEX_NONE && !Snapshot->Receivers.IsValidIndex(Selected))
				{
					UE_LOG(LogTemp, Display, TEXT("[Noise] Boundary receiver index out of range."));
					continue;
				}
				const auto& Environment = It->GetOutdoorNoiseEnvironment();
				UE_LOG(LogTemp, Display, TEXT("[Noise] N3 t=%.3f s plant=%d T=%.3f K RH=%.1f%% pressure=%.3f kPa ground_z=%.3f m G=%.2f screens=%d spectrum=DR95-shape"),
					Snapshot->SimTimeS, int32(Snapshot->bPlantRunning), Snapshot->TemperatureK, 100.0 * Environment.RelativeHumidity,
					Environment.PressurePa / 1000.0, Environment.GroundZM, Environment.GroundFactor, Environment.Screens.Num());
				for (int32 Index = Selected == INDEX_NONE ? 0 : Selected; Index < (Selected == INDEX_NONE ? Snapshot->Receivers.Num() : Selected + 1); ++Index)
				{
					const auto& Receiver = Snapshot->Receivers[Index];
					const auto& Pressure = Receiver.Pressure;
					const FString Level = Pressure.Db.IsSet() ? FString::Printf(TEXT("%.3f dBA"), *Pressure.Db) : TEXT("-");
					UE_LOG(LogTemp, Display, TEXT("[Noise] receiver=%d position=(%.3f, %.3f, %.3f) m LpA=%s %s known=%d stopped=%d missing=%d near=%d"),
						Index, Receiver.PositionM.X, Receiver.PositionM.Y, Receiver.PositionM.Z, *Level, BoundaryLevelState(Pressure),
						Pressure.KnownSources, Pressure.SilentSources, Pressure.MissingSources, Receiver.NearSources);
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GOutdoorNoisePath(
		TEXT("vrg.Noise.Path"), TEXT("Print per-octave propagation terms for a receiver and source index, both zero-based."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			int32 Receiver, Source;
			if (Args.Num() != 2 || !ParseNoiseIndex(Args[0], Receiver) || !ParseNoiseIndex(Args[1], Source))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.Path receiver_index source_index"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const auto* Path = It->GetOutdoorBoundaryPath(Receiver, Source);
				if (!Path || !Path->Loss.IsSet() || !Path->TransferDb.IsSet())
				{
					UE_LOG(LogTemp, Display, TEXT("[Noise] Path unavailable (near source, invalid or not yet sampled)."));
					continue;
				}
				const auto& Loss = *Path->Loss;
				UE_LOG(LogTemp, Display, TEXT("[Noise] receiver=%d source=%d id=%s distance=%.3f m Adiv=%.3f dB blocks=%d reflections=%d LwA_to_LpA=%.3f dB"),
					Receiver, Source, *It->GetOutdoorNoiseSources()[Source].Id, Loss.DistanceM, Loss.DivergenceDb, Loss.BlockingScreens, Loss.ReflectedPaths, *Path->TransferDb);
				for (int32 Band = 0; Band < VrgNoise::OutdoorOctaves; ++Band)
				{
					const auto& Term = Loss.Bands[Band];
					UE_LOG(LogTemp, Display, TEXT("[Noise] %.0f Hz Aatm=%.3f dB Agr=%.3f dB Abar=%.3f dB reflection_gain=%.3f dB"),
						VrgNoise::OutdoorNominalHz[Band], Term.AtmosphereDb, Term.GroundDb, Term.BarrierDb, Term.ReflectionGainDb);
				}
			}
		}));
}
