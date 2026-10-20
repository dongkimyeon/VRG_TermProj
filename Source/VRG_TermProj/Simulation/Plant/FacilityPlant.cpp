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
	bChw = false;
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

	if (!Thermal.bFixedSupplyTemp && Thermal.bChilledWater && Crahs.Num() > 0)
	{
		if (!BuildChilledWater(Crahs, Settings)) return false;
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

	// Condenser water design: the CW dry coolers are rated at EFT = air + ITD (48.9 C at 35 C); the chiller's design
	// condenser rise comes from the dataset reference flows (Q_cond_ref / (rho cp V_cond_ref)).
	const double RefCondW = K.RefCapacityW * (1.0 + 1.0 / K.RefCop);
	const double CwRiseK = RefCondW / (WaterRho * WaterCp * FMath::Max(K.RefCondenserFlowM3s, 1e-9));
	const double CwDesignInK = Thermal.DryCooler.RatedAirInK + Thermal.DryCooler.RatedItdK - CwRiseK;
	ChillerMaxCop = Chiller::MaxCopInRange(K);
	// Size the reference capacity so the duty chillers together make exactly the design load at the design point.
	ChillerCapRefW = DesignChwW / (NDuty * FMath::Max(Chiller::Biquadratic(K.CapFt,
		FMath::Clamp(ChwSetpointK - 273.15, K.MinX, K.MaxX), FMath::Clamp(CwDesignInK - 273.15, K.MinY, K.MaxY)), 1e-6));
	bool bOutside = false;
	const double DesignEir = Chiller::FullLoadEir(K, ChwSetpointK - 273.15, CwDesignInK - 273.15, ChillerMaxCop, bOutside);
	DesignCondW = DesignChwW * (1.0 + DesignEir * Chiller::Quadratic(K.EirFplr, 1.0));

	FFacilityLoopSpec CwSpec;
	CwSpec.BranchHxDesignDropPa = Thermal.CondenserDesignDropPa;
	for (int32 I = 0; I < NDuty; ++I) CwSpec.BranchDesignFlowM3s.Add(DesignCondW / NDuty / (WaterRho * WaterCp * CwRiseK));
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
	Chw.SupplyCapJK = ChwTotalM3s * WaterRho * WaterCp * TransitS;
	Chw.ReturnCapJK = ChwTotalM3s * WaterRho * WaterCp * TransitR;
	Chw.Init(ChwSetpointK);
	const double CwDesignKgs = CwLoop.GetDesignTotalFlowM3s() * WaterRho;
	Cw.SupplyCapJK = CwDesignKgs * WaterCp * TransitS;
	Cw.ReturnCapJK = CwDesignKgs * WaterCp * TransitR;
	Cw.FlowWK = CwKgs * WaterCp;
	Cw.Init(Thermal.CwSupplySetpointK);
	bChw = true;
	return true;
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
	int32 Running = 0;
	if (bChw)
	{
		CwBank.Control(DtS, Cw.ReturnK, CwKgs, WaterCp, Thermal.AmbientK);
		const double Request = FMath::Max(Chw.FlowWK * (Chw.ReturnK - ChwSetpointK), 0.0);
		const double UnitCap = Chiller::CapacityW(Thermal.Chiller, ChillerCapRefW, ChwSetpointK - 273.15, Cw.SupplyK - 273.15);
		if (Request > 0.0 && UnitCap > 0.0) Running = FMath::Clamp(FMath::CeilToInt32(Request / UnitCap - 1e-9), 1, FMath::Max(Thermal.DutyChillers, 1));
	}

	// Explicit Euler for all six volumes, sub-stepped below a fifth of the shortest time constant.
	double Tau = Fws.TauS();
	if (bChw) Tau = FMath::Min3(Tau, Chw.TauS(), Cw.TauS());
	const int32 Sub = FMath::Clamp(FMath::CeilToInt32(DtS / (0.2 * Tau)), 1, 1000);
	const double H = DtS / Sub;
	double FwsW = 0.0, CwW = 0.0, LoadW = 0.0, PowerW = 0.0;
	bool bLimited = false, bOutside = false;
	for (int32 K = 0; K < Sub; ++K)
	{
		const double Qf = GetDryCoolerHeatW(FwsBank.Fan, Fws.ReturnK);
		double Qe = 0.0, W = 0.0, Qw = 0.0;
		if (bChw)
		{
			if (Running > 0)
			{
				const FChillerOperatingPoint Op = Chiller::Evaluate(Thermal.Chiller, ChillerCapRefW, ChillerMaxCop, Chw.ReturnK,
					Chw.FlowWK / Running, ChwSetpointK, Cw.SupplyK);
				Qe = Running * Op.LoadW; W = Running * Op.PowerW;
				bLimited |= Op.bCapacityLimited; bOutside |= Op.bOutsideCurves;
			}
			Qw = CwBank.Heat(CwBank.Fan, Cw.ReturnK, CwKgs, WaterCp, Thermal.AmbientK);
			Chw.Advance(H, CrahW, Qe);
			Cw.Advance(H, Qe + W, Qw);   // condenser heat = Q_evap + W, computed once (rule 6)
		}
		Fws.Advance(H, DutyW, Qf);
		State.DutyInJ += DutyW * H; State.DryCoolerOutJ += Qf * H;
		State.CrahInJ += CrahW * H; State.CompressorInJ += W * H; State.CwDryCoolerOutJ += Qw * H;
		FwsW += Qf / Sub; CwW += Qw / Sub; LoadW += Qe / Sub; PowerW += W / Sub;
	}
	FwsBank.HeatW = FwsW; CwBank.HeatW = CwW;
	State.DryCoolerHeatW = FwsW; State.DutyW = DutyW;
	State.CrahDutyW = CrahW; State.ChillerLoadW = LoadW; State.ChillerPowerW = PowerW; State.ChillerCondenserW = LoadW + PowerW;
	State.CwDryCoolerHeatW = CwW; State.ChillersRunning = Running;
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
	return IsChilledWaterActive() ? CwBank.FanElectricW() : 0.0;
}

double FFacilityPlant::GetPumpElectricW() const
{
	if (!bBuilt) return 0.0;
	return Loop.GetTotalPumpElectricW() + (bChw ? ChwLoop.GetTotalPumpElectricW() + CwLoop.GetTotalPumpElectricW() : 0.0);
}

double FFacilityPlant::GetStorageJ() const
{
	return Fws.StorageJ() + (bChw ? Chw.StorageJ() + Cw.StorageJ() : 0.0);
}

double FFacilityPlant::GetLedgerResidual() const
{
	const double In = State.DutyInJ + State.CrahInJ + State.CompressorInJ;
	const double Out = State.DryCoolerOutJ + State.CwDryCoolerOutJ;
	const double Scale = FMath::Max3(FMath::Abs(In), FMath::Abs(Out), 1.0);
	return (In - Out - GetStorageJ()) / Scale;
}
