#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Hydraulics/InRackNetwork.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto RackTrayTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

struct FRackTrayCase
{
	FRackThermalParams Params;
	FRackThermalInput Input;
	FRackThermalState Initial;
	FInRackSpec Spec;
	FTrayCoolantInput Water;
	FRackThermalModel Rack;
	FRackTrayCase(bool bGb200 = true)
	{
		if (bGb200) Params = FRackThermalModel::MakeGb200Nvl72Params();
		else
		{
			// Synthetic two-part insulated fixture, not hardware properties.
			FRackDeviceType Device; Device.bLiquidCooled = true;
			FRackComponentParams Part; Part.HeatCapacityJK = 1000.0; Part.DefaultHeatW = 100.0;
			Part.ToCoolantUAWK = 20.0; Part.ToAirUAWK = Part.ToAmbientUAWK = 0.0;
			Device.Components.Add(Part); Params.DeviceTypes.Add(Device);
			FRackSlot Slot; Params.Slots.Add(Slot); Slot.BottomZM = 1.0; Params.Slots.Add(Slot);
			Params.NumSleds = 2; Params.CoolantHeatCapacityJK = 1000.0;
		}
		Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
		Input.FanSpeedOverride = 1.0; Initial.CoolantTempK = Input.CoolantInletTempK;
		Initial.ComponentTempK.Init(Initial.CoolantTempK, Input.ComponentHeatW.Num());
		Initial.SlotFanSpeedRatio.Init(1.0, Params.Slots.Num());
		Spec = InRackNetwork::FromThermal(Params, Input);
		Rack.Initialize(Params, Initial);
		Water.SupplyTempK = Water.ReturnBoundaryTempK = Initial.CoolantTempK;
		Water.ReturnMassFlowKgs = Input.CoolantMassFlowKgs;
		for (double F : Spec.DesignFraction)
		{
			Water.InletMassFlowKgs.Add(Input.CoolantMassFlowKgs * F);
			Water.OutletMassFlowKgs.Add(Input.CoolantMassFlowKgs * F); Water.LeakMassFlowKgs.Add(0.0);
		}
	}
	void ZeroFlow()
	{
		for (double& F : Water.InletMassFlowKgs) F = 0.0;
		for (double& F : Water.OutletMassFlowKgs) F = 0.0;
		for (double& F : Water.LeakMassFlowKgs) F = 0.0;
		Water.ReturnMassFlowKgs = Water.ReturnLeakMassFlowKgs = 0.0;
	}
};
double PeakPartK(const FRackThermalModel& Rack)
{
	double Peak = 0.0; for (double K : Rack.GetState().ComponentTempK) Peak = FMath::Max(Peak, K); return Peak;
}
double PeakChipK(const FRackThermalModel& Rack)
{
	double Peak = 0.0;
	for (int32 N = 0; N < Rack.GetNumNodes(); ++N)
	{
		const auto Kind = Rack.GetNodeParams(N).Kind;
		if (Kind == ERackPartKind::Cpu || Kind == ERackPartKind::Gpu || Kind == ERackPartKind::Switch)
			Peak = FMath::Max(Peak, Rack.GetState().ComponentTempK[N]);
	}
	return Peak;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackTrayNormal, "VRG.Water.RackInterior.ChipNormal", RackTrayTestFlags)
bool FRackTrayNormal::RunTest(const FString& Parameters)
{
	FRackTrayCase C;
	FRackThermalModel Legacy = C.Rack;
	TArray<int32> BadSlots = C.Spec.Slots; BadSlots[1] = BadSlots[0];
	TestFalse(TEXT("Duplicate mapping rejected without enabling"), C.Rack.EnableTrayCoolant(BadSlots, C.Spec.DesignFraction));
	TestFalse(TEXT("Incomplete mapping rejected"), C.Rack.EnableTrayCoolant({C.Spec.Slots[0]}, {1.0}));
	TestFalse(TEXT("Invalid mapping leaves legacy state"), C.Rack.UsesTrayCoolant());
	TestTrue(TEXT("27 liquid slots; 613 existing part nodes"), C.Spec.Slots.Num() == 27 && C.Rack.GetNumNodes() == 613);
	TestTrue(TEXT("Opt-in"), C.Rack.EnableTrayCoolant(C.Spec.Slots, C.Spec.DesignFraction));
	double Capacity = C.Rack.GetTrayCoolant().GetReturnCapacityJK();
	for (double V : C.Rack.GetTrayCoolant().GetTrayCapacityJK()) Capacity += V;
	TestTrue(TEXT("Existing total coolant capacity, no second scalar node"), FMath::Abs(Capacity - C.Params.CoolantHeatCapacityJK) <= 1e-8);
	TestFalse(TEXT("Legacy path remains opt-out"), Legacy.UsesTrayCoolant());
	bool bOk = true;
	for (int32 S = 0; S < 20000; ++S)
	{
		Legacy.Step(C.Input, 0.02);
		bOk &= C.Rack.StepWithTrayCoolant(C.Input, C.Water, 0.02);
		if (!bOk) break;
	}
	TestTrue(TEXT("Normal 400s steps"), bOk);
	double MinDelta = 1e30, MaxDelta = -1e30;
	for (double K : C.Rack.GetTrayCoolant().GetTrayTempK())
	{
		MinDelta = FMath::Min(MinDelta, K - C.Water.SupplyTempK); MaxDelta = FMath::Max(MaxDelta, K - C.Water.SupplyTempK);
	}
	TestTrue(TEXT("Existing design coolant rise plausibility"), MinDelta >= 5.0 && MaxDelta <= 15.0);
	const auto Snap = C.Rack.MakeSnapshot(400.0);
	TestTrue(TEXT("Air paths remain live"), Snap.TotalHeatToAirW > 0.0 && Snap.TotalFanPowerW > 0.0);
	// Initial measurement 2.068e-13; retain the existing tray-thermal 1e-10 energy criterion.
	TestTrue(TEXT("Coupled energy / electrical input <= 1e-10"), FMath::Abs(C.Rack.GetNormalizedEnergyResidual()) <= 1e-10);
	TestFalse(TEXT("Cannot replace a warm legacy model and rebase its ledger"), Legacy.EnableTrayCoolant(C.Spec.Slots, C.Spec.DesignFraction));
	AddInfo(FString::Printf(TEXT("Normal chips (CPU/GPU/NVSwitch): legacy %.12g K, tray %.12g K, difference %.12g K"),
		PeakChipK(Legacy), PeakChipK(C.Rack), PeakChipK(C.Rack) - PeakChipK(Legacy)));
	// Existing S2b overheat threshold is an estimate; this only checks the normal scenario.
	TestTrue(TEXT("Normal chip temperature below existing estimated 85C threshold"), PeakChipK(C.Rack) > 273.15 && PeakChipK(C.Rack) < 358.15);
	for (int32 S = 0; S < C.Params.Slots.Num(); ++S)
		if (!C.Spec.Slots.Contains(S))
			for (int32 N = C.Rack.GetSlotNodeStart(S); N < C.Rack.GetSlotNodeStart(S) + C.Rack.GetSlotNodeCount(S); ++N)
				TestEqual(TEXT("Air-only slots have no coolant heat"), Snap.ComponentHeatToCoolantW[N], 0.0);
	AddInfo(FString::Printf(TEXT("Normal400s: trayDelta %.12g..%.12g K; outlet legacy/tray %.12g/%.12g K; peakPart legacy/tray %.12g/%.12g K; energy %.12g J / %.12g; water %.12g J; IT difference %.12g W"),
		MinDelta, MaxDelta, Legacy.GetState().CoolantTempK, C.Rack.GetState().CoolantTempK, PeakPartK(Legacy), PeakPartK(C.Rack),
		C.Rack.GetEnergyResidualJ(), C.Rack.GetNormalizedEnergyResidual(), C.Rack.GetTrayCoolant().GetResidualJ(), Snap.TotalHeatW - Legacy.MakeSnapshot(400).TotalHeatW));
	FRackThermalModel Spine;
	const auto SpineParams = FRackThermalModel::MakeQuantumSpineParams({0.0}); Spine.Initialize(SpineParams, FRackThermalState());
	TestFalse(TEXT("Air-only spine does not acquire coolant trays"), Spine.EnableTrayCoolant({0}, {1.0}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackTrayReverse, "VRG.Water.RackInterior.ChipReverseAndAtomic", RackTrayTestFlags)
bool FRackTrayReverse::RunTest(const FString& Parameters)
{
	FRackTrayCase C(false);
	TestTrue(TEXT("Synthetic tray connection"), C.Rack.EnableTrayCoolant(C.Spec.Slots, C.Spec.DesignFraction));
	// Return boundary feeds a positive-pressure return manifold; the first outlet is actually an inlet.
	C.Water.InletMassFlowKgs = {0.01, 0.01}; C.Water.OutletMassFlowKgs = {-0.03, 0.01};
	C.Water.LeakMassFlowKgs = {0.04, 0.0}; C.Water.ReturnLeakMassFlowKgs = 0.001;
	C.Water.ReturnMassFlowKgs = -0.021; C.Water.ReturnBoundaryTempK += 20.0;
	// The rack connection must replace caller-supplied part heat/UA rather than double count them.
	C.Water.HeatFromPartsW.Init(1e100, 2); C.Water.PartsConductanceWK.Init(1e100, 2);
	for (int32 S = 0; S < 1000; ++S) if (!C.Rack.StepWithTrayCoolant(C.Input, C.Water, 0.02)) return false;
	const auto Before = C.Rack;
	const double Cp = C.Params.CoolantCpJKgK;
	const auto OldWater = Before.GetTrayCoolant();
	const double OldPartK = Before.GetState().ComponentTempK[0], OldK = OldWater.GetTrayTempK()[0];
	const double HeatW = 20.0 * (OldPartK - OldK);
	const double ExpectedK = OldK + 0.02 * (HeatW + 0.01 * Cp * (C.Water.SupplyTempK - C.Initial.CoolantTempK)
		+ 0.03 * Cp * (OldWater.GetReturnTempK() - C.Initial.CoolantTempK) - 0.04 * Cp * (OldK - C.Initial.CoolantTempK)) / OldWater.GetTrayCapacityJK()[0];
	TestTrue(TEXT("Reverse step"), C.Rack.StepWithTrayCoolant(C.Input, C.Water, 0.02));
	const double ReverseErrorK = FMath::Abs(C.Rack.GetTrayCoolant().GetTrayTempK()[0] - ExpectedK);
	TestTrue(TEXT("Actual return upstream temperature, existing analytic criterion"), ReverseErrorK <= 1e-10);
	const double ExchangeErrorW = C.Rack.MakeSnapshot(0).ComponentHeatToCoolantW[0] - HeatW;
	const double ExpectedLeakW = 0.04 * Cp * (OldK - C.Initial.CoolantTempK)
		+ 0.001 * Cp * (OldWater.GetReturnTempK() - C.Initial.CoolantTempK);
	AddInfo(FString::Printf(TEXT("Single-transfer errors: part %.12g W; leak %.12g W"), ExchangeErrorW, C.Rack.GetLastTrayLeakHeatW() - ExpectedLeakW));
	// Both first measured exactly zero; numerical flux comparison, not a hardware tolerance.
	TestTrue(TEXT("Part transfer once <= 1e-9 W"), FMath::Abs(ExchangeErrorW) <= 1e-9);
	TestTrue(TEXT("Leak transfer once <= 1e-9 W"), FMath::Abs(C.Rack.GetLastTrayLeakHeatW() - ExpectedLeakW) <= 1e-9);
	AddInfo(FString::Printf(TEXT("Reverse20.02s: analytic %.12g K; energy %.12g J / %.12g; mass %.12g kg/s; leak %.12g W"),
		ReverseErrorK, C.Rack.GetEnergyResidualJ(), C.Rack.GetNormalizedEnergyResidual(), C.Rack.GetTrayCoolant().GetLastMassResidualKgs(), C.Rack.GetLastTrayLeakHeatW()));
	const auto ReverseBefore = C.Rack;
	C.Water.InletMassFlowKgs[0] = -0.01; C.Water.OutletMassFlowKgs[0] = -0.02;
	C.Water.LeakMassFlowKgs[0] = 0.01; C.Water.ReturnMassFlowKgs = -0.011;
	const double TrayK = ReverseBefore.GetTrayCoolant().GetTrayTempK()[0];
	const double PartsW = 20.0 * (ReverseBefore.GetState().ComponentTempK[0] - TrayK);
	const double BothReverseK = TrayK + 0.02 * (PartsW - 0.01 * Cp * (TrayK - C.Initial.CoolantTempK)
		+ 0.02 * Cp * (ReverseBefore.GetTrayCoolant().GetReturnTempK() - C.Initial.CoolantTempK)
		- 0.01 * Cp * (TrayK - C.Initial.CoolantTempK)) / ReverseBefore.GetTrayCoolant().GetTrayCapacityJK()[0];
	TestTrue(TEXT("Both tray edges reversed"), C.Rack.StepWithTrayCoolant(C.Input, C.Water, 0.02));
	const double BothErrorK = FMath::Abs(C.Rack.GetTrayCoolant().GetTrayTempK()[0] - BothReverseK);
	TestTrue(TEXT("Reverse inlet exports local, not supply, enthalpy"), BothErrorK <= 1e-10);
	TestTrue(TEXT("Reverse energy criterion retained"), FMath::Abs(C.Rack.GetNormalizedEnergyResidual()) <= 1e-10);
	TestTrue(TEXT("Reverse mass criterion retained"), C.Rack.GetTrayCoolant().GetLastMassResidualKgs() <= 1e-12);
	AddInfo(FString::Printf(TEXT("Both-reverse: analytic %.12g K; normalized %.12g"), BothErrorK, C.Rack.GetNormalizedEnergyResidual()));
	const auto Stable = C.Rack;
	C.Water.SupplyTempK = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("Invalid boundary rejected"), C.Rack.StepWithTrayCoolant(C.Input, C.Water, 1.0));
	C.Water.SupplyTempK = C.Initial.CoolantTempK;
	TestFalse(TEXT("Failure after partial candidate work rolls back"), C.Rack.StepWithTrayCoolant(C.Input, C.Water, 100.0, 1));
	TestTrue(TEXT("All temperatures and fan state unchanged"), Stable.GetState().ComponentTempK == C.Rack.GetState().ComponentTempK
		&& Stable.GetState().SlotFanSpeedRatio == C.Rack.GetState().SlotFanSpeedRatio && Stable.GetState().CoolantTempK == C.Rack.GetState().CoolantTempK
		&& Stable.GetTrayCoolant().GetTrayTempK() == C.Rack.GetTrayCoolant().GetTrayTempK());
	TestEqual(TEXT("Input ledger unchanged"), C.Rack.GetEnergyInJ(), Stable.GetEnergyInJ());
	TestEqual(TEXT("Output ledger unchanged"), C.Rack.GetEnergyOutJ(), Stable.GetEnergyOutJ());
	TestEqual(TEXT("Water ledger unchanged"), C.Rack.GetTrayCoolant().GetHeatOutJ(), Stable.GetTrayCoolant().GetHeatOutJ());
	TestEqual(TEXT("Diagnostics unchanged"), C.Rack.GetLastTrayIntegratedTimeS(), Stable.GetLastTrayIntegratedTimeS());
	TestEqual(TEXT("Substep and leak diagnostics unchanged"), C.Rack.GetLastTraySubsteps(), Stable.GetLastTraySubsteps());
	TestEqual(TEXT("Leak diagnostic unchanged"), C.Rack.GetLastTrayLeakHeatW(), Stable.GetLastTrayLeakHeatW());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackTraySubsteps, "VRG.Water.RackInterior.ChipSubsteps", RackTrayTestFlags)
bool FRackTraySubsteps::RunTest(const FString& Parameters)
{
	FRackTrayCase C(false); C.ZeroFlow();
	TestTrue(TEXT("Opt-in"), C.Rack.EnableTrayCoolant(C.Spec.Slots, C.Spec.DesignFraction));
	TestTrue(TEXT("Closed no-flow interval"), C.Rack.StepWithTrayCoolant(C.Input, C.Water, 20.0));
	TestTrue(TEXT("Substeps used; entire requested interval integrated"), C.Rack.GetLastTraySubsteps() > 1 && C.Rack.GetLastTrayIntegratedTimeS() == 20.0);
	TestTrue(TEXT("Lost cooling accumulates heat in parts and tray"), C.Rack.GetState().ComponentTempK[0] > C.Initial.CoolantTempK
		&& C.Rack.GetTrayCoolant().GetTrayTempK()[0] > C.Initial.CoolantTempK);
	const double StorageErrorJ = C.Rack.GetEnergyStoredJ() - 200.0 * 20.0;
	TestEqual(TEXT("No advection at zero flow"), C.Rack.GetLastTrayLeakHeatW(), 0.0);
	const auto Snap = C.Rack.MakeSnapshot(20.0);
	const double MeanFluxErrorJ = Snap.TotalHeatToCoolantW * 20.0 - C.Rack.GetTrayCoolant().GetHeatInJ();
	AddInfo(FString::Printf(TEXT("Zero-flow20s: storage analytic %.12g J; mean part/water exchange %.12g J; residual %.12g; steps %d"),
		StorageErrorJ, MeanFluxErrorJ, C.Rack.GetNormalizedEnergyResidual(), C.Rack.GetLastTraySubsteps()));
	// Measured |storage|2.274e-11 J and mean transfer0 J; allow roundoff, not physical losses.
	TestTrue(TEXT("Closed storage analytic <= 1e-8 J"), FMath::Abs(StorageErrorJ) <= 1e-8);
	TestTrue(TEXT("Mean transfer preserves the full interval <= 1e-8 J"), FMath::Abs(MeanFluxErrorJ) <= 1e-8);
	// Exact closed two-node solution, each part C=1000 and water C=300 J/K. Euler should converge.
	double PreviousError = TNumericLimits<double>::Max();
	for (double Dt : {0.02, 0.01, 0.005})
	{
		FRackTrayCase Fine(false); Fine.ZeroFlow(); Fine.Rack.EnableTrayCoolant(Fine.Spec.Slots, Fine.Spec.DesignFraction);
		for (int32 S = 0; S < FMath::RoundToInt(10.0 / Dt); ++S)
			if (!Fine.Rack.StepWithTrayCoolant(Fine.Input, Fine.Water, Dt)) return false;
		const double Difference = (0.1 / (20.0 * (1.0 / 1000.0 + 1.0 / 300.0))) * (1.0 - FMath::Exp(-20.0 * (1.0 / 1000.0 + 1.0 / 300.0) * 10.0));
		const double ExactWaterK = Fine.Initial.CoolantTempK + 100.0 * 10.0 / 1300.0 - 1000.0 / 1300.0 * Difference;
		const double ErrorK = FMath::Abs(Fine.Rack.GetTrayCoolant().GetTrayTempK()[0] - ExactWaterK);
		AddInfo(FString::Printf(TEXT("No-flow analytic: dt %.6g s, water error %.12g K"), Dt, ErrorK));
		TestTrue(TEXT("Halving dt converges to independent two-node solution"), ErrorK < PreviousError); PreviousError = ErrorK;
	}
	// Measured 7.008e-5 K at dt0.005s; finite-step discretization error, not roundoff.
	TestTrue(TEXT("Fine-step analytic error <= 2e-4 K"), PreviousError <= 2e-4);
	FRackTrayCase Large;
	Large.Input.FanSpeedOverride = -1.0; // fan slews during substeps: means must remain conservative
	Large.Rack.EnableTrayCoolant(Large.Spec.Slots, Large.Spec.DesignFraction);
	TestTrue(TEXT("Large step also preserves air exchange"), Large.Rack.StepWithTrayCoolant(Large.Input, Large.Water, 2.0));
	const auto LargeSnap = Large.Rack.MakeSnapshot(2.0);
	const double OutMeanJ = 2.0 * (LargeSnap.TotalHeatToAirW + LargeSnap.TotalHeatToAmbientW + LargeSnap.HeatRemovedByCoolantW + Large.Rack.GetLastTrayLeakHeatW());
	AddInfo(FString::Printf(TEXT("Gb200 substeps %d: output flux mean error %.12g J; residual %.12g"),
		Large.Rack.GetLastTraySubsteps(), OutMeanJ - Large.Rack.GetEnergyOutJ(), Large.Rack.GetNormalizedEnergyResidual()));
	TestTrue(TEXT("Boundary plus air mean equals integrated output <= 1e-8 J"), FMath::Abs(OutMeanJ - Large.Rack.GetEnergyOutJ()) <= 1e-8);
	TestTrue(TEXT("Substepped coupled energy <= 1e-10"), FMath::Abs(Large.Rack.GetNormalizedEnergyResidual()) <= 1e-10);
	double AirW = 0.0;
	for (int32 S = 0; S < LargeSnap.SlotAirMassFlowKgs.Num(); ++S)
		AirW += LargeSnap.SlotAirMassFlowKgs[S] * Large.Params.AirCpJKgK * (LargeSnap.SledAirOutletTempK[S] - Large.Input.AirInletTempK);
	AddInfo(FString::Printf(TEXT("Mean forced-air enthalpy difference %.12g W"), AirW - LargeSnap.TotalHeatToAirW));
	TestTrue(TEXT("Existing relative air enthalpy criterion"), FMath::Abs(AirW - LargeSnap.TotalHeatToAirW)
		/ FMath::Max(1.0, FMath::Abs(LargeSnap.TotalHeatToAirW)) <= 1e-9);
	return true;
}
#endif
