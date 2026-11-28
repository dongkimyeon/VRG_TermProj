#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseSource.h"

#include <cmath>
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags VrgNoiseTestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	VrgNoise::FSourceProfile NoiseBandFixture()
	{
		auto Profile = VrgNoise::Liebert940Standard60Hz();
		Profile.Id = TEXT("synthetic-octave-test");
		Profile.Model = TEXT("Synthetic octave fixture, not a product rating");
		Profile.SourceUrl = TEXT("synthetic-test-input");
		Profile.SourceLocator = TEXT("NoiseSourceTest.cpp; ASHRAE table 5 weighting only");
		Profile.RatingCondition = TEXT("Independent octave inputs, unweighted");
		Profile.bEstimatedRating = true;
		Profile.Weighting = VrgNoise::EWeighting::Unweighted;
		Profile.FrequenciesHz = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
		Profile.LevelsDb = {90, 85, 80, 75, 70, 65, 60, 55};
		return Profile;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseConversionTest, "VRG.Noise.Source.Conversions", VrgNoiseTestFlags)
bool FNoiseConversionTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	double MaxPowerRelative = 0.0, MaxPressureRelative = 0.0, MaxRoundTripDb = 0.0;
	for (const auto Quantity : {EQuantity::SoundPower, EQuantity::SoundPressure})
	{
		const double Reference = Quantity == EQuantity::SoundPower ? 1e-12 : 4e-10;
		for (const double Db : {-120.0, 0.0, 30.0, 97.4, 120.0, 300.0, 3100.0})
		{
			double Linear = -1.0, Restored = -1.0;
			TestTrue(TEXT("Finite representable conversion"), TryDbToLinear(Db, Quantity, Linear));
			TestTrue(TEXT("Round trip succeeds"), TryLinearToDb(Linear, Quantity, Restored));
			// Independent natural-exponential form, reference outside exponent.
			const double Expected = Reference * std::exp((Db / 10.0) * std::log(10.0));
			// Extreme case has a representable result but an overflowing intermediate
			// in the independent form; its round trip is still checked.
			if (std::isfinite(Expected))
			{
				const double Error = std::abs(Linear / Expected - 1.0);
				if (Quantity == EQuantity::SoundPower) MaxPowerRelative = FMath::Max(MaxPowerRelative, Error);
				else MaxPressureRelative = FMath::Max(MaxPressureRelative, Error);
			}
			MaxRoundTripDb = FMath::Max(MaxRoundTripDb, std::abs(Restored - Db));
		}
	}
	double Linear = 7.0;
	TestTrue(TEXT("1 W corresponds to 120 dB re 1 pW"), TryDbToLinear(120.0, EQuantity::SoundPower, Linear) && Linear == 1.0);
	TestTrue(TEXT("0 dB power has positive reference power"), TryDbToLinear(0.0, EQuantity::SoundPower, Linear) && Linear == 1e-12);
	const double NaN = std::numeric_limits<double>::quiet_NaN(), Inf = std::numeric_limits<double>::infinity();
	for (const double Db : {NaN, Inf, -Inf, 4000.0, -4000.0})
	{
		Linear = 7.0;
		TestTrue(TEXT("Overflow, underflow or invalid dB preserves output"), !TryDbToLinear(Db, EQuantity::SoundPower, Linear) && Linear == 7.0);
	}
	for (const double Value : {0.0, -1.0, NaN, Inf})
	{
		double Db = 7.0;
		TestTrue(TEXT("Invalid linear input has no finite dB"), !TryLinearToDb(Value, EQuantity::SoundPressure, Db) && Db == 7.0);
	}
	AddInfo(FString::Printf(TEXT("Noise conversions: max power relative %.3e, pressure squared relative %.3e, roundtrip %.3e dB"), MaxPowerRelative, MaxPressureRelative, MaxRoundTripDb));
	// First measurement: relative 1.177e-14 / 1.144e-14, roundtrip 0 dB.
	TestTrue(TEXT("Measured conversion accuracy"), MaxPowerRelative < 1e-12 && MaxPressureRelative < 1e-12 && MaxRoundTripDb < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseSumTest, "VRG.Noise.Source.IncoherentSum", VrgNoiseTestFlags)
bool FNoiseSumTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	auto Profile = Liebert940Standard60Hz();
	const auto Unit = Evaluate(&Profile, true, 1.0);
	TestTrue(TEXT("Whole unit catalog rating"), Unit.Db.IsSet() && Unit.Db.GetValue() == 97.4 && !Unit.bEstimated && Unit.KnownSources == 1);
	double MaxDbError = 0.0;
	for (const int32 Count : {2, 10, 24})
	{
		TArray<FLevel> Sources;
		Sources.Init(Unit, Count);
		const auto Total = Combine(Sources, EQuantity::SoundPower, EWeighting::A);
		if (!TestTrue(TEXT("Known sum counts units"), Total.Db.IsSet() && Total.KnownSources == Count)) continue;
		const double Expected = 97.4 + 10.0 * std::log(double(Count)) / std::log(10.0);
		MaxDbError = FMath::Max(MaxDbError, std::abs(Total.Db.GetValue() - Expected));
	}
	Profile.LevelsDb = {88.36910013008057}; // N0 50-digit Decimal, unit rating minus 10log10(8)
	TArray<FLevel> Fans;
	Fans.Init(Evaluate(&Profile, true, 1.0), 8);
	const auto Rejoined = Combine(Fans, EQuantity::SoundPower, EWeighting::A);
	if (!TestTrue(TEXT("Eight sub-sources rejoin as one catalog unit"), Rejoined.Db.IsSet())) return false;
	MaxDbError = FMath::Max(MaxDbError, std::abs(Rejoined.Db.GetValue() - 97.4));
	// Levels can be summed in log space even when the equivalent W would overflow.
	Profile.LevelsDb = {4000};
	const auto Loud = Evaluate(&Profile, true, 1.0);
	Profile.LevelsDb = {-4000};
	const auto Quiet = Evaluate(&Profile, true, 1.0);
	const auto Extreme = Combine({Loud, Quiet}, EQuantity::SoundPower, EWeighting::A);
	TestTrue(TEXT("Extreme dynamic range remains finite"), Extreme.Db.IsSet() && Extreme.Db.GetValue() == 4000.0);
	const auto Nested = Combine({Combine(Fans, EQuantity::SoundPower, EWeighting::A), Unit}, EQuantity::SoundPower, EWeighting::A);
	if (!TestTrue(TEXT("Nested sum preserves source counts"), Nested.Db.IsSet() && Nested.KnownSources == 9)) return false;
	MaxDbError = FMath::Max(MaxDbError, std::abs(Nested.Db.GetValue() - (97.4 + 3.010299956639812)));
	FLevel Pressure = Unit;
	Pressure.Quantity = EQuantity::SoundPressure;
	Pressure.Db = 60.0;
	const auto PressureSum = Combine({Pressure, Pressure}, EQuantity::SoundPressure, EWeighting::A);
	if (!TestTrue(TEXT("Mean-square pressure sum succeeds"), PressureSum.Db.IsSet())) return false;
	MaxDbError = FMath::Max(MaxDbError, std::abs(PressureSum.Db.GetValue() - 63.01029995663981));
	const auto Reverse = Combine({Quiet, Loud}, EQuantity::SoundPower, EWeighting::A);
	TestTrue(TEXT("Extreme input order preserves energy"), Reverse.Db == Extreme.Db);
	AddInfo(FString::Printf(TEXT("Noise sum: max analytic/group/fan-split error %.3e dB; 24 units are 24 whole-unit ratings"), MaxDbError));
	TestTrue(TEXT("Measured logarithmic sum accuracy"), MaxDbError < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseFanSpeedTest, "VRG.Noise.Source.FanSpeedEstimate", VrgNoiseTestFlags)
bool FNoiseFanSpeedTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	auto Profile = Liebert940Standard60Hz();
	double MaxDbError = 0.0;
	const double Speeds[] = {.5, 1.0, 1.1};
	const double ExpectedDb[] = {82.34850021680094, 97.4, 99.46963425791125}; // independent Decimal
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Speeds); ++Index)
	{
		const auto Result = Evaluate(&Profile, true, Speeds[Index]);
		if (!TestTrue(TEXT("Valid speed estimate"), Result.Db.IsSet())) continue;
		MaxDbError = FMath::Max(MaxDbError, std::abs(Result.Db.GetValue() - ExpectedDb[Index]));
		TestEqual(TEXT("Only changed speed needs extrapolation flag"), Result.bEstimated, Speeds[Index] != 1.0);
	}
	Profile.ReferenceSpeedRatio = 2.0;
	TestTrue(TEXT("Profile reference speed can differ from one"), Evaluate(&Profile, true, 2.0).Db.GetValue() == 97.4);
	Profile.bFanSpeedEstimate = false;
	TestTrue(TEXT("Unmodelled speed is missing data"), Evaluate(&Profile, true, 1.0).State == ELevelState::MissingData);
	TestTrue(TEXT("Stopped source has no log zero"), Evaluate(&Profile, true, 0.0).State == ELevelState::Silent);
	TestTrue(TEXT("External power cut masks nonzero speed"), Evaluate(&Profile, false, 2.0).State == ELevelState::Silent);
	AddInfo(FString::Printf(TEXT("Noise speed: max reference/half/+10pct error %.3e dB; speed law is an estimate, not a Liebert multi-speed measurement"), MaxDbError));
	TestTrue(TEXT("Measured speed-law calculation accuracy"), MaxDbError < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseOctaveTest, "VRG.Noise.Source.OctaveWeighting", VrgNoiseTestFlags)
bool FNoiseOctaveTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	auto Profile = NoiseBandFixture();
	const auto Weighted = Evaluate(&Profile, true, 1.0);
	if (!TestTrue(TEXT("One spectrum is one source"), Weighted.Db.IsSet() && Weighted.KnownSources == 1 && Weighted.bEstimated)) return false;
	double MaxDbError = std::abs(Weighted.Db.GetValue() - 77.38573263009374); // Decimal log-sum of independent corrected bands
	const double Corrections[] = {-26.2, -16.1, -8.6, -3.2, 0, 1.2, 1.0, -1.1};
	for (int32 Band = 0; Band < Profile.LevelsDb.Num(); ++Band) Profile.LevelsDb[Band] += Corrections[Band];
	Profile.Weighting = EWeighting::A;
	const auto AlreadyA = Evaluate(&Profile, true, 1.0);
	if (!TestTrue(TEXT("Already weighted spectrum succeeds"), AlreadyA.Db.IsSet())) return false;
	MaxDbError = FMath::Max(MaxDbError, std::abs(AlreadyA.Db.GetValue() - Weighted.Db.GetValue()));
	TestTrue(TEXT("No broadband A to unweighted conversion"), Evaluate(&Profile, true, 1.0, EWeighting::Unweighted).State == ELevelState::MissingData);
	Profile = Liebert940Standard60Hz(); Profile.Weighting = EWeighting::Unweighted;
	TestTrue(TEXT("Unweighted broadband lacks spectrum for A"), Evaluate(&Profile, true, 1.0).State == ELevelState::MissingData);
	TestTrue(TEXT("Unweighted requested directly"), Evaluate(&Profile, true, 1.0, EWeighting::Unweighted).Db.GetValue() == 97.4);
	AddInfo(FString::Printf(TEXT("Noise octave: max synthetic A/log-sum error %.3e dB; corrections rounded to 0.1 dB, A applied once"), MaxDbError));
	TestTrue(TEXT("Measured octave calculation accuracy"), MaxDbError < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseStatesTest, "VRG.Noise.Source.MissingAndSilent", VrgNoiseTestFlags)
bool FNoiseStatesTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const auto Profile = Liebert940Standard60Hz();
	const auto Known = Evaluate(&Profile, true, 1.0);
	const auto Missing = Evaluate(nullptr, true, 1.0);
	const auto Silent = Evaluate(nullptr, false, 1.0);
	TestTrue(TEXT("Missing and silence are distinct"), Missing.State == ELevelState::MissingData && Missing.MissingSources == 1
		&& Silent.State == ELevelState::Silent && Silent.SilentSources == 1 && !Missing.Db.IsSet() && !Silent.Db.IsSet());
	const auto Partial = Combine({Known, Missing, Silent}, EQuantity::SoundPower, EWeighting::A);
	TestTrue(TEXT("Partial sum exposes missing count"), Partial.State == ELevelState::Known && Partial.Db.GetValue() == 97.4
		&& Partial.KnownSources == 1 && Partial.MissingSources == 1 && Partial.SilentSources == 1);
	const auto Nested = Combine({Partial, Missing}, EQuantity::SoundPower, EWeighting::A);
	TestTrue(TEXT("Nested missing count retained"), Nested.MissingSources == 2 && Nested.KnownSources == 1);
	TestTrue(TEXT("No known sources remains missing"), Combine({Silent, Missing}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::MissingData);
	TestTrue(TEXT("Empty scene is silent"), Combine({}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::Silent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseInvalidTest, "VRG.Noise.Source.InvalidInputs", VrgNoiseTestFlags)
bool FNoiseInvalidTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	const double NaN = std::numeric_limits<double>::quiet_NaN(), Inf = std::numeric_limits<double>::infinity();
	const auto Good = Liebert940Standard60Hz();
	for (int32 Case = 0; Case < 13; ++Case)
	{
		auto Bad = Good;
		switch (Case)
		{
		case 0: Bad.Id.Reset(); break;
		case 1: Bad.SourceUrl.Reset(); break;
		case 2: Bad.Quantity = EQuantity::SoundPressure; break;
		case 3: Bad.LevelsDb = {NaN}; break;
		case 4: Bad.LevelsDb = {Inf}; break;
		case 5: Bad.LevelsDb.Reset(); break;
		case 6: Bad.ReferenceSpeedRatio = 0.0; break;
		case 7: Bad.ReferenceSpeedRatio = Inf; break;
		case 8: Bad.FrequenciesHz = {63, 125}; break;
		case 9: Bad.FrequenciesHz = {63, 63}; Bad.LevelsDb = {70, 70}; break;
		case 10: Bad.FrequenciesHz = {125, 63}; Bad.LevelsDb = {70, 70}; break;
		case 11: Bad.FrequenciesHz = {NaN}; break;
		case 12: Bad.FrequenciesHz = {100}; break;
		}
		TestFalse(TEXT("Malformed profile rejected"), Bad.IsValid());
		TestTrue(TEXT("Invalid profile reported even while stopped"), Evaluate(&Bad, false, 0.0).State == ELevelState::InvalidInput);
	}
	for (const double Speed : {-1.0, NaN, Inf})
	{
		TestTrue(TEXT("Invalid running input rejected"), Evaluate(&Good, true, Speed).State == ELevelState::InvalidInput);
	}
	const auto Known = Evaluate(&Good, true, 1.0);
	auto Other = Known; Other.Quantity = EQuantity::SoundPressure;
	TestTrue(TEXT("Power and pressure cannot be added"), Combine({Known, Other}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	Other = Known; Other.Weighting = EWeighting::Unweighted;
	TestTrue(TEXT("Weightings cannot be mixed"), Combine({Known, Other}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	Other = Known; Other.Db.Reset();
	TestTrue(TEXT("Forged known result rejected"), Combine({Other}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	Other = Known; Other.Db = NaN;
	TestTrue(TEXT("Nonfinite known result rejected"), Combine({Other}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	Other = Known; Other.MissingSources = -1;
	TestTrue(TEXT("Negative result counts rejected"), Combine({Other}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	Other = Known; Other.KnownSources = MAX_int32;
	TestTrue(TEXT("Nested count overflow rejected"), Combine({Other, Known}, EQuantity::SoundPower, EWeighting::A).State == ELevelState::InvalidInput);
	double Value = 9.0;
	TestTrue(TEXT("Unknown quantity preserves output"), !TryDbToLinear(0, EQuantity(99), Value) && Value == 9.0);
	TestTrue(TEXT("Unknown band preserves output"), !TryOctaveACorrection(100, Value) && Value == 9.0);
	TestTrue(TEXT("Unknown weighting rejected"), Evaluate(&Good, true, 1.0, EWeighting(99)).State == ELevelState::InvalidInput);
	return true;
}
#endif
