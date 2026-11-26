#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseIndoor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Simulation/Acoustics/NoiseOutdoor.h"
#include "Simulation/Air/RoomAirDomain.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Visualization/GjRackLayout.h"

#include <cmath>

namespace
{
	constexpr EAutomationTestFlags IndoorTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndoorFieldTest, "VRG.Noise.Indoor.DirectDiffuse", IndoorTestFlags)
bool FIndoorFieldTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	FIndoorAbsorption Absorption;
	for (int32 I = 0; I < IndoorOctaves; ++I)
	{
		Absorption.Shell[I] = .2;
		Absorption.Equipment[I] = .4;
	}
	const FIndoorZone Zone{100.0, 25.0};
	const FSourceProfile Crah = Pcw035IndoorCrah();
	const auto Diffuse = IndoorDiffuseGain(Zone, Absorption);
	const auto Octave = IndoorDiffuseGain(Zone, Absorption, &Crah);
	if (!TestTrue(TEXT("area weighted room supports direct and octave transfer"), Diffuse.IsSet() && Octave.IsSet())) return false;
	const double ExpectedR = 100.0 * .25 / .75;
	const double GainError = FMath::Max(FMath::Abs(*Diffuse - 4.0 / ExpectedR), FMath::Abs(*Octave - 4.0 / ExpectedR));
	const double DropDb = 10.0 * std::log10(IndoorDirectGain(1.0) / IndoorDirectGain(2.0));
	const FSourceProfile Fan = SanAceIndoorFan(true);
	const FLevel BandPower = Evaluate(&Fan, true, 1.0);
	const FLevel FreeField = OutdoorContribution(BandPower, 10.0 * std::log10(IndoorDirectGain(1.0)));
	const FLevel One = OutdoorContribution(BandPower, 10.0 * std::log10(IndoorDirectGain(2.0) + *Diffuse));
	const FLevel Two = Combine({One, One}, EQuantity::SoundPressure, EWeighting::A);
	const double SumErrorDb = FMath::Abs(*Two.Db - *One.Db - 3.010299956639812);
	const double DistanceErrorDb = FMath::Abs(DropDb - 6.020599913279624);
	const double FanReferenceErrorDb = FMath::Abs(*FreeField.Db - 72.0);
	const double CrahReferenceErrorDb = FMath::Abs(*Evaluate(&Crah, true, 1.0).Db - 95.8);
	AddInfo(FString::Printf(TEXT("N4 analytic: R %.9f m2, diffuse gain error %.6e m^-2, distance %.6e dB, source sum %.6e dB, fan 1m %.6e dB, CRAH table %.6e dB"),
		ExpectedR, GainError, DistanceErrorDb, SumErrorDb, FanReferenceErrorDb, CrahReferenceErrorDb));
	TestTrue(TEXT("room gain, free field distance and incoherent sum meet measured numerical bounds"),
		GainError < 1e-12 && DistanceErrorDb < 1e-12 && SumErrorDb < 1e-12 && FanReferenceErrorDb < 1e-12);
	TestTrue(TEXT("63 Hz to 8 kHz energy agrees with manufacturer rounded LwA"), CrahReferenceErrorDb < 0.05);
	TestTrue(TEXT("diffuse transfer is positive and the second independent source increases level"), *Diffuse > 0.0 && *Two.Db > *One.Db);
	FIndoorAbsorption Lossless, Absorptive;
	for (int32 I = 0; I < IndoorOctaves; ++I)
	{
		Lossless.Shell[I] = Lossless.Equipment[I] = 0.0;
		Absorptive.Shell[I] = Absorptive.Equipment[I] = 1.0;
	}
	TestTrue(TEXT("lossless enclosure has no finite steady result; perfectly absorptive room has zero diffuse contribution"),
		!IndoorDiffuseGain(Zone, Lossless).IsSet() && IndoorDiffuseGain(Zone, Absorptive) == TOptional<double>(0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndoorZonesTest, "VRG.Noise.Indoor.ZonesAndOpenings", IndoorTestFlags)
bool FIndoorZonesTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	FAirGridParams Params;
	Params.NumCellsX = 6; Params.NumCellsY = 4; Params.NumCellsZ = 4; Params.CellSizeM = 0.5;
	FUniformAirGrid Grid;
	Grid.Initialize(Params);
	const FVector Origin(2, -3, 1);
	FIndoorGeometry Geometry;
	Geometry.Build(Grid, Origin);
	const double SurfaceErrorM2 = FMath::Abs(Geometry.Zones[0].SurfaceM2 - 32.0);
	const FVector Left = Origin + FVector(.75, 1.25, 1.25), Right = Origin + FVector(2.25, 1.25, 1.25);
	TestTrue(TEXT("empty room direct path and one zone"), Geometry.Zones.Num() == 1 && Geometry.HasDirectPath(Grid, Left, Right));
	for (int32 Z = 0; Z < 4; ++Z)
		for (int32 Y = 0; Y < 4; ++Y) Grid.bSolidCell[Grid.CellIndex(3, Y, Z)] = true;
	Geometry.Build(Grid, Origin);
	TestTrue(TEXT("sealed partition blocks direct and isolates diffuse zones"),
		Geometry.Zones.Num() == 2 && Geometry.ZoneAt(Grid, Left) != Geometry.ZoneAt(Grid, Right) && !Geometry.HasDirectPath(Grid, Left, Right));
	Grid.bSolidCell[Grid.CellIndex(3, 2, 2)] = false;
	Geometry.Build(Grid, Origin);
	const FVector Offset(0, -.5, 0);
	TestTrue(TEXT("opening rejoins the diffuse zones but only unobstructed rays see the other side"),
		Geometry.Zones.Num() == 1 && Geometry.HasDirectPath(Grid, Left, Right) && !Geometry.HasDirectPath(Grid, Left + Offset, Right + Offset));
	TestTrue(TEXT("receiver in solid and outside room is unavailable"),
		Geometry.ZoneAt(Grid, Origin + FVector(1.75, .25, .25)) == INDEX_NONE && Geometry.ZoneAt(Grid, Origin + FVector(-.1, 0, 0)) == INDEX_NONE);
	TestTrue(TEXT("voxel area agrees exactly with this axis-aligned box"), SurfaceErrorM2 == 0.0);
	Grid.ClearSolidCells();
	FRoomCeilingPlan Plate;
	Plate.MinCell = FIntVector(0, 0, 2);
	Plate.MaxCellExclusive = FIntVector(6, 4, 3);
	const FVector Below = Origin + FVector(.75, .75, .75), Above = Origin + FVector(.75, .75, 1.75);
	VrgRoomAirDomain::MarkCeilingCells(Grid, {Plate});
	Geometry.Build(Grid, Origin);
	TestTrue(TEXT("production ceiling mask divides upper and lower sound zones"),
		Geometry.Zones.Num() == 2 && !Geometry.HasDirectPath(Grid, Below, Above));
	Grid.ClearSolidCells();
	Plate.OpenCells.Add(FIntRect(1, 1, 2, 2));
	VrgRoomAirDomain::MarkCeilingCells(Grid, {Plate});
	Geometry.Build(Grid, Origin);
	TestTrue(TEXT("ceiling hole restores direct path and one diffuse zone"),
		Geometry.Zones.Num() == 1 && Geometry.HasDirectPath(Grid, Below, Above));
	TArray<FVector> Map;
	TestTrue(TEXT("map uses world floor plus ear height"), SampleIndoorMap(FBox(Origin, Origin + FVector(3, 2, 2)), 1.0, 1.5, Map)
		&& Map.Num() == 6 && Map[0] == Origin + FVector(.5, .5, 1.5));
	TestTrue(TEXT("dense invalid map does not replace prior receivers"), !SampleIndoorMap(FBox(Origin, Origin + FVector(3, 2, 2)), .001, 1.5, Map) && Map.Num() == 6);
	AddInfo(FString::Printf(TEXT("N4 voxel geometry: exposed box area error %.6e m2, sealed 2 zones, open 1 zone"), SurfaceErrorM2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIndoorRuntimeTest, "VRG.Noise.Indoor.Runtime", IndoorTestFlags)
bool FIndoorRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	const double PreviousSpeed = ADataCenterSimulationActor::GetRequestedSimSpeed();
	ADataCenterSimulationActor::SetRequestedSimSpeed(1.0);
	ON_SCOPE_EXIT
	{
		ADataCenterSimulationActor::SetRequestedSimSpeed(PreviousSpeed);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = Sim->bShowPlant = Sim->bFloorDrains = false;
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	TArray<ARackThermalActor*> Racks;
	for (const double Y : {0.0, 66.0})
	{
		auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset();
		Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Rack->Input.FanSpeedOverride = .5;
		Racks.Add(Rack);
	}
	auto* Spine = World->SpawnActor<ASpineCabinetActor>(FVector(0, 150, 0), FRotator::ZeroRotator);
	Spine->EnsureThermalModel();
	Spine->GetMutableInput().FanSpeedOverride = .5;
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-280, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	Sim->EnterLayoutMode();
	FRoomAirDomainSettings Manual;
	Manual.bManual = true;
	Manual.MinXYM = FVector2D(-4.0, -2.0); Manual.MaxXYM = FVector2D(5.0, 4.0); Manual.CeilingHeightM = 3.5;
	FString Reason;
	if (!TestTrue(TEXT("manual room contains all devices"), Sim->TrySetRoomAirDomain(Manual, Reason))) { AddError(Reason); return false; }
	auto Command = [&](const TCHAR* Cmd) { return IConsoleManager::Get().ProcessUserConsoleInput(Cmd, *GLog, World); };
	TestTrue(TEXT("probe registered"), Command(TEXT("vrg.Noise.IndoorProbe -2 1 1.5")));
	Sim->EnterSimulationMode();
	Sim->Tick(.021f);
	const auto Start = Sim->GetIndoorNoiseSnapshot();
	if (!TestTrue(TEXT("probe produced an indoor LpA on the shared clock"), Start.IsSet() && Start->Receivers.Num() == 1
		&& Start->Receivers[0].Pressure.Db.IsSet())) return false;
	const double TimeErrorS = FMath::Abs(Start->SimTimeS - Sim->GetSimClockTimeS());
	const auto Find = [&](const FIndoorSnapshot& Sample, EIndoorSourceKind Kind)
	{
		return Sample.Sources.FindByPredicate([Kind](const auto& S) { return S.Kind == Kind && S.Power.State == ELevelState::Known; });
	};
	const auto* RackSource = Find(*Start, EIndoorSourceKind::Rack);
	const auto* SpineSource = Find(*Start, EIndoorSourceKind::Spine);
	const auto* CrahSource = Find(*Start, EIndoorSourceKind::Crah);
	const auto* CduSource = Start->Sources.FindByPredicate([](const auto& S) { return S.Kind == EIndoorSourceKind::Cdu; });
	if (!TestTrue(TEXT("all four equipment classes mapped, unresolved CDU distinguished from silence"),
		RackSource && SpineSource && CrahSource && CduSource && CduSource->Power.State == ELevelState::MissingData)) return false;
	TestTrue(TEXT("one CDU source despite multiple modeled row branches"),
		Start->Sources.FilterByPredicate([](const auto& S) { return S.Kind == EIndoorSourceKind::Cdu; }).Num() == 1);
	const FIndoorSourceLevel* OnRack = RackSource;
	if (!TestTrue(TEXT("model excludes silent and out-of-room locations explicitly"), OnRack->Zone != INDEX_NONE && Start->Receivers[0].Zone != INDEX_NONE)) return false;
	const double SpineExpected = 71.0 + 10.0 * std::log10(4.0 * UE_DOUBLE_PI * 6.0) + 50.0 * std::log10(.5);
	const double SpineErrorDb = FMath::Abs(*SpineSource->Power.Db - SpineExpected);

	TestTrue(TEXT("uniform absorption command registered"), Command(TEXT("vrg.Noise.IndoorAbsorption .5 .5 .5 .5 .5 .5 .5 .5")));
	Sim->Tick(.021f);
	const auto Absorbed = Sim->GetIndoorNoiseSnapshot();
	if (!TestTrue(TEXT("higher absorption reduces the same receiver level"), Absorbed.IsSet() && Absorbed->Receivers[0].Pressure.Db.IsSet()
		&& *Absorbed->Receivers[0].Pressure.Db < *Start->Receivers[0].Pressure.Db)) return false;
	Spine->SetPowered(false);
	for (int32 Frame = 0; Frame < 16; ++Frame) Sim->Tick(.021f);
	const auto Stopped = Sim->GetIndoorNoiseSnapshot();
	TestTrue(TEXT("spine off reflected on next indoor sample"), Stopped.IsSet() && !Find(*Stopped, EIndoorSourceKind::Spine)
		&& Stopped->Sources.ContainsByPredicate([](const auto& S) { return S.Kind == EIndoorSourceKind::Spine && S.Power.State == ELevelState::Silent; }));
	TestTrue(TEXT("CDU LwA input can supply verified data"), Command(TEXT("vrg.Noise.IndoorCduLwA 76")));
	Sim->Tick(.021f);
	const auto Rated = Sim->GetIndoorNoiseSnapshot();
	TestTrue(TEXT("user supplied the whole-unit value once"), Rated.IsSet() && Find(*Rated, EIndoorSourceKind::Cdu)
		&& Find(*Rated, EIndoorSourceKind::Cdu)->Power.Db == TOptional<double>(76.0));
	TestTrue(TEXT("ear-height map and status registered"), Command(TEXT("vrg.Noise.IndoorMap 1 1.5")));
	Sim->Tick(.021f);
	TestTrue(TEXT("receivers laid out across floor at selected height"), Sim->GetIndoorNoiseSnapshot().IsSet()
		&& Sim->GetIndoorNoiseSnapshot()->Receivers.Num() == 54 && Command(TEXT("vrg.Noise.IndoorStatus 0")));

	const FString MovedId = OnRack->Id;
	const double PriorX = OnRack->PositionM.X;
	Sim->EnterLayoutMode();
	for (auto* Rack : Racks) Rack->AddActorWorldOffset(FVector(100, 0, 0));
	Room->RebuildRoomNow();
	Sim->EnterSimulationMode();
	Sim->Tick(.021f);
	const auto Moved = Sim->GetIndoorNoiseSnapshot();
	const auto* MovedSource = Moved.IsSet() ? Moved->Sources.FindByPredicate([&](const auto& S) { return S.Id == MovedId; }) : nullptr;
	const double MoveErrorM = MovedSource ? FMath::Abs(MovedSource->PositionM.X - PriorX - 1.0) : TNumericLimits<double>::Max();
	TestTrue(TEXT("new run uses moved source at the same stable slot ID"), MovedSource && MovedSource->PositionM.X > PriorX);
	AddInfo(FString::Printf(TEXT("N4 runtime: absorption %.6f -> %.6f dBA; spine power error %.6e dB, clock %.6e s, move %.6e m"),
		*Start->Receivers[0].Pressure.Db, *Absorbed->Receivers[0].Pressure.Db, SpineErrorDb, TimeErrorS, MoveErrorM));
	TestTrue(TEXT("speed mapping and rebuilt source coordinate meet measured bounds; timestamp is exact"),
		SpineErrorDb < 1e-12 && MoveErrorM < 1e-12 && TimeErrorS == 0.0);
	Command(TEXT("vrg.Noise.IndoorProbe clear"));
	Sim->Tick(.021f);
	TestFalse(TEXT("clear disables collection"), Sim->GetIndoorNoiseSnapshot().IsSet());
	Sim->EnterLayoutMode();
	return true;
}

#endif
