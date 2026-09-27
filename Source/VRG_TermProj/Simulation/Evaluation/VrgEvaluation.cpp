// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Evaluation/VrgEvaluation.h"

void FVrgEvaluation::Reset(const FVrgEvaluationSettings& InSettings, int32 NumInlets, const TArray<double>& InitialInletK)
{
	Settings = InSettings;
	const int32 NumBuckets = FMath::Max(1, FMath::RoundToInt32(Settings.WindowS));
	Inlets.SetNum(NumInlets);
	for (int32 I = 0; I < NumInlets; ++I)
	{
		FInlet& In = Inlets[I];
		In.Temperature.Reset(NumBuckets);
		const double T0 = InitialInletK.IsValidIndex(I) ? InitialInletK[I] : Settings.RecommendedInletK;
		In.PeakK = T0;
		In.AboveRecommendedS = In.AboveAllowableS = 0.0;
		In.Sensor.Reset(Settings.Sensor, T0, Settings.Seed + I);
	}
	for (FWindowed* W : { &It, &ServerFan, &CrahFan, &Pump }) W->Reset(NumBuckets);
	SimTimeS = SecondStartS = RunTimeS = 0.0;
	SecondsClosed = 0;
}

void FVrgEvaluation::AfterStep(double InSimTimeS, double DtS, const TArray<double>& InletK, const FVrgEvaluationSample& Power)
{
	if (!(DtS > 0.0)) return;
	SimTimeS = InSimTimeS;
	RunTimeS += DtS;
	for (int32 I = 0; I < Inlets.Num() && I < InletK.Num(); ++I)
	{
		FInlet& In = Inlets[I];
		const double T = InletK[I];
		In.Temperature.Add(T, DtS);
		In.PeakK = FMath::Max(In.PeakK, T);
		if (T > Settings.RecommendedInletK) In.AboveRecommendedS += DtS;
		if (T > Settings.AllowableInletK) In.AboveAllowableS += DtS;
		In.Sensor.Advance(SimTimeS, DtS, T);
	}
	It.Add(Power.ItW, DtS); ServerFan.Add(Power.ServerFanW, DtS); CrahFan.Add(Power.CrahFanW, DtS); Pump.Add(Power.PumpW, DtS);
	// Close every whole second that ended (a small tolerance for the accumulated step times).
	while (SimTimeS - SecondStartS >= 1.0 - 1e-9)
	{
		for (FInlet& In : Inlets) In.Temperature.CloseSecond();
		for (FWindowed* W : { &It, &ServerFan, &CrahFan, &Pump }) W->CloseSecond();
		SecondStartS += 1.0;
		++SecondsClosed;
	}
}

double FVrgEvaluation::GetWindowCoveredS() const
{
	return Inlets.Num() > 0 ? double(Inlets[0].Temperature.Filled) : double(It.Filled);
}

FVrgEvaluation::FInletStats FVrgEvaluation::GetInlet(int32 Index) const
{
	FInletStats S;
	if (!Inlets.IsValidIndex(Index)) return S;
	const FInlet& In = Inlets[Index];
	const double Covered = double(In.Temperature.Filled);
	S.WindowMeanK = Covered > 0.0 ? In.Temperature.WindowIntegral() / Covered : 0.0;
	S.RunMeanK = RunTimeS > 0.0 ? In.Temperature.RunIntegral / RunTimeS : 0.0;
	S.PeakK = In.PeakK;
	S.SecondsAboveRecommended = In.AboveRecommendedS;
	S.SecondsAboveAllowable = In.AboveAllowableS;
	S.SensorLastK = In.Sensor.GetLastReadingK();
	S.SensorFirstAlarmS = In.Sensor.GetFirstAlarmTimeS();
	S.SensorAlarms = In.Sensor.GetNumAlarms();
	S.bSensorAlarm = In.Sensor.IsAlarm();
	return S;
}

FVrgEvaluationSample FVrgEvaluation::GetWindowPower() const
{
	FVrgEvaluationSample P;
	const double Covered = double(It.Filled);
	if (Covered <= 0.0) return P;
	P.ItW = It.WindowIntegral() / Covered;
	P.ServerFanW = ServerFan.WindowIntegral() / Covered;
	P.CrahFanW = CrahFan.WindowIntegral() / Covered;
	P.PumpW = Pump.WindowIntegral() / Covered;
	return P;
}

bool FVrgEvaluation::GetPartialPue(double& OutPue) const
{
	const FVrgEvaluationSample P = GetWindowPower();
	if (P.ItW <= 0.0) return false;  // D8: not computable without IT power
	OutPue = (P.ItW + P.CrahFanW + P.PumpW) / P.ItW;
	return true;
}
