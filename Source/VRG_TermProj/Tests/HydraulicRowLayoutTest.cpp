// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// GB200 NVL72 한 줄(랙 8대)의 인로우 냉각 루프를 솔버 수준에서 검증한다.
// 위상은 방 액터와 같은 InRowLoopBuilder가 만든다 (기본값 = NVL72, 2026-09-27에 15 kW 하이브리드 기준에서 바꿨다).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicRowLayoutTest,
	"VRG.Hydraulics.Network.InRowLayoutFlowSpread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FHydraulicRowLayoutTest::RunTest(const FString& Parameters)
{
	FInRowLoopSpec Spec;
	Spec.NumRacks = 8;

	FHydraulicNetwork Network;
	const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, Spec);
	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	// 랙 분기 유량의 합은 펌프 유량과 같아야 한다.
	double RackFlowSumM3s = 0.0;
	double MinRackFlowM3s = TNumericLimits<double>::Max();
	double MaxRackFlowM3s = 0.0;
	FString PerRackFlows;
	for (const int32 PipeIndex : Handles.RackBranchPipeIndices)
	{
		const double FlowM3s = Network.GetPipeFlowM3s(PipeIndex);
		RackFlowSumM3s += FlowM3s;
		MinRackFlowM3s = FMath::Min(MinRackFlowM3s, FlowM3s);
		MaxRackFlowM3s = FMath::Max(MaxRackFlowM3s, FlowM3s);
		PerRackFlows += FString::Printf(TEXT("%.3f "), FlowM3s * 1000.0);
	}

	const double PumpFlowM3s = Network.GetPumpFlowM3s(Handles.PumpIndex);
	const double NormalizedSumError = FMath::Abs(RackFlowSumM3s - PumpFlowM3s) / PumpFlowM3s;
	const double SpreadPercent = (1.0 - MinRackFlowM3s / MaxRackFlowM3s) * 100.0;

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("펌프 유량 %.3f L/s, 양정 %.1f kPa, 전기 입력 %.2f kW"),
		PumpFlowM3s * 1000.0, Network.GetPumpDeltaPPa(Handles.PumpIndex) / 1000.0,
		Network.GetPumpElectricalInputPowerW(Handles.PumpIndex) / 1000.0));
	AddInfo(FString::Printf(TEXT("랙별 분기 유량 [L/s]: %s"), *PerRackFlows));
	AddInfo(FString::Printf(TEXT("최소 %.3f ~ 최대 %.3f L/s, 편차 %.2f%%"),
		MinRackFlowM3s * 1000.0, MaxRackFlowM3s * 1000.0, SpreadPercent));
	AddInfo(FString::Printf(TEXT("랙 분기 합 %.6f vs 펌프 유량 %.6f m3/s, 정규화 오차 %.3e"),
		RackFlowSumM3s, PumpFlowM3s, NormalizedSumError));

	// 물리적 타당성 검사. 솔버가 수학적으로 맞아도 파라미터가 크게 어긋나면 그림은 멀쩡해 보인다.
	// NVL72 랙 1대의 설계 냉각수 부하(부품별 냉각수 분담의 합)로 냉각수 온도 상승을 역산한다.
	const FRackThermalParams RackParams = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalInput RackInput;
	RackInput.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	const double RackCoolantHeatW = FRackThermalModel::DesignHeatToCoolantW(RackParams, RackInput);
	const double WaterDensityKgM3 = 997.0;
	const double WaterCpJKgK = 4182.0;
	const double AverageRackFlowM3s = RackFlowSumM3s / Handles.RackBranchPipeIndices.Num();
	const double ImpliedDeltaTK = RackCoolantHeatW / (AverageRackFlowM3s * WaterDensityKgM3 * WaterCpJKgK);
	double RackItW = 0.0;  // nominal parts only (power-shelf loss comes on top), enough for a 10% sanity bound
	for (const double H : RackInput.ComponentHeatW) RackItW += H;
	const double PumpToItPowerRatio = Network.GetPumpElectricalInputPowerW(Handles.PumpIndex) / (RackItW * Spec.NumRacks);

	AddInfo(FString::Printf(TEXT("[타당성] 랙 평균 유량 %.3f L/s, 랙 냉각수 설계 부하 %.1f kW -> 냉각수 ΔT %.2f K"),
		AverageRackFlowM3s * 1000.0, RackCoolantHeatW / 1000.0, ImpliedDeltaTK));
	AddInfo(FString::Printf(TEXT("[타당성] 펌프 전기 입력 / IT 부하(랙 %.1f kW x %d) = %.4f (%.2f%%)"),
		RackItW / 1000.0, Spec.NumRacks, PumpToItPowerRatio, PumpToItPowerRatio * 100.0));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("랙 분기 유량의 합이 펌프 유량과 같아야 한다"), NormalizedSumError < 1.0e-9);
	TestTrue(TEXT("모든 랙 분기 유량이 양수여야 한다"), MinRackFlowM3s > 0.0);

	// 아래 두 기준은 물리 법칙이 아니라 "기본 파라미터가 상식 범위인가"를 보는 넓은 안전망이다.
	TestTrue(TEXT("NVL72 냉각수 ΔT가 5~15 K 범위여야 한다"), ImpliedDeltaTK > 5.0 && ImpliedDeltaTK < 15.0);
	TestTrue(TEXT("펌프 전력이 IT 부하의 10%를 넘지 않아야 한다"), PumpToItPowerRatio < 0.10);

	// 헤더 저항 때문에 CDU에서 가까운 랙이 더 많이 받는다. 이 편차가 사라지면 헤더 저항이 반영되지 않았다는 뜻이다.
	TestTrue(TEXT("헤더 저항으로 인한 랙별 유량 편차가 존재해야 한다"),
		Network.GetPipeFlowM3s(Handles.RackBranchPipeIndices[0]) > Network.GetPipeFlowM3s(Handles.RackBranchPipeIndices.Last()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
