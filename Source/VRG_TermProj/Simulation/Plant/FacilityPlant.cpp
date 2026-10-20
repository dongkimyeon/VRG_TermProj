// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Plant/FacilityPlant.h"

bool FFacilityPlant::Build(const FFacilityLoopSpec& LoopSpec, const FFacilityThermalSpec& InThermal, double InDesignHeatW,
	double WaterDensityKgM3, double WaterCpJKgK, const FHydraulicSolveSettings& Settings)
{
	bBuilt = false;
	Thermal = InThermal;
	WaterCp = WaterCpJKgK;
	DesignHeatW = InDesignHeatW;
	State = FFacilityPlantState();
	if (!Loop.Build(LoopSpec)) return false;
	Loop.GetMutableNetwork().SetFluidDensityKgM3(WaterDensityKgM3);
	LoopResult = Loop.Solve(Settings);
	if (!LoopResult.bControlConverged) return false;
	BranchKgs.Reset();
	TotalKgs = 0.0;
	for (int32 G = 0; G < Loop.GetNumBranches(); ++G)
	{
		BranchKgs.Add(FMath::Max(Loop.GetBranchFlowM3s(G), 0.0) * WaterDensityKgM3);
		TotalKgs += BranchKgs.Last();
	}
	if (!DryCooler::FromCatalog(Thermal.DryCooler, Dc)) return false;

	// Catalog selection rule: heat scales with the inlet temperature difference (ITD) at the rated flows.
	const double DesignReturnK = Thermal.SupplySetpointK + (TotalKgs > 0.0 ? DesignHeatW / (TotalKgs * WaterCp) : 0.0);
	const double DesignItdK = DesignReturnK - Thermal.DesignAmbientK;
	if (Thermal.DryCoolers > 0) NumDryCoolers = Thermal.DryCoolers;
	else if (DesignItdK > 0.0) NumDryCoolers = FMath::Max(1, FMath::CeilToInt32(DesignHeatW / (Thermal.DryCooler.RatedHeatW * DesignItdK / Thermal.DryCooler.RatedItdK) - 1e-9));
	else NumDryCoolers = FMath::Max(1, Loop.GetHandles().DryCoolerPipes.Num());   // return colder than the design air: by flow

	// Water volumes as transit times at the design flow.
	const double DesignKgs = Loop.GetDesignTotalFlowM3s() * WaterDensityKgM3;
	SupplyCapJK = DesignKgs * WaterCp * FMath::Max(Thermal.SupplySideTransitS, 1.0);
	ReturnCapJK = DesignKgs * WaterCp * FMath::Max(Thermal.ReturnSideTransitS, 1.0);

	State.SupplyK = State.ReturnK = State.DryCoolerOutK = Thermal.SupplySetpointK;
	State.InitialSupplyK = State.SupplyK; State.InitialReturnK = State.ReturnK;
	State.FanRatio = State.FanTargetRatio = FMath::Clamp(Thermal.MinFanRatio, 0.0, 1.0);
	bBuilt = true;
	return true;
}

double FFacilityPlant::GetDryCoolerHeatW(double FanRatio, double ReturnK) const
{
	if (NumDryCoolers <= 0) return 0.0;
	return NumDryCoolers * DryCooler::HeatW(Dc, FanRatio, TotalKgs / NumDryCoolers, WaterCp, ReturnK, Thermal.AmbientK);
}

double FFacilityPlant::DryCoolerOutletK(double FanRatio) const
{
	const double WaterWK = TotalKgs * WaterCp;
	return WaterWK > 0.0 ? State.ReturnK - GetDryCoolerHeatW(FanRatio, State.ReturnK) / WaterWK : State.ReturnK;
}

double FFacilityPlant::SolveFanTarget() const
{
	const double Min = FMath::Clamp(Thermal.MinFanRatio, 0.0, 1.0);
	// Leaving temperature falls as the fans speed up (when the air is colder than the water).
	if (DryCoolerOutletK(1.0) > Thermal.SupplySetpointK) return 1.0 + 1e-6;   // marker: above full speed
	if (DryCoolerOutletK(Min) <= Thermal.SupplySetpointK) return Min;
	double Lo = Min, Hi = 1.0;
	for (int32 I = 0; I < 80; ++I)
	{
		const double Mid = 0.5 * (Lo + Hi);
		(DryCoolerOutletK(Mid) > Thermal.SupplySetpointK ? Lo : Hi) = Mid;
	}
	return 0.5 * (Lo + Hi);
}

void FFacilityPlant::Step(double DtS, const TArray<double>& BranchDutyW)
{
	if (!IsThermalActive() || DtS <= 0.0) return;
	double DutyW = 0.0;
	for (int32 G = 0; G < BranchDutyW.Num() && G < BranchKgs.Num(); ++G) DutyW += BranchDutyW[G];

	// Fan control from the state at the start of the step, ramp limited.
	const double Target = SolveFanTarget();
	State.bSetpointNotMet = Target > 1.0;
	State.FanTargetRatio = FMath::Min(Target, 1.0);
	State.bOverCooling = State.FanTargetRatio <= Thermal.MinFanRatio && DryCoolerOutletK(Thermal.MinFanRatio) < Thermal.SupplySetpointK - 1e-6;
	const double MaxChange = FMath::Max(Thermal.FanRampPerS, 0.0) * DtS;
	State.FanRatio = FMath::Clamp(State.FanTargetRatio, State.FanRatio - MaxChange, State.FanRatio + MaxChange);
	if (State.bSetpointNotMet) State.SetpointNotMetS += DtS;

	// Two mixed volumes, explicit Euler, sub-steps below a fifth of the shortest time constant.
	const double WaterWK = TotalKgs * WaterCp;
	const double Tau = WaterWK > 0.0 ? FMath::Min(SupplyCapJK, ReturnCapJK) / WaterWK : TNumericLimits<double>::Max();
	const int32 Sub = FMath::Clamp(FMath::CeilToInt32(DtS / (0.2 * Tau)), 1, 1000);
	const double H = DtS / Sub;
	double DryW = 0.0;
	for (int32 K = 0; K < Sub; ++K)
	{
		const double Qdc = GetDryCoolerHeatW(State.FanRatio, State.ReturnK);
		const double Ts = State.SupplyK, Tr = State.ReturnK;
		State.ReturnK = Tr + H / ReturnCapJK * (WaterWK * Ts + DutyW - WaterWK * Tr);
		State.SupplyK = Ts + H / SupplyCapJK * (WaterWK * Tr - Qdc - WaterWK * Ts);
		State.DutyInJ += DutyW * H;
		State.DryCoolerOutJ += Qdc * H;
		DryW += Qdc / Sub;
	}
	State.DryCoolerHeatW = DryW;
	State.DutyW = DutyW;
	State.DryCoolerOutK = WaterWK > 0.0 ? State.ReturnK - GetDryCoolerHeatW(State.FanRatio, State.ReturnK) / WaterWK : State.ReturnK;
}

double FFacilityPlant::GetFanElectricW() const
{
	return IsThermalActive() ? NumDryCoolers * DryCooler::FanElectricW(Dc, State.FanRatio) : 0.0;
}

double FFacilityPlant::GetStorageJ() const
{
	return SupplyCapJK * (State.SupplyK - State.InitialSupplyK) + ReturnCapJK * (State.ReturnK - State.InitialReturnK);
}

double FFacilityPlant::GetLedgerResidual() const
{
	const double Scale = FMath::Max3(FMath::Abs(State.DutyInJ), FMath::Abs(State.DryCoolerOutJ), 1.0);
	return (State.DutyInJ - State.DryCoolerOutJ - GetStorageJ()) / Scale;
}
