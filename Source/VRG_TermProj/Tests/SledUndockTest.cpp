// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// 장비 탈착 검증 (2026-09-27 재정립: GB200 NVL72 기준).
// [A] 수력: 수랭 장비를 뽑으면 그 랙 분기 저항이 R*(N/n)^2로 올라가 유량이 줄고 남은 랙이 더 받는다.
// [B] 열: 뽑힌 장비는 전원·콜드플레이트·자기 팬 기류가 모두 끊긴다. 다른 슬롯은 자기 팬으로 돌므로 풍량이 그대로다.
namespace
{
	constexpr EAutomationTestFlags VrgUndockTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// NVL72 한 줄 (FInRowLoopSpec 기본값 = 방 액터 기본값). 랙마다 꽂힌 수랭 장비 수를 준다.
	constexpr int32 RackCount = 8;

	struct FRowSolveOutput
	{
		FHydraulicSolveResult Result;
		double PumpFlowM3s = 0.0;
		// 길이 RackCount. 분기가 끊긴 랙은 0이다.
		TArray<double> RackBranchFlowM3s;
	};

	FRowSolveOutput SolveRow(const TArray<int32>& DockedLiquidSlots)
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = RackCount;
		Spec.DockedSledsPerRack = DockedLiquidSlots;

		FHydraulicNetwork Network;
		const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, Spec);

		FRowSolveOutput Output;
		Output.Result = Network.Solve(FHydraulicSolveSettings());
		Output.PumpFlowM3s = Network.GetPumpFlowM3s(Handles.PumpIndex);
		for (const int32 PipeIndex : Handles.RackBranchPipeIndices)
		{
			Output.RackBranchFlowM3s.Add(PipeIndex == INDEX_NONE ? 0.0 : FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
		}
		return Output;
	}

	FString BranchText(const FRowSolveOutput& Output)
	{
		FString Text;
		for (const double FlowM3s : Output.RackBranchFlowM3s) Text += FString::Printf(TEXT("%.3f "), FlowM3s * 1000.0);
		return Text;
	}
}

// 수랭 장비를 뽑으면 그 랙의 분기 저항이 올라가 유량이 줄고, 남은 랙이 더 받아야 한다.
// D4: 영향 범위를 위상표로 정하지 않고 네트워크 재계산 결과로 판정한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicSledUndockRedistributionTest,
	"VRG.Hydraulics.Network.SledUndockRedistributesFlow", VrgUndockTestFlags)

bool FHydraulicSledUndockRedistributionTest::RunTest(const FString& Parameters)
{
	const int32 LiquidSlots = FRackThermalModel::CountLiquidSlots(FRackThermalModel::MakeGb200Nvl72Params());
	TestEqual(TEXT("NVL72 수랭 장비는 27대 (컴퓨트 18 + NVLink 스위치 9)"), LiquidSlots, FInRowLoopSpec().SledsPerRack);

	TArray<int32> FullDocked;
	FullDocked.Init(LiquidSlots, RackCount);
	const FRowSolveOutput Baseline = SolveRow(FullDocked);

	// 랙 2번에서 수랭 장비 27대 중 9대를 뽑는다.
	TArray<int32> PartialDocked = FullDocked;
	PartialDocked[2] = LiquidSlots - 9;
	const FRowSolveOutput Partial = SolveRow(PartialDocked);

	AddInfo(FString::Printf(TEXT("만재: 펌프 %.4f L/s | 분기 %s L/s"), Baseline.PumpFlowM3s * 1000.0, *BranchText(Baseline)));
	AddInfo(FString::Printf(TEXT("랙2 %d/%d대: 펌프 %.4f L/s | 분기 %s L/s"),
		PartialDocked[2], LiquidSlots, Partial.PumpFlowM3s * 1000.0, *BranchText(Partial)));

	TestTrue(TEXT("만재 구성이 수렴해야 한다"), Baseline.Result.bConverged);
	TestTrue(TEXT("일부 탈착 구성이 수렴해야 한다"), Partial.Result.bConverged);
	TestTrue(TEXT("장비를 뽑은 랙은 분기 유량이 줄어야 한다"), Partial.RackBranchFlowM3s[2] < Baseline.RackBranchFlowM3s[2]);

	bool bOthersIncreased = true;
	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		if (RackIndex != 2 && !(Partial.RackBranchFlowM3s[RackIndex] > Baseline.RackBranchFlowM3s[RackIndex]))
		{
			bOthersIncreased = false;
		}
	}
	TestTrue(TEXT("남은 랙들은 분기 유량이 늘어야 한다"), bOthersIncreased);

	// 랙 3번을 통째로 비운다. 분기가 사라지므로 유량이 정확히 0이어야 한다.
	TArray<int32> EmptyRackDocked = FullDocked;
	EmptyRackDocked[3] = 0;
	const FRowSolveOutput EmptyRack = SolveRow(EmptyRackDocked);

	AddInfo(FString::Printf(TEXT("랙3 0/%d대: 펌프 %.4f L/s | 분기 %s L/s"), LiquidSlots, EmptyRack.PumpFlowM3s * 1000.0, *BranchText(EmptyRack)));

	TestTrue(TEXT("랙을 통째로 비운 구성이 수렴해야 한다"), EmptyRack.Result.bConverged);
	TestEqual(TEXT("비운 랙의 분기 유량은 정확히 0이어야 한다"), EmptyRack.RackBranchFlowM3s[3], 0.0);

	// 질량 보존: 분기 유량의 합 = 펌프 유량.
	double BranchSumM3s = 0.0;
	for (const double FlowM3s : EmptyRack.RackBranchFlowM3s)
	{
		BranchSumM3s += FlowM3s;
	}
	const double NormalizedMassError = FMath::Abs(BranchSumM3s - EmptyRack.PumpFlowM3s) / FMath::Max(EmptyRack.PumpFlowM3s, 1.0e-12);

	AddInfo(FString::Printf(TEXT("분기 합 %.9f vs 펌프 유량 %.9f m3/s, 정규화 오차 %.3e"),
		BranchSumM3s, EmptyRack.PumpFlowM3s, NormalizedMassError));
	TestTrue(TEXT("분기 유량의 합이 펌프 유량과 같아야 한다"), NormalizedMassError < 1.0e-9);

	// 타당성 안전망: 랙 하나가 빠졌다고 총 유량이 사라지거나 폭주하면 안 된다.
	const double PumpFlowRatio = EmptyRack.PumpFlowM3s / Baseline.PumpFlowM3s;
	AddInfo(FString::Printf(TEXT("랙 1대 격리 시 펌프 유량비 %.4f (만재 대비)"), PumpFlowRatio));
	TestTrue(TEXT("랙 1대를 빼도 펌프 유량이 만재의 0.7~1.0배 범위여야 한다"), PumpFlowRatio > 0.7 && PumpFlowRatio < 1.0);

	return true;
}

// 뽑힌 컴퓨트 트레이는 전원(발열), 콜드플레이트, 자기 팬 기류가 모두 끊긴다.
// 장비마다 자기 팬으로 공기를 끌므로 다른 슬롯의 풍량은 바뀌지 않는다 (예전 랙 단위 팬 모델의 "빈 슬롯이 공기를 훔친다"는 없다).
// 통로 압력차로 빈 슬롯을 지나는 재순환은 모델에 없다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackUndockedSlotTest,
	"VRG.Thermal.Rack.UndockedSlotHasNoAirOrHeat", VrgUndockTestFlags)

bool FRackUndockedSlotTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const int32 NumSlots = Params.Slots.Num();

	// 팬 회전수를 고정해 제어기 차이를 없앤다. 풍량 비교가 순수하게 탈착 효과만 본다.
	auto Run = [&Params, NumSlots](const TArray<bool>& SledDocked, FRackThermalModel& OutModel)
	{
		FRackThermalInput Input;
		Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
		Input.SledDocked = SledDocked;
		Input.CoolantMassFlowKgs = 2.3;
		Input.FanSpeedOverride = 0.6;

		FRackThermalState Initial;
		Initial.CoolantTempK = 300.15;
		OutModel.Initialize(Params, Initial);
		for (int32 StepIndex = 0; StepIndex < 20000; ++StepIndex) OutModel.Step(Input, 0.05);
		return OutModel.MakeSnapshot(1.0e3);
	};

	TArray<bool> FullDocked;
	FullDocked.Init(true, NumSlots);
	FRackThermalModel FullModel;
	const FRackThermalSnapshot Full = Run(FullDocked, FullModel);

	int32 Removed = INDEX_NONE, Other = INDEX_NONE;
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		if (Params.Slots[Slot].DeviceType == FRackThermalModel::Gb200DevCompute) { if (Removed == INDEX_NONE) Removed = Slot; else if (Other == INDEX_NONE) Other = Slot; }

	TArray<bool> OneRemoved = FullDocked;
	OneRemoved[Removed] = false;
	FRackThermalModel RemovedModel;
	const FRackThermalSnapshot After = Run(OneRemoved, RemovedModel);

	double MaxOtherFlowChangeKgs = 0.0;
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		if (Slot != Removed) MaxOtherFlowChangeKgs = FMath::Max(MaxOtherFlowChangeKgs, FMath::Abs(After.SlotAirMassFlowKgs[Slot] - Full.SlotAirMassFlowKgs[Slot]));

	double RemovedHeatW = 0.0, RemovedToCoolantW = 0.0, RemovedToAirW = 0.0;
	for (int32 N = RemovedModel.GetSlotNodeStart(Removed); N < RemovedModel.GetSlotNodeStart(Removed) + RemovedModel.GetSlotNodeCount(Removed); ++N)
	{
		RemovedHeatW += After.ComponentHeatW[N];
		RemovedToCoolantW += FMath::Abs(After.ComponentHeatToCoolantW[N]);
		RemovedToAirW += FMath::Abs(After.ComponentHeatToAirW[N]);
	}

	AddInfo(FString::Printf(TEXT("컴퓨트 트레이 슬롯 %d 탈착 | 만재 풍량 %.5f kg/s -> 탈착 %.5f kg/s"),
		Removed, Full.SlotAirMassFlowKgs[Removed], After.SlotAirMassFlowKgs[Removed]));
	AddInfo(FString::Printf(TEXT("다른 슬롯 풍량 최대 변화 %.3e kg/s (예: 슬롯 %d %.5f kg/s)"),
		MaxOtherFlowChangeKgs, Other, After.SlotAirMassFlowKgs[Other]));
	AddInfo(FString::Printf(TEXT("뽑은 장비: 발열 %.3f W / 냉각수로 %.3e W / 공기로 %.3e W | 랙 발열 %.1f -> %.1f kW"),
		RemovedHeatW, RemovedToCoolantW, RemovedToAirW, Full.TotalHeatW / 1000.0, After.TotalHeatW / 1000.0));

	TestTrue(TEXT("만재에서 컴퓨트 트레이 풍량이 0보다 커야 한다"), Full.SlotAirMassFlowKgs[Removed] > 0.0);
	TestEqual(TEXT("뽑은 슬롯의 풍량은 0이어야 한다"), After.SlotAirMassFlowKgs[Removed], 0.0);
	TestEqual(TEXT("다른 슬롯의 풍량은 그대로여야 한다 (장비마다 자기 팬)"), MaxOtherFlowChangeKgs, 0.0);
	TestEqual(TEXT("뽑은 장비는 발열이 없어야 한다"), RemovedHeatW, 0.0);
	TestEqual(TEXT("뽑은 장비는 냉각수로 열을 내보내지 않아야 한다"), RemovedToCoolantW, 0.0);
	TestEqual(TEXT("뽑은 장비는 강제 공랭으로 열을 내보내지 않아야 한다"), RemovedToAirW, 0.0);
	TestTrue(TEXT("랙 발열이 줄어야 한다"), After.TotalHeatW < Full.TotalHeatW);
	TestEqual(TEXT("뽑은 슬롯 배기는 입구 온도와 같아야 한다"), After.SledAirOutletTempK[Removed], 297.15);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
