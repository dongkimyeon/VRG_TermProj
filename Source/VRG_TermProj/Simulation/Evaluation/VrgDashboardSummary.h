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
	int32 InletIndex = INDEX_NONE;           // FVrgEvaluation inlet for racks and spines
	bool bAutoPlaced = false;                // automatic CRAH layout (no actor)
};

struct VRG_TERMPROJ_API FVrgDashboardLayout
{
	double SimTimeS = 0.0;
	bool bHasDomain = false;
	double DomainMinXM = 0.0, DomainMinYM = 0.0, DomainMaxXM = 0.0, DomainMaxYM = 0.0;
	TArray<FVrgDashboardEquipment> Equipment;
	TArray<double> InletWindowMeanK;         // by inlet index; empty before the first simulation step
	double RecommendedInletK = 0.0;          // ASHRAE recommended inlet limit used by the evaluation

	FString ToJson() const;
	// The same object, for embedding (stage 5: a report stores the plan view at its SimTime).
	TSharedRef<FJsonObject> ToJsonObject() const;
	static bool FromJson(const FString& Json, FVrgDashboardLayout& Out);
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

	FString RowsToJson() const;
	FString DetailToJson() const;   // empty when bHasDetail is false
	static bool RowsFromJson(const FString& Json, FVrgDashboardRacks& Out);
	static bool DetailFromJson(const FString& Json, FVrgDashboardRackDetail& Out);
};
