// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// W5-2 (Tools/누수_실시간_계획.md L5 (가), 2026-11-03): process-value leak monitoring. "Somewhere in this loop water is
// leaving", from the loop's instruments only - never the solve's leak flow (D9: readings with their sampling, lag, noise
// and resolution). Vendor practice (Watts, Icon Process Controls, TI): reservoir level trend, make-up water use, supply /
// return flow mismatch and pressure loss are the system-level layer; they do not locate the leak (contact detectors do).
// NVIDIA's Mission Control exports CDU pressure / flow as monitoring metrics but does not document them as leak inputs,
// so this layer follows the vendor sources. Every threshold and hold time is an ESTIMATE; a leak smaller than the
// instruments can resolve stays undetected here, and that is a result, not an error.
//
// One monitored loop = one CDU secondary loop (all four signals) or one facility loop (make-up only). A missing signal is
// NaN and is skipped. Plain C++, SI units (rule 1), game thread.

enum class EProcessLeakSignal : uint8 { LevelDrop, Makeup, FlowMismatch, PressureDrop };

struct VRG_TERMPROJ_API FProcessLeakSettings
{
	double LevelWindowS = 60.0;          // level drop measured from the highest reading in this window
	double LevelDropM3 = 0.2e-3;         // 0.2 L: about four level-noise sigmas of a 24 L vessel (ESTIMATE)
	double MakeupM3s = 2.0e-6;           // 2 mL/s of make-up water flowing (ESTIMATE)
	double MismatchFraction = 0.03;      // |supply - return| / design flow (meter noise 0.5 % each, ESTIMATE)
	double PressureDropPa = 20.0e3;      // below the loop's first reading (ESTIMATE)
	double HoldS = 3.0;                  // a condition must hold this long (ESTIMATE)
	double EvaluatePeriodS = 1.0;        // evaluated at the instruments' polling period (readings change only then)
};

// The readings of one loop at one evaluation (NaN = no such instrument / no reading yet).
struct VRG_TERMPROJ_API FProcessLeakReading
{
	double LevelM3 = NAN, MakeupM3s = NAN, SupplyM3s = NAN, ReturnM3s = NAN, PressurePa = NAN;
};

struct VRG_TERMPROJ_API FProcessLeakAlarm
{
	int32 Loop = INDEX_NONE;
	EProcessLeakSignal Signal = EProcessLeakSignal::LevelDrop;
	double TimeS = 0.0;
	double Value = 0.0;   // level drop [m^3], make-up [m^3/s], mismatch [-], pressure drop [Pa] at the alarm
};

class VRG_TERMPROJ_API FProcessLeakMonitor
{
public:
	// DesignFlowM3s per loop scales the mismatch (0 = no mismatch check for that loop).
	void Reset(const TArray<FString>& LoopNames, const TArray<double>& DesignFlowM3s, const FProcessLeakSettings& InSettings = FProcessLeakSettings());
	// One evaluation at SimTimeS with each loop's readings. Returns the alarms raised now (each signal latches per loop).
	TArray<FProcessLeakAlarm> Step(double SimTimeS, const TArray<FProcessLeakReading>& Readings);
	int32 ResetAlarms();   // clears every latch (a condition still present raises it again after HoldS)

	int32 NumLoops() const { return Loops.Num(); }
	const FString& GetLoopName(int32 Loop) const { return Loops[Loop].Name; }
	bool IsAlarm(int32 Loop, EProcessLeakSignal Signal) const { return Loops.IsValidIndex(Loop) && Loops[Loop].AlarmS[uint8(Signal)] >= 0.0; }
	double GetAlarmTimeS(int32 Loop, EProcessLeakSignal Signal) const { return Loops.IsValidIndex(Loop) ? Loops[Loop].AlarmS[uint8(Signal)] : -1.0; }
	const TArray<FProcessLeakAlarm>& GetAlarmLog() const { return AlarmLog; }
	const FProcessLeakSettings& GetSettings() const { return Settings; }
	static const TCHAR* SignalName(EProcessLeakSignal Signal);

private:
	struct FLoop
	{
		FString Name;
		double DesignFlowM3s = 0.0;
		double FirstPressurePa = NAN;
		TArray<TPair<double, double>> LevelHistory;   // (time, level) within the window
		double SinceS[4] = { -1.0, -1.0, -1.0, -1.0 };  // since when each condition has held
		double AlarmS[4] = { -1.0, -1.0, -1.0, -1.0 };
	};
	FProcessLeakSettings Settings;
	TArray<FLoop> Loops;
	double NextEvalS = 0.0;
	TArray<FProcessLeakAlarm> AlarmLog;
};
