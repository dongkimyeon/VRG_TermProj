// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Plant/FacilityPlant.h"
#include "Simulation/Thermal/CduHeatExchanger.h"

void FDryCoolerBank::Control(double DtS, double InK, double TotalKgs, double CpJKgK, double AirK)
{
	const double WaterWK = TotalKgs * CpJKgK;
	auto Outlet = [&](double R) { return WaterWK > 0.0 ? InK - Heat(R, InK, TotalKgs, CpJKgK, AirK) / WaterWK : InK; };
	const double Min = FMath::Clamp(MinFan, 0.0, 1.0);
	double T;
	if (Outlet(1.0) > SetpointK) T = 1.0 + 1e-6;            // above full speed: setpoint not met
	else if (Outlet(Min) <= SetpointK) T = Min;
	else
	{
		double Lo = Min, Hi = 1.0;
		for (int32 I = 0; I < 80; ++I) { const double Mid = 0.5 * (Lo + Hi); (Outlet(Mid) > SetpointK ? Lo : Hi) = Mid; }
		T = 0.5 * (Lo + Hi);
	}
	bNotMet = T > 1.0;
	Target = FMath::Min(T, 1.0);
	bOverCooling = Target <= Min && Outlet(Min) < SetpointK - 1e-6;
	const double MaxChange = FMath::Max(RampPerS, 0.0) * DtS;
	Fan = FMath::Clamp(Target, Fan - MaxChange, Fan + MaxChange);
	if (bNotMet) NotMetS += DtS;
}

bool FFacilityPlant::Build(const FFacilityLoopSpec& LoopSpec, const FFacilityThermalSpec& InThermal, double InDesignHeatW,
	double WaterDensityKgM3, double WaterCpJKgK, const FHydraulicSolveSettings& Settings, const TArray<FPlantCrahCoil>& Crahs,
	double InChwSetpointK)
{
	bBuilt = false;
	bChw = bCw = bTrim = bEconOn = false;
	TrimCapRefW = DesignTrimW = HxUA = DesignChwW = DesignCondW = 0.0;
	Thermal = InThermal;
	WaterCp = WaterCpJKgK;
	WaterRho = WaterDensityKgM3;
	DesignHeatW = InDesignHeatW;
	ChwSetpointK = InChwSetpointK;
	State = FFacilityPlantState();
	Fws = Chw = Cw = FWaterVolumes();
	FwsBank = CwBank = FDryCoolerBank();
	CrahWaterKgs.Reset(); CrahCfUA.Reset(); CrahEqUA.Reset(); CrahCoils.Reset();
	SolveSettings = Settings;
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
	if (!DryCooler::FromCatalog(Thermal.DryCooler, FwsBank.Dc)) return false;

	// Catalog selection rule: heat scales with the inlet temperature difference (ITD) at the rated flows.
	const double DesignReturnK = Thermal.SupplySetpointK + (TotalKgs > 0.0 ? DesignHeatW / (TotalKgs * WaterCp) : 0.0);
	const double DesignItdK = DesignReturnK - Thermal.DesignAmbientK;
	if (Thermal.DryCoolers > 0) FwsBank.Units = Thermal.DryCoolers;
	else if (DesignItdK > 0.0) FwsBank.Units = FMath::Max(1, FMath::CeilToInt32(DesignHeatW / (Thermal.DryCooler.RatedHeatW * DesignItdK / Thermal.DryCooler.RatedItdK) - 1e-9));
	else FwsBank.Units = FMath::Max(1, Loop.GetHandles().ReturnCoilPipes.Num());   // return colder than the design air: by flow
	FwsBank.SetpointK = Thermal.SupplySetpointK;
	FwsBank.MinFan = Thermal.MinFanRatio; FwsBank.RampPerS = Thermal.FanRampPerS;
	FwsBank.Fan = FwsBank.Target = FMath::Clamp(Thermal.MinFanRatio, 0.0, 1.0);

	// Water volumes as transit times at the design flow.
	const double DesignKgs = Loop.GetDesignTotalFlowM3s() * WaterDensityKgM3;
	Fws.SupplyCapJK = DesignKgs * WaterCp * FMath::Max(Thermal.SupplySideTransitS, 1.0);
	Fws.ReturnCapJK = DesignKgs * WaterCp * FMath::Max(Thermal.ReturnSideTransitS, 1.0);
	Fws.FlowWK = TotalKgs * WaterCp;
	Fws.Init(Thermal.SupplySetpointK);
	State.InitialSupplyK = State.InitialReturnK = Thermal.SupplySetpointK;

	if (!Thermal.bFixedSupplyTemp)
	{
		ChillerMaxCop = Chiller::MaxCopInRange(Thermal.Chiller);
		// Condenser water design point (shared by every chiller): the CW dry coolers are rated at EFT = air + ITD
		// (48.9 C at 35 C); the chiller's design condenser rise comes from the dataset reference flows.
		const FChillerCurves& K = Thermal.Chiller;
		const double RefCondW = K.RefCapacityW * (1.0 + 1.0 / K.RefCop);
		CwRiseK = RefCondW / (WaterRho * WaterCp * FMath::Max(K.RefCondenserFlowM3s, 1e-9));
		CwDesignInK = Thermal.DryCooler.RatedAirInK + Thermal.DryCooler.RatedItdK - CwRiseK;
		if (Thermal.bChilledWater && Crahs.Num() > 0 && !BuildChilledWater(Crahs, Settings)) return false;
		// F4 trim chiller: the gap the dry coolers leave at full fans, design air and design return.
		if (Thermal.bTrimChiller)
		{
			const double ReturnD = DesignReturnK;
			const double SaveAmbient = Thermal.AmbientK;
			Thermal.AmbientK = Thermal.DesignAmbientK;
			const double OutD = Fws.FlowWK > 0.0 ? ReturnD - GetDryCoolerHeatW(1.0, ReturnD) / Fws.FlowWK : ReturnD;
			Thermal.AmbientK = SaveAmbient;
			DesignTrimW = FMath::Max(Fws.FlowWK * (OutD - Thermal.SupplySetpointK), 0.0);
			if (DesignTrimW > 0.0)
			{
				const int32 N = FMath::Max(Thermal.TrimDutyChillers, 1);
				TrimCapRefW = DesignTrimW / (N * FMath::Max(Chiller::Biquadratic(K.CapFt, FMath::Clamp(Thermal.SupplySetpointK - 273.15, K.MinX, K.MaxX),
					FMath::Clamp(CwDesignInK - 273.15, K.MinY, K.MaxY)), 1e-6));
				bTrim = true;
			}
		}
		if ((bChw || bTrim) && !BuildCondenser(Settings)) return false;
		// Economizer plate exchanger at the design flows: with the CHW as C_min it leaves EconomizerApproachK above the
		// entering CW at the design chilled water rise, eps = rise / (rise + approach).
		if (bChw && bCw)
		{
			const double CMin = FMath::Min(Chw.FlowWK, Cw.FlowWK), CMax = FMath::Max(Chw.FlowWK, Cw.FlowWK);
			const double Eps = Thermal.ChwDesignRiseK / (Thermal.ChwDesignRiseK + FMath::Max(Thermal.EconomizerApproachK, 0.1));
			HxUA = CMin > 0.0 ? CduHeatExchanger::NtuForEffectiveness(Eps, CMin / CMax) * CMin : 0.0;
		}
	}
	bBuilt = true;
	SyncFwsState();
	return true;
}

bool FFacilityPlant::BuildChilledWater(const TArray<FPlantCrahCoil>& Crahs, const FHydraulicSolveSettings& Settings)
{
	// CRAH coils: design water flow from the catalog rise; counterflow UA that removes the old design heat.
	FFacilityLoopSpec ChwSpec;
	ChwSpec.BranchHxDesignDropPa = Thermal.CrahCoilDesignDropPa;
	DesignChwW = 0.0;
	TArray<double> DesignKgs;
	for (const FPlantCrahCoil& C : Crahs)
	{
		if (!(C.AirWK > 0.0) || !(C.DesignDutyW > 0.0)) return false;
		const double Kgs = C.DesignDutyW / (WaterCp * Thermal.ChwDesignRiseK);
		DesignKgs.Add(Kgs);
		ChwSpec.BranchDesignFlowM3s.Add(Kgs / WaterRho);
		DesignChwW += C.DesignDutyW;
		const double WaterWK = Kgs * WaterCp, CMin = FMath::Min(C.AirWK, WaterWK), CMax = FMath::Max(C.AirWK, WaterWK);
		const double Eps = C.DesignAirEps * C.AirWK / CMin;
		if (!(Eps > 0.0 && Eps < 1.0)) return false;
		CrahCfUA.Add(CduHeatExchanger::NtuForEffectiveness(Eps, CMin / CMax) * CMin);
	}
	// Chillers: identical units in parallel at the evaporators (the CHW loop's return coils).
	const FChillerCurves& K = Thermal.Chiller;
	const int32 NDuty = FMath::Max(Thermal.DutyChillers, 1);
	ChwSpec.ReturnCoils = NDuty;
	double ChwTotalM3s = 0.0;
	for (const double Q : ChwSpec.BranchDesignFlowM3s) ChwTotalM3s += Q;
	ChwSpec.ReturnCoilRatedFlowM3s = ChwTotalM3s / NDuty;
	ChwSpec.ReturnCoilRatedDropPa = Thermal.EvaporatorDesignDropPa;
	if (!ChwLoop.Build(ChwSpec)) return false;
	ChwLoop.GetMutableNetwork().SetFluidDensityKgM3(WaterRho);
	ChwResult = ChwLoop.Solve(Settings);
	if (!ChwResult.bControlConverged) return false;
	CrahCoils = Crahs;
	RefreshChwFlows();

	// Size the reference capacity so the duty chillers together make exactly the design load at the design point.
	ChillerCapRefW = DesignChwW / (NDuty * FMath::Max(Chiller::Biquadratic(K.CapFt,
		FMath::Clamp(ChwSetpointK - 273.15, K.MinX, K.MaxX), FMath::Clamp(CwDesignInK - 273.15, K.MinY, K.MaxY)), 1e-6));
	const double TransitS = FMath::Max(Thermal.SupplySideTransitS, 1.0), TransitR = FMath::Max(Thermal.ReturnSideTransitS, 1.0);
	Chw.SupplyCapJK = ChwTotalM3s * WaterRho * WaterCp * TransitS;
	Chw.ReturnCapJK = ChwTotalM3s * WaterRho * WaterCp * TransitR;
	Chw.Init(ChwSetpointK);
	bChw = true;
	return true;
}

bool FFacilityPlant::BuildCondenser(const FHydraulicSolveSettings& Settings)
{
	const FChillerCurves& K = Thermal.Chiller;
	bool bOutside = false;
	const int32 NChw = bChw ? FMath::Max(Thermal.DutyChillers, 1) : 0;
	const int32 NTrim = bTrim ? FMath::Max(Thermal.TrimDutyChillers, 1) : 0;
	const double ChwCondW = bChw ? DesignChwW * (1.0 + Chiller::FullLoadEir(K, ChwSetpointK - 273.15, CwDesignInK - 273.15, ChillerMaxCop, bOutside) * Chiller::Quadratic(K.EirFplr, 1.0)) : 0.0;
	const double TrimCondW = bTrim ? DesignTrimW * (1.0 + Chiller::FullLoadEir(K, Thermal.SupplySetpointK - 273.15, CwDesignInK - 273.15, ChillerMaxCop, bOutside) * Chiller::Quadratic(K.EirFplr, 1.0)) : 0.0;
	DesignCondW = ChwCondW + TrimCondW;
	if (DesignCondW <= 0.0) return false;
	FFacilityLoopSpec CwSpec;
	CwSpec.BranchHxDesignDropPa = Thermal.CondenserDesignDropPa;
	for (int32 I = 0; I < NChw; ++I) CwSpec.BranchDesignFlowM3s.Add(ChwCondW / NChw / (WaterRho * WaterCp * CwRiseK));
	for (int32 I = 0; I < NTrim; ++I) CwSpec.BranchDesignFlowM3s.Add(TrimCondW / NTrim / (WaterRho * WaterCp * CwRiseK));
	const double CwItdK = CwDesignInK + CwRiseK - Thermal.DesignAmbientK;
	CwBank.Units = FMath::Max(1, FMath::CeilToInt32(DesignCondW / (Thermal.DryCooler.RatedHeatW * FMath::Max(CwItdK, 0.1) / Thermal.DryCooler.RatedItdK) - 1e-9));
	CwSpec.ReturnCoils = CwBank.Units;
	if (!CwLoop.Build(CwSpec)) return false;
	CwLoop.GetMutableNetwork().SetFluidDensityKgM3(WaterRho);
	CwResult = CwLoop.Solve(Settings);
	if (!CwResult.bControlConverged) return false;
	CwKgs = 0.0;
	for (int32 I = 0; I < CwLoop.GetNumBranches(); ++I) CwKgs += FMath::Max(CwLoop.GetBranchFlowM3s(I), 0.0) * WaterRho;
	CwBank.Dc = FwsBank.Dc;
	CwBank.SetpointK = Thermal.CwSupplySetpointK;
	CwBank.MinFan = Thermal.MinFanRatio; CwBank.RampPerS = Thermal.FanRampPerS;
	CwBank.Fan = CwBank.Target = FMath::Clamp(Thermal.MinFanRatio, 0.0, 1.0);
	const double TransitS = FMath::Max(Thermal.SupplySideTransitS, 1.0), TransitR = FMath::Max(Thermal.ReturnSideTransitS, 1.0);
	const double CwDesignKgs = CwLoop.GetDesignTotalFlowM3s() * WaterRho;
	Cw.SupplyCapJK = CwDesignKgs * WaterCp * TransitS;
	Cw.ReturnCapJK = CwDesignKgs * WaterCp * TransitR;
	Cw.FlowWK = CwKgs * WaterCp;
	Cw.Init(Thermal.CwSupplySetpointK);
	bCw = true;
	return true;
}

double FFacilityPlant::EconomizerHeatW(double ChwInK, double CwInK) const
{
	if (!bChw || !bCw || HxUA <= 0.0 || ChwInK <= CwInK) return 0.0;
	const double Hot = Chw.FlowWK, Cold = Cw.FlowWK;
	const double CMin = FMath::Min(Hot, Cold), CMax = FMath::Max(Hot, Cold);
	if (CMin <= UE_DOUBLE_SMALL_NUMBER) return 0.0;
	return CduHeatExchanger::CounterflowEffectiveness(HxUA / CMin, CMin / CMax) * CMin * (ChwInK - CwInK);
}

double FFacilityPlant::CrahAirEffectiveness(int32 Unit, double WaterKgs) const
{
	if (!CrahCoils.IsValidIndex(Unit) || !CrahCfUA.IsValidIndex(Unit)) return 0.0;
	const double WaterWK = FMath::Max(WaterKgs, 0.0) * WaterCp, AirWK = CrahCoils[Unit].AirWK;
	const double CMin = FMath::Min(AirWK, WaterWK), CMax = FMath::Max(AirWK, WaterWK);
	if (CMin <= UE_DOUBLE_SMALL_NUMBER || AirWK <= 0.0) return 0.0;   // D2: no water, the coil does nothing
	return CduHeatExchanger::CounterflowEffectiveness(CrahCfUA[Unit] / CMin, CMin / CMax) * CMin / AirWK;
}

void FFacilityPlant::RefreshChwFlows()
{
	CrahWaterKgs.Reset(); CrahEqUA.Reset();
	ChwKgs = 0.0;
	for (int32 U = 0; U < CrahCoils.Num(); ++U)
	{
		const double Kgs = FMath::Max(ChwLoop.GetBranchFlowM3s(U), 0.0) * WaterRho;
		CrahWaterKgs.Add(Kgs);
		ChwKgs += Kgs;
		// Equivalent UA for the solver's eps = 1 - exp(-UA/C_air): the same air-side effectiveness as the real coil.
		const double EpsAir = FMath::Min(CrahAirEffectiveness(U, Kgs), 1.0 - 1e-12);
		CrahEqUA.Add(-CrahCoils[U].AirWK * FMath::Loge(1.0 - EpsAir));
	}
	Chw.FlowWK = ChwKgs * WaterCp;
}

double FFacilityPlant::CrahOpeningFor(int32 Unit, double IntakeK, double TargetK, bool& bSaturated) const
{
	bSaturated = false;
	const double SupplyK = Chw.SupplyK;
	if (IntakeK <= TargetK || IntakeK <= SupplyK) return 0.0;          // already at or below target: no cooling wanted
	const double Need = (IntakeK - TargetK) / (IntakeK - SupplyK);
	// Full-open flow under ideal header differential control: Q = sqrt(dp / R_branch(1)).
	const double Dp = ChwLoop.GetDifferentialSetpointPa();
	const double RFull = ChwLoop.BranchResistancePaS2M6(Unit, 1.0);
	const double MaxKgs = FMath::Sqrt(Dp / RFull) * WaterRho;
	if (CrahAirEffectiveness(Unit, MaxKgs) <= Need) { bSaturated = true; return 1.0; }
	double Lo = 0.0, Hi = MaxKgs;
	for (int32 I = 0; I < 100; ++I) { const double Mid = 0.5 * (Lo + Hi); (CrahAirEffectiveness(Unit, Mid) < Need ? Lo : Hi) = Mid; }
	const double Q = 0.5 * (Lo + Hi) / WaterRho;
	if (Q <= 0.0) return 0.0;
	// R_valve(x) = R_branch - R_rest = dp / Q^2 - R_rest;  R_valve(x) = R_open Rv^(2 (1 - x))  ->  x.
	const double ROpen = ChwLoop.ValveResistancePaS2M6(Unit, 1.0);
	const double RRest = RFull - ROpen;
	const double RValve = Dp / (Q * Q) - RRest;
	if (RValve <= ROpen) return 1.0;
	const double Rv = FMath::Max(ChwLoop.GetSpec().ValveRangeability, 1.0 + 1e-9);
	return FMath::Clamp(1.0 - FMath::Loge(RValve / ROpen) / (2.0 * FMath::Loge(Rv)), 0.0, 1.0);
}

bool FFacilityPlant::ControlCrahValves(double DtS, const TArray<double>& IntakeMixK)
{
	State.bCrahReadingsMissing = false;
	if (!IsChilledWaterActive() || !Thermal.bCrahValveControl || IntakeMixK.Num() != CrahCoils.Num()) return false;
	const double MaxStep = Thermal.ValveStrokeS > 0.0 ? DtS / Thermal.ValveStrokeS : 1.0;
	bool bChanged = false;
	int32 Full = 0, Shut = 0;
	for (int32 U = 0; U < CrahCoils.Num(); ++U)
	{
		if (CrahCoils[U].CoilLeavingSetpointK <= 0.0) continue;
		bool bSat = false;
		const double Target = CrahOpeningFor(U, IntakeMixK[U], CrahCoils[U].CoilLeavingSetpointK, bSat);
		const double Now = ChwLoop.GetValveOpening(U);
		const double Next = FMath::Clamp(Target, Now - MaxStep, Now + MaxStep);
		if (Next != Now) { ChwLoop.SetValveOpening(U, Next); bChanged = true; }
		Full += Next >= 1.0 ? 1 : 0;
		Shut += Next <= 0.0 ? 1 : 0;
	}
	State.CrahValvesFullOpen = Full; State.CrahValvesShut = Shut;
	if (bChanged)
	{
		ChwResult = ChwLoop.Solve(SolveSettings);
		RefreshChwFlows();
	}
	return bChanged;
}

void FFacilityPlant::SyncFwsState()
{
	State.SupplyK = Fws.SupplyK; State.ReturnK = Fws.ReturnK;
	State.FanRatio = FwsBank.Fan; State.FanTargetRatio = FwsBank.Target;
	State.bSetpointNotMet = FwsBank.bNotMet; State.bOverCooling = FwsBank.bOverCooling; State.SetpointNotMetS = FwsBank.NotMetS;
	State.DryCoolerOutK = Fws.FlowWK > 0.0 ? Fws.ReturnK - GetDryCoolerHeatW(FwsBank.Fan, Fws.ReturnK) / Fws.FlowWK : Fws.ReturnK;
	State.ChwSupplyK = Chw.SupplyK; State.ChwReturnK = Chw.ReturnK; State.CwSupplyK = Cw.SupplyK; State.CwReturnK = Cw.ReturnK;
}

double FFacilityPlant::GetDryCoolerHeatW(double FanRatio, double ReturnK) const
{
	return FwsBank.Heat(FanRatio, ReturnK, TotalKgs, WaterCp, Thermal.AmbientK);
}

double FFacilityPlant::DryCoolerOutletK(double FanRatio) const
{
	return Fws.FlowWK > 0.0 ? Fws.ReturnK - GetDryCoolerHeatW(FanRatio, Fws.ReturnK) / Fws.FlowWK : Fws.ReturnK;
}

void FFacilityPlant::Step(double DtS, const TArray<double>& BranchDutyW, double CrahDutyW)
{
	if (!IsThermalActive() || DtS <= 0.0) return;
	double DutyW = 0.0;
	for (int32 G = 0; G < BranchDutyW.Num() && G < BranchKgs.Num(); ++G) DutyW += BranchDutyW[G];
	const double CrahW = bChw ? CrahDutyW : 0.0;

	// Controls from the state at the start of the step.
	FwsBank.Control(DtS, Fws.ReturnK, TotalKgs, WaterCp, Thermal.AmbientK);
	// Economizer switch with hysteresis; while on, the CW fans aim below the CHW setpoint (as cold as is useful).
	if (bChw && bCw && Thermal.bEconomizer)
	{
		// Both ways on what the CW dry coolers could make at full fans (not the present CW, which the fans hold at their
		// own setpoint while the economizer is off): on when that is clearly below the CHW return, off when it no
		// longer is by the smaller margin. The decision follows the outdoor air, not the loop's own transient.
		const double CwBestK = Cw.FlowWK > 0.0 ? Cw.ReturnK - CwBank.Heat(1.0, Cw.ReturnK, CwKgs, WaterCp, Thermal.AmbientK) / Cw.FlowWK : Cw.ReturnK;
		const bool bWas = bEconOn;
		if (!bEconOn && Chw.ReturnK - CwBestK > Thermal.EconomizerOnMarginK) bEconOn = true;
		else if (bEconOn && Chw.ReturnK - CwBestK < Thermal.EconomizerOffMarginK) bEconOn = false;
		if (bEconOn != bWas) ++State.EconomizerSwitches;
	}
	else bEconOn = false;
	if (bCw)
	{
		CwBank.SetpointK = bEconOn ? ChwSetpointK - Thermal.EconomizerApproachK : Thermal.CwSupplySetpointK;
		CwBank.Control(DtS, Cw.ReturnK, CwKgs, WaterCp, Thermal.AmbientK);
	}
	// A chiller starts for a request above 0.1 % of one unit's capacity (deadband against numerical crumbs; ESTIMATE).
	auto Stage = [](double Request, double UnitCap, int32 Max) { return Request > 1e-3 * UnitCap && UnitCap > 0.0 ? FMath::Clamp(FMath::CeilToInt32(Request / UnitCap - 1e-9), 1, FMath::Max(Max, 1)) : 0; };
	int32 Running = 0, Trim = 0;
	if (bChw)
	{
		const double After = Chw.ReturnK - (bEconOn ? EconomizerHeatW(Chw.ReturnK, Cw.SupplyK) / FMath::Max(Chw.FlowWK, 1e-30) : 0.0);
		Running = Stage(FMath::Max(Chw.FlowWK * (After - ChwSetpointK), 0.0),
			Chiller::CapacityW(Thermal.Chiller, ChillerCapRefW, ChwSetpointK - 273.15, Cw.SupplyK - 273.15), Thermal.DutyChillers);
	}
	if (bTrim)
	{
		const double After = Fws.FlowWK > 0.0 ? Fws.ReturnK - GetDryCoolerHeatW(FwsBank.Fan, Fws.ReturnK) / Fws.FlowWK : Fws.ReturnK;
		Trim = Stage(FMath::Max(Fws.FlowWK * (After - Thermal.SupplySetpointK), 0.0),
			Chiller::CapacityW(Thermal.Chiller, TrimCapRefW, Thermal.SupplySetpointK - 273.15, Cw.SupplyK - 273.15), Thermal.TrimDutyChillers);
	}

	// Explicit Euler for all volumes, sub-stepped below a fifth of the shortest time constant.
	double Tau = Fws.TauS();
	if (bChw) Tau = FMath::Min(Tau, Chw.TauS());
	if (bCw) Tau = FMath::Min(Tau, Cw.TauS());
	const int32 Sub = FMath::Clamp(FMath::CeilToInt32(DtS / (0.2 * Tau)), 1, 1000);
	const double H = DtS / Sub;
	double FwsW = 0.0, CwW = 0.0, LoadW = 0.0, PowerW = 0.0, TrimW = 0.0, TrimPW = 0.0, EconW = 0.0;
	bool bLimited = false, bOutside = false, bTrimLimited = false, bFwsNotMet = false;
	for (int32 K = 0; K < Sub; ++K)
	{
		// Order along the water: economizer, then chillers (CHW); dry coolers, then trim chiller (FWS); the CW supply
		// passes the economizer first, then the condensers (all condensers see the same entering CW).
		const double Qhx = bEconOn ? EconomizerHeatW(Chw.ReturnK, Cw.SupplyK) : 0.0;
		const double CwInK = bCw ? Cw.SupplyK + (Cw.FlowWK > 0.0 ? Qhx / Cw.FlowWK : 0.0) : 0.0;
		const double Qf = GetDryCoolerHeatW(FwsBank.Fan, Fws.ReturnK);
		double Qe = 0.0, W = 0.0, Qt = 0.0, Wt = 0.0, Qw = 0.0;
		if (bChw && Running > 0)
		{
			const double InK = Chw.ReturnK - Qhx / FMath::Max(Chw.FlowWK, 1e-30);
			const FChillerOperatingPoint Op = Chiller::Evaluate(Thermal.Chiller, ChillerCapRefW, ChillerMaxCop, InK, Chw.FlowWK / Running, ChwSetpointK, CwInK);
			Qe = Running * Op.LoadW; W = Running * Op.PowerW;
			bLimited |= Op.bCapacityLimited; bOutside |= Op.bOutsideCurves;
		}
		if (bTrim && Trim > 0)
		{
			const double InK = Fws.FlowWK > 0.0 ? Fws.ReturnK - Qf / Fws.FlowWK : Fws.ReturnK;
			const FChillerOperatingPoint Op = Chiller::Evaluate(Thermal.Chiller, TrimCapRefW, ChillerMaxCop, InK, Fws.FlowWK / Trim, Thermal.SupplySetpointK, CwInK);
			Qt = Trim * Op.LoadW; Wt = Trim * Op.PowerW;
			bTrimLimited |= Op.bCapacityLimited; bOutside |= Op.bOutsideCurves;
		}
		if (bCw) Qw = CwBank.Heat(CwBank.Fan, Cw.ReturnK, CwKgs, WaterCp, Thermal.AmbientK);
		if (bChw) Chw.Advance(H, CrahW, Qhx + Qe);
		if (bCw) Cw.Advance(H, Qhx + Qe + W + Qt + Wt, Qw);   // condenser heat = Q_evap + W, computed once (rule 6)
		const double FwsOutK = Fws.FlowWK > 0.0 ? Fws.ReturnK - (Qf + Qt) / Fws.FlowWK : Fws.ReturnK;
		bFwsNotMet |= FwsOutK > Thermal.SupplySetpointK + 1e-6;
		Fws.Advance(H, DutyW, Qf + Qt);
		State.DutyInJ += DutyW * H; State.DryCoolerOutJ += Qf * H;
		State.CrahInJ += CrahW * H; State.CompressorInJ += (W + Wt) * H; State.CwDryCoolerOutJ += Qw * H;
		FwsW += Qf / Sub; CwW += Qw / Sub; LoadW += Qe / Sub; PowerW += W / Sub; TrimW += Qt / Sub; TrimPW += Wt / Sub; EconW += Qhx / Sub;
	}
	FwsBank.HeatW = FwsW; CwBank.HeatW = CwW;
	State.DryCoolerHeatW = FwsW; State.DutyW = DutyW;
	State.CrahDutyW = CrahW; State.ChillerLoadW = LoadW; State.ChillerPowerW = PowerW; State.ChillerCondenserW = LoadW + PowerW;
	State.CwDryCoolerHeatW = CwW; State.ChillersRunning = Running;
	State.TrimRunning = Trim; State.TrimLoadW = TrimW; State.TrimPowerW = TrimPW; State.bTrimLimited = bTrimLimited;
	State.EconomizerW = EconW; State.bEconomizerOn = bEconOn;
	if (bEconOn) State.EconomizerOnS += DtS;
	State.bFwsSupplyNotMet = bFwsNotMet;
	if (bFwsNotMet) State.FwsSupplyNotMetS += DtS;
	State.bChillerCapacityLimited = bLimited; State.bChillerOutsideCurves = bOutside;
	if (bLimited) State.ChillerLimitedS += DtS;
	if (bOutside) State.ChillerOutsideCurvesS += DtS;
	SyncFwsState();
}

double FFacilityPlant::GetFanElectricW() const
{
	return IsThermalActive() ? FwsBank.FanElectricW() : 0.0;
}

double FFacilityPlant::GetCwFanElectricW() const
{
	return IsCondenserActive() ? CwBank.FanElectricW() : 0.0;
}

double FFacilityPlant::GetPumpElectricW() const
{
	if (!bBuilt) return 0.0;
	return Loop.GetTotalPumpElectricW() + (bChw ? ChwLoop.GetTotalPumpElectricW() : 0.0) + (bCw ? CwLoop.GetTotalPumpElectricW() : 0.0);
}

double FFacilityPlant::GetStorageJ() const
{
	return Fws.StorageJ() + (bChw ? Chw.StorageJ() : 0.0) + (bCw ? Cw.StorageJ() : 0.0);
}

double FFacilityPlant::GetLedgerResidual() const
{
	const double In = State.DutyInJ + State.CrahInJ + State.CompressorInJ;
	const double Out = State.DryCoolerOutJ + State.CwDryCoolerOutJ;
	const double Scale = FMath::Max3(FMath::Abs(In), FMath::Abs(Out), 1.0);
	return (In - Out - GetStorageJ()) / Scale;
}
