// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Sensors/VirtualSensor.h"

// S6 (2026-09-27): what an evaluation run records, step by step on the integrated clock (rule 4).
//   - per rack inlet: a virtual sensor (D9, simulated measurement), time averages over the last WindowS and the whole
//     run, peak, time above the ASHRAE recommended (27 C) and allowable A1 (32 C) inlet limits
//   - power: IT (rack components incl. server fans and PSU losses), CRAH fans, pumps; averaged over the same window
// D8: the "partial PUE" counts only the equipment in the model (IT + CRAH fans + pumps) / IT. Chillers, cooling towers,
// UPS / distribution losses and lighting are not modelled, so it is not a facility PUE. IT = 0 -> not computable.
// Plain C++, no UObject; the integrated actor feeds it after every coupled step.
struct FVrgEvaluationSettings
{
	double WindowS = 100.0;               // averages over the last WindowS (1 s buckets)
	double RecommendedInletK = 273.15 + 27.0;
	double AllowableInletK = 273.15 + 32.0;
	FVirtualSensorSpec Sensor;
	int32 Seed = 1;                       // sensor noise; sensor i uses Seed + i
};

struct FVrgEvaluationSample
{
	double ItW = 0.0, ServerFanW = 0.0, CrahFanW = 0.0, PumpW = 0.0;
};

class VRG_TERMPROJ_API FVrgEvaluation
{
public:
	void Reset(const FVrgEvaluationSettings& InSettings, int32 NumInlets, const TArray<double>& InitialInletK);
	void AfterStep(double SimTimeS, double DtS, const TArray<double>& InletK, const FVrgEvaluationSample& Power);

	struct FInletStats
	{
		double WindowMeanK = 0.0, RunMeanK = 0.0, PeakK = 0.0;
		double SecondsAboveRecommended = 0.0, SecondsAboveAllowable = 0.0;
		double SensorLastK = 0.0, SensorFirstAlarmS = -1.0;
		int32 SensorAlarms = 0;
		bool bSensorAlarm = false;
	};
	FInletStats GetInlet(int32 Index) const;
	int32 GetNumInlets() const { return Inlets.Num(); }
	// Max and mean over inlets of the window-averaged inlet temperature [K] (report summary and web dashboard);
	// false when there are no inlets.
	bool GetInletWindowSummary(double& OutMaxK, double& OutMeanK) const;
	// Window averages of the power categories [W].
	FVrgEvaluationSample GetWindowPower() const;
	// (IT + CRAH fans + pumps) / IT over the window; false when IT is zero.
	bool GetPartialPue(double& OutPue) const;
	double GetSimTimeS() const { return SimTimeS; }
	double GetWindowCoveredS() const;
	const FVrgEvaluationSettings& GetSettings() const { return Settings; }

private:
	// Per-second buckets of time-integrated values; a ring of WindowS buckets gives the last-window average.
	struct FWindowed
	{
		TArray<double> Buckets;     // completed seconds (integral of the value over that second)
		int32 Next = 0, Filled = 0;
		double Current = 0.0;       // integral inside the running second
		double RunIntegral = 0.0;
		void Reset(int32 NumBuckets) { Buckets.Init(0.0, FMath::Max(NumBuckets, 1)); Next = Filled = 0; Current = RunIntegral = 0.0; }
		void Add(double Value, double DtS) { Current += Value * DtS; RunIntegral += Value * DtS; }
		void CloseSecond() { Buckets[Next] = Current; Next = (Next + 1) % Buckets.Num(); Filled = FMath::Min(Filled + 1, Buckets.Num()); Current = 0.0; }
		double WindowIntegral() const { double S = 0.0; for (int32 I = 0; I < Filled; ++I) S += Buckets[I]; return S; }
	};
	struct FInlet
	{
		FWindowed Temperature;
		double PeakK = 0.0, AboveRecommendedS = 0.0, AboveAllowableS = 0.0;
		FVirtualSensor Sensor;
	};
	FVrgEvaluationSettings Settings;
	TArray<FInlet> Inlets;
	FWindowed It, ServerFan, CrahFan, Pump;
	double SimTimeS = 0.0, SecondStartS = 0.0, RunTimeS = 0.0;
	int32 SecondsClosed = 0;
};
