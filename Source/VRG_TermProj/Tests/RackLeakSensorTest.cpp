#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/InstrumentBank.h"
#include "Simulation/Water/RackInteriorCoupling.h"
#include "Simulation/Water/FloorWater.h"

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackLeakPendingFloor, "VRG.Sensors.RackInterior.PendingFloor", RackLeakSensorTestFlags)
bool FRackLeakPendingFloor::RunTest(const FString& Parameters)
{
	FRackInteriorCoupling Coupling;
	TestTrue(TEXT("two racks with front and rear sources"), Coupling.Initialize({3, 3}, {0, .5, 1, 0, .5, 1}, {{0, 0, 2, false}, {1, 0, 1, true}}));
	FFloorWater UnavailableFloor; // No grid: all actual bottom arrivals must remain queued.
	FInstrumentBank Bank;
	FInstrumentPoint Drip; Drip.bWaterContact = true; Drip.Site = EInstrumentSite::RackDripTray;
	FInstrumentPoint Rope = Drip; Rope.Site = EInstrumentSite::RackManifoldRope; Rope.Contact.WettingVolumeM3 = 0.0;
	Bank.AddPoint(Drip, {}); Bank.AddPoint(Rope, {}); Bank.AddPoint(Drip, {});
	TMap<int32, double> Input; Input.Add(0, .001); Input.Add(1, 0.0);
	double FirstFloorS = -1.0, FirstRearS = -1.0;
	const double Dt = .02;
	for (int32 S = 1; S <= 350; ++S)
	{
		const double NowS = S * Dt;
		if (S == 201) Input[1] = .0002;
		TestTrue(TEXT("consume only new inventory deltas"), Coupling.Step(Input, Dt, NowS - Dt));
		Coupling.TransferToFloor(UnavailableFloor, {FVector::ZeroVector, FVector(5, 0, 0)}, {FVector::ZeroVector, FVector(5, 0, 0)});
		const auto& W = Coupling.GetWater();
		const double BottomM3 = W.FloorM3[0] + W.RearFloorM3[0];
		double RearM3 = 0.0; for (int32 I = W.Start[0]; I < W.Start[1]; ++I) RearM3 = FMath::Max(RearM3, W.RearExposureM3[I]);
		if (BottomM3 > 0 && FirstFloorS < 0) FirstFloorS = NowS;
		if (RearM3 > 0 && FirstRearS < 0) FirstRearS = NowS;
		Bank.Advance(NowS, Dt, {}, {}, {BottomM3, RearM3, W.FloorM3[1] + W.RearFloorM3[1]});
		if (BottomM3 == 0) TestFalse(TEXT("no drip alarm before bottom arrival"), Bank.IsLeakAlarm(0));
		if (S <= 200) TestFalse(TEXT("front water does not force rear rope contact"), Bank.IsLeakAlarm(1));
		TestFalse(TEXT("other rack remains dry"), Bank.IsLeakAlarm(2));
	}
	const auto& W = Coupling.GetWater();
	const double BottomM3 = W.FloorM3[0] + W.RearFloorM3[0];
	TestTrue(TEXT("pending W3 cannot suppress either actual contact alarm"), Bank.IsLeakAlarm(0) && Bank.IsLeakAlarm(1));
	TestEqual(TEXT("no W3 acknowledgements"), Coupling.GetFloorDeliveredM3(false)[0] + Coupling.GetFloorDeliveredM3(true)[0], 0.0);
	TestEqual(TEXT("pending retains every bottom arrival"), Coupling.PendingFloorM3(), BottomM3);
	TestEqual(TEXT("drip reads arrivals once, not pending plus arrivals"), Bank.GetContactWaterM3(0), BottomM3);
	TestTrue(TEXT("source volume and interior conservation unchanged"), FMath::Abs(W.ReceivedM3 - .0012) < 1e-15 && FMath::Abs(W.BalanceErrorM3()) < 1e-15);
	for (const auto& A : Bank.GetAlarmLog())
		TestTrue(TEXT("contact delay and polling bounds"), A.TimeS >= A.ThresholdS + Drip.Contact.ResponseDelayS - 1e-9
			&& A.TimeS <= A.ThresholdS + Drip.Contact.ResponseDelayS + Drip.Contact.SamplePeriodS + Dt);
	AddInfo(FString::Printf(TEXT("pending source %.12g bottom/pending %.12g m3 balance %.12g m3; first floor %.12g rear %.12g s; drip %.12g rope %.12g s"),
		W.ReceivedM3, BottomM3, W.BalanceErrorM3(), FirstFloorS, FirstRearS, Bank.GetLeakAlarmTimeS(0), Bank.GetLeakAlarmTimeS(1)));
	return true;
}
#endif
