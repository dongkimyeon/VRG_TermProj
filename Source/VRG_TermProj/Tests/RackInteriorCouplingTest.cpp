#include "Misc/AutomationTest.h"
#include "Simulation/Water/RackInteriorCoupling.h"
#include "Simulation/Water/FloorWater.h"
#include "VrgFloorWaterGpu.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackWaterFloor, "VRG.Water.RackInterior.FloorHandoff", RackWaterCouplingFlags)
bool FRackWaterFloor::RunTest(const FString& Parameters)
{
	FRackInteriorCoupling C;
	C.Initialize({2}, {.1, .8}, {{0, 0, 1, false}, {1, 0, 1, true}});
	TMap<int32, double> Input; Input.Add(0, .0003); Input.Add(1, .0002);
	for (int32 S = 0; S < 300; ++S) C.Step(Input, .02, S * .02);
	FFloorWater Floor; FFloorWaterSettings Settings; Settings.GpuMode = 0;
	Floor.Reset(FVector2D::ZeroVector, 40, 40, 0.0, Settings);
	for (int32 J = 0; J < Floor.GetNy(); ++J) for (int32 I = 0; I < Floor.GetNx(); ++I) Floor.SetSolid(I, J, true);
	const TArray<FVector> Front{FVector(.5, .5, 0.0)}, OffGrid{FVector(2.0, 2.0, 0.0)};
	const double Waiting = C.PendingFloorM3();
	C.TransferToFloor(Floor, Front, OffGrid);
	TestEqual(TEXT("solid/off-grid: no silent volume loss"), C.PendingFloorM3(), Waiting);
	TestEqual(TEXT("W3 accepts nothing when blocked"), Floor.GetTotals().ReceivedM3, 0.0);
	Floor.SetSolid(20, 20, false);
	C.TransferToFloor(Floor, Front, OffGrid);
	TestTrue(TEXT("front accepted, rear remains queued"), Floor.GetTotals().ReceivedM3 > 0.0 && C.PendingFloorM3() > 0.0);
	const double Once = Floor.GetTotals().ReceivedM3;
	C.TransferToFloor(Floor, Front, OffGrid);
	TestEqual(TEXT("accepted front never injected twice"), Floor.GetTotals().ReceivedM3, Once);
	C.TransferToFloor(Floor, Front, Front);
	Floor.Step(.02, 6.0);
	TestEqual(TEXT("recovered destination accepts pending rear"), C.PendingFloorM3(), 0.0);
	double Delivered = C.GetFloorDeliveredM3(false)[0] + C.GetFloorDeliveredM3(true)[0];
	double Held = 0.0, Transit = 0.0;
	for (double V : C.GetWater().HeldM3) Held += V;
	for (double V : C.GetWater().TransitM3) Transit += V;
	for (double V : C.GetWater().GetRearTransitM3()) Transit += V;
	const double Residual = C.GetWater().ReceivedM3 - Held - Transit - Delivered - C.PendingFloorM3();
	AddInfo(FString::Printf(TEXT("floor handoff input %.12g held %.12g transit %.12g delivered %.12g pending %.12g residual %.12g m3; W3 received diff %.12g m3 residual %.12g"),
		C.GetWater().ReceivedM3, Held, Transit, Delivered, C.PendingFloorM3(), Residual, Floor.GetTotals().ReceivedM3 - Delivered, Floor.GetTotals().Residual));
	TestTrue(TEXT("interior/floor ledger conservation"), FMath::Abs(Residual) <= 1e-15 && FMath::Abs(Floor.GetTotals().ReceivedM3 - Delivered) <= 1e-15);
	TestTrue(TEXT("existing W3 conservation criterion"), Floor.GetTotals().Residual < 1e-12);
	TestTrue(TEXT("explicit new run also clears delivery acknowledgement"), C.Initialize({2}, {.1, .8}, {{0, 0, 1, false}, {1, 0, 1, true}}));
	TestEqual(TEXT("no old floor acknowledgements"), C.GetFloorDeliveredM3(false)[0] + C.GetFloorDeliveredM3(true)[0] + C.PendingFloorM3(), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackWaterFloorGpu, "VRG.Gpu.FloorWater.RackInterior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRackWaterFloorGpu::RunTest(const FString& Parameters)
{
	if (!FVrgFloorWaterGpu::IsAvailable()) { AddWarning(TEXT("GPU unavailable: rack handoff GPU verification skipped; run without NullRHI.")); return true; }
	FRackInteriorCoupling A, B;
	A.Initialize({2}, {.1, .8}, {{0, 0, 1, false}, {1, 0, 1, true}}); B = A;
	FFloorWater Cpu, Gpu; FFloorWaterSettings Settings;
	Settings.GpuMode = 0; Cpu.Reset(FVector2D::ZeroVector, 40, 40, 0.0, Settings);
	Settings.GpuMode = 1; Gpu.Reset(FVector2D::ZeroVector, 40, 40, 0.0, Settings);
	const TArray<FVector> Front{FVector(.7, .5, 0.0)}, Rear{FVector(.3, .5, 0.0)};
	TMap<int32, double> Input;
	for (int32 S = 0; S < 60; ++S)
	{
		if (S < 30) { Input.FindOrAdd(0) += .00001; Input.FindOrAdd(1) += .00001; }
		A.Step(Input, .02, S * .02); B.Step(Input, .02, S * .02);
		A.TransferToFloor(Cpu, Front, Rear); B.TransferToFloor(Gpu, Front, Rear);
		if (Cpu.HasWater()) Cpu.Step(.02, S * .02);
		if (Gpu.HasWater()) Gpu.Step(.02, S * .02);
	}
	double MaxDepthDiffM = 0.0;
	for (int32 I = 0; I < Cpu.GetDepthM().Num(); ++I) MaxDepthDiffM = FMath::Max(MaxDepthDiffM, FMath::Abs(Cpu.GetDepthM()[I] - Gpu.GetDepthM()[I]));
	AddInfo(FString::Printf(TEXT("rack->W3 GPU steps %d, received CPU/GPU %.12g/%.12g m3, max depth difference %.12g m / 0.02m %.12g; residual CPU/GPU %.12g/%.12g"),
		Gpu.GetTotals().GpuSteps, Cpu.GetTotals().ReceivedM3, Gpu.GetTotals().ReceivedM3, MaxDepthDiffM, MaxDepthDiffM / .02, Cpu.GetTotals().Residual, Gpu.GetTotals().Residual));
	TestTrue(TEXT("real GPU exercised"), Gpu.GetTotals().GpuSteps > 0);
	TestEqual(TEXT("same accepted volumes"), Cpu.GetTotals().ReceivedM3, Gpu.GetTotals().ReceivedM3);
	TestTrue(TEXT("existing W3 GPU tolerance (2cm depth scale)"), MaxDepthDiffM / .02 < 1e-9 && Cpu.GetTotals().Residual < 1e-12 && Gpu.GetTotals().Residual < 1e-12);
	return true;
}
#endif
