// Fill out your copyright notice in the Description page of Project Settings.

// S8 F2 (Tools/설비냉각_계획.md, 2026-10-20): dry cooler epsilon-NTU against its catalog rating, the FWS water
// volumes' ledger, steady states against closed forms (controlled and fan-saturated), monotonic response to the
// outdoor air, and the coupled room: CDU duty in = plant duty, plant ledger, outdoor air -> rack coolant.

#include "Misc/AutomationTest.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgPlantThermalFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double TestRhoKgM3 = 997.0, TestCpJKgK = 4180.0;

	// One CDU branch carrying an 0.8 MW design duty at a 10 K rise (synthetic).
	FFacilityPlant MakePlant(double AmbientC, double TransitS = 60.0)
	{
		FFacilityLoopSpec Loop;
		const double DesignHeatW = 0.8e6;
		Loop.BranchDesignFlowM3s = { DesignHeatW / (TestCpJKgK * 10.0) / TestRhoKgM3 };
		FFacilityThermalSpec Th;
		Th.AmbientK = AmbientC + 273.15;
		Th.SupplySideTransitS = Th.ReturnSideTransitS = TransitS;
		FFacilityPlant Plant;
		Plant.Build(Loop, Th, DesignHeatW, TestRhoKgM3, TestCpJKgK, FHydraulicSolveSettings());
		return Plant;
	}

	void RunConstant(FFacilityPlant& Plant, double DutyW, double Seconds, double Dt)
	{
		const TArray<double> Duty = { DutyW };
		for (int32 I = 0; I < FMath::RoundToInt32(Seconds / Dt); ++I) Plant.Step(Dt, Duty);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDryCoolerCatalogTest, "VRG.Plant.DryCooler.CatalogRoundTrip", VrgPlantThermalFlags)
bool FDryCoolerCatalogTest::RunTest(const FString& Parameters)
{
	const FDryCoolerCatalog C;
	FDryCoolerParams P;
	TestTrue(TEXT("UA derived from the Liebert 940 rating"), DryCooler::FromCatalog(C, P));
	const double FluidKgs = C.RatedFluidFlowM3s * C.RatedFluidDensityKgM3;
	const double Heat = DryCooler::HeatW(P, 1.0, FluidKgs, C.RatedFluidCpJKgK, C.RatedAirInK + C.RatedItdK, C.RatedAirInK);
	const double Rel = FMath::Abs(Heat - C.RatedHeatW) / C.RatedHeatW;
	const double AirWK = C.RatedAirFlowM3s * DryCooler::AirDensityKgM3(C.RatedAirInK) * DryCooler::AirCpJKgK;
	AddInfo(FString::Printf(TEXT("Liebert 940: UA %.1f kW/K (NTU %.3f), 공기 C %.2f kW/K, 정격 재현 %.6f kW (상대 %.3e), 팬 전기 %.2f kW"),
		P.RatedUAWK * 1e-3, P.RatedUAWK / AirWK, AirWK * 1e-3, Heat * 1e-3, Rel, P.RatedFanElectricW * 1e-3));
	TestTrue(TEXT("rating point reproduced (rel < 1e-9)"), Rel < 1e-9);
	// Limits and signs of the crossflow formula.
	TestTrue(TEXT("Cr -> 0 gives 1 - exp(-NTU)"), FMath::Abs(DryCooler::CrossflowUnmixedEffectiveness(2.0, 0.0) - (1.0 - FMath::Exp(-2.0))) < 1e-15);
	TestTrue(TEXT("effectiveness rises with NTU and stays below 1"), DryCooler::CrossflowUnmixedEffectiveness(1.0, .5) < DryCooler::CrossflowUnmixedEffectiveness(2.0, .5)
		&& DryCooler::CrossflowUnmixedEffectiveness(50.0, .5) < 1.0);
	TestTrue(TEXT("warmer air heats the water (negative), no water flow exchanges nothing (D2)"),
		DryCooler::HeatW(P, 1.0, 10.0, 4180.0, 300.0, 310.0) < 0.0 && DryCooler::HeatW(P, 1.0, 0.0, 4180.0, 320.0, 300.0) == 0.0);
	TestTrue(TEXT("fan input at the operating point of the same fan: r^3"), FMath::Abs(DryCooler::FanElectricW(P, 0.5) - P.RatedFanElectricW / 8.0) <= 1e-12 * P.RatedFanElectricW);
	TestTrue(TEXT("less fan speed, less heat"), DryCooler::HeatW(P, 0.5, FluidKgs, 3580.0, 330.0, 300.0) < DryCooler::HeatW(P, 1.0, FluidKgs, 3580.0, 330.0, 300.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsThermalControlledTest, "VRG.Plant.FwsThermal.ControlledSteadyAndLedger", VrgPlantThermalFlags)
bool FFwsThermalControlledTest::RunTest(const FString& Parameters)
{
	// Cool outdoor air (15 C): the fans can hold the 32 C setpoint, so the steady state is the setpoint itself.
	FFacilityPlant Plant = MakePlant(15.0);
	TestTrue(TEXT("built, thermal active"), Plant.IsBuilt() && Plant.IsThermalActive());
	const double Duty = Plant.GetDesignHeatW();
	RunConstant(Plant, Duty, 3000.0, 0.5);
	const FFacilityPlantState& S = Plant.GetState();
	const double WaterWK = Plant.GetTotalMassFlowKgs() * TestCpJKgK;
	const double SupplyErrK = FMath::Abs(S.SupplyK - Plant.GetThermalSpec().SupplySetpointK);
	const double ReturnErrK = FMath::Abs(S.ReturnK - (S.SupplyK + Duty / WaterWK));
	const double HeatRel = FMath::Abs(S.DryCoolerHeatW - Duty) / Duty;
	AddInfo(FString::Printf(TEXT("외기 15 C, 듀티 %.0f kW, 드라이쿨러 %d대: 공급 %.6f C(설정 32) 회수 %.6f C, 팬 %.4f, 방열 %.3f kW, 장부 %.3e, 미달 %d, 과냉 %d"),
		Duty * 1e-3, Plant.GetNumDryCoolers(), S.SupplyK - 273.15, S.ReturnK - 273.15, S.FanRatio, S.DryCoolerHeatW * 1e-3, Plant.GetLedgerResidual(),
		S.bSetpointNotMet ? 1 : 0, S.bOverCooling ? 1 : 0));
	TestTrue(TEXT("plant ledger closes: duty in = dry cooler out + storage (rel < 1e-12)"), FMath::Abs(Plant.GetLedgerResidual()) < 1e-12);
	TestTrue(TEXT("controlled: supply at the setpoint (< 1e-6 K), fans between min and full"), SupplyErrK < 1e-6 && S.FanRatio > Plant.GetThermalSpec().MinFanRatio && S.FanRatio < 1.0 && !S.bSetpointNotMet);
	TestTrue(TEXT("steady: return = supply + duty / (m cp) (< 1e-6 K), dry coolers reject the duty (rel < 1e-6)"), ReturnErrK < 1e-6 && HeatRel < 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsThermalSaturatedTest, "VRG.Plant.FwsThermal.SaturatedSteadyAnalytic", VrgPlantThermalFlags)
bool FFwsThermalSaturatedTest::RunTest(const FString& Parameters)
{
	// Design outdoor air (35 C) above the 32 C setpoint: fans at full speed, setpoint not met. Steady state:
	// N * HeatW(1, m/N, T_r, T_amb) = Duty  ->  T_r by bisection from the model's own formula; T_s = T_r - Duty/(m cp).
	TArray<double> SteadySupply;
	for (const double AmbientC : { 30.0, 35.0, 40.0 })
	{
		FFacilityPlant Plant = MakePlant(AmbientC);
		const double Duty = Plant.GetDesignHeatW();
		RunConstant(Plant, Duty, 4000.0, 0.5);
		const FFacilityPlantState& S = Plant.GetState();
		const double WaterWK = Plant.GetTotalMassFlowKgs() * TestCpJKgK;
		double Lo = Plant.GetAmbientK(), Hi = Plant.GetAmbientK() + 200.0;
		for (int32 I = 0; I < 200; ++I) { const double Mid = 0.5 * (Lo + Hi); (Plant.GetDryCoolerHeatW(1.0, Mid) < Duty ? Lo : Hi) = Mid; }
		const double ReturnK = 0.5 * (Lo + Hi), SupplyK = ReturnK - Duty / WaterWK;
		AddInfo(FString::Printf(TEXT("외기 %.0f C: 공급 %.6f C(해석 %.6f), 회수 %.6f C(해석 %.6f), 팬 %.3f, 미달 %d(누적 %.0f s), 장부 %.3e"),
			AmbientC, S.SupplyK - 273.15, SupplyK - 273.15, S.ReturnK - 273.15, ReturnK - 273.15, S.FanRatio, S.bSetpointNotMet ? 1 : 0, S.SetpointNotMetS, Plant.GetLedgerResidual()));
		if (AmbientC >= 35.0)
		{
			TestTrue(FString::Printf(TEXT("%.0f C: fans at full speed, setpoint not met"), AmbientC), S.FanRatio == 1.0 && S.bSetpointNotMet);
			TestTrue(FString::Printf(TEXT("%.0f C: steady state = closed form (< 1e-6 K)"), AmbientC),
				FMath::Abs(S.ReturnK - ReturnK) < 1e-6 && FMath::Abs(S.SupplyK - SupplyK) < 1e-6);
		}
		TestTrue(FString::Printf(TEXT("%.0f C: ledger (rel < 1e-12)"), AmbientC), FMath::Abs(Plant.GetLedgerResidual()) < 1e-12);
		SteadySupply.Add(S.SupplyK);
	}
	TestTrue(TEXT("warmer outdoor air never gives a colder supply (30 <= 35 < 40 C)"), SteadySupply[0] <= SteadySupply[1] + 1e-9 && SteadySupply[1] < SteadySupply[2]);
	return true;
}

// Coupled room: the plant's duty is the coupling's CDU duty (rule 6), the plant ledger closes, the global ledger is
// untouched (its boundary is still the CDU until F5), and warmer outdoor air reaches the rack coolant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsThermalCoupledTest, "VRG.Plant.FwsThermal.CoupledRoom", VrgPlantThermalFlags)
bool FFwsThermalCoupledTest::RunTest(const FString& Parameters)
{
	struct FOut { double DutyGap = 0.0, Ledger = 0.0, Global = 0.0, InletK = 0.0, SupplyK = 0.0, HxInK = 0.0; bool bActive = false; };
	auto RunOnce = [](bool bPlant, double AmbientC)
	{
		FOut O;
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false;
		Sim->MaxStepsPerFrame = 16;
		Sim->bFacilityPlant = bPlant;
		Sim->PlantThermalSpec.AmbientK = AmbientC + 273.15;
		Sim->PlantThermalSpec.SupplySideTransitS = Sim->PlantThermalSpec.ReturnSideTransitS = 5.0;   // short, so 20 s shows the trend
		ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
		World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
		for (const double X : { 0.0, 400.0 })
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(X, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
		Room->RebuildRoomNow();
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 200; ++F) Sim->Tick(.1f);   // 20 s
		const FFacilityPlant& Plant = Sim->GetFacilityPlant();
		double DutySum = 0.0;
		for (const double D : Sim->GetCouplingBridge().GetLastCduDutyW()) DutySum += D;
		O.bActive = Sim->IsFacilityPlantActive() && Plant.IsThermalActive();
		O.DutyGap = O.bActive ? FMath::Abs(Plant.GetState().DutyW - DutySum) : 0.0;
		O.Ledger = O.bActive ? Plant.GetLedgerResidual() : 0.0;
		O.Global = Sim->GetSnapshot().NormalizedEnergyResidual;
		O.InletK = Room->GetRoutedRacks()[0]->Input.CoolantInletTempK;
		O.SupplyK = O.bActive ? Plant.GetState().SupplyK : 0.0;
		O.HxInK = Sim->GetCouplingBridge().GetCduGroups()[0].Hx.FacilitySupplyTempK;
		Sim->EnterLayoutMode();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return O;
	};
	const FOut Fixed = RunOnce(false, 35.0), Cool = RunOnce(true, 20.0), Hot = RunOnce(true, 40.0);
	AddInfo(FString::Printf(TEXT("20 s: 고정 22 C 경계 랙 냉각수 입구 %.3f C | 설비 외기 20 C: 공급 %.3f C, 랙 입구 %.3f C, 장부 %.3e, 듀티 차 %.3e W, 전역 %.3e | 외기 40 C: 공급 %.3f C, 랙 입구 %.3f C, 장부 %.3e, 전역 %.3e"),
		Fixed.InletK - 273.15, Cool.SupplyK - 273.15, Cool.InletK - 273.15, Cool.Ledger, Cool.DutyGap, Cool.Global, Hot.SupplyK - 273.15, Hot.InletK - 273.15, Hot.Ledger, Hot.Global));
	TestTrue(TEXT("plant active in both plant runs"), Cool.bActive && Hot.bActive);
	TestTrue(TEXT("plant duty = the coupling's CDU duty (rule 6, exact)"), Cool.DutyGap == 0.0 && Hot.DutyGap == 0.0);
	TestTrue(TEXT("plant ledger (rel < 1e-12)"), FMath::Abs(Cool.Ledger) < 1e-12 && FMath::Abs(Hot.Ledger) < 1e-12);
	TestTrue(TEXT("global ledger unchanged in kind (|residual| < 1e-9)"), FMath::Abs(Cool.Global) < 1e-9 && FMath::Abs(Hot.Global) < 1e-9 && FMath::Abs(Fixed.Global) < 1e-9);
	TestTrue(TEXT("warmer outdoor air -> warmer plant supply -> warmer rack coolant"), Hot.SupplyK > Cool.SupplyK && Hot.InletK > Cool.InletK);
	// Plausibility net: W32 plant (32 C) runs the rack coolant warmer than the old 22 C boundary, and below the
	// GB200 NVL72 maximum liquid inlet of 45 C (QCT white paper) while the dry coolers hold the setpoint.
	TestTrue(TEXT("W32 coolant warmer than the 22 C boundary, below 45 C with cool air"), Cool.InletK > Fixed.InletK && Cool.InletK < 318.15);
	return true;
}

#endif
