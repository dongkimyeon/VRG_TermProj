// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/FacilityWaterLoop.h"
#include "Simulation/Plant/Chiller.h"
#include "Simulation/Plant/DryCooler.h"
#include "FacilityPlant.generated.h"

// S8 F2/F3 (Tools/설비냉각_계획.md, 2026-10-20): the facility plant outside the room, separate topology.
//
//   FWS  (warm water): CDU primaries  -> FWS dry coolers (fans hold the FWS supply setpoint, W32)
//   CHW  (chilled):    CRAH coils     -> chiller evaporators (staged N+1 chillers hold the CHW supply setpoint)
//   CW   (condenser):  chiller condensers (Q_evap + W) -> CW dry coolers (fans hold the condenser water setpoint)
//   F4: FWS trim chiller in series after the FWS dry coolers (cools what they cannot reach, condenser on the CW loop);
//       water-side economizer = plate exchanger on the CHW return ahead of the chillers, fed by the CW supply ahead of
//       the condensers (integrated / series operation, so it can run together with the chillers), switched with
//       hysteresis; while it runs the CW fans aim below the CHW setpoint.
//
// Every loop is the same pieces: an FFacilityWaterLoop network (pumps N+1, differential-pressure control, branch valves,
// return coils) for its flows and pump power, and two well-mixed water volumes on the integrated clock (rule 4, D2):
//   C_r dT_r/dt = W T_s + In - W T_r,   C_s dT_s/dt = W T_r - Out - W T_s     (W = m cp of the loop)
// so C_s dT_s + C_r dT_r = (In - Out) dt exactly. The heat handed in is the coupling's own value (CDU duties from the
// bridge, CRAH coil heat from the air solver, rule 6); the chiller's condenser heat is Q_evap + W computed once.
// Plant ledger: in = CDU duty + CRAH coil heat + compressor work, out = both dry cooler banks, storage = six volumes.
//
// CRAH coils with finite chilled water (F3): a coil is counterflow epsilon-NTU with C_r > 0. Its UA is re-sized so that
// at the design air and water flows it removes the same design heat as the old infinite-water coil (so the room at
// design is unchanged). The air solver keeps its own coil formula eps = 1 - exp(-UA/C_air) on every path (CPU, GPU,
// octree); the plant hands it the EQUIVALENT UA with the same air-side effectiveness, UA_eq = -C_air ln(1 - eps_air),
// and the chilled water supply temperature. The air side is then exact, and the chilled water return follows from the
// solver's total coil heat (all units share one supply temperature).
// CRAH valve control (F3, 2026-10-20): each CRAH's 2-way valve holds its coil leaving air at a setpoint (design: chilled
// water supply + design approach). Model-based steady inverse from the unit's mixed intake of the last step: the
// required air-side effectiveness -> the chilled water flow giving it (monotonic, bisection) -> the valve opening giving
// that flow at the controlled header differential (Q = sqrt(dp / R_branch), equal-percentage inverse) -> stroke-rate
// limit. The CHW network is then re-solved. Per-unit intake temperatures exist only on the CPU uniform air path; on the
// GPU / octree paths the valves hold their position and the plant says so.

USTRUCT(BlueprintType)
struct FFacilityThermalSpec
{
	GENERATED_BODY()

	// ---- FWS (F2) ----
	// W32 decision (2026-10-20, user): FWS supply 32 C.
	UPROPERTY(EditAnywhere, Category = "Plant") double SupplySetpointK = 305.15;
	// Outdoor dry bulb now and at the design point used to size the dry coolers (35 C: the catalog rating point,
	// decision 2026-10-20; not a site design value).
	UPROPERTY(EditAnywhere, Category = "Plant") double AmbientK = 308.15;
	UPROPERTY(EditAnywhere, Category = "Plant") double DesignAmbientK = 308.15;
	UPROPERTY(EditAnywhere, Category = "Plant") FDryCoolerCatalog DryCooler;
	// 0 = size by the catalog selection rule: units = ceil(design heat / (rated heat * ITD_design / ITD_rated)), with
	// ITD_design = design return - design ambient (manufacturers' tables rate heat per degree of ITD).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0")) int32 DryCoolers = 0;
	// Fan speed range and ramp. Below the EC fans' minimum speed the units cycle; the model takes the time average,
	// so the ratio runs continuously down to 0 (F4; F2 used 0.1 and over-cooled in cold air). Ramp: ESTIMATE.
	UPROPERTY(EditAnywhere, Category = "Plant") double MinFanRatio = 0.0;
	UPROPERTY(EditAnywhere, Category = "Plant") double FanRampPerS = 0.05;
	// Water volume of each side as a transit time at the design flow [s] (no plant piping geometry exists; ESTIMATE).
	UPROPERTY(EditAnywhere, Category = "Plant") double SupplySideTransitS = 60.0;
	UPROPERTY(EditAnywhere, Category = "Plant") double ReturnSideTransitS = 60.0;
	// Comparison option: hydraulics only, every CDU keeps its own fixed facility supply temperature (the old boundary);
	// the chilled water side then stays the old fixed coil water too.
	UPROPERTY(EditAnywhere, Category = "Plant") bool bFixedSupplyTemp = false;

	// ---- CHW + CW (F3) ----
	// Chilled water loop for the CRAH coils. Off = the old fixed coil water temperature.
	UPROPERTY(EditAnywhere, Category = "Plant") bool bChilledWater = true;
	// Design chilled water rise 10 F (5.56 K): Vertiv Liebert PDX/PCW System Design Catalog rates its chilled water
	// units at 45 F EWT / 55 F LWT. The supply setpoint is the integrated actor's CoilSupplyTempK (18 C) at build.
	UPROPERTY(EditAnywhere, Category = "Plant") double ChwDesignRiseK = 10.0 * 5.0 / 9.0;
	UPROPERTY(EditAnywhere, Category = "Plant") FChillerCurves Chiller;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "1")) int32 DutyChillers = 2;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0")) int32 StandbyChillers = 1;
	// Exchanger water-side design pressure drops (evaporator, condenser, CRAH coil) [Pa]. ESTIMATES.
	UPROPERTY(EditAnywhere, Category = "Plant") double EvaporatorDesignDropPa = 5.0e4;
	UPROPERTY(EditAnywhere, Category = "Plant") double CondenserDesignDropPa = 5.0e4;
	UPROPERTY(EditAnywhere, Category = "Plant") double CrahCoilDesignDropPa = 3.0e4;
	// Condenser water: fans hold the chiller's entering condenser water at 85 F (29.44 C), the AHRI 550/590 standard
	// rating condition for water-cooled chillers (ESTIMATE as a control setpoint; original standard not re-checked here).
	UPROPERTY(EditAnywhere, Category = "Plant") double CwSupplySetpointK = 302.594;
	// CRAH valve control on the coil leaving air (see above). Full stroke time 60 s: ESTIMATE (no public figure found
	// in the Liebert PCW catalog, which only says the actuator is managed proportionally).
	UPROPERTY(EditAnywhere, Category = "Plant") bool bCrahValveControl = true;
	UPROPERTY(EditAnywhere, Category = "Plant") double ValveStrokeS = 60.0;

	// ---- F4 ----
	// FWS trim chiller (same curve set as the CHW chillers), sized for the gap the dry coolers leave at the design air.
	UPROPERTY(EditAnywhere, Category = "Plant") bool bTrimChiller = true;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "1")) int32 TrimDutyChillers = 1;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0")) int32 TrimStandbyChillers = 1;
	// Water-side economizer (plate exchanger). Sized at the design flows so the CHW leaves it EconomizerApproachK above
	// the entering CW (plate exchangers are commonly selected for 1-2 K; ESTIMATE). On when the CW supply is more than
	// OnMargin below the CHW return, off when the gap falls under OffMargin (hysteresis; ESTIMATES).
	UPROPERTY(EditAnywhere, Category = "Plant") bool bEconomizer = true;
	UPROPERTY(EditAnywhere, Category = "Plant") double EconomizerApproachK = 2.0;
	UPROPERTY(EditAnywhere, Category = "Plant") double EconomizerOnMarginK = 1.0;
	UPROPERTY(EditAnywhere, Category = "Plant") double EconomizerOffMarginK = 0.3;
};

// One CRAH coil handed to the plant (sized by the integrated actor at its design air flow).
struct FPlantCrahCoil
{
	double AirWK = 0.0;            // air capacity rate rho V cp [W/K]
	double DesignAirEps = 0.0;     // air-side effectiveness of the old infinite-water coil at design
	double DesignDutyW = 0.0;      // AirWK * design air temperature drop
	double OldUAWK = 0.0;          // the infinite-water UA (for reference)
	double CoilLeavingSetpointK = 0.0;   // valve control target (0 = no control for this unit)
};

// Two well-mixed water volumes of one loop (see the header comment).
struct FWaterVolumes
{
	double SupplyCapJK = 0.0, ReturnCapJK = 0.0, FlowWK = 0.0;
	double SupplyK = 0.0, ReturnK = 0.0, InitialSupplyK = 0.0, InitialReturnK = 0.0;
	void Init(double TempK) { SupplyK = ReturnK = InitialSupplyK = InitialReturnK = TempK; }
	void Advance(double H, double InW, double OutW)
	{
		const double Ts = SupplyK, Tr = ReturnK;
		ReturnK = Tr + H / ReturnCapJK * (FlowWK * Ts + InW - FlowWK * Tr);
		SupplyK = Ts + H / SupplyCapJK * (FlowWK * Tr - OutW - FlowWK * Ts);
	}
	double StorageJ() const { return SupplyCapJK * (SupplyK - InitialSupplyK) + ReturnCapJK * (ReturnK - InitialReturnK); }
	double TauS() const { return FlowWK > 0.0 ? FMath::Min(SupplyCapJK, ReturnCapJK) / FlowWK : TNumericLimits<double>::Max(); }
};

// A bank of identical dry coolers at one fan speed that tries to hold its leaving water at a setpoint.
struct FDryCoolerBank
{
	FDryCoolerParams Dc;
	int32 Units = 0;
	double SetpointK = 0.0, MinFan = 0.1, RampPerS = 0.05;
	double Fan = 0.1, Target = 0.1, NotMetS = 0.0, HeatW = 0.0;
	bool bNotMet = false, bOverCooling = false;
	double Heat(double FanRatio, double InK, double TotalKgs, double CpJKgK, double AirK) const
	{
		return Units > 0 ? Units * DryCooler::HeatW(Dc, FanRatio, TotalKgs / Units, CpJKgK, InK, AirK) : 0.0;
	}
	double FanElectricW() const { return Units * DryCooler::FanElectricW(Dc, Fan); }
	// Target speed from the inlet temperature (bisection on the monotonic leaving temperature), then the ramp.
	void Control(double DtS, double InK, double TotalKgs, double CpJKgK, double AirK);
};

struct FFacilityPlantState
{
	// FWS (kept as before for F2 callers)
	double SupplyK = 0.0, ReturnK = 0.0;
	double FanRatio = 0.0, FanTargetRatio = 0.0;
	double DryCoolerHeatW = 0.0, DryCoolerOutK = 0.0, DutyW = 0.0;
	bool bSetpointNotMet = false, bOverCooling = false;
	double SetpointNotMetS = 0.0;
	// CHW / CW (F3)
	double ChwSupplyK = 0.0, ChwReturnK = 0.0, CwSupplyK = 0.0, CwReturnK = 0.0;
	double CrahDutyW = 0.0, ChillerLoadW = 0.0, ChillerPowerW = 0.0, ChillerCondenserW = 0.0, CwDryCoolerHeatW = 0.0;
	int32 ChillersRunning = 0;
	bool bChillerCapacityLimited = false, bChillerOutsideCurves = false;
	int32 CrahValvesFullOpen = 0, CrahValvesShut = 0;
	// F4
	int32 TrimRunning = 0;
	double TrimLoadW = 0.0, TrimPowerW = 0.0, EconomizerW = 0.0, EconomizerOnS = 0.0;
	bool bTrimLimited = false, bEconomizerOn = false, bFwsSupplyNotMet = false;
	int32 EconomizerSwitches = 0;
	double FwsSupplyNotMetS = 0.0;
	bool bCrahReadingsMissing = false;   // valves held: no per-unit intake temperatures on this air path
	double ChillerLimitedS = 0.0, ChillerOutsideCurvesS = 0.0;
	// Plant ledger since the start of the run [J].
	double DutyInJ = 0.0, DryCoolerOutJ = 0.0, CrahInJ = 0.0, CompressorInJ = 0.0, CwDryCoolerOutJ = 0.0;
	double InitialSupplyK = 0.0, InitialReturnK = 0.0;
};

class VRG_TERMPROJ_API FFacilityPlant
{
public:
	// FWS from LoopSpec (F1/F2); with Crahs and InThermal.bChilledWater, also CHW + CW (F3). DesignHeatW = sum of the CDU
	// design duties; ChwSetpointK = the chilled water supply setpoint (the old fixed coil water temperature).
	bool Build(const FFacilityLoopSpec& LoopSpec, const FFacilityThermalSpec& InThermal, double DesignHeatW,
		double WaterDensityKgM3, double WaterCpJKgK, const FHydraulicSolveSettings& Settings,
		const TArray<FPlantCrahCoil>& Crahs = TArray<FPlantCrahCoil>(), double ChwSetpointK = 291.15);
	bool IsBuilt() const { return bBuilt; }
	bool IsThermalActive() const { return bBuilt && !Thermal.bFixedSupplyTemp; }
	bool IsChilledWaterActive() const { return IsThermalActive() && bChw; }
	bool IsCondenserActive() const { return IsThermalActive() && bCw; }
	bool IsTrimActive() const { return IsThermalActive() && bTrim; }
	double GetTrimRefCapacityW() const { return TrimCapRefW; }
	double GetDesignTrimW() const { return DesignTrimW; }
	double GetEconomizerUAWK() const { return HxUA; }

	// CDU inlet temperature for this step (the FWS supply volume at the start of the step).
	double GetSupplyTempK() const { return Fws.SupplyK; }
	// CRAH coil water inlet for this step and the equivalent UA the air solver should use for unit U.
	double GetChwSupplyTempK() const { return Chw.SupplyK; }
	double GetCrahEquivalentUAWK(int32 Unit) const { return CrahEqUA.IsValidIndex(Unit) ? CrahEqUA[Unit] : 0.0; }
	double GetCrahCounterflowUAWK(int32 Unit) const { return CrahCfUA.IsValidIndex(Unit) ? CrahCfUA[Unit] : 0.0; }
	double GetCrahWaterKgs(int32 Unit) const { return CrahWaterKgs.IsValidIndex(Unit) ? CrahWaterKgs[Unit] : 0.0; }
	// CRAH valve control from the units' mixed intake temperatures of the last step (one per CRAH). Returns true when
	// any opening changed (then the CHW network was re-solved and the equivalent UAs updated).
	bool ControlCrahValves(double DtS, const TArray<double>& IntakeMixK);
	void MarkCrahReadingsMissing() { State.bCrahReadingsMissing = true; }
	double GetCrahOpening(int32 Unit) const { return ChwLoop.GetValveOpening(Unit); }
	// Opening that makes the unit's coil leave at TargetK from IntakeK (inverse used by the control; no ramp).
	double CrahOpeningFor(int32 Unit, double IntakeK, double TargetK, bool& bSaturated) const;
	// Air-side effectiveness of a CRAH coil at a water flow [kg/s] (counterflow, UA_cf).
	double CrahAirEffectiveness(int32 Unit, double WaterKgs) const;
	const FFacilityLoopResult& GetChwResult() const { return ChwResult; }
	// Advances the plant over DtS with the CDU duties (one per FWS branch) and the CRAH coils' total heat of this step.
	void Step(double DtS, const TArray<double>& BranchDutyW, double CrahDutyW = 0.0);
	void SetAmbientK(double AmbientK) { Thermal.AmbientK = AmbientK; }
	double GetAmbientK() const { return Thermal.AmbientK; }

	// FWS dry cooler leaving water at fan speed r from the current return temperature (the control's model).
	double DryCoolerOutletK(double FanRatio) const;
	double GetDryCoolerHeatW(double FanRatio, double ReturnK) const;
	double GetFanElectricW() const;                 // FWS dry cooler fans
	double GetCwFanElectricW() const;               // CW dry cooler fans
	double GetPumpElectricW() const;                // FWS + CHW + CW pumps at their operating points
	double GetStorageJ() const;
	// (in - out - storage) / max(in, |out|, 1 J): the plant ledger residual (rule: in, out, sources AND storage).
	double GetLedgerResidual() const;
	double GetBranchMassFlowKgs(int32 Branch) const { return BranchKgs.IsValidIndex(Branch) ? BranchKgs[Branch] : 0.0; }
	double GetTotalMassFlowKgs() const { return TotalKgs; }
	double GetSupplyCapacityJK() const { return Fws.SupplyCapJK; }
	double GetReturnCapacityJK() const { return Fws.ReturnCapJK; }
	int32 GetNumDryCoolers() const { return FwsBank.Units; }
	int32 GetNumCwDryCoolers() const { return CwBank.Units; }
	const FDryCoolerParams& GetDryCoolerParams() const { return FwsBank.Dc; }
	const FFacilityPlantState& GetState() const { return State; }
	const FFacilityThermalSpec& GetThermalSpec() const { return Thermal; }
	const FFacilityWaterLoop& GetLoop() const { return Loop; }
	FFacilityWaterLoop& GetMutableLoop() { return Loop; }
	const FFacilityWaterLoop& GetChwLoop() const { return ChwLoop; }
	const FFacilityWaterLoop& GetCwLoop() const { return CwLoop; }
	const FFacilityLoopResult& GetLoopResult() const { return LoopResult; }
	double GetDesignHeatW() const { return DesignHeatW; }
	double GetDesignChwLoadW() const { return DesignChwW; }
	double GetDesignCondenserW() const { return DesignCondW; }
	double GetChillerRefCapacityW() const { return ChillerCapRefW; }
	double GetEconomizerHeatW(double ChwInK, double CwInK) const { return EconomizerHeatW(ChwInK, CwInK); }
	double GetChillerMaxCop() const { return ChillerMaxCop; }
	double GetWaterCpJKgK() const { return WaterCp; }
	double GetChwSetpointK() const { return ChwSetpointK; }

private:
	FFacilityWaterLoop Loop, ChwLoop, CwLoop;
	FFacilityLoopResult LoopResult, ChwResult, CwResult;
	FFacilityThermalSpec Thermal;
	FFacilityPlantState State;
	FWaterVolumes Fws, Chw, Cw;
	FDryCoolerBank FwsBank, CwBank;
	TArray<double> BranchKgs, CrahWaterKgs, CrahCfUA, CrahEqUA;
	TArray<FPlantCrahCoil> CrahCoils;
	FHydraulicSolveSettings SolveSettings;
	void RefreshChwFlows();
	double TotalKgs = 0.0, WaterCp = 4180.0, WaterRho = 997.0, DesignHeatW = 0.0;
	double ChwKgs = 0.0, CwKgs = 0.0, DesignChwW = 0.0, DesignCondW = 0.0, ChwSetpointK = 291.15;
	double ChillerCapRefW = 0.0, ChillerMaxCop = 0.0;
	double TrimCapRefW = 0.0, DesignTrimW = 0.0, HxUA = 0.0, CwRiseK = 0.0, CwDesignInK = 0.0;
	bool bBuilt = false, bChw = false, bCw = false, bTrim = false, bEconOn = false;
	bool BuildChilledWater(const TArray<FPlantCrahCoil>& Crahs, const FHydraulicSolveSettings& Settings);
	bool BuildCondenser(const FHydraulicSolveSettings& Settings);
	// Economizer heat at the given temperatures (counterflow, CHW return hot side, CW supply cold side), >= 0.
	double EconomizerHeatW(double ChwInK, double CwInK) const;
	void SyncFwsState();
};
