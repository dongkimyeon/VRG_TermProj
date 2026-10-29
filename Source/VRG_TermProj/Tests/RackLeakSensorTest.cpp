#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/InstrumentBank.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto RackLeakSensorTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackLeakContactUnit, "VRG.Sensors.RackInterior.ContactTiming", RackLeakSensorTestFlags)
bool FRackLeakContactUnit::RunTest(const FString& Parameters)
{
	FInstrumentBank Bank; Bank.Reset();
	FInstrumentPoint Drip; Drip.bWaterContact = true; Drip.Site = EInstrumentSite::RackDripTray;
	// Synthetic timing fixture; production estimates remain in FWaterContactSpec.
	Drip.Contact.SamplePeriodS = .4; Drip.Contact.ResponseDelayS = .3;
	Drip.Id = TEXT("a:drip"); Drip.RackId = TEXT("a");
	FInstrumentPoint Rope = Drip; Rope.Site = EInstrumentSite::RackManifoldRope; Rope.Contact.WettingVolumeM3 = 0.0;
	Rope.Id = TEXT("a:rope");
	FInstrumentPoint Other = Drip; Other.Id = TEXT("b:drip"); Other.RackId = TEXT("b");
	Bank.AddPoint(Drip, {}); Bank.AddPoint(Rope, {}); Bank.AddPoint(Other, {});
	const double Dt = .02;
	for (int32 S = 1; S <= 100; ++S)
	{
		const double T = S * Dt;
		const double DripM3 = S < 12 ? 0.0 : S < 25 ? .5 * Drip.Contact.WettingVolumeM3 : Drip.Contact.WettingVolumeM3;
		const double RopeM3 = S < 15 ? 0.0 : 1e-6;
		Bank.Advance(T, Dt, {}, {{FVector::ZeroVector, .01}}, {DripM3, RopeM3, 0.0});
		if (S < 40) TestFalse(TEXT("arrival/threshold/delay cannot be bypassed by huge nearby hydraulic leak"), Bank.IsLeakAlarm(0) || Bank.IsLeakAlarm(1));
		TestFalse(TEXT("another rack stays dry"), Bank.IsLeakAlarm(2));
	}
	TestEqual(TEXT("one latched event per sensor"), Bank.GetAlarmLog().Num(), 2);
	TestTrue(TEXT("first poll after threshold and debounce"), FMath::Abs(Bank.GetLeakAlarmTimeS(0) - .8) < 1e-12
		&& FMath::Abs(Bank.GetLeakAlarmTimeS(1) - .8) < 1e-12);
	TestEqual(TEXT("cumulative contact is not integrated repeatedly"), Bank.GetContactWaterM3(0), Drip.Contact.WettingVolumeM3);
	for (const auto& A : Bank.GetAlarmLog())
	{
		const double ExpectedThreshold = A.Point == 0 ? .5 : .3;
		TestTrue(TEXT("threshold timestamp follows the model's contact data"), FMath::Abs(A.ThresholdS - ExpectedThreshold) < 1e-12);
		TestTrue(TEXT("measured detection is within delay + poll + solver interval"), A.TimeS >= A.ThresholdS + Drip.Contact.ResponseDelayS - 1e-12
			&& A.TimeS <= A.ThresholdS + Drip.Contact.ResponseDelayS + Drip.Contact.SamplePeriodS + Dt);
		AddInfo(FString::Printf(TEXT("contact point%d first %.12g threshold %.12g alarm %.12g s; delay bound excess %.12g s; volume %.12g m3"),
			A.Point, A.FirstContactS, A.ThresholdS, A.TimeS, A.TimeS - A.ThresholdS - Drip.Contact.ResponseDelayS, A.WaterM3));
	}
	Bank.Advance(2.02, Dt, {}, {}, {0.0, 0.0, 0.0}); // regressing/missing input never clears a latch
	TestTrue(TEXT("repair and bad counter do not clear retained alarms"), Bank.IsLeakAlarm(0) && Bank.IsLeakAlarm(1));
	Bank.Reset(); Bank.AddPoint(Drip, {}); Bank.Advance(Dt, Dt, {}, {}, {0.0});
	TestTrue(TEXT("new run clears alarm and cumulative baseline"), !Bank.IsLeakAlarm(0) && Bank.GetAlarmLog().IsEmpty() && Bank.GetContactWaterM3(0) == 0.0);
	return true;
}
#endif
