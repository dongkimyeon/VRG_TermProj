// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Simulation/Evaluation/VrgEvaluation.h"

// Web dashboard stage 1 (2026-10-13): the summary card values, copied on the game thread (rule 5) and served as JSON.
// Units stay SI (W, K, s; rule 1/2); the page converts to kW and C. Window values reuse FVrgEvaluation (same as the
// report); "now" values are the coupling snapshot at the same SimTime. Plain C++, no UObject.
struct VRG_TERMPROJ_API FVrgDashboardSummary
{
	double SimTimeS = 0.0;
	bool bSimulating = false;     // false in layout mode (physics paused)
	bool bEvalReady = false;      // at least one coupled step since entering simulation
	double WindowS = 0.0;         // seconds actually covered by the window averages
	double ItWindowW = 0.0;       // window average, IT incl. server fans and PSU losses (D8)
	double ItNowW = 0.0;
	double CoolantRemovedNowW = 0.0;   // radiators + CDU exchangers to facility water
	double AirRemovedNowW = 0.0;       // CRAH coils / ceiling coil
	bool bPartialPueValid = false;
	double PartialPueWindow = 0.0;     // (IT + CRAH fans + pumps) / IT, not a facility PUE (D8)
	int32 InletCount = 0;
	double InletWindowMeanMaxK = 0.0;
	double InletWindowMeanAvgK = 0.0;

	FString ToJson() const;
	static bool FromJson(const FString& Json, FVrgDashboardSummary& Out);
};

// Web dashboard stage 2 (2026-10-13): timed event markers (vrg.Eval.Event) drawn as vertical lines on the series.
struct FVrgDashboardEvent
{
	double TimeS = 0.0;
	FString Kind;
	int32 Index = INDEX_NONE;
	double Value = 0.0;
	bool bApplied = false;
};

// Series payload copied on the game thread (rule 5); the request handler serializes only points after SinceS.
struct VRG_TERMPROJ_API FVrgDashboardSeries
{
	int32 Generation = 0;      // FVrgEvaluation::GetGeneration(); changes when the run restarts
	double SimTimeS = 0.0;
	double LengthS = 0.0;      // kept history [s], one point per second
	TArray<FVrgSeriesPoint> Points;
	TArray<FVrgDashboardEvent> Events;

	FString ToJson(double SinceS) const;
	static bool FromJson(const FString& Json, FVrgDashboardSeries& Out);
};

// Web dashboard stage 3 (2026-10-13): plan view. One footprint per equipment box seen from above, world metres
// (rule 1: the cm -> m conversion happens when the actor builds this copy). Inlet colours come from the evaluation's
// window means (the report's values) by inlet index.
struct FVrgDashboardEquipment
{
	FString Kind;              // rack, spine, cdu, crah, barrier
	FString Name;
	double CenterXM = 0.0, CenterYM = 0.0;   // footprint centre (actor origin = bottom centre)
	double YawDeg = 0.0;                     // local +X (depth / CRAH supply direction) in world XY
	double DepthM = 0.0, WidthM = 0.0;       // local X / Y extents
	double BaseZM = 0.0, HeightM = 0.0;      // bottom height and Z extent (3D view, 2026-10-13)
	int32 InletIndex = INDEX_NONE;           // FVrgEvaluation inlet for racks and spines
	bool bAutoPlaced = false;                // automatic CRAH layout (no actor)
};

// 3D view stage 2 (2026-10-14): one drawn coolant pipe = the room actor's mesh path for a solved network pipe.
// Flow and temperature are this step's solved values (SI: m^3/s, K); no pipe thermal lag exists in the model, so a
// branch shows its rack's inlet (supply) or outlet (return) temperature and headers/pump links have none (unknown).
struct FVrgDashboardPipe
{
	int32 Edge = INDEX_NONE;          // network pipe index, or -1 - pump for the CDU -> pump link
	bool bSupply = false, bBranch = false;
	double FlowM3s = 0.0;             // signed along PointsM (positive = first point to last)
	double DiameterM = 0.0;
	bool bTempValid = false;
	double TempK = 0.0;
	TArray<FVector> PointsM;          // world metres
};

struct VRG_TERMPROJ_API FVrgDashboardLayout
{
	double SimTimeS = 0.0;
	bool bHasDomain = false;
	double DomainMinXM = 0.0, DomainMinYM = 0.0, DomainMaxXM = 0.0, DomainMaxYM = 0.0;
	double DomainMinZM = 0.0, DomainMaxZM = 0.0;
	TArray<FVrgDashboardEquipment> Equipment;
	TArray<double> InletWindowMeanK;         // by inlet index; empty before the first simulation step
	double RecommendedInletK = 0.0;          // ASHRAE recommended inlet limit used by the evaluation
	TArray<FVrgDashboardPipe> Pipes;         // empty without a room actor (optional in JSON: older reports lack it)
	bool bHydraulicsConverged = false;

	FString ToJson() const;
	// The same object, for embedding (stage 5: a report stores the plan view at its SimTime).
	TSharedRef<FJsonObject> ToJsonObject() const;
	static bool FromJson(const FString& Json, FVrgDashboardLayout& Out);
};

class FAirSolver;

// 3D view stage 3 (2026-10-14): airflow layer. Cell-centre velocities of the uniform air grid (mean of the two MAC faces
// per axis, FAirSolver::CellCenterVelocityMs) sampled every StrideCells cells in X and Y on the cell layers nearest to
// the requested heights. Solid cells are skipped. World metres and m/s (rule 1); a sample, not a streamline.
struct VRG_TERMPROJ_API FVrgDashboardAirflow
{
	double SimTimeS = 0.0;
	bool bSimulating = false;
	bool bOctree = false;              // octree path: the uniform grid is not the solved field, so no samples are sent
	double CellM = 0.0;
	int32 StrideCells = 0;
	TArray<double> LayerHeightsM;      // actual cell-centre heights sampled (world Z, m)
	TArray<FVector> PositionsM;        // cell centres, world m
	TArray<FVector> VelocitiesMs;
	double MaxSpeedMs = 0.0;           // over the samples

	// OriginM = world position of the grid's (0,0,0) corner. Heights outside the grid are dropped; duplicates merged.
	static FVrgDashboardAirflow Sample(const FAirSolver& Solver, const FVector& OriginM, int32 StrideCells, const TArray<double>& HeightsM);
	FString ToJson() const;            // direct writer (a few thousand numbers)
	static bool FromJson(const FString& Json, FVrgDashboardAirflow& Out);
};

// 3D view stage 4 (2026-10-14): sensor layer. One virtual inlet sensor per evaluation inlet (S6, D9): a simulated
// measurement with a first-order lag, sampling period, Gaussian noise, rounding and an alarm with hysteresis. Each row
// carries the reading next to the true inlet air of the last step so the page can show the lag/error, never only the
// reading as if it were exact. Positions come from the plan view (inlet index); SI units (K, s).
struct FVrgDashboardSensor
{
	int32 Inlet = INDEX_NONE;
	double AirNowK = 0.0, ElementK = 0.0;
	bool bHasReading = false;
	double ReadingK = 0.0, ReadingTimeS = -1.0;
	int32 Readings = 0;
	bool bAlarm = false;
	double FirstAlarmS = -1.0;
	int32 Alarms = 0;
};

struct VRG_TERMPROJ_API FVrgDashboardSensors
{
	double SimTimeS = 0.0;
	bool bSimulating = false;
	bool bEvalReady = false;
	FVirtualSensorSpec Spec;
	TArray<FVrgDashboardSensor> Sensors;

	static FVrgDashboardSensors FromEvaluation(const FVrgEvaluation& Eval);
	FString ToJson() const;
	static bool FromJson(const FString& Json, FVrgDashboardSensors& Out);
};

// Web dashboard tab 4 (2026-10-14): cooling plant and power. Every value is one the last step already computed
// (coupling bridge, air solver duct record, hydraulic solution, CDU sizing on the actor); nothing new is modelled.
// SI units (W, K, m^3/s, kg/s, Pa, W/K). Copied on the game thread, JSON built per request (rule 5).
struct FVrgDashboardCrah
{
	FString Kind;                 // inrow (rear intake) / perimeter (top intake)
	bool bPlaced = false;         // ACrahUnitActor in the level, else the automatic layout
	double XM = 0.0, YM = 0.0;    // bottom centre, world [m]
	double FlowM3s = 0.0, UAWK = 0.0, FanW = 0.0, WaterSupplyK = 0.0;
	bool bHasAir = false;         // per-unit air values recorded (CPU uniform air path only)
	double RemovedW = 0.0, IntakeMixK = 0.0, SupplyK = 0.0;
};

struct FVrgDashboardCdu
{
	FString Name;
	int32 NumRacks = 0;
	double DesignDutyW = 0.0, UAWK = 0.0, FacilityFlowKgs = 0.0, FacilityCpJKgK = 0.0, FacilitySupplyK = 0.0;
	double DutyW = 0.0, ReturnK = 0.0, SupplyK = 0.0, FacilityReturnK = 0.0, HotWK = 0.0, CoolantKgs = 0.0;
};

struct FVrgDashboardPump
{
	int32 Row = INDEX_NONE;
	FString Cdu;                  // empty: virtual loop of a row without a CDU (racks use their own radiators)
	double FlowM3s = 0.0, HeadPa = 0.0, ElectricW = 0.0, HydraulicW = 0.0;
	int32 Branches = 0;
	double BranchMinM3s = 0.0, BranchMaxM3s = 0.0, BranchMeanM3s = 0.0;
};

struct VRG_TERMPROJ_API FVrgDashboardCooling
{
	double SimTimeS = 0.0;
	bool bSimulating = false;
	bool bCoupled = false;                 // coupling built (simulation mode entered at least once)
	FString CrahMode;                      // units / ceilingCoil
	bool bCrahPerUnit = false;             // per-unit air values available
	double AirRemovedW = 0.0, CrahCoilRemovedW = 0.0, CoolantRemovedW = 0.0, RadiatorRemovedW = 0.0;
	int32 RadiatorRacks = 0;               // racks on their own radiator (no CDU in their row)
	TArray<FVrgDashboardCrah> Crahs;
	TArray<FVrgDashboardCdu> Cdus;
	bool bHydraulics = false, bHydConverged = false, bHydSparse = false;
	int32 HydIterations = 0;
	double HydResidual = 0.0;
	TArray<FVrgDashboardPump> Pumps;
	// Power of the last step [W] (D8): IT = parts + server fans + PSU conversion loss; CRAH fans and pumps are electric inputs.
	double ItW = 0.0, ServerFanW = 0.0, PsuLossW = 0.0, CrahFanW = 0.0, PumpW = 0.0;
	bool bPartialPueValid = false;
	double PartialPue = 0.0;               // (IT + CRAH fans + pumps) / IT of the last step, not a facility PUE

	FString ToJson() const;
	static bool FromJson(const FString& Json, FVrgDashboardCooling& Out);
};

// Web dashboard stage 4 (2026-10-13): rack tab. One row per evaluation inlet (racks, then spines), from the thermal
// model's state and last-step totals; SI units (W, K, kg/s, m^3/s). Built cheaply every publish (no snapshots).
struct FVrgDashboardRackRow
{
	int32 Inlet = INDEX_NONE;            // evaluation inlet index = plan view's InletIndex
	FString Kind;                        // rack, spine
	FString Name;
	double XM = 0.0, YM = 0.0;
	double Utilization = 1.0;
	double HeatW = 0.0;                  // last step, IT incl. server fans and PSU losses (D8)
	double ToCoolantW = 0.0, ToRoomAirW = 0.0;   // last step: coolant / room air (forced air + case convection, D1)
	double InletNowK = 0.0;              // air inlet the coupling handed in this step
	double CoolantInK = 0.0, CoolantOutK = 0.0, CoolantFlowM3s = 0.0;
	double FanSpeedMean = 0.0;           // mean over slots with fans
	double FanPowerW = 0.0;
	double HottestPartK = 0.0;
	FString HottestPart;                 // "<slot> <part>"
	int32 DockedSlots = 0, NumSlots = 0;
};

struct FVrgDashboardRackPart
{
	FString Name, Kind;
	double TempK = 0.0, HeatW = 0.0, ToCoolantW = 0.0, ToAirW = 0.0;
};

struct FVrgDashboardRackSlot
{
	int32 Index = 0;
	FString Device;
	double BottomZM = 0.0, HeightM = 0.0;
	bool bDocked = true;
	double InletK = 0.0, HeatW = 0.0, HeatToAirW = 0.0, AirFlowKgs = 0.0, OutletAirK = 0.0, FanSpeed = 0.0;
	TArray<FVrgDashboardRackPart> Parts;
};

// The selected rack only (MakeSnapshot is a few thousand values, too heavy to copy for every rack each publish).
struct FVrgDashboardRackDetail
{
	int32 Inlet = INDEX_NONE;
	double TotalHeatW = 0.0, ToCoolantW = 0.0, ToAirW = 0.0, ToAmbientW = 0.0;   // removal split (forced air / case)
	double MixedAirOutletK = 0.0, CoolantOutletK = 0.0;
	TArray<FVrgDashboardRackSlot> Slots;
};

struct VRG_TERMPROJ_API FVrgDashboardRacks
{
	double SimTimeS = 0.0;
	bool bSimulating = false;
	TArray<FVrgDashboardRackRow> Rows;
	bool bHasDetail = false;
	FVrgDashboardRackDetail Detail;

	// Written straight into one string (2026-10-13): building an FJsonObject tree for the 613-part detail cost ~4 ms
	// on the game thread per request (vrg.Web.ProfileLayout). Numbers use std::to_chars (shortest exact round-trip).
	FString RowsToJson() const;
	FString DetailToJson() const;   // empty when bHasDetail is false
	// The earlier FJsonObject versions, kept as the reference the fast ones are tested against and for the profile.
	FString RowsToJsonDom() const;
	FString DetailToJsonDom() const;
	static bool RowsFromJson(const FString& Json, FVrgDashboardRacks& Out);
	static bool DetailFromJson(const FString& Json, FVrgDashboardRackDetail& Out);
};
