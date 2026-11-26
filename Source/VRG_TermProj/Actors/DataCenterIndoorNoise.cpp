#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/GjRackLayout.h"

#include <cmath>
#include <cstdlib>

namespace
{
	using namespace VrgNoise;
	constexpr double IndoorSampleIntervalS = 0.2;

	const FSourceProfile& CrahProxy()
	{
		static const FSourceProfile Profile = Pcw035IndoorCrah();
		return Profile;
	}

	const FSourceProfile* CabinetFanProfile(const FRackFanSpec& Fan)
	{
		static const FSourceProfile San9Crj = SanAceIndoorFan(true);
		static const FSourceProfile San9Hva = SanAceIndoorFan(false);
		// FRackFanSpec carries ratings in place of a SKU. Match the three catalog ratings.
		if (Fan.RatedInputW == 37.2 && Fan.ShutoffPressurePa == 2400.0 && Fan.MaxFlowM3s == 1.06 / 60.0) return &San9Crj;
		if (Fan.RatedInputW == 31.2 && Fan.ShutoffPressurePa == 2300.0 && Fan.MaxFlowM3s == 1.05 / 60.0) return &San9Hva;
		return nullptr;
	}

	bool ParseIndoorNumber(const FString& Text, double& Out)
	{
		const FTCHARToUTF8 Utf8(*Text);
		char* End = nullptr;
		Out = std::strtod(Utf8.Get(), &End);
		return End != Utf8.Get() && End == Utf8.Get() + Utf8.Length() && std::isfinite(Out);
	}
}

bool ADataCenterSimulationActor::SetIndoorNoiseReceivers(const FVector2D& PositionM, double HeightM, TOptional<double> MapSpacingM)
{
	if (PositionM.ContainsNaN() || !FMath::IsFinite(HeightM) || HeightM <= 0.0
		|| (MapSpacingM.IsSet() && (!FMath::IsFinite(*MapSpacingM) || *MapSpacingM <= 0.0))) return false;
	const FBox RoomM = GetAirDomainBoundsM();
	if (RoomM.IsValid && HeightM >= RoomM.GetSize().Z) return false;
	if (RoomM.IsValid && MapSpacingM.IsSet())
	{
		TArray<FVector> Points;
		if (!SampleIndoorMap(RoomM, *MapSpacingM, HeightM, Points)) return false;
	}
	IndoorNoisePositionM = PositionM;
	IndoorNoiseHeightM = HeightM;
	IndoorNoiseMapSpacingM = MapSpacingM;
	ResetIndoorNoiseRun();
	return true;
}

void ADataCenterSimulationActor::ClearIndoorNoiseReceivers()
{
	IndoorNoisePositionM.Reset();
	IndoorNoiseMapSpacingM.Reset();
	ResetIndoorNoiseRun();
}

bool ADataCenterSimulationActor::SetIndoorNoiseAbsorption(const FIndoorAbsorption& Absorption)
{
	if (!Absorption.IsValid()) return false;
	IndoorNoiseAbsorption = Absorption;
	ResetIndoorNoiseRun();
	return true;
}

bool ADataCenterSimulationActor::SetIndoorNoiseCduRating(TOptional<double> WholeUnitLwA)
{
	if (WholeUnitLwA.IsSet() && !FMath::IsFinite(*WholeUnitLwA)) return false;
	IndoorCduRating = WholeUnitLwA;
	IndoorNoiseSnapshot.Reset();
	NextIndoorNoiseSampleS = 0.0;
	return true;
}

void ADataCenterSimulationActor::ResetIndoorNoiseRun()
{
	IndoorNoiseSnapshot.Reset();
	IndoorSourceBindings.Reset();
	IndoorReceivers.Reset();
	IndoorSources.Reset();
	IndoorNoiseZones.Reset();
	IndoorReceiverPaths.Reset();
	bIndoorNoisePrepared = false;
	NextIndoorNoiseSampleS = 0.0;
}

void ADataCenterSimulationActor::BuildIndoorNoise()
{
	bIndoorNoisePrepared = true;
	if (!AirField) return;
	const FUniformAirGrid& Grid = AirField->GetSolver().GetGrid();
	const FVector OriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
	const FBox RoomM(OriginM, OriginM + Grid.GetDomainSizeM());
	TArray<FVector> Points;
	if (IndoorNoiseMapSpacingM.IsSet())
	{
		if (!SampleIndoorMap(RoomM, *IndoorNoiseMapSpacingM, IndoorNoiseHeightM, Points)) return;
	}
	else Points.Add(FVector(*IndoorNoisePositionM, OriginM.Z + IndoorNoiseHeightM));

	FIndoorGeometry Geometry;
	Geometry.Build(Grid, OriginM, SolidOwner, Racks.Num() + Spines.Num() + CrahPlacements.Num());
	IndoorNoiseZones = Geometry.Zones;
	for (const FVector& Point : Points)
	{
		auto& Receiver = IndoorReceivers.AddDefaulted_GetRef();
		Receiver.PositionM = Point;
		Receiver.Zone = Geometry.ZoneAt(Grid, Point);
		Receiver.Pressure.Quantity = EQuantity::SoundPressure;
	}

	auto AddSource = [&](FString Id, EIndoorSourceKind Kind, const FVector& PositionM)
	{
		auto& Source = IndoorSources.AddDefaulted_GetRef();
		Source.Id = MoveTemp(Id);
		Source.Kind = Kind;
		Source.PositionM = PositionM;
		Source.Zone = Geometry.ZoneAt(Grid, PositionM);
	};
	auto AddCabinet = [&](const AActor& Actor, const FRackThermalModel& Model, const FString& Id,
		EIndoorSourceKind Kind, int32 Index, double DepthM)
	{
		const auto& Params = Model.GetParams();
		const auto* Rack = Cast<ARackThermalActor>(&Actor);
		for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
		{
			const auto& Layout = Params.Slots[Slot];
			if (!Params.DeviceTypes.IsValidIndex(Layout.DeviceType)) continue;
			const FRackFanSpec& Fans = Params.DeviceTypes[Layout.DeviceType].Fans;
			if (!Fans.HasFans()) continue;
			double BottomM = Layout.BottomZM, HeightM = Layout.HeightM;
			if (Rack) Rack->GetSlotHeightRangeM(Slot, BottomM, HeightM);
			const FVector Position = Actor.GetActorTransform().TransformPosition(
				SimUnits::MetersToUnreal(FVector(-.5 * DepthM - .02, 0, BottomM + .5 * HeightM)));
			AddSource(Id + FString::Printf(TEXT("/slot/%d"), Slot), Kind, SimUnits::UnrealToMeters(Position));
			auto& Binding = IndoorSourceBindings.AddDefaulted_GetRef();
			Binding.Index = Index;
			Binding.Slot = Slot;
			Binding.Fans = Fans.NumFans;
			Binding.RatedFanW = Fans.NumFans * Fans.RatedInputW;
			Binding.Profile = CabinetFanProfile(Fans);
			Binding.MinDistanceM = .5;
		}
	};
	for (int32 I = 0; I < Racks.Num(); ++I)
		if (Racks[I]) AddCabinet(*Racks[I], Racks[I]->GetModel(), TEXT("rack/") + VrgRackLifecycle::Identity(Racks[I]),
			EIndoorSourceKind::Rack, I, Racks[I]->RackSizeM.X);
	for (int32 I = 0; I < Spines.Num(); ++I)
		if (Spines[I]) AddCabinet(*Spines[I], Spines[I]->GetMutableModel(), TEXT("spine/") + VrgSpineLifecycle::Identity(Spines[I]),
			EIndoorSourceKind::Spine, I, GjRackLayout::FrameDepthM);

	const auto& Units = Bridge.GetCrahUnits();
	for (int32 I = 0; I < Units.Num(); ++I)
	{
		const auto& Unit = Units[I];
		TArray<int32> Exhaust;
		FVector Average = FVector::ZeroVector;
		const int32 Nx = Grid.GetNumCellsX(), Ny = Grid.GetNumCellsY();
		const auto Center = [&](int32 Cell) { return Grid.CellCenterM(Cell % Nx, (Cell / Nx) % Ny, Cell / (Nx * Ny)); };
		for (int32 Face = 0; Face < Unit.FaceFluidCell.Num(); ++Face)
			if (Unit.FaceIsExhaust[Face] != 0)
			{
				Exhaust.Add(Unit.FaceFluidCell[Face]);
				Average += Center(Exhaust.Last());
			}
		if (Exhaust.IsEmpty()) continue;
		Average /= Exhaust.Num();
		double Best = TNumericLimits<double>::Max();
		FVector Position = OriginM + Center(Exhaust[0]);
		for (const int32 Cell : Exhaust)
		{
			const FVector Point = Center(Cell);
			const double Distance = FVector::DistSquared(Average, Point);
			if (Distance < Best) { Best = Distance; Position = OriginM + Point; }
		}
		const auto* Actor = CrahUnitActors.IsValidIndex(I) ? CrahUnitActors[I].Get() : nullptr;
		AddSource(Actor ? TEXT("crah/") + VrgCrahLifecycle::Identity(Actor) : FString::Printf(TEXT("auto/crah/%d"), I),
			EIndoorSourceKind::Crah, Position);
		auto& Binding = IndoorSourceBindings.AddDefaulted_GetRef();
		Binding.Index = I;
		Binding.Profile = &CrahProxy();
	}

	if (Room)
	{
		TMap<const ACoolingDistributionUnitActor*, int32> BindingOfCdu;
		for (int32 Row = 0; Row < Room->GetNumRows(); ++Row)
		{
			const auto* Cdu = Room->GetRowCdu(Row);
			const int32 Pump = Room->GetRowPumpIndex(Row);
			if (!Cdu || Pump == INDEX_NONE) continue;
			int32* Existing = BindingOfCdu.Find(Cdu);
			if (!Existing)
			{
				const FVector Center = Cdu->GetActorTransform().TransformPosition(FVector(0, 0, 50.0 * Cdu->SizeM.Z));
				AddSource(TEXT("cdu/") + VrgCduLifecycle::Identity(Cdu), EIndoorSourceKind::Cdu, SimUnits::UnrealToMeters(Center));
				IndoorSourceBindings.AddDefaulted();
				Existing = &BindingOfCdu.Add(Cdu, IndoorSourceBindings.Num() - 1);
			}
			IndoorSourceBindings[*Existing].Pumps.AddUnique(Pump);
		}
	}

	TArray<TOptional<double>> Broadband, Octave;
	for (const FIndoorZone& Zone : Geometry.Zones)
	{
		Broadband.Add(IndoorDiffuseGain(Zone, IndoorNoiseAbsorption));
		Octave.Add(IndoorDiffuseGain(Zone, IndoorNoiseAbsorption, &CrahProxy()));
	}
	for (const auto& Receiver : IndoorReceivers)
		for (int32 I = 0; I < IndoorSources.Num(); ++I)
		{
			auto& Path = IndoorReceiverPaths.AddDefaulted_GetRef();
			const auto& Source = IndoorSources[I];
			if (Source.Zone == INDEX_NONE || Source.Zone != Receiver.Zone) continue;
			Path.bLocal = true;
			const bool bDirect = Geometry.HasDirectPath(Grid, Source.PositionM, Receiver.PositionM);
			Path.bBlockedDirect = !bDirect;
			const double DistanceM = FVector::Distance(Source.PositionM, Receiver.PositionM);
			Path.bNear = bDirect && DistanceM < IndoorSourceBindings[I].MinDistanceM;
			const auto& Diffuse = Source.Kind == EIndoorSourceKind::Crah ? Octave[Source.Zone] : Broadband[Source.Zone];
			if (Path.bNear || !Diffuse.IsSet()) continue;
			const double Gain = *Diffuse + (bDirect ? IndoorDirectGain(DistanceM) : 0.0);
			Path.bZero = Gain == 0.0;
			if (Gain > 0.0 && FMath::IsFinite(Gain)) Path.TransferDb = 10.0 * std::log10(Gain);
		}
}

void ADataCenterSimulationActor::IndoorNoiseAfterStep()
{
	if (!IndoorNoisePositionM.IsSet()) return;
	const double NowS = Clock.GetSimTimeS();
	if (NowS + 1e-9 < NextIndoorNoiseSampleS) return;
	NextIndoorNoiseSampleS = NowS + IndoorSampleIntervalS;
	if (!bIndoorNoisePrepared) BuildIndoorNoise();
	if (IndoorReceivers.IsEmpty()) return;

	const auto& Units = Bridge.GetCrahUnits();
	int32 Unlocated = 0;
	for (int32 I = 0; I < IndoorSources.Num(); ++I)
	{
		auto& Source = IndoorSources[I];
		const auto& Binding = IndoorSourceBindings[I];
		bool bRunning = false;
		double SpeedRatio = 1.0;
		switch (Source.Kind)
		{
		case EIndoorSourceKind::Rack:
		case EIndoorSourceKind::Spine:
		{
			const auto& Model = Source.Kind == EIndoorSourceKind::Rack ? Racks[Binding.Index]->GetModel() : Spines[Binding.Index]->GetMutableModel();
			const double FanW = Model.GetLastSlotFanPowerW(Binding.Slot);
			bRunning = FanW > 0.0;
			if (Binding.RatedFanW <= 0.0)
			{
				const auto& Flows = Model.GetLastSlotAirMassFlowKgs();
				bRunning = Flows.IsValidIndex(Binding.Slot) && Flows[Binding.Slot] > 0.0;
			}
			// Invert the very same electrical proxy used by SlotFanPowerW to read the interval speed.
			if (bRunning && Binding.RatedFanW > 0.0) SpeedRatio = std::cbrt(FanW / Binding.RatedFanW);
			break;
		}
		case EIndoorSourceKind::Crah:
			bRunning = Units.IsValidIndex(Binding.Index) && Units[Binding.Index].bPowered && Units[Binding.Index].FanPowerW > 0.0
				&& Bridge.GetLastCrahFanPowerW() > 0.0;
			break;
		case EIndoorSourceKind::Cdu:
			if (Room)
			{
				const auto& Network = Room->GetNetwork();
				for (const int32 Pump : Binding.Pumps)
					bRunning |= Network.IsPumpPowered(Pump) && Network.GetPumpSpeedRatio(Pump) > 0.0;
			}
			break;
		}
		Source.Power = Evaluate(Binding.Profile, bRunning, SpeedRatio);
		if (Source.Kind == EIndoorSourceKind::Cdu && bRunning && IndoorCduRating.IsSet())
		{
			Source.Power.State = ELevelState::Known;
			Source.Power.Db = IndoorCduRating;
			Source.Power.KnownSources = 1;
			Source.Power.MissingSources = 0;
			Source.Power.bEstimated = true;
		}
		if (Source.Power.Db.IsSet() && Binding.Fans > 0) *Source.Power.Db += 10.0 * std::log10(double(Binding.Fans));
		if (bRunning && Source.Zone == INDEX_NONE) ++Unlocated;
	}

	TArray<FLevel> Contributions;
	Contributions.Reserve(IndoorSources.Num());
	for (int32 R = 0; R < IndoorReceivers.Num(); ++R)
	{
		auto& Receiver = IndoorReceivers[R];
		Receiver.BlockedDirectSources = Receiver.NearSources = 0;
		if (Receiver.Zone == INDEX_NONE) continue;
		Contributions.Reset();
		for (int32 S = 0; S < IndoorSources.Num(); ++S)
		{
			const auto& Path = IndoorReceiverPaths[R * IndoorSources.Num() + S];
			const auto& Power = IndoorSources[S].Power;
			if (!Path.bLocal) continue;
			if (Power.State != ELevelState::Silent)
			{
				if (Path.bBlockedDirect) ++Receiver.BlockedDirectSources;
				if (Path.bNear) ++Receiver.NearSources;
				if (Path.bZero) continue;
			}
			Contributions.Add(OutdoorContribution(Power, Path.TransferDb));
		}
		Receiver.Pressure = Combine(Contributions, EQuantity::SoundPressure, EWeighting::A);
	}
	if (!IndoorNoiseSnapshot.IsSet()) IndoorNoiseSnapshot.Emplace();
	IndoorNoiseSnapshot->SimTimeS = NowS;
	IndoorNoiseSnapshot->Sources = IndoorSources;
	IndoorNoiseSnapshot->Receivers = IndoorReceivers;
	IndoorNoiseSnapshot->Zones = IndoorNoiseZones;
	IndoorNoiseSnapshot->UnlocatedRunningSources = Unlocated;
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GIndoorProbe(
		TEXT("vrg.Noise.IndoorProbe"), TEXT("Set a room probe: world_x_m world_y_m [height_above_floor_m] or clear."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			const bool bClear = Args.Num() == 1 && Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase);
			FVector2D Point = FVector2D::ZeroVector;
			double Height = DefaultEarHeightM;
			if (!bClear && (Args.Num() < 2 || Args.Num() > 3 || !ParseIndoorNumber(Args[0], Point.X) || !ParseIndoorNumber(Args[1], Point.Y)
				|| (Args.Num() == 3 && !ParseIndoorNumber(Args[2], Height))))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.IndoorProbe x_m y_m [height_m] or clear"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				if (bClear) It->ClearIndoorNoiseReceivers();
				else UE_LOG(LogTemp, Display, TEXT("[Noise] Indoor probe %s"), It->SetIndoorNoiseReceivers(Point, Height) ? TEXT("set") : TEXT("rejected"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GIndoorMap(
		TEXT("vrg.Noise.IndoorMap"), TEXT("Sample a horizontal room grid: max_spacing_m [height_above_floor_m], up to 256 receivers."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			double Spacing, Height = DefaultEarHeightM;
			if (Args.IsEmpty() || Args.Num() > 2 || !ParseIndoorNumber(Args[0], Spacing) || (Args.Num() == 2 && !ParseIndoorNumber(Args[1], Height)))
			{
				UE_LOG(LogTemp, Display, TEXT("[Noise] vrg.Noise.IndoorMap max_spacing_m [height_m]"));
				return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const bool bSet = It->SetIndoorNoiseReceivers(FVector2D::ZeroVector, Height, Spacing);
				UE_LOG(LogTemp, Display, TEXT("[Noise] %s"), bSet ? TEXT("Indoor map set") : TEXT("Map rejected: check height, spacing and the 256-point limit"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GIndoorAbsorption(
		TEXT("vrg.Noise.IndoorAbsorption"), TEXT("Set uniform absorption per octave (63 Hz to 8 kHz, eight values in 0..1) or reset."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld()) return;
			FIndoorAbsorption Absorption;
			if (!(Args.Num() == 1 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase)))
			{
				if (Args.Num() != IndoorOctaves) return;
				for (int32 Band = 0; Band < IndoorOctaves; ++Band)
				{
					if (!ParseIndoorNumber(Args[Band], Absorption.Shell[Band])) return;
					Absorption.Equipment[Band] = Absorption.Shell[Band];
				}
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
				UE_LOG(LogTemp, Display, TEXT("[Noise] Indoor absorption %s"), It->SetIndoorNoiseAbsorption(Absorption) ? TEXT("set") : TEXT("rejected"));
		}));

	FAutoConsoleCommandWithWorldAndArgs GIndoorCdu(
		TEXT("vrg.Noise.IndoorCduLwA"), TEXT("Use a whole-CDU A-weighted sound power level for operating CDUs, or clear it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld() || Args.Num() != 1) return;
			TOptional<double> Level;
			if (!Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase))
			{
				double Db;
				if (!ParseIndoorNumber(Args[0], Db)) return;
				Level = Db;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) It->SetIndoorNoiseCduRating(Level);
		}));

	FAutoConsoleCommandWithWorldAndArgs GIndoorStatus(
		TEXT("vrg.Noise.IndoorStatus"), TEXT("Print the latest indoor receiver map, optionally limited to one zero-based receiver."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || !World->IsGameWorld() || Args.Num() > 1) return;
			int32 Index = INDEX_NONE;
			if (Args.Num() == 1)
			{
				TCHAR* End = nullptr;
				Index = FCString::Strtoi(*Args[0], &End, 10);
				if (Index < 0 || End == *Args[0] || *End != 0) return;
			}
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				const auto& Snapshot = It->GetIndoorNoiseSnapshot();
				if (!Snapshot.IsSet())
				{
					UE_LOG(LogTemp, Display, TEXT("[Noise] No indoor map. Set a probe or a map and advance the simulation with a valid room."));
					continue;
				}
				if (Index != INDEX_NONE && !Snapshot->Receivers.IsValidIndex(Index)) continue;
				UE_LOG(LogTemp, Display, TEXT("[Noise] N4 t=%.3f s zones=%d sources=%d unlocated_running=%d estimated"),
					Snapshot->SimTimeS, Snapshot->Zones.Num(), Snapshot->Sources.Num(), Snapshot->UnlocatedRunningSources);
				const int32 First = Index == INDEX_NONE ? 0 : Index;
				const int32 End = Index == INDEX_NONE ? Snapshot->Receivers.Num() : Index + 1;
				for (int32 R = First; R < End; ++R)
				{
					const auto& Receiver = Snapshot->Receivers[R];
					const auto& Level = Receiver.Pressure;
					const FString Db = Level.Db.IsSet() ? FString::Printf(TEXT("%.3f dBA"), *Level.Db)
						: Receiver.Zone == INDEX_NONE ? TEXT("solid/outside")
						: Level.State == ELevelState::MissingData ? TEXT("missing")
						: Level.State == ELevelState::InvalidInput ? TEXT("invalid") : TEXT("silent");
					UE_LOG(LogTemp, Display, TEXT("[Noise] receiver=%d position=(%.3f, %.3f, %.3f) m zone=%d LpA=%s known=%d stopped=%d missing=%d near=%d direct_blocked=%d"),
						R, Receiver.PositionM.X, Receiver.PositionM.Y, Receiver.PositionM.Z, Receiver.Zone, *Db,
						Level.KnownSources, Level.SilentSources, Level.MissingSources, Receiver.NearSources, Receiver.BlockedDirectSources);
				}
			}
		}));
}
