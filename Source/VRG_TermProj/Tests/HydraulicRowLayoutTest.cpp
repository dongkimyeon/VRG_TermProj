// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// AHydraulicNetworkDebugActor가 만드는 인로우 냉각 루프와 같은 위상을 솔버 수준에서 검증한다.
// 액터 없이도 돌아야 하므로 여기서 그래프를 직접 세운다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicRowLayoutTest,
	"VRG.Hydraulics.Network.InRowLayoutFlowSpread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FHydraulicRowLayoutTest::RunTest(const FString& Parameters)
{
	// AHydraulicNetworkDebugActor의 기본값과 같아야 한다.
	// 이 값이 바뀌면 여기도 같이 바꿔야 테스트가 실제 배치를 검증한다.
	const int32 RackCount = 6;
	const double HeaderSegmentResistancePaS2M6 = 2.7e8;
	const double RackBranchResistancePaS2M6 = 1.4e11;
	const double ReturnToCduResistancePaS2M6 = 5.4e8;

	FHydraulicNetwork Network;

	FHydraulicNode CduNode;
	CduNode.bFixedPressure = true;
	CduNode.PressurePa = 2.0e5;
	const int32 Cdu = Network.AddNode(CduNode);

	TArray<int32> SupplyNodes;
	TArray<int32> RackNodes;
	TArray<int32> ReturnNodes;
	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		SupplyNodes.Add(Network.AddNode(FHydraulicNode()));
		RackNodes.Add(Network.AddNode(FHydraulicNode()));
		ReturnNodes.Add(Network.AddNode(FHydraulicNode()));
	}

	FHydraulicPump Pump;
	Pump.FromNodeIndex = Cdu;
	Pump.ToNodeIndex = SupplyNodes[0];
	Pump.ShutoffHeadPa = 4.0e5;
	Pump.CurveCoeffPaS2M6 = 8.0e9;
	Pump.Efficiency = 0.7;
	Network.AddPump(Pump);

	auto AddPipe = [&Network](int32 FromNodeIndex, int32 ToNodeIndex, double ResistancePaS2M6)
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = FromNodeIndex;
		Pipe.ToNodeIndex = ToNodeIndex;
		Pipe.ResistancePaS2M6 = ResistancePaS2M6;
		return Network.AddPipe(Pipe);
	};

	TArray<int32> RackBranchPipes;
	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		if (RackIndex + 1 < RackCount)
		{
			AddPipe(SupplyNodes[RackIndex], SupplyNodes[RackIndex + 1], HeaderSegmentResistancePaS2M6);
		}
		RackBranchPipes.Add(AddPipe(SupplyNodes[RackIndex], RackNodes[RackIndex], RackBranchResistancePaS2M6));
		AddPipe(RackNodes[RackIndex], ReturnNodes[RackIndex], RackBranchResistancePaS2M6);
		if (RackIndex + 1 < RackCount)
		{
			AddPipe(ReturnNodes[RackIndex + 1], ReturnNodes[RackIndex], HeaderSegmentResistancePaS2M6);
		}
	}
	AddPipe(ReturnNodes[0], Cdu, ReturnToCduResistancePaS2M6);

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	// 랙 분기 유량의 합은 펌프 유량과 같아야 한다.
	double RackFlowSumM3s = 0.0;
	double MinRackFlowM3s = TNumericLimits<double>::Max();
	double MaxRackFlowM3s = 0.0;
	FString PerRackFlows;
	for (int32 RackIndex = 0; RackIndex < RackBranchPipes.Num(); ++RackIndex)
	{
		const double FlowM3s = Network.GetPipeFlowM3s(RackBranchPipes[RackIndex]);
		RackFlowSumM3s += FlowM3s;
		MinRackFlowM3s = FMath::Min(MinRackFlowM3s, FlowM3s);
		MaxRackFlowM3s = FMath::Max(MaxRackFlowM3s, FlowM3s);
		PerRackFlows += FString::Printf(TEXT("%.2f "), FlowM3s * 1000.0);
	}

	const double PumpFlowM3s = Network.GetPumpFlowM3s(0);
	const double NormalizedSumError = FMath::Abs(RackFlowSumM3s - PumpFlowM3s) / PumpFlowM3s;
	const double SpreadPercent = (1.0 - MinRackFlowM3s / MaxRackFlowM3s) * 100.0;

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("펌프 유량 %.3f L/s, 양정 %.1f kPa, 전기 입력 %.2f kW"),
		PumpFlowM3s * 1000.0, Network.GetPumpDeltaPPa(0) / 1000.0,
		Network.GetPumpElectricalInputPowerW(0) / 1000.0));
	AddInfo(FString::Printf(TEXT("랙별 분기 유량 [L/s]: %s"), *PerRackFlows));
	AddInfo(FString::Printf(TEXT("최소 %.3f ~ 최대 %.3f L/s, 편차 %.2f%%"),
		MinRackFlowM3s * 1000.0, MaxRackFlowM3s * 1000.0, SpreadPercent));
	AddInfo(FString::Printf(TEXT("랙 분기 합 %.6f vs 펌프 유량 %.6f m3/s, 정규화 오차 %.3e"),
		RackFlowSumM3s, PumpFlowM3s, NormalizedSumError));

	// 물리적 타당성 검사.
	// 솔버가 수학적으로 맞아도 파라미터가 1000배 어긋나면 그림은 멀쩡해 보인다.
	// 랙 1대 15 kW를 기준으로 냉각수 온도 상승을 역산해서 상식 범위인지 본다.
	// (S1 열모델과 연결하기 전이라 여기서는 참고 계산이다.)
	const double RackHeatWForSanity = 1.5e4;
	const double WaterDensityKgM3 = 997.0;
	const double WaterCpJKgK = 4182.0;
	const double AverageRackFlowM3s = RackFlowSumM3s / RackBranchPipes.Num();
	const double AverageMassFlowKgs = AverageRackFlowM3s * WaterDensityKgM3;
	const double ImpliedDeltaTK = RackHeatWForSanity / (AverageMassFlowKgs * WaterCpJKgK);
	const double PumpToItPowerRatio = Network.GetPumpElectricalInputPowerW(0)
		/ (RackHeatWForSanity * RackBranchPipes.Num());

	AddInfo(FString::Printf(TEXT("[타당성] 랙 평균 유량 %.3f L/s -> 15 kW 기준 냉각수 ΔT %.2f K"),
		AverageRackFlowM3s * 1000.0, ImpliedDeltaTK));
	AddInfo(FString::Printf(TEXT("[타당성] 펌프 전기 입력 / IT 부하 = %.4f (%.2f%%)"),
		PumpToItPowerRatio, PumpToItPowerRatio * 100.0));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("랙 분기 유량의 합이 펌프 유량과 같아야 한다"), NormalizedSumError < 1.0e-9);
	TestTrue(TEXT("모든 랙 분기 유량이 양수여야 한다"), MinRackFlowM3s > 0.0);

	// 아래 두 기준은 물리 법칙이 아니라 "기본 파라미터가 상식 범위인가"를 보는
	// 넓은 안전망이다. 파라미터를 의도적으로 바꾸면 같이 갱신해야 한다.
	TestTrue(TEXT("15 kW 기준 냉각수 ΔT가 1~15 K 범위여야 한다"),
		ImpliedDeltaTK > 1.0 && ImpliedDeltaTK < 15.0);
	TestTrue(TEXT("펌프 전력이 IT 부하의 10%를 넘지 않아야 한다"), PumpToItPowerRatio < 0.10);

	// 헤더 저항 때문에 CDU에서 가까운 랙이 더 많이 받는다. 이 편차가 사라지면
	// 헤더 저항이 반영되지 않았다는 뜻이다.
	TestTrue(TEXT("헤더 저항으로 인한 랙별 유량 편차가 존재해야 한다"),
		Network.GetPipeFlowM3s(RackBranchPipes[0]) > Network.GetPipeFlowM3s(RackBranchPipes.Last()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
