// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/FacilityWaterLoop.h"
#include "Simulation/Plant/DryCooler.h"
#include "FacilityPlant.generated.h"

// S8 F2 (Tools/설비냉각_계획.md, 2026-10-20): facility water (FWS) heat and the dry coolers.
//
// The FWS water is two well-mixed volumes on the integrated clock (rule 4, D2: transient, never overwritten by a
// steady value): the supply side (dry cooler outlet -> pumps -> mains -> CDU inlets) at T_s and the return side
// (CDU outlets -> mains -> dry cooler inlet) at T_r. Every CDU exchanger sees T_s and hands back its duty, the SAME
// value the rack side lost (rule 6, the coupling bridge computes it once). With m = sum of the branch mass flows:
//   C_r dT_r/dt = m cp T_s + sum(Duty) - m cp T_r
//   C_s dT_s/dt = m cp T_r - Q_dc - m cp T_s            (Q_dc = dry coolers' heat to the outdoor air)
// so C_s dT_s + C_r dT_r = (sum Duty - Q_dc) dt exactly: the plant ledger (in = duty, out = dry coolers, storage =
// the two volumes) closes to round-off by construction (explicit Euler, sub-stepped below a fifth of the shortest
// volume time constant). Pump shaft heat into the water and the global ledger out to the outdoor air come in F5.
//
// Control: the dry cooler fans (all units at one speed) hold the dry cooler leaving water at the supply setpoint.
// The target speed solves T_out(r) = setpoint from the current return temperature (monotonic in r, bisection); the
// applied speed follows it at a limited ramp rate. A target above full speed is reported as "setpoint not met"
// (the trim chiller of F3 takes that part); below the minimum speed the fans stay at the minimum (over-cooling flag).
// The fixed-temperature boundary of the old model stays available as a comparison option (bFixedSupplyTemp).

USTRUCT(BlueprintType)
struct FFacilityThermalSpec
{
	GENERATED_BODY()

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
	// Fan speed range and ramp (EC fans; ESTIMATES).
	UPROPERTY(EditAnywhere, Category = "Plant") double MinFanRatio = 0.1;
	UPROPERTY(EditAnywhere, Category = "Plant") double FanRampPerS = 0.05;
	// Water volume of each side as a transit time at the design flow [s] (no plant piping geometry exists; ESTIMATE).
	UPROPERTY(EditAnywhere, Category = "Plant") double SupplySideTransitS = 60.0;
	UPROPERTY(EditAnywhere, Category = "Plant") double ReturnSideTransitS = 60.0;
	// Comparison option: hydraulics only, every CDU keeps its own fixed facility supply temperature (the old boundary).
	UPROPERTY(EditAnywhere, Category = "Plant") bool bFixedSupplyTemp = false;
};

struct FFacilityPlantState
{
	double SupplyK = 0.0, ReturnK = 0.0;
	double FanRatio = 0.0, FanTargetRatio = 0.0;
	double DryCoolerHeatW = 0.0, DryCoolerOutK = 0.0, DutyW = 0.0;
	bool bSetpointNotMet = false, bOverCooling = false;
	double SetpointNotMetS = 0.0;
	// Plant ledger since the start of the run [J].
	double DutyInJ = 0.0, DryCoolerOutJ = 0.0;
	double InitialSupplyK = 0.0, InitialReturnK = 0.0;
};

class VRG_TERMPROJ_API FFacilityPlant
{
public:
	// Hydraulics (F1) from LoopSpec, then the dry coolers and the water volumes. DesignHeatW = sum of the CDU design
	// duties (sizing). WaterDensity / Cp: the facility water (the caller's single sources). Starts at the setpoint.
	bool Build(const FFacilityLoopSpec& LoopSpec, const FFacilityThermalSpec& InThermal, double DesignHeatW,
		double WaterDensityKgM3, double WaterCpJKgK, const FHydraulicSolveSettings& Settings);
	bool IsBuilt() const { return bBuilt; }
	bool IsThermalActive() const { return bBuilt && !Thermal.bFixedSupplyTemp; }

	// CDU inlet temperature for this step (the supply volume at the start of the step).
	double GetSupplyTempK() const { return State.SupplyK; }
	// Advances the plant over DtS with the CDU duties the coupling computed in this step (one per branch).
	void Step(double DtS, const TArray<double>& BranchDutyW);
	void SetAmbientK(double AmbientK) { Thermal.AmbientK = AmbientK; }
	double GetAmbientK() const { return Thermal.AmbientK; }

	// Dry cooler leaving water at fan speed r from the current return temperature (the control's model).
	double DryCoolerOutletK(double FanRatio) const;
	double GetDryCoolerHeatW(double FanRatio, double ReturnK) const;
	double GetFanElectricW() const;
	double GetStorageJ() const;
	// (in - out - storage) / max(in, |out|, 1 J): the plant ledger residual (rule: in, out, sources AND storage).
	double GetLedgerResidual() const;
	double GetBranchMassFlowKgs(int32 Branch) const { return BranchKgs.IsValidIndex(Branch) ? BranchKgs[Branch] : 0.0; }
	double GetTotalMassFlowKgs() const { return TotalKgs; }
	double GetSupplyCapacityJK() const { return SupplyCapJK; }
	double GetReturnCapacityJK() const { return ReturnCapJK; }
	int32 GetNumDryCoolers() const { return NumDryCoolers; }
	const FDryCoolerParams& GetDryCoolerParams() const { return Dc; }
	const FFacilityPlantState& GetState() const { return State; }
	const FFacilityThermalSpec& GetThermalSpec() const { return Thermal; }
	const FFacilityWaterLoop& GetLoop() const { return Loop; }
	FFacilityWaterLoop& GetMutableLoop() { return Loop; }
	const FFacilityLoopResult& GetLoopResult() const { return LoopResult; }
	double GetDesignHeatW() const { return DesignHeatW; }
	double GetWaterCpJKgK() const { return WaterCp; }

private:
	FFacilityWaterLoop Loop;
	FFacilityLoopResult LoopResult;
	FFacilityThermalSpec Thermal;
	FDryCoolerParams Dc;
	FFacilityPlantState State;
	TArray<double> BranchKgs;
	double TotalKgs = 0.0, WaterCp = 4180.0, SupplyCapJK = 0.0, ReturnCapJK = 0.0, DesignHeatW = 0.0;
	int32 NumDryCoolers = 0;
	bool bBuilt = false;
	double SolveFanTarget() const;
};
