// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

// S6 (2026-09-27): a virtual temperature sensor. D9: its output is a *simulated measurement*, never the true field and
// never "100 % reliable". It models what a real inlet sensor adds between the air and the number on a dashboard:
//   - a first-order response (the sensing element lags the air):  tau dTe/dt = T_air - Te
//     integrated exactly for a piecewise-constant air temperature over each step: Te += (1 - exp(-dt/tau)) (T_air - Te)
//   - a sampling period (a reading only every SamplePeriodS; the reading time is the end of the step that reaches it,
//     so it is quantised to the solver step)
//   - Gaussian noise (fixed seed per sensor: a rerun gives the same noise) and a resolution (rounding)
//   - an alarm on the *readings* with hysteresis, and the sim time it was first detected.
// The defaults are assumptions for a generic rack-inlet thermistor, not a product's specification. Plain C++, no UObject.
struct FVirtualSensorSpec
{
	double SamplePeriodS = 1.0;
	double TimeConstantS = 10.0;
	double NoiseStdK = 0.1;
	double ResolutionK = 0.1;
	double AlarmThresholdK = 273.15 + 27.0;  // ASHRAE recommended inlet upper limit 27 C
	double AlarmHysteresisK = 0.5;
};

class VRG_TERMPROJ_API FVirtualSensor
{
public:
	void Reset(const FVirtualSensorSpec& InSpec, double InitialAirK, int32 Seed);

	// Advance to SimTimeS (the end of a step of length DtS) with the air temperature of that step.
	void Advance(double SimTimeS, double DtS, double AirK);

	const FVirtualSensorSpec& GetSpec() const { return Spec; }
	bool HasReading() const { return NumReadings > 0; }
	double GetLastReadingK() const { return LastReadingK; }
	double GetLastReadingTimeS() const { return LastReadingTimeS; }
	double GetElementK() const { return ElementK; }
	int32 GetNumReadings() const { return NumReadings; }
	bool IsAlarm() const { return bAlarm; }
	// First time a reading reached the threshold; negative when it never did.
	double GetFirstAlarmTimeS() const { return FirstAlarmTimeS; }
	int32 GetNumAlarms() const { return NumAlarms; }

private:
	FVirtualSensorSpec Spec;
	FRandomStream Random;
	double ElementK = 0.0;
	double NextSampleTimeS = 0.0;
	double LastReadingK = 0.0, LastReadingTimeS = -1.0;
	int32 NumReadings = 0;
	bool bAlarm = false;
	double FirstAlarmTimeS = -1.0;
	int32 NumAlarms = 0;
	double Gaussian();
};
