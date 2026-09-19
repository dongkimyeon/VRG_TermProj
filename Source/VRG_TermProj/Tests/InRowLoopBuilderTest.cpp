// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgRoomTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// ADataCenterRoomActor / AHydraulicNetworkDebugActor의 기본값과 같아야 한다.
	FInRowLoopSpec MakeDefaultSpec(int32 NumRacks)
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = NumRacks;
		Spec.SledsPerRack = 12;
		return Spec;
	}

	double SumBranchFlowM3s(const FHydraulicNetwork& Network, const FInRowLoopHandles& Handles)
	{
		double SumM3s = 0.0;
		for (const int32 PipeIndex : Handles.RackBranchPipeIndices)
		{
			if (PipeIndex != INDEX_NONE)
			{
				SumM3s += FMath::Abs(Network.GetPipeFlowM3s(PipeIndex));
			}
		}
		return SumM3s;
	}
}

// 한 줄만 얹었을 때 S2가 기록한 인로우 참고값을 그대로 내야 한다.
// 위상 코드를 공용 빌더로 옮기면서 결과가 바뀌지 않았는지 보는 회귀 검사다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRowLoopSingleRowTest,
	"VRG.Hydraulics.InRowLoop.MatchesRecordedReference", VrgRoomTestFlags)

bool FInRowLoopSingleRowTest::RunTest(const FString& Parameters)
{
	FHydraulicNetwork Network;
	const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, MakeDefaultSpec(6));

	FHydraulicSolveSettings Settings;
	const FHydraulicSolveResult Result = Network.Solve(Settings);

	const double PumpFlowM3s = Network.GetPumpFlowM3s(Handles.PumpIndex);
	const double PumpHeadPa = Network.GetPumpDeltaPPa(Handles.PumpIndex);
	const double PumpElectricalW = Network.GetPumpElectricalInputPowerW(Handles.PumpIndex);

	FString BranchText;
	for (const int32 PipeIndex : Handles.RackBranchPipeIndices)
	{
		BranchText += FString::Printf(TEXT("%.2f "), FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)) * 1000.0);
	}

	AddInfo(FString::Printf(TEXT("펌프 %.3f L/s / 양정 %.1f kPa / 전기 입력 %.2f kW / Newton %d회"),
		PumpFlowM3s * 1000.0, PumpHeadPa / 1000.0, PumpElectricalW / 1000.0, Result.IterationsUsed));
	AddInfo(FString::Printf(TEXT("랙별 분기 유량 [L/s]: %s"), *BranchText));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);

	// CLAUDE.md에 기록된 S2 참고값: 펌프 4.87 L/s, 양정 210.3 kPa, 전기 입력 1.46 kW.
	// 기준은 기록값 대비 0.5% 이내로 둔다. 위상이 같으면 해가 같아야 한다.
	TestTrue(TEXT("펌프 유량이 기록된 4.87 L/s와 일치해야 한다"),
		FMath::Abs(PumpFlowM3s * 1000.0 - 4.87) / 4.87 < 5.0e-3);
	TestTrue(TEXT("양정이 기록된 210.3 kPa와 일치해야 한다"),
		FMath::Abs(PumpHeadPa / 1000.0 - 210.3) / 210.3 < 5.0e-3);
	TestTrue(TEXT("전기 입력이 기록된 1.46 kW와 일치해야 한다"),
		FMath::Abs(PumpElectricalW / 1000.0 - 1.46) / 1.46 < 5.0e-3);

	return true;
}

// 방 액터처럼 여러 줄을 한 네트워크에 얹어도 줄끼리 간섭하지 않아야 한다.
// 각 줄에 고정 압력 노드(CDU)가 따로 있으므로 해는 줄마다 독립이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRowLoopMultiRowTest,
	"VRG.Hydraulics.InRowLoop.MultipleRowsAreIndependent", VrgRoomTestFlags)

bool FInRowLoopMultiRowTest::RunTest(const FString& Parameters)
{
	// 먼저 한 줄만 푼다.
	FHydraulicNetwork SingleNetwork;
	const FInRowLoopHandles SingleHandles = InRowLoopBuilder::AddLoop(SingleNetwork, MakeDefaultSpec(8));
	FHydraulicSolveSettings Settings;
	const FHydraulicSolveResult SingleResult = SingleNetwork.Solve(Settings);
	const double SinglePumpFlowM3s = SingleNetwork.GetPumpFlowM3s(SingleHandles.PumpIndex);

	// 같은 줄을 네 개 얹어 한 번에 푼다 (방 액터가 하는 일).
	const int32 RowCount = 4;
	FHydraulicNetwork RoomNetwork;
	TArray<FInRowLoopHandles> RowHandles;
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		RowHandles.Add(InRowLoopBuilder::AddLoop(RoomNetwork, MakeDefaultSpec(8)));
	}
	const FHydraulicSolveResult RoomResult = RoomNetwork.Solve(Settings);

	AddInfo(FString::Printf(TEXT("한 줄: 노드 %d / 파이프 %d / 펌프 %.4f L/s, Newton %d회"),
		SingleNetwork.GetNumNodes(), SingleNetwork.GetNumPipes(),
		SinglePumpFlowM3s * 1000.0, SingleResult.IterationsUsed));
	AddInfo(FString::Printf(TEXT("%d줄: 노드 %d / 파이프 %d / 펌프 %d대, Newton %d회, 정규화 잔차 %.3e"),
		RowCount, RoomNetwork.GetNumNodes(), RoomNetwork.GetNumPipes(),
		RoomNetwork.GetNumPumps(), RoomResult.IterationsUsed, RoomResult.NormalizedResidual));

	TestTrue(TEXT("한 줄 구성이 수렴해야 한다"), SingleResult.bConverged);
	TestTrue(TEXT("여러 줄 구성이 수렴해야 한다"), RoomResult.bConverged);

	double MaxRelativeDifference = 0.0;
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		const double RowPumpFlowM3s = RoomNetwork.GetPumpFlowM3s(RowHandles[RowIndex].PumpIndex);
		MaxRelativeDifference = FMath::Max(MaxRelativeDifference,
			FMath::Abs(RowPumpFlowM3s - SinglePumpFlowM3s) / SinglePumpFlowM3s);
	}

	AddInfo(FString::Printf(TEXT("줄별 펌프 유량이 한 줄 해와 다른 정도: 최대 %.3e (상대)"),
		MaxRelativeDifference));
	TestTrue(TEXT("줄을 여러 개 얹어도 각 줄의 해가 바뀌면 안 된다"), MaxRelativeDifference < 1.0e-9);

	// 방 전체 질량 보존: 줄마다 분기 합 = 그 줄 펌프 유량.
	double MaxNormalizedMassError = 0.0;
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		const double RowPumpFlowM3s = RoomNetwork.GetPumpFlowM3s(RowHandles[RowIndex].PumpIndex);
		const double BranchSumM3s = SumBranchFlowM3s(RoomNetwork, RowHandles[RowIndex]);
		MaxNormalizedMassError = FMath::Max(MaxNormalizedMassError,
			FMath::Abs(BranchSumM3s - RowPumpFlowM3s) / FMath::Max(RowPumpFlowM3s, 1.0e-12));
	}

	AddInfo(FString::Printf(TEXT("줄별 (분기 합 - 펌프 유량) 정규화 오차: 최대 %.3e"), MaxNormalizedMassError));
	TestTrue(TEXT("줄마다 분기 유량의 합이 펌프 유량과 같아야 한다"), MaxNormalizedMassError < 1.0e-9);

	return true;
}

// 한 줄에서 유닛을 뽑으면 그 줄만 바뀌고 다른 줄은 그대로여야 한다.
// D4: 영향 범위는 재계산 결과로 판정한다. 위상표로 정하지 않는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRowLoopRowIsolationTest,
	"VRG.Hydraulics.InRowLoop.UndockAffectsOwnRowOnly", VrgRoomTestFlags)

bool FInRowLoopRowIsolationTest::RunTest(const FString& Parameters)
{
	FHydraulicSolveSettings Settings;

	// 두 줄을 얹고, 1번 줄의 3번 랙만 유닛을 4대 남긴다.
	FHydraulicNetwork Network;
	const FInRowLoopHandles RowA = InRowLoopBuilder::AddLoop(Network, MakeDefaultSpec(8));

	FInRowLoopSpec PartialSpec = MakeDefaultSpec(8);
	PartialSpec.DockedSledsPerRack.Init(12, 8);
	PartialSpec.DockedSledsPerRack[3] = 4;
	const FInRowLoopHandles RowB = InRowLoopBuilder::AddLoop(Network, PartialSpec);

	const FHydraulicSolveResult Result = Network.Solve(Settings);

	// 비교용으로 만재 한 줄을 따로 푼다.
	FHydraulicNetwork ReferenceNetwork;
	const FInRowLoopHandles ReferenceHandles = InRowLoopBuilder::AddLoop(ReferenceNetwork, MakeDefaultSpec(8));
	ReferenceNetwork.Solve(Settings);
	const double ReferencePumpFlowM3s = ReferenceNetwork.GetPumpFlowM3s(ReferenceHandles.PumpIndex);

	const double RowAPumpFlowM3s = Network.GetPumpFlowM3s(RowA.PumpIndex);
	const double RowBPumpFlowM3s = Network.GetPumpFlowM3s(RowB.PumpIndex);

	AddInfo(FString::Printf(TEXT("만재 기준 펌프 %.4f L/s"), ReferencePumpFlowM3s * 1000.0));
	AddInfo(FString::Printf(TEXT("줄 A (만재) %.4f L/s / 줄 B (3번 랙 4/12대) %.4f L/s"),
		RowAPumpFlowM3s * 1000.0, RowBPumpFlowM3s * 1000.0));
	AddInfo(FString::Printf(TEXT("줄 B 3번 랙 분기 %.4f L/s (줄 A의 같은 자리 %.4f L/s)"),
		FMath::Abs(Network.GetPipeFlowM3s(RowB.RackBranchPipeIndices[3])) * 1000.0,
		FMath::Abs(Network.GetPipeFlowM3s(RowA.RackBranchPipeIndices[3])) * 1000.0));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);

	TestTrue(TEXT("건드리지 않은 줄은 만재 해와 같아야 한다"),
		FMath::Abs(RowAPumpFlowM3s - ReferencePumpFlowM3s) / ReferencePumpFlowM3s < 1.0e-9);
	TestTrue(TEXT("유닛을 뺀 줄은 펌프 유량이 줄어야 한다"), RowBPumpFlowM3s < RowAPumpFlowM3s);
	TestTrue(TEXT("유닛을 뺀 랙의 분기 유량이 줄어야 한다"),
		FMath::Abs(Network.GetPipeFlowM3s(RowB.RackBranchPipeIndices[3]))
		< FMath::Abs(Network.GetPipeFlowM3s(RowA.RackBranchPipeIndices[3])));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
