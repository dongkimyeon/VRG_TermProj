// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/VirtualSensor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr auto VirtualSensorTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

// S6 (2026-09-27): the virtual sensor (D9). Lag against the analytic first-order step response, detection time against
// the analytic crossing, the same seed gives the same readings, noise statistics, alarm hysteresis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVirtualSensorTest, "VRG.Sensor.VirtualSensorModel", VirtualSensorTestFlags)
bool FVirtualSensorTest::RunTest(const FString& Parameters)
{
	constexpr double Dt = .02, T0 = 293.15, T1 = 303.15;

	// 1) Step response, noise-free: the reading at each sample is T1 - (T1 - T0) exp(-t / tau) exactly (constant input).
	{
		FVirtualSensorSpec Spec; Spec.NoiseStdK = 0.0; Spec.ResolutionK = 0.0; Spec.TimeConstantS = 8.0; Spec.SamplePeriodS = 1.0;
		Spec.AlarmThresholdK = T0 + 7.0;
		FVirtualSensor S; S.Reset(Spec, T0, 1);
		double MaxErr = 0.0;
		int32 Readings = 0;
		double T = 0.0;
		for (int32 N = 1; N <= 3000; ++N)
		{
			T = N * Dt;
			const int32 Before = S.GetNumReadings();
			S.Advance(T, Dt, T1);
			if (S.GetNumReadings() != Before)
			{
				++Readings;
				const double Expected = T1 - (T1 - T0) * FMath::Exp(-T / Spec.TimeConstantS);
				MaxErr = FMath::Max(MaxErr, FMath::Abs(S.GetLastReadingK() - Expected));
			}
		}
		// Detection: the first sample at or after the analytic crossing of the threshold by the element.
		const double Crossing = -Spec.TimeConstantS * FMath::Loge((T1 - Spec.AlarmThresholdK) / (T1 - T0));
		const double ExpectedDetection = FMath::CeilToDouble(Crossing / Spec.SamplePeriodS - 1e-9) * Spec.SamplePeriodS;
		AddInfo(FString::Printf(TEXT("계단 응답(tau 8 s, 1 s 표본): 표본 %d개, 해석해 대비 최대 오차 %.3e K (상승 10 K) | 감지 %.2f s (해석 교차 %.4f s -> 다음 표본 %.2f s)"),
			Readings, MaxErr, S.GetFirstAlarmTimeS(), Crossing, ExpectedDetection));
		TestEqual(TEXT("One reading per second over 60 s"), Readings, 60);
		TestTrue(TEXT("Lag matches the analytic step response"), MaxErr < 1e-10);
		TestTrue(TEXT("Detected at the first sample after the analytic crossing"), FMath::Abs(S.GetFirstAlarmTimeS() - ExpectedDetection) < 1e-9);
	}

	// 2) Same seed -> same readings; another seed -> different readings.
	{
		FVirtualSensorSpec Spec;
		FVirtualSensor A, B, C;
		A.Reset(Spec, T0, 7); B.Reset(Spec, T0, 7); C.Reset(Spec, T0, 8);
		bool bSame = true, bDifferent = false;
		for (int32 N = 1; N <= 5000; ++N)
		{
			A.Advance(N * Dt, Dt, T0); B.Advance(N * Dt, Dt, T0); C.Advance(N * Dt, Dt, T0);
			bSame &= A.GetLastReadingK() == B.GetLastReadingK();
			bDifferent |= A.GetLastReadingK() != C.GetLastReadingK();
		}
		TestTrue(TEXT("Same seed, same readings"), bSame);
		TestTrue(TEXT("Different seed, different readings"), bDifferent);
	}

	// 3) Noise statistics at steady state (element = air), no rounding: mean ~ 0, standard deviation ~ sigma.
	{
		FVirtualSensorSpec Spec; Spec.NoiseStdK = 0.2; Spec.ResolutionK = 0.0; Spec.SamplePeriodS = Dt;
		FVirtualSensor S; S.Reset(Spec, T0, 3);
		double Sum = 0.0, Sum2 = 0.0;
		constexpr int32 N = 20000;
		for (int32 I = 1; I <= N; ++I)
		{
			S.Advance(I * Dt, Dt, T0);
			const double E = S.GetLastReadingK() - T0;
			Sum += E; Sum2 += E * E;
		}
		const double Mean = Sum / N, Std = FMath::Sqrt(Sum2 / N - Mean * Mean);
		AddInfo(FString::Printf(TEXT("잡음 sigma 0.2 K, 표본 %d개: 평균 %.4f K, 표준편차 %.4f K"), S.GetNumReadings(), Mean, Std));
		TestTrue(TEXT("Noise mean within 4 standard errors"), FMath::Abs(Mean) < 4.0 * .2 / FMath::Sqrt(double(N)));
		TestTrue(TEXT("Noise spread within 3 %"), FMath::Abs(Std / .2 - 1.0) < .03);
	}

	// 4) Hysteresis: the alarm stays on inside the band and clears below it.
	{
		FVirtualSensorSpec Spec; Spec.NoiseStdK = 0.0; Spec.ResolutionK = 0.0; Spec.TimeConstantS = 0.0; Spec.SamplePeriodS = Dt;
		Spec.AlarmThresholdK = 300.0; Spec.AlarmHysteresisK = 0.5;
		FVirtualSensor S; S.Reset(Spec, 295.0, 1);
		S.Advance(Dt, Dt, 301.0);
		const bool bOn = S.IsAlarm();
		S.Advance(2 * Dt, Dt, 299.7);
		const bool bStillOn = S.IsAlarm();
		S.Advance(3 * Dt, Dt, 299.0);
		const bool bOff = !S.IsAlarm();
		S.Advance(4 * Dt, Dt, 300.2);
		TestTrue(TEXT("Alarm on at the threshold"), bOn);
		TestTrue(TEXT("Stays on inside the hysteresis band"), bStillOn);
		TestTrue(TEXT("Clears below the band"), bOff);
		TestTrue(TEXT("Second alarm counted, first time kept"), S.GetNumAlarms() == 2 && FMath::IsNearlyEqual(S.GetFirstAlarmTimeS(), Dt));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
