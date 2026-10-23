// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "FacilityWaterLoop.generated.h"

// S8 F1 (Tools/설비냉각_계획.md, 2026-10-20): facility water system (FWS) hydraulics, the CDU primary side.
//
//   suction T (expansion tank, fixed) -> duty / standby pumps -> check valves -> plant supply -> supply main -> S
//   S -> per CDU: supply run -> 2-way control valve -> exchanger primary -> return run -> R
//   R -> return main -> plant return -> dry cooler coils (parallel) -> T
//
// One builder makes the network for every plant topology (only Separate is filled in F1; shared chilled water and the
// series cascade are added in F6 to the same builder, so the equipment models stay identical across topologies).
// The pumps run at one common speed that holds the supply-return header differential pressure at its setpoint
// (outer secant/bisection loop around the D3 Newton solve, D5: normalized residual + iteration cap + failure flag;
// a pump at its speed limit is reported as saturated, never silently accepted).
//
// Sizing rule (same for every topology, D8 fair comparison): every element is sized at the design flows the caller
// hands in, so with every valve fully open and the setpoint met, each CDU branch carries exactly its design flow and
// the pumps run at speed 1. Element pressure drops at design are ESTIMATES except the dry cooler coil (Liebert table).
// Standby pumps are switched off by closing their check-valve pipe (no back-flow through an idle pump; a check valve
// is not a large resistance). Plain C++ (USTRUCT only for editable settings), no actors (rule 5).

UENUM()
enum class EPlantTopology : uint8
{
	Separate = 0,     // FWS warm water + CHW chilled water + CW (default, 2026-10-15 decision)
	SharedChw = 1,    // one chilled water loop serves CDU primaries and CRAH coils in parallel (F6)
	Series = 2,       // CRAH coil return feeds the CDU primaries (cascade, F6)
};

// S8 F6b: where a facility leak sits. Its reach (which branches lose flow, whether the pumps saturate) comes from the
// network solve, never from this label (D4). Supply-side water leaves at the supply temperature, return-side at the
// return temperature (the plant's two volumes).
UENUM()
enum class EPlantLeakSite : uint8
{
	SupplyHeader = 0,   // the supply header node
	ReturnHeader = 1,   // the return header node
	BranchSupply = 2,   // a branch between its supply run and its valve
	BranchReturn = 3,   // a branch between its exchanger and its return run
};

struct FPlantLeak
{
	EPlantLeakSite Site = EPlantLeakSite::SupplyHeader;
	int32 Branch = INDEX_NONE;
	int32 NetworkLeak = INDEX_NONE;
	bool bSupplySide = true;
};

USTRUCT(BlueprintType)
struct FFacilityLoopSpec
{
	GENERATED_BODY()

	// Design flow of every branch [m^3/s] (CDU primaries, CRAH coils, condensers ...). Sets the branch count.
	UPROPERTY(EditAnywhere, Category = "Plant")
	TArray<double> BranchDesignFlowM3s;

	// S8 F6 series (cascade) topology: a second stage of branches between a middle header and the return header, fed by
	// the first stage's return plus a fixed bypass from the supply header that carries the design difference when the
	// second stage needs more flow (sized open at the design point). Empty = one stage (separate and shared topologies).
	UPROPERTY(EditAnywhere, Category = "Plant")
	TArray<double> SecondStageDesignFlowM3s;

	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "1", ClampMax = "16"))
	int32 DutyPumps = 2;

	// N+1 by default (plan: redundancy). A standby pump is built but off until a duty pump fails.
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0", ClampMax = "8"))
	int32 StandbyPumps = 1;

	// Suction (expansion tank) gauge pressure [Pa(g)]. ESTIMATE, the same 2 bar as the CDU loop reference.
	UPROPERTY(EditAnywhere, Category = "Plant")
	double SuctionPressurePa = 2.0e5;

	// Design pressure drops [Pa] at the design flows. ESTIMATES (no public CDU primary-side or piping figure found):
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double BranchHxDesignDropPa = 5.0e4;          // exchanger primary side at its design flow
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double BranchRunDesignDropPa = 2.0e4;      // supply + return runs between the headers and one CDU
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double MainDesignDropPa = 3.0e4;           // supply + return mains between the plant and the headers
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double CheckValveDesignDropPa = 5.0e3;     // one pump's check valve at the pump design flow

	// Control valve design: authority N = dp_valve / (dp_valve + dp_rest of the branch) with the valve fully open at the
	// design flow (0.5 is a common design rule, ESTIMATE), equal-percentage characteristic Kv/Kvs = Rv^(x-1) (the
	// IEC 60534 inherent characteristic shape) with rangeability Rv 50 (typical, ESTIMATE). Opening 0 = shut (closed pipe).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.05", ClampMax = "0.95"))
	double ValveAuthority = 0.5;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "2.0"))
	double ValveRangeability = 50.0;

	// Dry cooler coil: Vertiv Liebert direct-drive drycooler 940 model, 910 L/min at 37 kPa (Technical Design Manual
	// 2023, table 2.1, 40 % ethylene glycol; this model runs water, so the drop is only of the right order).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double ReturnCoilRatedFlowM3s = 910.0 / 60000.0;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double ReturnCoilRatedDropPa = 3.7e4;
	// 0 = as many as the total design flow needs at the rated coil flow (F2 sizes them by heat).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0"))
	int32 ReturnCoils = 0;
	// Standby units (2026-10-22): piped like the duty ones but isolated, so they show where the plant has them. Built
	// after the duty coils / every other branch with their pipe closed (like a standby pump's check valve) and left out of
	// the design total, the pump sizing and the differential-pressure setpoint, so they change no solved flow.
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0"))
	int32 StandbyReturnCoils = 0;
	UPROPERTY(EditAnywhere, Category = "Plant")
	TArray<double> StandbyBranchDesignFlowM3s;
	// S8 F4 water-side economizer, one side of the plate exchanger in series in a main (2026-10-22: the exchanger the
	// plant's heat balance already passes the whole loop flow through). Design drop at the design total flow [Pa];
	// 0 = none. Return side: return header -> return main -> exchanger -> plant return (chilled water, before the
	// evaporators). Supply side: plant supply -> exchanger -> supply main -> supply header (condenser water, before the
	// condensers). Counted in the pump design head; always flowing (no bypass valve: limitation).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double SeriesHxDesignDropPa = 0.0;
	UPROPERTY(EditAnywhere, Category = "Plant")
	bool bSeriesHxOnSupply = false;

	// Pump curve: shutoff head / design head (centrifugal pumps commonly 1.1-1.3; ESTIMATE 1.25) and the combined
	// pump + motor efficiency used for the electric input at the operating point (D8; ESTIMATE, same as the CDU pump).
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "1.01"))
	double PumpShutoffToDesignHead = 1.25;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	double PumpEfficiency = 0.7;
	// Variable-speed drive range of the differential-pressure control.
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.0"))
	double MinPumpSpeed = 0.2;
	UPROPERTY(EditAnywhere, Category = "Plant", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	double MaxPumpSpeed = 1.2;

	EPlantTopology Topology = EPlantTopology::Separate;
};

// Indices into the FWS network (rule 7: index graph, no pointers).
struct FFacilityLoopHandles
{
	int32 SuctionNode = INDEX_NONE, PlantSupplyNode = INDEX_NONE, SupplyHeaderNode = INDEX_NONE;
	int32 MiddleNode = INDEX_NONE, BypassPipe = INDEX_NONE, NumFirstStage = 0;   // F6 series topology
	int32 ReturnHeaderNode = INDEX_NONE, PlantReturnNode = INDEX_NONE;
	TArray<int32> Pumps, PumpCheckPipes;             // duty pumps first, then standby
	int32 SupplyMainPipe = INDEX_NONE, ReturnMainPipe = INDEX_NONE;
	TArray<int32> BranchSupplyRunPipes, ValvePipes, HxPipes, BranchReturnRunPipes;   // per CDU branch
	TArray<int32> ReturnCoilPipes;
	int32 NumStandbyBranches = 0, NumStandbyCoils = 0;   // the last ones of the branches / return coils (isolated)
	int32 SeriesHxPipe = INDEX_NONE, SeriesHxNode = INDEX_NONE;   // economizer side and its node on the main side
};

// Result of one controlled solve (D5).
struct FFacilityLoopResult
{
	FHydraulicSolveResult Hydraulics;
	double PumpSpeedRatio = 0.0;
	double HeaderDeltaPPa = 0.0;          // achieved supply - return header differential (piezometric) [Pa]
	double SetpointPa = 0.0;
	double ControlResidual = 0.0;         // |dp - setpoint| / setpoint
	int32 ControlIterations = 0;
	bool bControlConverged = false;
	bool bSpeedSaturatedHigh = false;     // at MaxPumpSpeed and still below the setpoint ("differential pressure not met")
	bool bSpeedSaturatedLow = false;      // at MinPumpSpeed and still above the setpoint (bypass would be needed)
	bool bNoPumpRunning = false;
};

class VRG_TERMPROJ_API FFacilityWaterLoop
{
public:
	// Builds the network from the spec (replaces any previous one). Returns false for an empty / invalid spec.
	bool Build(const FFacilityLoopSpec& InSpec);
	bool IsBuilt() const { return bBuilt; }

	// Valve opening x in [0, 1] (0 = shut) of a CDU branch. Takes effect at the next Solve.
	void SetValveOpening(int32 Branch, double Opening);
	double GetValveOpening(int32 Branch) const { return Openings.IsValidIndex(Branch) ? Openings[Branch] : 0.0; }
	// Duty / standby state of a pump (failure or switchover). An off pump's check valve is closed.
	void SetPumpRunning(int32 Pump, bool bRunning);
	bool IsPumpRunning(int32 Pump) const { return Running.IsValidIndex(Pump) && Running[Pump]; }
	// F6b: a pump trips (marked failed, its check valve closes) and the first idle, healthy pump starts in its place
	// (automatic standby switchover, instantaneous: ESTIMATE). Returns the pump that started or INDEX_NONE (no standby
	// left). Takes effect at the next Solve.
	int32 TripPump(int32 Pump);
	bool IsPumpTripped(int32 Pump) const { return Tripped.IsValidIndex(Pump) && Tripped[Pump]; }
	int32 GetNumRunningPumps() const;
	// F6b leaks (orifice Cd A sqrt(2 dp / rho) at the node's gauge pressure, S2b model). The suction stays at its fixed
	// pressure: the make-up (pressurisation) system replaces whatever leaks, without a rate limit (limitation).
	// Returns the leak index or INDEX_NONE (bad site / branch). Takes effect at the next Solve.
	int32 AddLeak(EPlantLeakSite Site, int32 Branch, double HoleDiameterM, double DischargeCoefficient = 0.61);
	void SetLeakDiameterM(int32 Leak, double HoleDiameterM);
	// W1: the node a site's orifice sits on (INDEX_NONE for a bad site / branch), and the leak already at a site.
	int32 GetLeakSiteNode(EPlantLeakSite Site, int32 Branch) const;
	int32 FindLeak(EPlantLeakSite Site, int32 Branch) const;
	// Every hole to zero diameter (the orifices stay so indices hold; a zero hole adds exactly nothing to the solve).
	void ClearLeaks();
	int32 GetNumLeaks() const { return LoopLeaks.Num(); }
	const FPlantLeak& GetLeak(int32 Leak) const { return LoopLeaks[Leak]; }
	double GetLeakFlowM3s(int32 Leak) const;
	double GetSupplySideLeakM3s() const;
	double GetReturnSideLeakM3s() const;
	bool HasOpenLeak() const;
	void SetDifferentialSetpointPa(double SetpointPa) { DpSetpointPa = SetpointPa; }
	double GetDifferentialSetpointPa() const { return DpSetpointPa; }

	// Solves with the pumps' common speed holding the header differential pressure (D3 Newton inside, D5 outside).
	FFacilityLoopResult Solve(const FHydraulicSolveSettings& Settings, double ControlTolerance = 1.0e-9, int32 MaxControlIterations = 80);
	// Solves at a fixed common pump speed (no control), e.g. for analytic checks.
	FHydraulicSolveResult SolveAtSpeed(const FHydraulicSolveSettings& Settings, double SpeedRatio);

	const FFacilityLoopSpec& GetSpec() const { return Spec; }
	const FFacilityLoopHandles& GetHandles() const { return Handles; }
	const FHydraulicNetwork& GetNetwork() const { return Network; }
	FHydraulicNetwork& GetMutableNetwork() { return Network; }
	int32 GetNumBranches() const { return Handles.ValvePipes.Num(); }
	// Primary flow through a CDU branch [m^3/s] (the valve pipe, >= 0 in normal operation).
	double GetBranchFlowM3s(int32 Branch) const;
	// Piezometric drop across a branch (supply-run start to return-run end) at the last solve [Pa]. Equals the header
	// differential for a one-stage loop; in the series topology each stage sees its own part.
	double GetBranchDeltaPPa(int32 Branch) const;
	double GetBypassFlowM3s() const { return Handles.BypassPipe != INDEX_NONE ? Network.GetPipeFlowM3s(Handles.BypassPipe) : 0.0; }
	double GetTotalPumpFlowM3s() const;
	double GetTotalPumpElectricW() const;
	double GetTotalPumpHydraulicW() const;   // dp * Q of the running pumps (becomes heat in the water)
	// Branch resistance at an opening (runs + valve + exchanger) [Pa s^2/m^6]; infinite when shut.
	double BranchResistancePaS2M6(int32 Branch, double Opening) const;
	// Valve resistance at an opening; R(x) = R_open * Rv^(2 (1 - x)) (equal percentage: Kv ~ Rv^(x-1), R ~ 1/Kv^2).
	double ValveResistancePaS2M6(int32 Branch, double Opening) const;
	double GetPumpShutoffHeadPa() const { return PumpShutoffPa; }
	double GetPumpCurveCoeffPaS2M6() const { return PumpCurveCoeff; }
	double GetDesignPumpHeadPa() const { return DesignPumpHeadPa; }
	double GetDesignTotalFlowM3s() const { return DesignTotalFlowM3s; }

private:
	FFacilityLoopSpec Spec;
	FFacilityLoopHandles Handles;
	FHydraulicNetwork Network;
	TArray<double> Openings;
	TArray<bool> Running, Tripped;
	TArray<double> ValveOpenResistance;
	TArray<FPlantLeak> LoopLeaks;
	double DpSetpointPa = 0.0;
	double PumpShutoffPa = 0.0, PumpCurveCoeff = 0.0, DesignPumpHeadPa = 0.0, DesignTotalFlowM3s = 0.0;
	bool bBuilt = false;
	void ApplyValve(int32 Branch);
	double HeaderDeltaPa() const;
};
