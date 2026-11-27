#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Visualization/GjRackLayout.h"

namespace
{
	struct FNoiseIntegrationFrame
	{
		TArray<double> TempK, PressurePa, FlowM3s, VolumeM3, DepthM, EnergyJ, PowerW, ClockS;
		void ForEachMeasure(TFunctionRef<void(int32, const TArray<double>&)> Visit) const
		{
			Visit(0, TempK); Visit(1, PressurePa); Visit(2, FlowM3s); Visit(3, VolumeM3);
			Visit(4, DepthM); Visit(5, EnergyJ); Visit(6, PowerW); Visit(7, ClockS);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseIntegrationTest, "VRG.Noise.Integration.ScenariosAndPhysicalIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FNoiseIntegrationTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const double PreviousSpeed = ADataCenterSimulationActor::GetRequestedSimSpeed();
	ADataCenterSimulationActor::SetRequestedSimSpeed(1.0);
	ON_SCOPE_EXIT { ADataCenterSimulationActor::SetRequestedSimSpeed(PreviousSpeed); };
	TArray<FNoiseIntegrationFrame> Reference;
	double MaxDelta[8]{};
	double MaxHeatResidual = 0.0, MaxWaterResidual = 0.0, MaxInternalResidualM3 = 0.0;

	for (const bool bNoise : {false, true})
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ON_SCOPE_EXIT
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		};
		auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = Sim->bShowPlant = Sim->bFloorDrains = false;
		Sim->bFacilityPlant = true;
		Sim->PlantThermalSpec.AmbientK = 298.15;
		Sim->PlantThermalSpec.MinFanRatio = 0.35;
		Sim->MaxStepsPerFrame = 1;
		auto* Room = World->SpawnActor<ADataCenterRoomActor>();
		World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
		TArray<ARackThermalActor*> Racks;
		for (const double Y : {0.0, 66.0})
		{
			auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
			Rack->ApplyGb200Nvl72Preset();
			Rack->bShowReadout = false;
			Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			Rack->Input.Utilization = .4;
			Racks.Add(Rack);
		}
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
		Room->RebuildRoomNow();
		const auto SpawnCooler = [World](const TCHAR* Id, EPlantUnitKind Kind, FVector PositionCm)
		{
			return VrgPlantEquipment::Spawn(World, {Id, FName(Id), FTransform(PositionCm), Kind, APlantEquipmentActor::DefaultSizeM(Kind)});
		};
		auto* MovedCooler = SpawnCooler(TEXT("NoiseN6FwsA"), EPlantUnitKind::FwsDryCooler, FVector(2000, 0, 0));
		if (!TestTrue(TEXT("two FWS coolers and one CW cooler are placed"), MovedCooler
			&& SpawnCooler(TEXT("NoiseN6FwsB"), EPlantUnitKind::FwsDryCooler, FVector(2000, 1500, 0))
			&& SpawnCooler(TEXT("NoiseN6Cw"), EPlantUnitKind::CwDryCooler, FVector(1500, -1700, 0)))) return false;
		Sim->EnterLayoutMode();
		FRoomAirDomainSettings Domain;
		Domain.bManual = true;
		Domain.MinXYM = FVector2D(-4, -2);
		Domain.MaxXYM = FVector2D(5, 4);
		Domain.CeilingHeightM = 3.5;
		FString Reason;
		if (!TestTrue(TEXT("manual room fits the fixtures"), Sim->TrySetRoomAirDomain(Domain, Reason))) { AddError(Reason); return false; }

		FNoiseLayout Acoustics;
		Acoustics.Probe = FOutdoorReceiver{FVector(45, 0, 2), EOutdoorRadiation::Sphere};
		Acoustics.Boundary = FBoundaryRequest{{{45, -10}, {45, 10}}, 10.0, 2.0};
		Acoustics.IndoorPositionM = FVector2D(-2, 1);
		Acoustics.CduLwA = 76.0; // Synthetic input for the fixture.
		if (bNoise) Sim->ApplyNoiseLayout(Acoustics);
		Sim->EnterSimulationMode();
		const auto& Plant = Sim->GetFacilityPlant();
		if (!TestTrue(TEXT("facility runs with two physical FWS units"), Sim->IsFacilityPlantActive() && Plant.GetNumDryCoolers() == 2
			&& Plant.GetNumCwDryCoolers() == 1)) return false;
		int32 StepCount = 0;
		bool bIdentical = true;
		bool bAllConverged = true;
		const auto Step = [&](int32 Count)
		{
			for (int32 I = 0; I < Count; ++I)
			{
				const double Before = Sim->GetSimClockTimeS();
				Sim->Tick(.021f);
				const auto S = Sim->GetSnapshot();
				const auto& P = Plant.GetState();
				const auto& Water = Sim->GetRackInteriorCoupling().GetWater();
				const auto& Floor = Sim->GetFloorWater().GetTotals();
				MaxHeatResidual = FMath::Max(MaxHeatResidual, FMath::Abs(S.NormalizedEnergyResidual));
				MaxWaterResidual = FMath::Max(MaxWaterResidual, Floor.Residual);
				MaxInternalResidualM3 = FMath::Max(MaxInternalResidualM3, FMath::Abs(Water.BalanceErrorM3()));
				bAllConverged &= S.SimTimeS > Before && S.bAirPressureConverged && Room->GetLastSolveResult().bConverged;
				FNoiseIntegrationFrame F;
				F.ClockS = {S.SimTimeS};
				F.TempK = S.RackAirInletTempK;
				F.TempK.Append(S.CduSupplyTempK);
				F.TempK.Append({P.SupplyK, P.ReturnK, P.ChwSupplyK, P.CwSupplyK});
				F.FlowM3s.Add(S.CrahFlowM3s);
				F.VolumeM3 = {Water.ReceivedM3, Floor.ReceivedM3, Floor.StoredM3, Floor.DrainedM3, Floor.ClippedM3, Floor.TrayM3};
				F.DepthM = Sim->GetFloorWater().GetDepthM();
				F.EnergyJ = {Sim->GetSiteEnergyInJ(), Sim->GetCouplingBridge().GetEnergyInJ(), S.CoolantRejectedJ, S.CoolantLeakHeatJ,
					P.DutyInJ, P.DryCoolerOutJ, P.CwDryCoolerOutJ, P.CompressorInJ, P.PumpInJ};
				F.PowerW = {S.TotalItHeatW, S.TotalFanPowerW, S.CrahFanPowerW, P.DryCoolerHeatW, P.CwDryCoolerHeatW};
				for (const auto* Rack : Racks) F.TempK.Append(Rack->GetModel().GetState().ComponentTempK);
				for (const auto& Tank : Room->GetCoolantInventory().GetTanks())
					F.VolumeM3.Append({Tank.State.WaterVolumeM3, Tank.State.LeakedM3, Tank.State.MakeupM3});
				for (const auto* Net : {&Room->GetNetwork(), &Plant.GetLoop().GetNetwork(),
					&Plant.GetChwLoop().GetNetwork(), &Plant.GetCwLoop().GetNetwork()})
				{
					for (int32 N = 0; N < Net->GetNumNodes(); ++N) F.PressurePa.Add(Net->GetNodePressurePa(N));
					for (int32 Pipe = 0; Pipe < Net->GetNumPipes(); ++Pipe) F.FlowM3s.Add(Net->GetPipeFlowM3s(Pipe));
				}
				if (!bNoise) Reference.Add(MoveTemp(F));
				else if (Reference.IsValidIndex(StepCount))
				{
					const TArray<double>* Candidate[8]{};
					F.ForEachMeasure([&](int32 Measure, const TArray<double>& Values) { Candidate[Measure] = &Values; });
					Reference[StepCount].ForEachMeasure([&](int32 Measure, const TArray<double>& Values)
					{
						const auto& Other = *Candidate[Measure];
						bIdentical &= Values.Num() == Other.Num();
						for (int32 J = 0; J < FMath::Min(Values.Num(), Other.Num()); ++J)
						{
							const double Delta = FMath::Abs(Values[J] - Other[J]);
							bIdentical &= Delta == 0.0;
							MaxDelta[Measure] = FMath::Max(MaxDelta[Measure], Delta);
						}
					});
				}
				else bIdentical = false;
				++StepCount;
			}
		};
		const auto ReadDb = [Sim](EReceiverSet Set, int32 Index)
		{
			const auto& Group = Sim->GetNoiseReceiverGroup(Set);
			return Group.Tracks.IsValidIndex(Index) ? Group.Tracks[Index].Latest.Pressure.Db : TOptional<double>();
		};

		Step(15);
		const auto Baseline = Sim->GetSnapshot();
		const auto NormalIndoor = ReadDb(EReceiverSet::Indoor, 0);
		const auto NormalOutside = ReadDb(EReceiverSet::Boundary, 1);
		const double NormalSupplyK = Plant.GetState().SupplyK;
		for (auto* Rack : Racks) Rack->Input.Utilization = .95;
		if (!TestTrue(TEXT("cold-plate leak opens"), Room->SetInternalLeakHoleDiameterM(Racks[0], 0, 2, .003))) return false;
		Step(35);
		const auto Loaded = Sim->GetSnapshot();
		const auto LoadedIndoor = ReadDb(EReceiverSet::Indoor, 0);
		const double LeakedM3 = Sim->GetRackInteriorCoupling().GetWater().ReceivedM3;
		TestTrue(TEXT("load and leak change the model while collecting"), Loaded.TotalItHeatW > Baseline.TotalItHeatW && LeakedM3 > 0.0);
		TestTrue(TEXT("repair closes the plate leak"), Room->SetInternalLeakHoleDiameterM(Racks[0], 0, 2, 0.0));
		const int32 BranchLeak = Sim->ResolveLiveLeakSite(TEXT("room:0:supply"));
		if (!TestTrue(TEXT("external 3 mm supply branch leak opens"), Sim->SetLiveLeak(BranchLeak, .003, TEXT("Noise integration fixture"))))
			return false;
		Step(50);
		TestTrue(TEXT("supply branch repair"), Sim->SetLiveLeak(BranchLeak, 0.0, TEXT("Noise integration fixture")));

		TestTrue(TEXT("one FWS unit trips"), Sim->ApplyPlantFault({TEXT("drycooler"), TEXT("fws"), TEXT("1")}));
		Step(15);
		const auto PartialOutside = ReadDb(EReceiverSet::Boundary, 1);
		const auto PartialSnapshot = Sim->GetOutdoorBoundarySnapshot();
		const double PartialSupplyK = Plant.GetState().SupplyK;
		if (bNoise)
			TestTrue(TEXT("remaining FWS and CW sources still contribute"), PartialOutside.IsSet() && PartialSnapshot.IsSet()
				&& PartialSnapshot->Receivers[1].Pressure.KnownSources == 2 && PartialSnapshot->Receivers[1].Pressure.SilentSources == 1);

		bool bReserved = false;
		Sim->StartDemoScenario(TEXT("heatwave"), bReserved);
		if (!TestTrue(TEXT("existing heatwave demo reserves ambient and all-FWS trip"), bReserved)) return false;
		Step(360);
		const auto HotOutside = ReadDb(EReceiverSet::Boundary, 1);
		TestTrue(TEXT("heatwave ran at 35 C and FWS fans rejected zero heat"), Plant.GetAmbientK() == 308.15 && Plant.GetState().DryCoolerHeatW == 0.0);
		const double HotSupplyK = Plant.GetState().SupplyK;

		FOutdoorEnvironment Screened = Acoustics.Environment;
		Screened.Screens.Add({{35, -40}, {35, 40}, 6, 0.0});
		FIndoorAbsorption Absorption;
		for (int32 I = 0; I < IndoorOctaves; ++I) Absorption.Shell[I] = Absorption.Equipment[I] = .5;
		const auto BeforeAbsorption = ReadDb(EReceiverSet::Indoor, 0);
		if (bNoise)
		{
			TestTrue(TEXT("acoustic-only properties apply"), Sim->SetOutdoorNoiseEnvironment(Screened) && Sim->SetIndoorNoiseAbsorption(Absorption));
		}
		Step(1);
		const auto ScreenedOutside = ReadDb(EReceiverSet::Boundary, 1);
		const auto AbsorbedIndoor = ReadDb(EReceiverSet::Indoor, 0);
		if (bNoise)
		{
			if (!TestTrue(TEXT("all demonstration levels are finite"), NormalIndoor.IsSet() && LoadedIndoor.IsSet() && NormalOutside.IsSet()
				&& PartialOutside.IsSet() && HotOutside.IsSet() && ScreenedOutside.IsSet() && BeforeAbsorption.IsSet() && AbsorbedIndoor.IsSet()))
				return false;
			TestTrue(TEXT("screen and higher absorption reduce measured receiver levels"), *ScreenedOutside < *HotOutside && *AbsorbedIndoor < *BeforeAbsorption);
			TestTrue(TEXT("all paired steps match exactly"), bIdentical && StepCount == Reference.Num());
			AddInfo(FString::Printf(TEXT("N6 paired %d steps: max abs %.6e K %.6e Pa %.6e m3/s %.6e m3 %.6e m %.6e J %.6e W %.6e s"),
				StepCount, MaxDelta[0], MaxDelta[1], MaxDelta[2], MaxDelta[3], MaxDelta[4], MaxDelta[5], MaxDelta[6], MaxDelta[7]));
			const double FloorReceivedM3 = Sim->GetFloorWater().GetTotals().ReceivedM3;
			TestTrue(TEXT("the external leak reaches the floor"), FloorReceivedM3 > 0.0);
			AddInfo(FString::Printf(TEXT("N6 residuals: heat %.6e floor %.6e internal %.6e m3; tray input %.6e m3 floor input %.6e m3"),
				MaxHeatResidual, MaxWaterResidual, MaxInternalResidualM3, LeakedM3, FloorReceivedM3));
			AddInfo(FString::Printf(TEXT("N6 demo: load IT %.3f -> %.3f W, rack fans %.3f -> %.3f W, indoor %.6f -> %.6f dBA; boundary normal/partial/hot/screen %.6f %.6f %.6f %.6f dBA; supply %.6f %.6f %.6f K; absorption %.6f -> %.6f dBA"),
				Baseline.TotalItHeatW, Loaded.TotalItHeatW, Baseline.TotalFanPowerW, Loaded.TotalFanPowerW, *NormalIndoor, *LoadedIndoor,
				*NormalOutside, *PartialOutside, *HotOutside, *ScreenedOutside, NormalSupplyK, PartialSupplyK, HotSupplyK, *BeforeAbsorption, *AbsorbedIndoor));

			const auto RestartAtInitialLoad = [&]()
			{
				Sim->EnterLayoutMode();
				Sim->StartDemoScenario(TEXT("clear"), bReserved);
				Sim->PlantThermalSpec.AmbientK = 298.15;
				for (auto* Rack : Racks) Rack->Input.Utilization = .4;
				Sim->ApplyNoiseLayout(Acoustics);
				Sim->EnterSimulationMode();
				Sim->Tick(.021f);
				return ReadDb(EReceiverSet::Boundary, 1);
			};
			const auto Original = RestartAtInitialLoad();
			Sim->EnterLayoutMode();
			MovedCooler->AddActorWorldOffset(FVector(500, 0, 0));
			const auto Moved = RestartAtInitialLoad();
			const auto& Boundary = Sim->GetNoiseReceiverGroup(EReceiverSet::Boundary);
			if (!TestTrue(TEXT("new run samples moved equipment at the same fixed boundary point"), Original.IsSet() && Moved.IsSet()
				&& *Original != *Moved && Boundary.StartS == 0.0 && Boundary.Tracks[1].Latest.PositionM == FVector(45, 0, 2))) return false;
			AddInfo(FString::Printf(TEXT("N6 placement: +5 m toward the fixed receiver %.6f -> %.6f dBA"), *Original, *Moved));
		}
		else TestTrue(TEXT("all acoustic collectors remain disabled"), !NormalIndoor.IsSet() && !NormalOutside.IsSet());
		TestTrue(TEXT("paired fixed steps and air/room pressure converge"), bAllConverged);
		TestTrue(TEXT("existing heat-ledger normalized residual limit"), MaxHeatResidual < 1e-9);
		Sim->EnterLayoutMode();
	}
	return true;
}

#endif
