#include "Misc/AutomationTest.h"
#include "Simulation/Water/RackInteriorCoupling.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto RackWaterCouplingFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackWaterDeltas, "VRG.Water.RackInterior.CouplingDeltas", RackWaterCouplingFlags)
bool FRackWaterDeltas::RunTest(const FString& Parameters)
{
	FRackInteriorCoupling C;
	// Deliberately unordered synthetic slots, including a non-liquid slot at index2.
	const TArray<int32> Counts{5, 2};
	const TArray<double> Heights{.1, .6, .3, .9, 1.5, .2, .8};
	const TArray<FRackWaterSite> Sites{{100, 0, 3, false}, {101, 0, 1, true}, {200, 1, 0, false}};
	TestTrue(TEXT("height/site setup"), C.Initialize(Counts, Heights, Sites));
	TMap<int32, double> Input; Input.Add(100, .002); Input.Add(101, .001);
	TestTrue(TEXT("first interval consumes both sites"), C.Step(Input, .02, 0.0));
	TestEqual(TEXT("delta counted once"), C.GetWater().ReceivedM3, .003);
	const double FirstPassed = C.GetWater().PassedM3[3];
	for (int32 S = 1; S < 1000; ++S) TestTrue(TEXT("repair retains and drains existing water"), C.Step(Input, .02, .02 * S));
	const auto& W = C.GetWater();
	TestEqual(TEXT("unchanged cumulative input not injected again"), W.ReceivedM3, .003);
	TestEqual(TEXT("source passed amount not repeated"), W.PassedM3[3], FirstPassed);
	TestEqual(TEXT("no wetting above source"), W.FirstWetS[4], -1.0);
	TestEqual(TEXT("other rack stays dry"), W.FirstWetS[5], -1.0);
	TestTrue(TEXT("intervening air slot wets"), W.FirstWetS[2] >= 0.0);
	TestTrue(TEXT("rear exposure and both bottom paths"), W.RearExposureM3[0] > 0.0 && W.FloorM3[0] > 0.0 && W.RearFloorM3[0] > 0.0);
	for (double Held : W.HeldM3) TestTrue(TEXT("existing slot capacity plausibility"), Held >= 0.0 && Held <= W.CapacityM3);
	AddInfo(FString::Printf(TEXT("coupling deltas received %.12g m3; front/rear bottom %.12g / %.12g m3; residual %.12g m3 normalized %.12g"),
		W.ReceivedM3, W.FloorM3[0], W.RearFloorM3[0], W.BalanceErrorM3(), FMath::Abs(W.BalanceErrorM3()) / W.ReceivedM3));
	TestTrue(TEXT("existing interior conservation criterion"), FMath::Abs(W.BalanceErrorM3()) <= 1e-15);

	const auto BeforeHeld = W.HeldM3;
	TestFalse(TEXT("invalid interval rejected"), C.Step(Input, 0.0, 20.0));
	Input.Add(999, .0001);
	TestFalse(TEXT("unmapped volume retained upstream"), C.Step(Input, .02, 20.0));
	TestTrue(TEXT("unmapped pending diagnostic"), C.PendingInputM3(Input) > 0.0);
	TestTrue(TEXT("failed batch has no partial change"), C.GetWater().HeldM3 == BeforeHeld && C.GetWater().ReceivedM3 == .003);
	Input.Remove(999); Input[100] = .001;
	TestFalse(TEXT("implicit counter reset refused"), C.Step(Input, .02, 20.0));
	TestFalse(TEXT("bad reinitialization leaves state"), C.Initialize(Counts, Heights, {{100, 0, 3, false}, {100, 0, 1, true}}));
	TestEqual(TEXT("prior water preserved"), C.GetWater().ReceivedM3, .003);
	TestTrue(TEXT("explicit new run resets all state"), C.Initialize(Counts, Heights, Sites));
	TestEqual(TEXT("new run dry"), C.GetWater().ReceivedM3, 0.0);
	TestTrue(TEXT("same cumulative values in new run counted once anew"), C.Step(Input, .02, 0.0));
	TestEqual(TEXT("fresh input baseline"), C.GetWater().ReceivedM3, .002);
	return true;
}
#endif
