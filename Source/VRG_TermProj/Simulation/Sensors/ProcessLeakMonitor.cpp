// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Sensors/ProcessLeakMonitor.h"

const TCHAR* FProcessLeakMonitor::SignalName(EProcessLeakSignal Signal)
{
	switch (Signal)
	{
	case EProcessLeakSignal::LevelDrop: return TEXT("level_drop");
	case EProcessLeakSignal::Makeup: return TEXT("makeup_flow");
	case EProcessLeakSignal::FlowMismatch: return TEXT("flow_mismatch");
	case EProcessLeakSignal::PressureDrop: return TEXT("pressure_drop");
	default: return TEXT("unknown");
	}
}

void FProcessLeakMonitor::Reset(const TArray<FString>& LoopNames, const TArray<double>& DesignFlowM3s, const FProcessLeakSettings& InSettings)
{
	Settings = InSettings;
	Loops.Reset();
	AlarmLog.Reset();
	NextEvalS = 0.0;
	for (int32 L = 0; L < LoopNames.Num(); ++L)
	{
		FLoop& Loop = Loops.AddDefaulted_GetRef();
		Loop.Name = LoopNames[L];
		Loop.DesignFlowM3s = DesignFlowM3s.IsValidIndex(L) ? FMath::Max(DesignFlowM3s[L], 0.0) : 0.0;
	}
}

int32 FProcessLeakMonitor::ResetAlarms()
{
	int32 Cleared = 0;
	for (FLoop& L : Loops)
		for (int32 K = 0; K < 4; ++K) { Cleared += L.AlarmS[K] >= 0.0 ? 1 : 0; L.AlarmS[K] = -1.0; L.SinceS[K] = -1.0; }
	return Cleared;
}

TArray<FProcessLeakAlarm> FProcessLeakMonitor::Step(double SimTimeS, const TArray<FProcessLeakReading>& Readings)
{
	TArray<FProcessLeakAlarm> Raised;
	if (SimTimeS + 1e-9 < NextEvalS) return Raised;
	while (NextEvalS <= SimTimeS + 1e-9) NextEvalS += FMath::Max(Settings.EvaluatePeriodS, 1e-3);
	for (int32 L = 0; L < Loops.Num() && L < Readings.Num(); ++L)
	{
		FLoop& Loop = Loops[L];
		const FProcessLeakReading& R = Readings[L];
		double Value[4] = { 0.0, 0.0, 0.0, 0.0 };
		bool bHolds[4] = { false, false, false, false };
		// Level: drop below the highest reading of the window (a slow leak accumulates, a fast one drops at once).
		if (FMath::IsFinite(R.LevelM3))
		{
			Loop.LevelHistory.Add({ SimTimeS, R.LevelM3 });
			while (Loop.LevelHistory.Num() > 1 && Loop.LevelHistory[0].Key < SimTimeS - Settings.LevelWindowS) Loop.LevelHistory.RemoveAt(0);
			double Max = R.LevelM3;
			for (const TPair<double, double>& H : Loop.LevelHistory) Max = FMath::Max(Max, H.Value);
			Value[0] = Max - R.LevelM3;
			bHolds[0] = Value[0] > Settings.LevelDropM3;
		}
		if (FMath::IsFinite(R.MakeupM3s)) { Value[1] = R.MakeupM3s; bHolds[1] = R.MakeupM3s > Settings.MakeupM3s; }
		if (FMath::IsFinite(R.SupplyM3s) && FMath::IsFinite(R.ReturnM3s) && Loop.DesignFlowM3s > 0.0)
		{
			Value[2] = FMath::Abs(R.SupplyM3s - R.ReturnM3s) / Loop.DesignFlowM3s;
			bHolds[2] = Value[2] > Settings.MismatchFraction;
		}
		if (FMath::IsFinite(R.PressurePa))
		{
			if (!FMath::IsFinite(Loop.FirstPressurePa)) Loop.FirstPressurePa = R.PressurePa;   // the loop's normal pressure
			Value[3] = Loop.FirstPressurePa - R.PressurePa;
			bHolds[3] = Value[3] > Settings.PressureDropPa;
		}
		for (int32 K = 0; K < 4; ++K)
		{
			if (!bHolds[K]) { Loop.SinceS[K] = -1.0; continue; }
			if (Loop.SinceS[K] < 0.0) Loop.SinceS[K] = SimTimeS;
			if (Loop.AlarmS[K] >= 0.0 || SimTimeS + 1e-9 < Loop.SinceS[K] + Settings.HoldS) continue;
			Loop.AlarmS[K] = SimTimeS;
			Raised.Add({ L, static_cast<EProcessLeakSignal>(K), SimTimeS, Value[K] });
		}
	}
	AlarmLog.Append(Raised);
	return Raised;
}
