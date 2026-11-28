#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseOutdoor.h"

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
#include <limits>

namespace
{
	constexpr EAutomationTestFlags NoiseFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOutdoorTransferTest, "VRG.Noise.Outdoor.FreeField", NoiseFlags)
bool FNoiseOutdoorTransferTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const FSourceProfile Profile = Liebert940Standard60Hz();
	const FLevel Power = Evaluate(&Profile, true, 1.0);
	const FVector Origin = FVector::ZeroVector;
	const auto At10 = OutdoorTransferDb({ FVector(6.0, 0.0, 8.0) }, Origin, 5.0);
	const auto At20 = OutdoorTransferDb({ FVector(12.0, 0.0, 16.0) }, Origin, 5.0);
	const auto HalfSpace = OutdoorTransferDb({ FVector(6.0, 0.0, 8.0), EOutdoorRadiation::Hemisphere }, Origin, 5.0);
	if (!TestTrue(TEXT("valid transfer for both distances and radiation models"), At10.IsSet() && At20.IsSet() && HalfSpace.IsSet()))
		return false;
	const FLevel One = OutdoorContribution(Power, At10);
	const FLevel Two = Combine({One, One}, EQuantity::SoundPressure, EWeighting::A);
	if (!TestTrue(TEXT("one and two sources have a finite pressure level"), One.Db.IsSet() && Two.Db.IsSet())) return false;
	const double AbsoluteDb = FMath::Abs(*One.Db - 66.40790135977904);
	const double DistanceDb = FMath::Abs((*At10 - *At20) - 6.020599913279624);
	const double HemisphereDb = FMath::Abs((*HalfSpace - *At10) - 3.010299956639812);
	const double CountDb = FMath::Abs((*Two.Db - *One.Db) - 3.010299956639812);
	AddInfo(FString::Printf(TEXT("N2 analytic absolute error: spherical %.3e dB, double-distance %.3e dB, half-space %.3e dB, double-source %.3e dB"),
		AbsoluteDb, DistanceDb, HemisphereDb, CountDb));
	TestTrue(TEXT("point-transfer and two-source energy sum agree with closed form"),
		FMath::Max(FMath::Max(AbsoluteDb, DistanceDb), FMath::Max(HemisphereDb, CountDb)) < 1e-12);

	const auto Near = OutdoorTransferDb({ Origin }, Origin, 5.0);
	const FLevel Missing = OutdoorContribution(Power, Near);
	const FLevel Silent = OutdoorContribution(Evaluate(&Profile, false, 1.0), Near);
	const FLevel Partial = Combine({One, Missing, Silent}, EQuantity::SoundPressure, EWeighting::A);
	TestTrue(TEXT("near running source is missing, stopped source is silent, and the partial sum keeps counts"),
		!Near.IsSet() && Missing.State == ELevelState::MissingData && !Missing.Db.IsSet() && Silent.State == ELevelState::Silent
		&& Partial.Db == One.Db && Partial.KnownSources == 1 && Partial.MissingSources == 1 && Partial.SilentSources == 1);
	FVector Invalid = FVector::ZeroVector;
	Invalid.X = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("nonfinite receiver rejected"), OutdoorTransferDb({ Invalid }, Origin, 5.0).IsSet());
	TestFalse(TEXT("zero exclusion radius rejected"), OutdoorTransferDb({ Origin }, Origin, 0.0).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOutdoorPlantTest, "VRG.Noise.Outdoor.PlantProbe", NoiseFlags)
bool FNoiseOutdoorPlantTest::RunTest(const FString& Parameters)
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

	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->bShowPlant = false;
	Sim->bFloorDrains = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	Sim->PlantThermalSpec.CwDryCoolers = 2;
	// Keep both banks running during the short cold-start test so the FWS trip removes audible power.
	Sim->PlantThermalSpec.MinFanRatio = 0.35;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	for (const double Y : {0.0, 66.0})
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset();
		Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	const FVector Box = APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::FwsDryCooler);
	const auto* B = VrgPlantEquipment::Spawn(World, { TEXT("noise/b"), FName(TEXT("NoiseFwsB")), FTransform(FVector(1800, -500, 0)), EPlantUnitKind::FwsDryCooler, Box });
	auto* A = VrgPlantEquipment::Spawn(World, { TEXT("noise/a"), FName(TEXT("NoiseFwsA")), FTransform(FVector(2100, -500, 100)), EPlantUnitKind::FwsDryCooler, Box });
	if (!TestTrue(TEXT("both placed units spawned"), A && B)) return false;

	auto Command = [World](const TCHAR* Text) { return IConsoleManager::Get().ProcessUserConsoleInput(Text, *GLog, World); };
	const FVector ProbeM(45, 25, 2);
	TestTrue(TEXT("probe console registered"), Command(TEXT("vrg.Noise.Probe 45 25 2")));
	Sim->EnterSimulationMode();
	for (int32 Frame = 0; Frame < 20; ++Frame) Sim->Tick(.1f);
	const FFacilityPlant& Plant = Sim->GetFacilityPlant();
	const auto Baseline = Sim->GetOutdoorNoiseHistory();
	const auto& Sources = Sim->GetOutdoorNoiseSources();
	if (!TestTrue(TEXT("active FWS and CW mapped 2 units each"), Plant.GetNumDryCoolers() == 2
		&& Plant.GetNumCwDryCoolers() == 2 && Sources.Num() == 4 && !Baseline.IsEmpty() && Baseline.Last().bPlantRunning))
		return false;

	double PositionErrorM = 0.0;
	TSet<FString> Ids;
	for (const auto& Source : Sources)
	{
		Ids.Add(Source.Id);
		TestTrue(TEXT("probe outside exclusion distance"), Source.TransferDb.IsSet());
		if (Source.Loop == EPlantLoopId::Fws)
		{
			const auto* Expected = Source.UnitIndex == 0 ? A : B;
			TestTrue(TEXT("placed identity follows the solved FWS bank order"), Source.bPlaced && Source.Id == TEXT("plant/") + VrgPlantEquipment::Identity(Expected));
			const FVector ExpectedM = Expected->GetActorLocation() * .01 + FVector(0, 0, Box.Z * .5);
			PositionErrorM = FMath::Max(PositionErrorM, FVector::Distance(ExpectedM, Source.CenterM));
		}
		else TestTrue(TEXT("unplaced CW units have stable automatic IDs"), Source.Loop == EPlantLoopId::Cw && !Source.bPlaced
			&& Source.Id == FString::Printf(TEXT("auto/plant/cw/drycooler/%d"), Source.UnitIndex));
	}
	TestTrue(TEXT("one ID per unit and correct placed centre in metres"), Ids.Num() == 4 && PositionErrorM < 1e-12);
	double ErrorDb = 0.0;
	double ClockErrorS = 0.0;
	double SupplyErrorK = 0.0;
	double CoolingErrorW = 0.0;
	auto Verify = [&](const FOutdoorSample& Sample, int32 Running)
	{
		double Energy = 0.0;
		for (const auto& Source : Sources)
		{
			const FPlantUnitTruth Unit = Plant.GetDryCoolerTruth(Source.Loop, Source.UnitIndex);
			if (Unit.bRunning && !Unit.bFailed)
			{
				// Independent linear-space reference for the unit rating, fifth-power speed law and spherical area.
				Energy += std::pow(10.0, 9.74) * std::pow(Unit.FanRatio, 5.0)
					/ (4.0 * UE_DOUBLE_PI * FVector::DistSquared(ProbeM, Source.CenterM));
			}
		}
		const FLevel& Pressure = Sample.Pressure;
		TestTrue(TEXT("operating and silent counts match current plant, with no profile or transfer gaps"),
			Pressure.KnownSources == Running && Pressure.SilentSources == 4 - Running && Pressure.MissingSources == 0 && Sample.NearSources == 0);
		if (Running > 0 && TestTrue(TEXT("running units produce a finite estimated LpA"), Pressure.Db.IsSet() && Pressure.bEstimated && Energy > 0.0))
			ErrorDb = FMath::Max(ErrorDb, FMath::Abs(*Pressure.Db - 10.0 * std::log10(Energy)));
		if (Running == 0)
			TestTrue(TEXT("all stopped is silent with zero fan power and drycooler heat rejection"), Pressure.State == ELevelState::Silent
				&& !Pressure.Db.IsSet() && Sample.DryCoolerFanW == 0.0 && Sample.DryCoolerRejectedW == 0.0);
		const auto& State = Plant.GetState();
		ClockErrorS = FMath::Max(ClockErrorS, FMath::Abs(Sample.SimTimeS - Sim->GetSimClockTimeS()));
		SupplyErrorK = FMath::Max(SupplyErrorK, FMath::Max(FMath::Abs(Sample.FwsSupplyK - State.SupplyK), FMath::Abs(Sample.CwSupplyK - State.CwSupplyK)));
		CoolingErrorW = FMath::Max(CoolingErrorW, FMath::Max(FMath::Abs(Sample.DryCoolerFanW - (Plant.GetFanElectricW() + Plant.GetCwFanElectricW())),
			FMath::Abs(Sample.DryCoolerRejectedW - (State.DryCoolerHeatW + State.CwDryCoolerHeatW))));
	};
	Verify(Baseline.Last(), 4);
	Command(TEXT("vrg.Noise.Probe 45bad 25 2"));
	TestEqual(TEXT("invalid coordinate leaves probe history intact"), Sim->GetOutdoorNoiseHistory().Num(), Baseline.Num());
	TestTrue(TEXT("partial trip applied"), Sim->ApplyPlantFault({ TEXT("drycooler"), TEXT("fws"), TEXT("1") }));
	Sim->QueueWarmupSteps(3);
	for (int32 Frame = 0; Frame < 20 && Sim->GetOutdoorNoiseHistory().Num() < Baseline.Num() + 3; ++Frame) Sim->Tick(0.0f);
	const auto Partial = Sim->GetOutdoorNoiseHistory();
	TestEqual(TEXT("warm-up adds exactly one sample per completed step"), Partial.Num() - Baseline.Num(), 3);
	TestTrue(TEXT("FWS trip selects sorted unit 1, CW remains running"), Plant.GetDryCoolerTruth(EPlantLoopId::Fws, 1).bFailed
		&& !Plant.GetDryCoolerTruth(EPlantLoopId::Fws, 0).bFailed && Plant.GetDryCoolerTruth(EPlantLoopId::Cw, 1).bRunning);
	Verify(Partial.Last(), 3);
	TestTrue(TEXT("both banks tripped"), Sim->ApplyPlantFault({ TEXT("drycooler"), TEXT("fws"), TEXT("2") })
		&& Sim->ApplyPlantFault({ TEXT("drycooler"), TEXT("cw"), TEXT("2") }));
	Sim->Tick(.021f);
	const auto Stopped = Sim->GetOutdoorNoiseHistory();
	TestTrue(TEXT("live tick appended a sample"), Stopped.Num() > Partial.Num());
	Verify(Stopped.Last(), 0);
	double IntervalErrorS = 0.0;
	for (int32 I = 1; I < Stopped.Num(); ++I)
		IntervalErrorS = FMath::Max(IntervalErrorS, FMath::Abs(Stopped[I].SimTimeS - Stopped[I - 1].SimTimeS - Sim->FixedStepS));
	AddInfo(FString::Printf(TEXT("N2 plant FWS/CW 2+2: normal %.6f dBA (%d known), partial %.6f dBA (%d known), stopped %d silent. Errors LpA %.3e dB, position %.3e m, clock %.3e s, interval %.3e s, supply %.3e K, fan/rejection %.3e W"),
		Baseline.Last().Pressure.Db.Get(0.0), Baseline.Last().Pressure.KnownSources, Partial.Last().Pressure.Db.Get(0.0), Partial.Last().Pressure.KnownSources,
		Stopped.Last().Pressure.SilentSources, ErrorDb, PositionErrorM, ClockErrorS, IntervalErrorS, SupplyErrorK, CoolingErrorW));
	TestTrue(TEXT("pressure reference and shared time/cooling values agree"),
		ErrorDb < 1e-12 && ClockErrorS == 0.0 && IntervalErrorS < 1e-12 && SupplyErrorK == 0.0 && CoolingErrorW == 0.0);
	TestTrue(TEXT("status console registered"), Command(TEXT("vrg.Noise.Status 1")));

	Sim->EnterLayoutMode();
	A->SetActorLocation(FVector(2400, -500, 100));
	Sim->EnterSimulationMode();
	TestTrue(TEXT("new run clears history and geometry"), Sim->GetOutdoorNoiseHistory().IsEmpty() && Sim->GetOutdoorNoiseSources().IsEmpty());
	Sim->Tick(.021f);
	const auto* Moved = Sim->GetOutdoorNoiseSources().FindByPredicate([](const auto& Source) { return Source.Id == TEXT("plant/noise/a"); });
	if (!TestTrue(TEXT("saved probe uses the moved unit in the new run"), Moved && Moved->CenterM.X == 24.0)) return false;
	const FVector NearM = Moved->CenterM;
	TestTrue(TEXT("accept a probe at the centre of a source"), Sim->SetOutdoorNoiseReceiver({ NearM }));
	Sim->Tick(.021f);
	const auto NearSamples = Sim->GetOutdoorNoiseHistory();
	TestTrue(TEXT("near operating units report missing data, distinct from silence"),
		!NearSamples.IsEmpty() && NearSamples.Last().NearSources > 0
		&& NearSamples.Last().Pressure.MissingSources == NearSamples.Last().NearSources);

	Sim->EnterLayoutMode();
	Sim->bFacilityPlant = false;
	Sim->EnterSimulationMode();
	Sim->Tick(.021f);
	const auto Inactive = Sim->GetOutdoorNoiseHistory();
	TestTrue(TEXT("uncoupled plant has no operating sources even if yard equipment exists"),
		!Inactive.IsEmpty() && !Inactive.Last().bPlantRunning && Inactive.Last().Pressure.State == ELevelState::Silent
		&& Inactive.Last().Pressure.SilentSources == 0 && Sim->GetOutdoorNoiseSources().IsEmpty());

	Sim->EnterLayoutMode();
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.DryCooler.RatedHeatW *= 1.1;
	Sim->SetOutdoorNoiseReceiver({ ProbeM });
	Sim->EnterSimulationMode();
	Sim->Tick(.021f);
	const auto Unknown = Sim->GetOutdoorNoiseHistory();
	TestTrue(TEXT("different catalog rating leaves sound data unavailable while cooling still runs"),
		!Unknown.IsEmpty() && Unknown.Last().DryCoolerFanW > 0.0 && Unknown.Last().Pressure.State == ELevelState::MissingData
		&& !Unknown.Last().Pressure.Db.IsSet() && Unknown.Last().Pressure.MissingSources == 4);

	Command(TEXT("vrg.Noise.Probe clear"));
	Sim->Tick(.021f);
	TestTrue(TEXT("clear removes the probe and stops recording"), Sim->GetOutdoorNoiseHistory().IsEmpty() && Sim->GetOutdoorNoiseSources().IsEmpty());
	Sim->EnterLayoutMode();
	return true;
}

#endif
