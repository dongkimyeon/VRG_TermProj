// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
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
