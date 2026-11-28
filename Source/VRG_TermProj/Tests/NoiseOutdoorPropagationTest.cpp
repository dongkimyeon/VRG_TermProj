#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseOutdoorPropagation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Visualization/GjRackLayout.h"

#include <cmath>

namespace
{
	constexpr EAutomationTestFlags PropagationFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOutdoorReferenceTest, "VRG.Noise.Propagation.PublishedFlatGround", PropagationFlags)
bool FNoiseOutdoorReferenceTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const auto Air = OutdoorAir(293.15, 0.7, 101325.0);
	if (!TestTrue(TEXT("reference atmosphere available"), Air.IsSet())) return false;
	constexpr double RefAbsorptionDbPerKm[OutdoorOctaves] = {0.1, 0.3, 1.1, 2.8, 5.0, 9.0, 22.9, 76.6};
	constexpr double RefLpA[3][OutdoorOctaves + 1] = {
		{13.70, 23.76, 31.10, 36.17, 38.95, 39.37, 36.47, 23.94, 44.29},
		{13.70, 20.07, 24.42, 30.00, 36.11, 37.53, 34.63, 22.10, 41.53},
		{13.70, 16.38, 17.73, 23.83, 33.27, 35.69, 32.79, 20.26, 39.14}
	};
	const FSourceProfile Surrogate = Liebert940EstimatedOctaves();
	const FLevel SurrogateRating = Evaluate(&Surrogate, true, 1.0);
	const FLevel SurrogateHalf = Evaluate(&Surrogate, true, 0.5);
	const FSourceProfile Broadband = Liebert940Standard60Hz();
	const FLevel RatedBroadband = Evaluate(&Broadband, true, 1.0);
	if (!TestTrue(TEXT("proxy spectrum and reference are available"), SurrogateRating.Db.IsSet() && SurrogateHalf.Db.IsSet() && RatedBroadband.Db.IsSet()))
		return false;
	const double NormalizationErrorDb = FMath::Abs(*SurrogateRating.Db - *RatedBroadband.Db);
	const double SpeedErrorDb = FMath::Abs(*SurrogateHalf.Db - *SurrogateRating.Db - 50.0 * std::log10(0.5));
	TestTrue(TEXT("proxy identifies itself as estimated for all running speeds"), SurrogateRating.bEstimated && SurrogateHalf.bEstimated);
	FSourceProfile ReferenceProfile = Surrogate;
	ReferenceProfile.LevelsDb.Init(93.0, OutdoorOctaves);
	const FLevel ReferencePower = Evaluate(&ReferenceProfile, true, 1.0);
	double AtmosphereErrorDbPerKm = 0.0, BandErrorDb = 0.0, TotalErrorDb = 0.0;
	FOutdoorEnvironment Env;
	for (int32 Case = 0; Case < 3; ++Case)
	{
		Env.GroundFactor = 0.5 * Case;
		const auto Path = OutdoorPath(FVector(10, 10, 1), FVector(200, 50, 4), Env, *Air);
		if (!TestTrue(TEXT("ISO/TR T01-T03 paths available"), Path.IsSet())) return false;
		for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
		{
			double A = 0.0;
			TryOctaveACorrection(OutdoorNominalHz[Band], A);
			const auto& Term = Path->Bands[Band];
			const double LpA = 93.0 + A - Path->DivergenceDb - Term.AtmosphereDb - Term.GroundDb;
			BandErrorDb = FMath::Max(BandErrorDb, FMath::Abs(LpA - RefLpA[Case][Band]));
			AtmosphereErrorDbPerKm = FMath::Max(AtmosphereErrorDbPerKm, FMath::Abs(1000.0 * Air->AbsorptionDbPerM[Band] - RefAbsorptionDbPerKm[Band]));
		}
		const auto Transfer = OutdoorOctaveTransferDb(*Path, ReferenceProfile);
		if (!TestTrue(TEXT("broadband reduction of the bandwise transfer is available"), Transfer.IsSet() && ReferencePower.Db.IsSet())) return false;
		const double Total = *ReferencePower.Db + *Transfer;
		TotalErrorDb = FMath::Max(TotalErrorDb, FMath::Abs(Total - RefLpA[Case][OutdoorOctaves]));
	}
	AddInfo(FString::Printf(TEXT("N3 ISO/TR T01-T03 discrepancies: atmosphere %.6e dB/km, band %.6e dB, sum %.6e dB; proxy rating %.6e dB, speed %.6e dB"),
		AtmosphereErrorDbPerKm, BandErrorDb, TotalErrorDb, NormalizationErrorDb, SpeedErrorDb));
	TestTrue(TEXT("atmospheric coefficients match the published 0.1 dB/km rounded table"), AtmosphereErrorDbPerKm < 0.05);
	TestTrue(TEXT("proxy preserves rated LwA and the common speed law"), NormalizationErrorDb < 1e-12 && SpeedErrorDb < 1e-12);
	TestTrue(TEXT("bands and LpA sums within the published 0.05 dB reference tolerance"), BandErrorDb <= 0.05 && TotalErrorDb <= 0.05);
	TestFalse(TEXT("invalid humidity rejected"), OutdoorAir(293.15, 1.1, 101325.0).IsSet());
	TestTrue(TEXT("frequency dependent absorption and air inputs are live"), Air->AbsorptionDbPerM[7] > Air->AbsorptionDbPerM[0]
		&& OutdoorAir(303.15, 0.5, 90000.0).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOutdoorScreenTest, "VRG.Noise.Propagation.ScreensAndReflection", PropagationFlags)
bool FNoiseOutdoorScreenTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const FVector Source(0, 0, 2), Receiver(20, 0, 2);
	const auto Air = OutdoorAir(293.15, 0.7, 101325.0);
	FOutdoorEnvironment Env;
	Env.Screens.Add({FVector2D(10, -100), FVector2D(10, 100), 4.0, 0.0});
	const auto Screen = OutdoorPath(Source, Receiver, Env, *Air);
	const auto Clear = OutdoorPath(Source, FVector(20, 0, 8), Env, *Air);
	Env.Screens[0] = {FVector2D(-20, 10), FVector2D(40, 10), 10.0, 0.5};
	const auto Reflect = OutdoorPath(Source, Receiver, Env, *Air);
	if (!TestTrue(TEXT("geometric paths available"), Screen.IsSet() && Clear.IsSet() && Reflect.IsSet())) return false;
	constexpr int32 Band = 3;
	const double Wavelength = 331.3 * std::sqrt(293.15 / 273.15) / 500.0;
	const double Excess = 2.0 * std::sqrt(104.0) - 20.0;
	const double TopInsertion = 10.0 * std::log10(3.0 + 20.0 * Excess / Wavelength) + 3.0;
	const double ExpectedScreenDb = -10.0 * std::log10(std::pow(10.0, -TopInsertion / 10.0) + 0.02);
	const double ReflectLength = std::sqrt(800.0);
	const double ExpectedReflectDb = 10.0 * std::log10(1.0 + 0.25 * std::pow(10.0, -Air->AbsorptionDbPerM[Band] * (ReflectLength - 20.0) / 10.0));
	const double ScreenErrorDb = FMath::Abs(Screen->Bands[Band].BarrierDb - ExpectedScreenDb);
	const double ReflectionErrorDb = FMath::Abs(Reflect->Bands[Band].ReflectionGainDb - ExpectedReflectDb);
	AddInfo(FString::Printf(TEXT("N3 single wall 500 Hz diffraction %.6f dB err %.6e dB; reflection gain %.6f dB err %.6e dB"),
		Screen->Bands[Band].BarrierDb, ScreenErrorDb, Reflect->Bands[Band].ReflectionGainDb, ReflectionErrorDb));
	TestTrue(TEXT("screen and reflection agree with the independent symmetric-path solution"), ScreenErrorDb < 1e-12 && ReflectionErrorDb < 1e-12);
	TestTrue(TEXT("opaque wall blocks line of sight and gives more loss at higher frequencies"),
		Screen->BlockingScreens == 1 && Screen->Bands[Band].BarrierDb > Screen->Bands[0].BarrierDb);
	TestTrue(TEXT("ray over the screen and reflection from the same side are distinct"),
		Clear->BlockingScreens == 0 && Clear->Bands[Band].BarrierDb == 0.0 && Reflect->BlockingScreens == 0
		&& Reflect->ReflectedPaths == 1 && Reflect->Bands[Band].ReflectionGainDb > 0.0);
	Env.Screens.Add({FVector2D(-20, 5), FVector2D(40, 5), 10.0, 0.0});
	const auto OccludedReflection = OutdoorPath(Source, Receiver, Env, *Air);
	TestTrue(TEXT("another wall occludes the bounce legs"), OccludedReflection.IsSet() && OccludedReflection->ReflectedPaths == 0);
	TestFalse(TEXT("source below configured ground rejected"), OutdoorPath(FVector(0, 0, -1), Receiver, Env, *Air).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOutdoorBoundaryTest, "VRG.Noise.Propagation.BoundaryRuntime", PropagationFlags)
bool FNoiseOutdoorBoundaryTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	TArray<FVector2D> Sampled;
	TestTrue(TEXT("closed boundary retains corners, subdivisions and one copy of the origin"),
		SampleOutdoorBoundary({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}, 6.0, Sampled)
		&& Sampled.Num() == 8 && Sampled[1] == FVector2D(5, 0) && Sampled.Last() == FVector2D(0, 5));
	TestTrue(TEXT("out-of-budget input leaves last valid geometry intact"),
		!SampleOutdoorBoundary({{0, 0}, {100, 0}}, 1.0, Sampled) && Sampled.Num() == 8);

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
	Sim->bShowSummary = false;
	Sim->bShowPlant = false;
	Sim->bFloorDrains = false;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	Sim->PlantThermalSpec.MinFanRatio = 0.35;
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	for (const double Y : {0.0, 66.0})
	{
		auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset();
		Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	const auto Kind = EPlantUnitKind::FwsDryCooler;
	if (!TestTrue(TEXT("placed source spawned"), VrgPlantEquipment::Spawn(World, { TEXT("boundary/unit"), FName(TEXT("BoundaryUnit")),
		FTransform(FVector(2000, 0, 0)), Kind, APlantEquipmentActor::DefaultSizeM(Kind) }) != nullptr)) return false;

	auto Command = [World](const TCHAR* Text) { return IConsoleManager::Get().ProcessUserConsoleInput(Text, *GLog, World); };
	TestTrue(TEXT("boundary and environment consoles registered"), Command(TEXT("vrg.Noise.Boundary 10 2 45 -10 45 10"))
		&& Command(TEXT("vrg.Noise.Environment 0 0 70 101.325")));
	Sim->EnterSimulationMode();
	Sim->Tick(.021f);
	const auto Baseline = Sim->GetOutdoorBoundarySnapshot();
	if (!TestTrue(TEXT("boundary-only runtime samples all three positions with no N2 receiver"),
		Baseline.IsSet() && Baseline->Receivers.Num() == 3 && Baseline->Receivers[1].PositionM == FVector(45, 0, 2)
		&& Baseline->Receivers[1].Pressure.Db.IsSet() && Baseline->Receivers[1].Pressure.bEstimated
		&& Sim->GetOutdoorNoiseHistory().IsEmpty())) return false;
	TestTrue(TEXT("all runtime sources counted once at midpoint"), Baseline->Receivers[1].Pressure.KnownSources
		== Sim->GetFacilityPlant().GetNumDryCoolers() + Sim->GetFacilityPlant().GetNumCwDryCoolers());

	Command(TEXT("vrg.Noise.Boundary 0.01 2 45 -10 45 10"));
	Command(TEXT("vrg.Noise.Environment 0 2 70 101.325"));
	TestTrue(TEXT("invalid console changes leave existing results, geometry and environment"), Sim->GetOutdoorBoundarySnapshot().IsSet()
		&& Sim->GetOutdoorBoundarySnapshot()->Receivers.Num() == 3 && Sim->GetOutdoorNoiseEnvironment().GroundFactor == 0.0);
	TestTrue(TEXT("barrier console accepts screen in world metres"), Command(TEXT("vrg.Noise.Barrier add 35 -40 35 40 6")));
	Command(TEXT("vrg.Noise.Probe 45 0 2"));
	Sim->Tick(.021f);
	const auto Blocked = Sim->GetOutdoorBoundarySnapshot();
	const auto N2 = Sim->GetOutdoorNoiseHistory();
	const auto* Path = Sim->GetOutdoorBoundaryPath(1, 0);
	if (!TestTrue(TEXT("screen is tracked on source path and N2 records concurrently"),
		Blocked.IsSet() && Blocked->Receivers[1].Pressure.Db.IsSet() && Path && Path->Loss.IsSet() && Path->Loss->BlockingScreens == 1
		&& !N2.IsEmpty())) return false;
	const double ScreenDiffDb = *Baseline->Receivers[1].Pressure.Db - *Blocked->Receivers[1].Pressure.Db;
	const double ClockErrorS = FMath::Abs(Blocked->SimTimeS - N2.Last().SimTimeS);
	const double BeforeAirDb = Path->Loss->Bands[7].AtmosphereDb;
	TestTrue(TEXT("same receiver at same height is quieter when screened"), ScreenDiffDb > 0.0);
	TestTrue(TEXT("term and receiver diagnostic consoles registered"), Command(TEXT("vrg.Noise.Path 1 0")) && Command(TEXT("vrg.Noise.BoundaryStatus 1")));
	Sim->GetMutableFacilityPlant().SetAmbientK(303.15);
	Sim->Tick(.021f);
	const auto* Updated = Sim->GetOutdoorBoundaryPath(1, 0);
	TestTrue(TEXT("live thermal ambient invalidates path attenuation"), Updated && Updated->Loss.IsSet()
		&& Updated->Loss->Bands[7].AtmosphereDb != BeforeAirDb && Sim->GetOutdoorBoundarySnapshot()->TemperatureK == 303.15);
	AddInfo(FString::Printf(TEXT("N3 boundary runtime middle source sum %.6f -> %.6f dBA, screen %.6f dB, N2/N3 time diff %.6e s"),
		*Baseline->Receivers[1].Pressure.Db, *Blocked->Receivers[1].Pressure.Db, ScreenDiffDb, ClockErrorS));
	TestTrue(TEXT("N2 probe and N3 boundary sample the same step"), ClockErrorS == 0.0);

	const FString AllCw = FString::FromInt(Sim->GetFacilityPlant().GetNumCwDryCoolers());
	TestTrue(TEXT("plant fault shuts off both banks"), Sim->ApplyPlantFault({TEXT("drycooler"), TEXT("fws"), TEXT("1")})
		&& Sim->ApplyPlantFault({TEXT("drycooler"), TEXT("cw"), AllCw}));
	Sim->Tick(.021f);
	TestTrue(TEXT("fully stopped dry coolers give silent boundary receivers"), Sim->GetOutdoorBoundarySnapshot()->Receivers[1].Pressure.State == ELevelState::Silent);
	Command(TEXT("vrg.Noise.Boundary clear"));
	Sim->Tick(.021f);
	TestTrue(TEXT("clear removes boundary while N2 continues"), !Sim->GetOutdoorBoundarySnapshot().IsSet() && Sim->GetOutdoorNoiseHistory().Num() > N2.Num());
	Sim->EnterLayoutMode();
	return true;
}

#endif
