// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/DenseLinearSystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgHydraulicTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	FHydraulicNode MakeFixedNode(double PressurePa)
	{
		FHydraulicNode Node;
		Node.bFixedPressure = true;
		Node.PressurePa = PressurePa;
		return Node;
	}

	FHydraulicNode MakeFreeNode(double ExternalInflowM3s = 0.0)
	{
		FHydraulicNode Node;
		Node.bFixedPressure = false;
		Node.ExternalInflowM3s = ExternalInflowM3s;
		return Node;
	}

	FHydraulicPipe MakePipe(int32 FromNodeIndex, int32 ToNodeIndex, double ResistancePaS2M6)
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = FromNodeIndex;
		Pipe.ToNodeIndex = ToNodeIndex;
		Pipe.ResistancePaS2M6 = ResistancePaS2M6;
		return Pipe;
	}
}

// 밀행렬 LU가 맞는지 먼저 확인한다. 여기가 틀리면 Newton 결과를 해석할 수 없다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDenseLinearSystemTest,
	"VRG.Hydraulics.DenseLinearSystem.SolvesKnownSystem", VrgHydraulicTestFlags)

bool FDenseLinearSystemTest::RunTest(const FString& Parameters)
{
	// 해가 x = (2, 3, -1)인 3x3 계.
	TArray<double> Matrix = {
		2.0, 1.0, -1.0,
		-3.0, -1.0, 2.0,
		-2.0, 1.0, 2.0
	};
	TArray<double> Rhs = { 8.0, -11.0, -3.0 };

	const bool bSolved = SolveDenseLinearSystem(Matrix, Rhs, 3);

	TestTrue(TEXT("선형 계를 풀 수 있어야 한다"), bSolved);
	TestTrue(TEXT("x0"), FMath::Abs(Rhs[0] - 2.0) < 1.0e-12);
	TestTrue(TEXT("x1"), FMath::Abs(Rhs[1] - 3.0) < 1.0e-12);
	TestTrue(TEXT("x2"), FMath::Abs(Rhs[2] + 1.0) < 1.0e-12);

	// 특이 행렬은 실패로 보고돼야 한다.
	TArray<double> Singular = { 1.0, 2.0, 2.0, 4.0 };
	TArray<double> SingularRhs = { 1.0, 2.0 };
	TestFalse(TEXT("특이 행렬은 실패해야 한다"), SolveDenseLinearSystem(Singular, SingularRhs, 2));

	return true;
}

// 해석해 1: 파이프 하나. Δp = K * Flow^2.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicSinglePipeTest,
	"VRG.Hydraulics.Network.SinglePipeAnalytic", VrgHydraulicTestFlags)

bool FHydraulicSinglePipeTest::RunTest(const FString& Parameters)
{
	const double SupplyPressurePa = 4.0e5;
	const double ResistancePaS2M6 = 2.0e6;
	const double DemandM3s = 0.2;

	FHydraulicNetwork Network;
	const int32 Supply = Network.AddNode(MakeFixedNode(SupplyPressurePa));
	const int32 Outlet = Network.AddNode(MakeFreeNode(-DemandM3s));
	Network.AddPipe(MakePipe(Supply, Outlet, ResistancePaS2M6));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	const double AnalyticPressurePa = SupplyPressurePa - ResistancePaS2M6 * DemandM3s * DemandM3s;
	const double PressureErrorPa = FMath::Abs(Network.GetNodePressurePa(Outlet) - AnalyticPressurePa);
	const double NormalizedPressureError = PressureErrorPa / SupplyPressurePa;
	const double FlowErrorM3s = FMath::Abs(Network.GetPipeFlowM3s(0) - DemandM3s);
	const double NormalizedFlowError = FlowErrorM3s / DemandM3s;

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e (기준 유량 %.4f m3/s, 기준 압력 %.1f Pa)"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed,
		Result.NormalizedResidual, Result.ReferenceFlowM3s, Result.ReferencePressurePa));
	AddInfo(FString::Printf(TEXT("출구 압력 수치 %.6f Pa / 해석해 %.6f Pa / 오차 %.3e Pa (공급압 기준 %.3e)"),
		Network.GetNodePressurePa(Outlet), AnalyticPressurePa, PressureErrorPa, NormalizedPressureError));
	AddInfo(FString::Printf(TEXT("유량 오차 %.3e m3/s (수요 기준 %.3e)"), FlowErrorM3s, NormalizedFlowError));

	// 해의 오차 하한은 솔버의 수렴 목표(NormalizedResidualTarget, 기본 1e-10)가 결정한다.
	// 수렴 목표보다 엄격한 기준을 걸면 테스트가 불안정해지므로 한 자릿수 위에 둔다.
	// 측정값: 압력 3.54e-11, 유량 8.85e-11 (둘 다 무차원).
	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("출구 압력이 해석해와 일치해야 한다"), NormalizedPressureError < 1.0e-9);
	TestTrue(TEXT("파이프 유량이 수요와 일치해야 한다"), NormalizedFlowError < 1.0e-9);

	return true;
}

// 해석해 2: 직렬 저항은 그대로 더해진다. Δp_total = (K1 + K2) * Flow^2.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicSeriesTest,
	"VRG.Hydraulics.Network.SeriesAnalytic", VrgHydraulicTestFlags)

bool FHydraulicSeriesTest::RunTest(const FString& Parameters)
{
	const double InletPressurePa = 5.0e5;
	const double OutletPressurePa = 1.0e5;
	const double Resistance1PaS2M6 = 2.0e6;
	const double Resistance2PaS2M6 = 3.0e6;

	FHydraulicNetwork Network;
	const int32 Inlet = Network.AddNode(MakeFixedNode(InletPressurePa));
	const int32 Middle = Network.AddNode(MakeFreeNode());
	const int32 Outlet = Network.AddNode(MakeFixedNode(OutletPressurePa));
	Network.AddPipe(MakePipe(Inlet, Middle, Resistance1PaS2M6));
	Network.AddPipe(MakePipe(Middle, Outlet, Resistance2PaS2M6));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	const double AnalyticFlowM3s = FMath::Sqrt((InletPressurePa - OutletPressurePa)
		/ (Resistance1PaS2M6 + Resistance2PaS2M6));
	const double AnalyticMiddlePa = InletPressurePa - Resistance1PaS2M6 * AnalyticFlowM3s * AnalyticFlowM3s;

	const double FlowErrorM3s = FMath::Abs(Network.GetPipeFlowM3s(0) - AnalyticFlowM3s);
	const double NormalizedFlowError = FlowErrorM3s / AnalyticFlowM3s;
	const double PressureErrorPa = FMath::Abs(Network.GetNodePressurePa(Middle) - AnalyticMiddlePa);

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("유량 수치 %.9f / 해석해 %.9f m3/s / 정규화 오차 %.3e"),
		Network.GetPipeFlowM3s(0), AnalyticFlowM3s, NormalizedFlowError));
	AddInfo(FString::Printf(TEXT("중간 노드 압력 오차 %.3e Pa"), PressureErrorPa));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("직렬 유량이 해석해와 일치해야 한다"), NormalizedFlowError < 1.0e-10);
	TestTrue(TEXT("두 파이프 유량이 같아야 한다"),
		FMath::Abs(Network.GetPipeFlowM3s(0) - Network.GetPipeFlowM3s(1)) < 1.0e-10);

	return true;
}

// 해석해 3: 병렬 저항. 1/sqrt(K_eq) = 1/sqrt(K1) + 1/sqrt(K2).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicParallelTest,
	"VRG.Hydraulics.Network.ParallelAnalytic", VrgHydraulicTestFlags)

bool FHydraulicParallelTest::RunTest(const FString& Parameters)
{
	const double SupplyPressurePa = 4.0e5;
	const double Resistance1PaS2M6 = 2.0e6;
	const double Resistance2PaS2M6 = 8.0e6;
	const double DemandM3s = 0.3;

	FHydraulicNetwork Network;
	const int32 Supply = Network.AddNode(MakeFixedNode(SupplyPressurePa));
	const int32 Outlet = Network.AddNode(MakeFreeNode(-DemandM3s));
	Network.AddPipe(MakePipe(Supply, Outlet, Resistance1PaS2M6));
	Network.AddPipe(MakePipe(Supply, Outlet, Resistance2PaS2M6));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	const double InverseRootSum = 1.0 / FMath::Sqrt(Resistance1PaS2M6) + 1.0 / FMath::Sqrt(Resistance2PaS2M6);
	const double AnalyticDeltaPPa = FMath::Square(DemandM3s / InverseRootSum);
	const double AnalyticOutletPa = SupplyPressurePa - AnalyticDeltaPPa;

	const double PressureErrorPa = FMath::Abs(Network.GetNodePressurePa(Outlet) - AnalyticOutletPa);
	const double NormalizedPressureError = PressureErrorPa / SupplyPressurePa;
	const double TotalFlowM3s = Network.GetPipeFlowM3s(0) + Network.GetPipeFlowM3s(1);

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("가지 유량 %.6f + %.6f = %.6f m3/s (수요 %.6f)"),
		Network.GetPipeFlowM3s(0), Network.GetPipeFlowM3s(1), TotalFlowM3s, DemandM3s));
	AddInfo(FString::Printf(TEXT("출구 압력 오차 %.3e Pa (공급압 기준 %.3e)"),
		PressureErrorPa, NormalizedPressureError));

	// 기준 근거는 SinglePipeAnalytic과 같다 (수렴 목표가 오차 하한을 정한다).
	// 측정값: 압력 3.54e-11, 유량 합 무차원 오차 8.85e-11.
	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("병렬 압력강하가 해석해와 일치해야 한다"), NormalizedPressureError < 1.0e-9);
	TestTrue(TEXT("가지 유량의 합이 수요와 같아야 한다"),
		FMath::Abs(TotalFlowM3s - DemandM3s) / DemandM3s < 1.0e-9);

	return true;
}

// 해석해 4 + D8: 펌프 운전점. 펌프 곡선과 시스템 곡선의 교점.
// Shutoff - Curve*F^2 = K*F^2  =>  F = sqrt(Shutoff / (Curve + K))
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicPumpOperatingPointTest,
	"VRG.Hydraulics.Network.PumpOperatingPointAnalytic", VrgHydraulicTestFlags)

bool FHydraulicPumpOperatingPointTest::RunTest(const FString& Parameters)
{
	const double ShutoffHeadPa = 3.0e5;
	const double CurveCoeffPaS2M6 = 1.0e6;
	const double ResistancePaS2M6 = 2.0e6;
	const double Efficiency = 0.7;

	FHydraulicNetwork Network;
	const int32 Reference = Network.AddNode(MakeFixedNode(1.0e5));
	const int32 Discharge = Network.AddNode(MakeFreeNode());

	FHydraulicPump Pump;
	Pump.FromNodeIndex = Reference;
	Pump.ToNodeIndex = Discharge;
	Pump.ShutoffHeadPa = ShutoffHeadPa;
	Pump.CurveCoeffPaS2M6 = CurveCoeffPaS2M6;
	Pump.Efficiency = Efficiency;
	Network.AddPump(Pump);

	// 토출에서 기준 노드로 돌아오는 폐루프.
	Network.AddPipe(MakePipe(Discharge, Reference, ResistancePaS2M6));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	const double AnalyticFlowM3s = FMath::Sqrt(ShutoffHeadPa / (CurveCoeffPaS2M6 + ResistancePaS2M6));
	const double AnalyticDeltaPPa = ResistancePaS2M6 * AnalyticFlowM3s * AnalyticFlowM3s;
	const double AnalyticHydraulicPowerW = AnalyticDeltaPPa * AnalyticFlowM3s;
	const double AnalyticElectricalPowerW = AnalyticHydraulicPowerW / Efficiency;

	const double FlowErrorM3s = FMath::Abs(Network.GetPumpFlowM3s(0) - AnalyticFlowM3s);
	const double NormalizedFlowError = FlowErrorM3s / AnalyticFlowM3s;
	const double PowerErrorW = FMath::Abs(Network.GetPumpHydraulicPowerW(0) - AnalyticHydraulicPowerW);
	const double NormalizedPowerError = PowerErrorW / AnalyticHydraulicPowerW;

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("운전점 유량 수치 %.9f / 해석해 %.9f m3/s / 정규화 오차 %.3e"),
		Network.GetPumpFlowM3s(0), AnalyticFlowM3s, NormalizedFlowError));
	AddInfo(FString::Printf(TEXT("운전점 양정 %.3f Pa / 유체 동력 %.3f W / 전기 입력 %.3f W (효율 %.2f)"),
		Network.GetPumpDeltaPPa(0), Network.GetPumpHydraulicPowerW(0),
		Network.GetPumpElectricalInputPowerW(0), Efficiency));
	AddInfo(FString::Printf(TEXT("유체 동력 정규화 오차 %.3e"), NormalizedPowerError));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("운전점 유량이 해석해와 일치해야 한다"), NormalizedFlowError < 1.0e-9);
	TestTrue(TEXT("유체 동력이 Δp*Flow와 일치해야 한다"), NormalizedPowerError < 1.0e-9);
	TestTrue(TEXT("전기 입력은 유체 동력을 효율로 나눈 값이어야 한다"),
		FMath::Abs(Network.GetPumpElectricalInputPowerW(0) - AnalyticElectricalPowerW)
			/ AnalyticElectricalPowerW < 1.0e-9);

	return true;
}

// 보존: 자유 노드마다 유입과 유출이 맞아야 하고, 전체 공급량과 전체 수요가 같아야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicMassConservationTest,
	"VRG.Hydraulics.Network.MassConservation", VrgHydraulicTestFlags)

bool FHydraulicMassConservationTest::RunTest(const FString& Parameters)
{
	// 펌프 1대 + 공급 헤더 + 랙 3대 + 회수 헤더로 이뤄진 폐루프.
	const double RackDemandsM3s[] = { 0.08, 0.05, 0.11 };

	FHydraulicNetwork Network;
	const int32 Reference = Network.AddNode(MakeFixedNode(1.0e5));
	const int32 SupplyHeader = Network.AddNode(MakeFreeNode());
	const int32 ReturnHeader = Network.AddNode(MakeFreeNode());

	FHydraulicPump Pump;
	Pump.FromNodeIndex = Reference;
	Pump.ToNodeIndex = SupplyHeader;
	Pump.ShutoffHeadPa = 6.0e5;
	Pump.CurveCoeffPaS2M6 = 5.0e5;
	Network.AddPump(Pump);

	// 랙마다 공급 헤더 -> 랙 노드 -> 회수 헤더.
	TArray<int32> RackNodes;
	for (int32 RackIndex = 0; RackIndex < 3; ++RackIndex)
	{
		const int32 RackNode = Network.AddNode(MakeFreeNode());
		RackNodes.Add(RackNode);
		Network.AddPipe(MakePipe(SupplyHeader, RackNode, 1.0e6 + RackIndex * 4.0e5));
		Network.AddPipe(MakePipe(RackNode, ReturnHeader, 8.0e5 + RackIndex * 3.0e5));
	}

	Network.AddPipe(MakePipe(ReturnHeader, Reference, 4.0e5));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	double MaxImbalanceM3s = 0.0;
	for (int32 NodeIndex = 0; NodeIndex < Network.GetNumNodes(); ++NodeIndex)
	{
		// 고정 압력 노드는 경계 공급원이라 불균형이 0이 아닌 게 정상이다.
		if (NodeIndex == Reference)
		{
			continue;
		}
		MaxImbalanceM3s = FMath::Max(MaxImbalanceM3s, FMath::Abs(Network.GetNodeImbalanceM3s(NodeIndex)));
	}

	const double PumpFlowM3s = Network.GetPumpFlowM3s(0);
	const double NormalizedImbalance = MaxImbalanceM3s / FMath::Max(FMath::Abs(PumpFlowM3s), 1.0e-12);

	AddInfo(FString::Printf(TEXT("수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("펌프 유량 %.6f m3/s, 양정 %.1f Pa, 전기 입력 %.1f W"),
		PumpFlowM3s, Network.GetPumpDeltaPPa(0), Network.GetPumpElectricalInputPowerW(0)));
	AddInfo(FString::Printf(TEXT("최대 노드 불균형 %.3e m3/s / 펌프 유량 기준 정규화 %.3e"),
		MaxImbalanceM3s, NormalizedImbalance));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("펌프 유량이 양수여야 한다"), PumpFlowM3s > 0.0);
	TestTrue(TEXT("자유 노드 질량 보존이 성립해야 한다"), NormalizedImbalance < 1.0e-9);

	return true;
}

// D5: 반복 상한에 걸리면 수렴 실패를 결과에 표시해야 한다. 조용히 답을 내놓으면 안 된다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicNonConvergenceReportedTest,
	"VRG.Hydraulics.Network.NonConvergenceReported", VrgHydraulicTestFlags)

bool FHydraulicNonConvergenceReportedTest::RunTest(const FString& Parameters)
{
	FHydraulicNetwork Network;
	const int32 Inlet = Network.AddNode(MakeFixedNode(5.0e5));
	const int32 Middle = Network.AddNode(MakeFreeNode());
	const int32 Outlet = Network.AddNode(MakeFixedNode(1.0e5));
	Network.AddPipe(MakePipe(Inlet, Middle, 2.0e6));
	Network.AddPipe(MakePipe(Middle, Outlet, 3.0e6));

	FHydraulicSolveSettings Settings;
	Settings.MaxIterations = 1;
	Settings.NormalizedResidualTarget = 1.0e-14;

	const FHydraulicSolveResult Result = Network.Solve(Settings);

	AddInfo(FString::Printf(TEXT("반복 상한 1회 | 수렴 %s, 반복 %d회, 정규화 잔차 %.3e"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));

	TestFalse(TEXT("반복 상한에 걸리면 수렴 실패로 보고돼야 한다"), Result.bConverged);
	TestEqual(TEXT("반복 상한만큼만 돌아야 한다"), Result.IterationsUsed, 1);
	TestTrue(TEXT("잔차가 결과에 실려야 한다"), Result.NormalizedResidual > 0.0);

	return true;
}

// 구성이 잘못된 네트워크(고정 압력 노드 없음)는 조용히 풀지 말고 거부해야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicIllPosedTest,
	"VRG.Hydraulics.Network.IllPosedReported", VrgHydraulicTestFlags)

bool FHydraulicIllPosedTest::RunTest(const FString& Parameters)
{
	FHydraulicNetwork Network;
	const int32 NodeA = Network.AddNode(MakeFreeNode(0.1));
	const int32 NodeB = Network.AddNode(MakeFreeNode(-0.1));
	Network.AddPipe(MakePipe(NodeA, NodeB, 1.0e6));

	const FHydraulicSolveResult Result = Network.Solve(FHydraulicSolveSettings());

	AddInfo(FString::Printf(TEXT("고정 압력 노드 없음 | bIllPosed=%s, bConverged=%s"),
		Result.bIllPosed ? TEXT("true") : TEXT("false"),
		Result.bConverged ? TEXT("true") : TEXT("false")));

	TestTrue(TEXT("고정 압력 노드가 없으면 구성 오류로 보고돼야 한다"), Result.bIllPosed);
	TestFalse(TEXT("구성 오류일 때 수렴했다고 말하면 안 된다"), Result.bConverged);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
