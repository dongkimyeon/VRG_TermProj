// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Thermal/CduHeatExchanger.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// CDU liquid-to-liquid heat exchanger (2026-09-27): counterflow epsilon-NTU and a row of racks sharing one CDU.

namespace
{
	constexpr EAutomationTestFlags VrgCduTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduEffectivenessTest, "VRG.Thermal.Cdu.EffectivenessAnalytic", VrgCduTestFlags)
bool FCduEffectivenessTest::RunTest(const FString& Parameters)
{
	using namespace CduHeatExchanger;
	// Closed forms: Cr = 0 -> 1 - e^-NTU; Cr = 1 -> NTU / (1 + NTU); Cr = 0.5, NTU = 2 -> (1 - e^-1) / (1 - 0.5 e^-1).
	const double E1 = FMath::Exp(-1.0);
	const double Err0 = FMath::Abs(CounterflowEffectiveness(1.3, 0.0) - (1.0 - FMath::Exp(-1.3)));
	const double Err1 = FMath::Abs(CounterflowEffectiveness(1.0, 1.0) - .5);
	const double ErrH = FMath::Abs(CounterflowEffectiveness(2.0, .5) - (1.0 - E1) / (1.0 - .5 * E1));
	// Inverse used for sizing returns the NTU it was given.
	double MaxInverse = 0.0;
	for (const double Cr : {0.0, .3, .7, 1.0})
		for (const double Ntu : {.2, 1.0, 3.0})
			MaxInverse = FMath::Max(MaxInverse, FMath::Abs(NtuForEffectiveness(CounterflowEffectiveness(Ntu, Cr), Cr) - Ntu) / Ntu);
	AddInfo(FString::Printf(TEXT("eps 해석값 오차: Cr 0 %.1e / Cr 1 %.1e / Cr 0.5 %.1e | NTU 역함수 왕복 최대 상대 오차 %.2e"), Err0, Err1, ErrH, MaxInverse));
	TestTrue(TEXT("Cr = 0 closed form"), Err0 < 1e-15);
	TestTrue(TEXT("Cr = 1 closed form"), Err1 < 1e-15);
	TestTrue(TEXT("Cr = 0.5 closed form"), ErrH < 1e-15);
	TestTrue(TEXT("NTU inverse round trip"), MaxInverse < 1e-9);

	// Duty saturates at C_min * dT; zero flow on either side gives zero duty (D2).
	FCduHeatExchangerParams Hx; Hx.UAWK = 1e12; Hx.FacilitySupplyTempK = 295.15; Hx.FacilityMassFlowKgs = 5.0;
	const double Hot = 3.0 * 4180.0, Limit = FMath::Min(Hot, 5.0 * 4180.0) * 10.0;
	TestTrue(TEXT("Huge UA saturates at C_min dT"), FMath::Abs(DutyW(Hx, Hot, 305.15) - Limit) / Limit < 1e-9);
	TestEqual(TEXT("No hot flow, no duty"), DutyW(Hx, 0.0, 305.15), 0.0);
	Hx.FacilityMassFlowKgs = 0.0;
	TestEqual(TEXT("No facility flow, no duty"), DutyW(Hx, Hot, 305.15), 0.0);
	return true;
}

// Three GB200 racks at different load and flow share one CDU. Every step the duty equals the heat the racks hand
// to the coolant (one mixed return, one supply), the coolant loop closes, the design point lands on the design
// approach, and a starved facility flow caps the duty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduRowTest, "VRG.Coupling.CduRowSharedExchanger", VrgCduTestFlags)
bool FCduRowTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	struct FCase { double Utilization, FlowKgs; };
	const FCase Cases[3] = { {1.0, 2.3}, {.5, 2.0}, {.2, 2.6} };
	constexpr double FacilityK = 295.15, RiseK = 10.0, ApproachK = 4.0, Dt = .02;

	// No air solver here, so the global ledger would lose the racks' air heat; the coolant side is checked directly:
	// cumulative heat the racks hand to the coolant vs cumulative CDU duty (OutLoopErr, normalised by the duty).
	auto Run = [&](double FacilityFlowScale, int32 Steps, FCouplingSnapshot& OutSnap, TArray<FRackThermalInput>& Inputs, double& OutMaxStepErr, double& OutLoopErr)
	{
		TArray<FRackThermalModel> Models; Models.SetNum(3);
		Inputs.SetNum(3);
		TArray<FRadiatorParams> Radiators; Radiators.SetNum(3);
		TArray<FRackCouplingTarget> Targets; TArray<FRackCouplingLink> Links;
		FCduCouplingGroup Group;
		double DesignW = 0.0, HotWK = 0.0;
		for (int32 R = 0; R < 3; ++R)
		{
			FRackThermalState Init; Init.CoolantTempK = 300.15;
			Models[R].Initialize(Params, Init);
			FRackThermalInput& In = Inputs[R];
			In.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW(); In.SledDocked.Init(true, Params.Slots.Num());
			In.CoolantMassFlowKgs = Cases[R].FlowKgs; In.Utilization = Cases[R].Utilization; In.AirInletTempK = 297.15;
			Radiators[R].UAWK = 1.1e4;  // present but unused: the CDU group overrides it
			Targets.Add({&Models[R], &In, &Radiators[R]});
			FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs = Cases[R].FlowKgs; Links.Add(Link);
			Group.RackIndices.Add(R);
			// Sizing as the integrated actor does it: full-load design duty, design flows.
			FRackThermalInput Design = In; Design.Utilization = 1.0;
			DesignW += FRackThermalModel::DesignHeatToCoolantW(Params, Design);
			HotWK += Cases[R].FlowKgs * Params.CoolantCpJKgK;
		}
		FCduHeatExchangerParams& Hx = Group.Hx;
		Hx.FacilitySupplyTempK = FacilityK;
		Hx.FacilityMassFlowKgs = DesignW / (Hx.FacilityCpJKgK * RiseK);
		const double FacWK = Hx.FacilityMassFlowKgs * Hx.FacilityCpJKgK;
		const double CMin = FMath::Min(HotWK, FacWK), CMax = FMath::Max(HotWK, FacWK);
		Hx.UAWK = CduHeatExchanger::NtuForEffectiveness(DesignW / (CMin * (ApproachK + DesignW / HotWK)), CMin / CMax) * CMin;
		Hx.FacilityMassFlowKgs *= FacilityFlowScale;

		FCouplingBridge Bridge;
		Bridge.Initialize(Targets, Links);
		Bridge.SetCduGroups({Group});
		OutMaxStepErr = 0.0;
		double CoolantJ = 0.0, DutyJ = 0.0;
		for (int32 S = 0; S < Steps; ++S)
		{
			Bridge.Step(Dt, false, true);
			const FCouplingSnapshot Snap = Bridge.MakeSnapshot((S + 1) * Dt);
			for (const FRackThermalModel& M : Models) CoolantJ += M.GetLastHeatRemovedByCoolantW() * Dt;
			DutyJ += Snap.CduDutyW[0] * Dt;
			// Rule 6 every step: duty = C_hot (T_mix - T_supply) = the eps-NTU duty at that mix.
			const double Duty = Snap.CduDutyW[0];
			const double Book = HotWK * (Snap.CduReturnTempK[0] - Snap.CduSupplyTempK[0]);
			const double Formula = CduHeatExchanger::DutyW(Hx, HotWK, Snap.CduReturnTempK[0]);
			if (FMath::Abs(Duty) > 0.0) OutMaxStepErr = FMath::Max(OutMaxStepErr, FMath::Max(FMath::Abs(Duty - Book), FMath::Abs(Duty - Formula)) / FMath::Abs(Duty));
			if (S + 1 == Steps) OutSnap = Snap;
		}
		OutLoopErr = FMath::Abs(CoolantJ - DutyJ) / FMath::Abs(DutyJ);
		AddInfo(FString::Printf(TEXT("설비수 x%.2f, %d s: 설계 부하 %.1f kW, UA %.0f W/K, 설비수 %.2f kg/s | 부하 %.1f kW | 회수 %.2f -> 공급 %.2f C | 설비수 %.2f -> %.2f C | 냉각수 루프 누적 오차 %.3e (CDU %.4e J 기준) | 스텝별 장부 오차 최대 %.2e"),
			FacilityFlowScale, int32(Steps * Dt), DesignW / 1000.0, Hx.UAWK, Hx.FacilityMassFlowKgs, OutSnap.CduDutyW[0] / 1000.0,
			OutSnap.CduReturnTempK[0] - 273.15, OutSnap.CduSupplyTempK[0] - 273.15, FacilityK - 273.15, OutSnap.CduFacilityReturnTempK[0] - 273.15,
			OutLoopErr, DutyJ, OutMaxStepErr));
		AddInfo(Bridge.DescribeEnergyLedger());
		return Hx;
	};

	// 1) Design facility flow, 400 s.
	FCouplingSnapshot Snap; TArray<FRackThermalInput> Inputs; double StepErr = 0.0, LoopErr = 0.0;
	const FCduHeatExchangerParams Hx = Run(1.0, 20000, Snap, Inputs, StepErr, LoopErr);
	TestTrue(TEXT("Duty equals the racks' coolant heat every step"), StepErr < 1e-12);
	TestTrue(TEXT("Coolant loop closes over the run"), LoopErr < 1e-12);
	bool bSameSupply = true;
	for (const FRackThermalInput& In : Inputs) bSameSupply &= In.CoolantInletTempK == Snap.CduSupplyTempK[0];
	TestTrue(TEXT("Every rack gets the CDU supply"), bSameSupply);
	TestTrue(TEXT("Facility water warms by duty / C"), FMath::Abs(Snap.CduFacilityReturnTempK[0] - FacilityK - Snap.CduDutyW[0] / (Hx.FacilityMassFlowKgs * Hx.FacilityCpJKgK)) < 1e-9);
	// Plausibility: part-load row (1 / 0.5 / 0.2) runs below its full-load design duty, and the supply sits
	// between the facility water and the design approach above it.
	TestTrue(TEXT("Supply between facility water and design approach"), Snap.CduSupplyTempK[0] > FacilityK && Snap.CduSupplyTempK[0] < FacilityK + ApproachK + .5);

	// 2) Facility water starved to 10%: the duty is capped by C_fac (T_return - T_facility) and the loop runs hotter.
	FCouplingSnapshot Starved; TArray<FRackThermalInput> Inputs2; double StepErr2 = 0.0, LoopErr2 = 0.0;
	const FCduHeatExchangerParams Hx2 = Run(.1, 5000, Starved, Inputs2, StepErr2, LoopErr2);
	const double Cap = Hx2.FacilityMassFlowKgs * Hx2.FacilityCpJKgK * (Starved.CduReturnTempK[0] - FacilityK);
	TestTrue(TEXT("Starved duty below the facility capacity"), Starved.CduDutyW[0] <= Cap * (1.0 + 1e-12));
	TestTrue(TEXT("Starved loop runs hotter"), Starved.CduSupplyTempK[0] > Snap.CduSupplyTempK[0] + 5.0);
	TestTrue(TEXT("Starved loop still closes"), LoopErr2 < 1e-12 && StepErr2 < 1e-12);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
