// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Sensors/VirtualSensor.h"

void FVirtualSensor::Reset(const FVirtualSensorSpec& InSpec, double InitialAirK, int32 Seed)
{
	Spec = InSpec;
	Random.Initialize(Seed);
	ElementK = InitialAirK;
	NextSampleTimeS = FMath::Max(Spec.SamplePeriodS, 1e-9);  // the first reading one period after the start
	LastReadingK = 0.0;
	LastReadingTimeS = -1.0;
	NumReadings = 0;
	bAlarm = false;
	FirstAlarmTimeS = -1.0;
	NumAlarms = 0;
}

double FVirtualSensor::Gaussian()
{
	// Box-Muller from the sensor's own stream (the same seed gives the same noise sequence).
	const double U1 = FMath::Max(double(Random.GetFraction()), 1e-12);
	const double U2 = double(Random.GetFraction());
	return FMath::Sqrt(-2.0 * FMath::Loge(U1)) * FMath::Cos(2.0 * UE_DOUBLE_PI * U2);
}

void FVirtualSensor::Advance(double SimTimeS, double DtS, double AirK)
{
	if (!(DtS > 0.0)) return;
	// Exact first-order response for an air temperature held over the step.
	ElementK = Spec.TimeConstantS > 0.0 ? ElementK + (1.0 - FMath::Exp(-DtS / Spec.TimeConstantS)) * (AirK - ElementK) : AirK;

	// Sampling: one reading when the step reaches the next sample time (tolerance for the accumulated step times).
	if (SimTimeS + 1e-9 < NextSampleTimeS) return;
	const double Period = FMath::Max(Spec.SamplePeriodS, 1e-9);
	while (NextSampleTimeS <= SimTimeS + 1e-9) NextSampleTimeS += Period;
	double Reading = ElementK + (Spec.NoiseStdK > 0.0 ? Spec.NoiseStdK * Gaussian() : 0.0);
	if (Spec.ResolutionK > 0.0) Reading = FMath::RoundToDouble(Reading / Spec.ResolutionK) * Spec.ResolutionK;
	LastReadingK = Reading;
	LastReadingTimeS = SimTimeS;
	++NumReadings;

	// Alarm on readings with hysteresis (a noisy reading near the threshold does not toggle it every sample).
	if (!bAlarm && Reading >= Spec.AlarmThresholdK)
	{
		bAlarm = true;
		++NumAlarms;
		if (FirstAlarmTimeS < 0.0) FirstAlarmTimeS = SimTimeS;
	}
	else if (bAlarm && Reading <= Spec.AlarmThresholdK - Spec.AlarmHysteresisK)
	{
		bAlarm = false;
	}
}
