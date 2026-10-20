// Fill out your copyright notice in the Description page of Project Settings.

// S8 F1 (Tools/설비냉각_계획.md, 2026-10-20): facility water (FWS) hydraulics. Analytic checks of the builder's
// sizing and of the differential-pressure control, mass conservation, valve monotonicity, pump N+1 switchover, and
// the coupling regression: the plant at its design point hands every CDU exchanger its old design flow.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/FacilityWaterLoop.h"
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
	constexpr EAutomationTestFlags VrgPlantTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// Four CDU branches of unequal design flow (synthetic, of the order of one NVL72 row each: ~0.8 MW / (cp 10 K)).
	FFacilityLoopSpec FourBranchSpec()
	{
		FFacilityLoopSpec Spec;
		Spec.BranchDesignFlowM3s = { 0.0195, 0.0180, 0.0210, 0.0150 };
		return Spec;
	}

	double MaxRelImbalance(const FHydraulicNetwork& Net, double RefFlow)
	{
		double Worst = 0.0;
		for (int32 N = 0; N < Net.GetNumNodes(); ++N) Worst = FMath::Max(Worst, FMath::Abs(Net.GetNodeImbalanceM3s(N)) / RefFlow);
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsDesignPointTest, "VRG.Plant.Fws.DesignPointExact", VrgPlantTestFlags)
bool FFwsDesignPointTest::RunTest(const FString& Parameters)
{
	FFacilityWaterLoop Loop;
	const FFacilityLoopSpec Spec = FourBranchSpec();
	TestTrue(TEXT("built"), Loop.Build(Spec));
	const FFacilityLoopResult R = Loop.Solve(FHydraulicSolveSettings());
	double WorstBranch = 0.0, Total = 0.0;
	for (int32 G = 0; G < Spec.BranchDesignFlowM3s.Num(); ++G)
	{
		WorstBranch = FMath::Max(WorstBranch, FMath::Abs(Loop.GetBranchFlowM3s(G) - Spec.BranchDesignFlowM3s[G]) / Spec.BranchDesignFlowM3s[G]);
		Total += Loop.GetBranchFlowM3s(G);
	}
	const FHydraulicNetwork& Net = Loop.GetNetwork();
	const FFacilityLoopHandles& H = Loop.GetHandles();
	const double Q0 = Net.GetPumpFlowM3s(H.Pumps[0]), Q1 = Net.GetPumpFlowM3s(H.Pumps[1]), QStandby = Net.GetPumpFlowM3s(H.Pumps[2]);
	double DryTotal = 0.0;
	for (const int32 P : H.ReturnCoilPipes) DryTotal += Net.GetPipeFlowM3s(P);
	const double Imbalance = MaxRelImbalance(Net, Loop.GetDesignTotalFlowM3s());
	AddInfo(FString::Printf(TEXT("설계점: 속도 %.12f, 차압 %.3f kPa(설정 %.3f), 분기 최대 상대 오차 %.3e, 펌프 %.5f / %.5f / 대기 %.3e L/s, 드라이쿨러 %d대, 양정 %.1f kPa, 전력 %.2f kW, 노드 수지 %.3e, 제어 %d회"),
		R.PumpSpeedRatio, R.HeaderDeltaPPa * 1e-3, R.SetpointPa * 1e-3, WorstBranch, Q0 * 1e3, Q1 * 1e3, QStandby, H.ReturnCoilPipes.Num(),
		Loop.GetDesignPumpHeadPa() * 1e-3, Loop.GetTotalPumpElectricW() * 1e-3, Imbalance, R.ControlIterations));
	TestTrue(TEXT("control converged (D5)"), R.bControlConverged && R.Hydraulics.bConverged && !R.bSpeedSaturatedHigh && !R.bSpeedSaturatedLow);
	TestTrue(TEXT("design point is speed 1 (rel < 1e-9)"), FMath::Abs(R.PumpSpeedRatio - 1.0) < 1e-9);
	TestTrue(TEXT("every branch carries its design flow (rel < 1e-9)"), WorstBranch < 1e-9);
	TestTrue(TEXT("duty pumps share equally, standby carries exactly 0"), FMath::Abs(Q0 - Q1) <= 1e-12 * Q0 && QStandby == 0.0
		&& Net.GetPipeFlowM3s(H.PumpCheckPipes[2]) == 0.0);
	TestTrue(TEXT("mass: pumps = branches = dry coolers (rel < 1e-12), node balance < 1e-12"),
		FMath::Abs(Loop.GetTotalPumpFlowM3s() - Total) <= 1e-12 * Total && FMath::Abs(DryTotal - Total) <= 1e-12 * Total && Imbalance < 1e-12);
	// Plausibility net (not a physical law): FWS pump electric input per flow against the 90.1 Appendix G baseline
	// chilled water figure 22 W/gpm (secondary source, Tools/설비냉각_계획.md); accept a factor of 3 either way.
	const double Gpm = Loop.GetTotalPumpFlowM3s() * 15850.32;
	const double WPerGpm = Loop.GetTotalPumpElectricW() / Gpm;
	AddInfo(FString::Printf(TEXT("타당성: 펌프 %.2f W/gpm (90.1 기준 냉수 22 W/gpm, 2차 자료)"), WPerGpm));
	TestTrue(TEXT("pump power per flow within 1/3..3x of 22 W/gpm"), WPerGpm > 22.0 / 3.0 && WPerGpm < 66.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsControlAnalyticTest, "VRG.Plant.Fws.ControlledBranchesAnalytic", VrgPlantTestFlags)
bool FFwsControlAnalyticTest::RunTest(const FString& Parameters)
{
	FFacilityWaterLoop Loop;
	TestTrue(TEXT("built"), Loop.Build(FourBranchSpec()));
	const TArray<double> Openings = { 1.0, 0.7, 0.4, 0.0 };
	for (int32 G = 0; G < 4; ++G) Loop.SetValveOpening(G, Openings[G]);
	const FFacilityLoopResult R = Loop.Solve(FHydraulicSolveSettings());
	// With the header differential held, the branches are independent parallel paths: Q = sqrt(dp / R_branch(x)).
	double Worst = 0.0;
	for (int32 G = 0; G < 3; ++G)
	{
		const double Expected = FMath::Sqrt(R.HeaderDeltaPPa / Loop.BranchResistancePaS2M6(G, Openings[G]));
		Worst = FMath::Max(Worst, FMath::Abs(Loop.GetBranchFlowM3s(G) - Expected) / Expected);
	}
	// Equal percentage: R(x) / R(1) = Rv^(2 (1 - x)).
	const double Rv = Loop.GetSpec().ValveRangeability;
	const double CharErr = FMath::Abs(Loop.ValveResistancePaS2M6(1, 0.7) / Loop.ValveResistancePaS2M6(1, 1.0) - FMath::Pow(Rv, 0.6)) / FMath::Pow(Rv, 0.6);
	AddInfo(FString::Printf(TEXT("열림 1/0.7/0.4/0: 유량 %.5f / %.5f / %.5f / %.3e L/s, 닫힌 해 대비 최대 %.3e, 속도 %.6f, 특성 오차 %.3e"),
		Loop.GetBranchFlowM3s(0) * 1e3, Loop.GetBranchFlowM3s(1) * 1e3, Loop.GetBranchFlowM3s(2) * 1e3, Loop.GetBranchFlowM3s(3), Worst, R.PumpSpeedRatio, CharErr));
	TestTrue(TEXT("controlled"), R.bControlConverged);
	TestTrue(TEXT("branch flows = sqrt(dp / R_branch) (rel < 1e-9)"), Worst < 1e-9);
	TestTrue(TEXT("shut valve carries exactly 0"), Loop.GetBranchFlowM3s(3) == 0.0);
	TestTrue(TEXT("equal-percentage characteristic (rel < 1e-12)"), CharErr < 1e-12);
	TestTrue(TEXT("less total flow -> lower pump speed"), R.PumpSpeedRatio < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsValveMonotonicTest, "VRG.Plant.Fws.ValveMonotonic", VrgPlantTestFlags)
bool FFwsValveMonotonicTest::RunTest(const FString& Parameters)
{
	FFacilityWaterLoop Loop;
	TestTrue(TEXT("built"), Loop.Build(FourBranchSpec()));
	Loop.Solve(FHydraulicSolveSettings());
	const double Other0 = Loop.GetBranchFlowM3s(1);
	double Prev = -1.0, WorstOther = 0.0;
	bool bMonotonic = true, bControlled = true;
	for (int32 Step = 1; Step <= 20; ++Step)
	{
		Loop.SetValveOpening(0, 0.05 * Step);
		const FFacilityLoopResult R = Loop.Solve(FHydraulicSolveSettings());
		bControlled &= R.bControlConverged;
		const double Q = Loop.GetBranchFlowM3s(0);
		bMonotonic &= Q > Prev;
		Prev = Q;
		WorstOther = FMath::Max(WorstOther, FMath::Abs(Loop.GetBranchFlowM3s(1) - Other0) / Other0);
	}
	AddInfo(FString::Printf(TEXT("열림 0.05~1: 분기 0 유량 단조 %d, 다른 분기 변화 최대 %.3e (헤더 차압 제어)"), bMonotonic ? 1 : 0, WorstOther));
	TestTrue(TEXT("every step controlled"), bControlled);
	TestTrue(TEXT("flow rises strictly with the opening"), bMonotonic);
	TestTrue(TEXT("other branches keep their flow under header dp control (rel < 1e-9)"), WorstOther < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsPumpFailoverTest, "VRG.Plant.Fws.PumpFailover", VrgPlantTestFlags)
bool FFwsPumpFailoverTest::RunTest(const FString& Parameters)
{
	FFacilityWaterLoop Loop;
	const FFacilityLoopSpec Spec = FourBranchSpec();
	TestTrue(TEXT("built"), Loop.Build(Spec));
	const FFacilityLoopResult Base = Loop.Solve(FHydraulicSolveSettings());
	const double Flow0 = Loop.GetBranchFlowM3s(0);
	// Duty pump 0 fails, the standby starts: identical pumps, so the same operating point.
	Loop.SetPumpRunning(0, false);
	Loop.SetPumpRunning(2, true);
	const FFacilityLoopResult Swap = Loop.Solve(FHydraulicSolveSettings());
	const FHydraulicNetwork& Net = Loop.GetNetwork();
	const FFacilityLoopHandles& H = Loop.GetHandles();
	const double SwapRel = FMath::Abs(Loop.GetBranchFlowM3s(0) - Flow0) / Flow0;
	TestTrue(TEXT("N+1 switchover: same speed and flows (rel < 1e-9), failed pump exactly 0"), Swap.bControlConverged
		&& FMath::Abs(Swap.PumpSpeedRatio - Base.PumpSpeedRatio) < 1e-9 && SwapRel < 1e-9
		&& Net.GetPumpFlowM3s(H.Pumps[0]) == 0.0 && Net.GetPipeFlowM3s(H.PumpCheckPipes[0]) == 0.0);
	// Then the standby fails too: one pump left. It speeds up; if it cannot hold the setpoint it reports saturation.
	Loop.SetPumpRunning(2, false);
	const FFacilityLoopResult One = Loop.Solve(FHydraulicSolveSettings());
	AddInfo(FString::Printf(TEXT("대기 전환 상대 차 %.3e | 펌프 1대: 속도 %.4f, 차압 %.1f / %.1f kPa, 포화 %d, 총 유량 %.3f L/s (설계 %.3f)"),
		SwapRel, One.PumpSpeedRatio, One.HeaderDeltaPPa * 1e-3, One.SetpointPa * 1e-3, One.bSpeedSaturatedHigh ? 1 : 0,
		Loop.GetTotalPumpFlowM3s() * 1e3, Loop.GetDesignTotalFlowM3s() * 1e3));
	TestTrue(TEXT("one pump: either holds the setpoint faster than speed 1, or reports saturation below it"),
		(One.bControlConverged && One.PumpSpeedRatio > 1.0) || (One.bSpeedSaturatedHigh && One.HeaderDeltaPPa < One.SetpointPa));
	TestTrue(TEXT("saturated result is not reported as converged"), !(One.bSpeedSaturatedHigh && One.bControlConverged));
	// Every pump off: no flow anywhere, flagged.
	Loop.SetPumpRunning(1, false);
	const FFacilityLoopResult None = Loop.Solve(FHydraulicSolveSettings());
	TestTrue(TEXT("no pump running: flagged, branch flow 0"), None.bNoPumpRunning && FMath::Abs(Loop.GetBranchFlowM3s(0)) < 1e-12);
	return true;
}

// Coupling regression: the plant at its design point gives each CDU its old design facility flow, so a run with the
// plant on (hydraulics only, bFixedSupplyTemp) equals the run with fixed flows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFwsCouplingDesignTest, "VRG.Plant.Fws.CouplingAtDesignUnchanged", VrgPlantTestFlags)
bool FFwsCouplingDesignTest::RunTest(const FString& Parameters)
{
	auto RunOnce = [](bool bPlant, TArray<double>& OutFacilityKgs, TArray<double>& OutCoolantK, FFacilityLoopResult& OutPlant)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false;
		Sim->MaxStepsPerFrame = 16;
		Sim->bFacilityPlant = bPlant;
		Sim->PlantThermalSpec.bFixedSupplyTemp = true;   // F1 comparison: hydraulics only, the CDU's fixed supply temperature
		ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
		World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
		for (const double X : { 0.0, 400.0 })   // two rows of two racks, one CDU (two exchanger groups)
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(X, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
		Room->RebuildRoomNow();
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
		OutFacilityKgs.Reset();
		for (const FCduCouplingGroup& G : Sim->GetCouplingBridge().GetCduGroups()) OutFacilityKgs.Add(G.Hx.FacilityMassFlowKgs);
		OutCoolantK.Reset();
		for (ARackThermalActor* Rack : Room->GetRoutedRacks()) OutCoolantK.Add(Rack->GetMutableModel().GetState().CoolantTempK);
		OutPlant = Sim->GetFacilityLoopResult();
		Sim->EnterLayoutMode();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	TArray<double> FixedKgs, PlantKgs, FixedK, PlantK;
	FFacilityLoopResult Unused, Plant;
	RunOnce(false, FixedKgs, FixedK, Unused);
	RunOnce(true, PlantKgs, PlantK, Plant);
	double WorstFlow = 0.0, WorstK = 0.0;
	bool bSame = FixedKgs.Num() == PlantKgs.Num() && FixedKgs.Num() == 2 && FixedK.Num() == PlantK.Num() && FixedK.Num() == 4;
	for (int32 I = 0; bSame && I < FixedKgs.Num(); ++I) WorstFlow = FMath::Max(WorstFlow, FMath::Abs(PlantKgs[I] - FixedKgs[I]) / FixedKgs[I]);
	for (int32 I = 0; bSame && I < FixedK.Num(); ++I) WorstK = FMath::Max(WorstK, FMath::Abs(PlantK[I] - FixedK[I]));
	AddInfo(FString::Printf(TEXT("설비 켬/끔 1 s: CDU 설비수 유량 상대 차 최대 %.3e, 랙 냉각수 온도 차 최대 %.3e K, FWS 속도 %.9f, 제어 수렴 %d"),
		WorstFlow, WorstK, Plant.PumpSpeedRatio, Plant.bControlConverged ? 1 : 0));
	TestTrue(TEXT("two exchanger groups, four racks"), bSame);
	TestTrue(TEXT("plant solved and controlled"), Plant.bControlConverged);
	TestTrue(TEXT("CDU facility flows = fixed design flows (rel < 1e-9)"), WorstFlow < 1e-9);
	TestTrue(TEXT("rack coolant temperatures unchanged (< 1e-6 K)"), WorstK < 1e-6);
	return true;
}

#endif
