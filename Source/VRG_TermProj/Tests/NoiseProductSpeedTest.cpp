#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseSource.h"

#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS
// 2026-11-28 (user request: product measurements as the follow-up check of the fan speed correction).
// The model scales a unit's sound power by 50 log10(n2/n1). Each row below is the SAME product (or fan frame) at two
// speeds, read from the manufacturer's tables; the measured level difference is compared with the model's difference.
// Data were transcribed by hand; the values and sources are listed in docs/기록/10_소음.md (2026-11-28).
namespace
{
	constexpr EAutomationTestFlags VrgNoiseProductTestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	struct FSpeedPair { const TCHAR* Name; double SpeedRatio; double MeasuredDeltaDb; };

	// Speed ratio >= 0.5: fan law region. Liebert 940 family: 60 -> 50 Hz direct drive, ratio = synchronous 5/6
	// (the manual gives no rpm; airflow ratios 0.832..0.836 agree). Kaltra: airflow ratio (no rpm published).
	const FSpeedPair Moderate[] = {
		{ TEXT("Liebert 940 LwA 60->50 Hz"), 5.0 / 6.0, -3.4 },
		{ TEXT("Liebert 940 LpA 1.5 m 60->50 Hz"), 5.0 / 6.0, -2.8 },
		{ TEXT("Liebert 033 LwA"), 5.0 / 6.0, -4.0 },
		{ TEXT("Liebert 139 LwA"), 5.0 / 6.0, -5.1 },
		{ TEXT("Liebert 260 LwA"), 5.0 / 6.0, -4.6 },
		{ TEXT("Liebert 352 LwA"), 5.0 / 6.0, -3.4 },
		{ TEXT("Liebert 620 LwA"), 5.0 / 6.0, -4.4 },
		{ TEXT("Liebert Quiet-Line 040 LwA"), 5.0 / 6.0, -3.3 },
		{ TEXT("Liebert Quiet-Line 453 LwA"), 5.0 / 6.0, -3.3 },
		{ TEXT("San Ace 9CRD J03 -> G03"), 16700.0 / 19000.0, -3.0 },
		{ TEXT("San Ace 9CRD J03 -> H03"), 15000.0 / 19000.0, -5.0 },
		{ TEXT("San Ace 9CRD J03 -> M03"), 14000.0 / 19000.0, -7.0 },
		{ TEXT("ebm-papst W3G910 1020 -> 900 rpm"), 900.0 / 1020.0, -3.0 },
		{ TEXT("ebm-papst W3G910 1020 -> 750 rpm"), 750.0 / 1020.0, -8.0 },
		{ TEXT("ebm-papst W3G300 1800 -> 1500 rpm"), 1500.0 / 1800.0, -4.0 },
		{ TEXT("ebm-papst W3G300 1800 -> 1200 rpm"), 1200.0 / 1800.0, -10.0 },
		{ TEXT("ebm-papst W3G250 2100 -> 1800 rpm"), 1800.0 / 2100.0, -4.0 },
		{ TEXT("ebm-papst W3G250 2100 -> 1500 rpm"), 1500.0 / 2100.0, -8.0 },
		{ TEXT("Kaltra Mistral-W W80 6 -> 8 pole"), 0.728, -7.0 },
	};
	// Deep turn-down (< 0.6): integer catalog values near the measuring floor (15..34 dBA) for the small fans.
	const FSpeedPair Deep[] = {
		{ TEXT("San Ace 9HVA PWM 100 -> 20 %"), 8000.0 / 38000.0, -37.0 },
		{ TEXT("San Ace 9CRJ PWM 100 -> 20 %"), 4500.0 / 36200.0, -44.0 },
		{ TEXT("San Ace 9CRD J03 100 -> 0 %"), 3700.0 / 19000.0, -39.0 },
		{ TEXT("San Ace 9CRD M03 100 -> 0 %"), 2000.0 / 14000.0, -39.0 },
		{ TEXT("ebm-papst W3G910 1020 -> 600 rpm"), 600.0 / 1020.0, -14.0 },
	};
	// Tolerances set from the measured spread (largest 1.3 dB / 3.5 dB) with catalog integer rounding (0.5 dB).
	constexpr double ModerateToleranceDb = 1.5;
	constexpr double DeepToleranceDb = 4.0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseProductSpeedTest, "VRG.Noise.Source.ProductSpeedData", VrgNoiseProductTestFlags)
bool FNoiseProductSpeedTest::RunTest(const FString& Parameters)
{
	auto Profile = VrgNoise::Liebert940Standard60Hz();
	Profile.bFanSpeedEstimate = true;   // the whole point: the model's own speed correction, applied to one unit
	const VrgNoise::FLevel Rated = VrgNoise::Evaluate(&Profile, true, 1.0);
	if (!TestTrue(TEXT("rated level known"), Rated.Db.IsSet())) return false;

	auto Check = [&](const FSpeedPair& P, double Tolerance, double& MaxAbs)
	{
		const VrgNoise::FLevel At = VrgNoise::Evaluate(&Profile, true, P.SpeedRatio);
		if (!TestTrue(FString::Printf(TEXT("%s: level known"), P.Name), At.Db.IsSet())) return;
		const double ModelDelta = *At.Db - *Rated.Db;
		const double Diff = P.MeasuredDeltaDb - ModelDelta;
		MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Diff));
		AddInfo(FString::Printf(TEXT("%s: speed %.3f, measured %+.1f dB, model %+.2f dB, diff %+.2f dB"), P.Name, P.SpeedRatio, P.MeasuredDeltaDb, ModelDelta, Diff));
		TestTrue(FString::Printf(TEXT("%s within %.1f dB"), P.Name, Tolerance), FMath::Abs(Diff) <= Tolerance);
	};
	double MaxModerate = 0.0, MaxDeep = 0.0;
	for (const FSpeedPair& P : Moderate) Check(P, ModerateToleranceDb, MaxModerate);
	for (const FSpeedPair& P : Deep) Check(P, DeepToleranceDb, MaxDeep);
	AddInfo(FString::Printf(TEXT("max |measured - model|: speed >= 0.58 %.2f dB (%d pairs), deep turn-down %.2f dB (%d pairs)"),
		MaxModerate, int32(UE_ARRAY_COUNT(Moderate)), MaxDeep, int32(UE_ARRAY_COUNT(Deep))));
	return true;
}
#endif
