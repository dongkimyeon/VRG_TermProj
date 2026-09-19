// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgPumpSpeedTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// 펌프 하나 + 저항 하나로 된 가장 단순한 닫힌 루프.
	// 운전점은 해석적으로 풀린다:
	//   Shutoff*r^2 - Curve*F^2 = Resistance*F^2  ->  F = r * sqrt(Shutoff/(Curve+Resistance))
	// 즉 유량은 회전수비에 정확히 비례해야 한다 (상사칙 Flow ~ r).
	struct FPumpLoop
	{
		double ShutoffHeadPa = 4.0e5;
		double CurveCoeffPaS2M6 = 8.0e9;
		double ResistancePaS2M6 = 1.2e10;
		double Efficiency = 0.7;

		double AnalyticFlowM3s(double SpeedRatio) const
		{
			return SpeedRatio * FMath::Sqrt(ShutoffHeadPa / (CurveCoeffPaS2M6 + ResistancePaS2M6));
		}

		double AnalyticDeltaPPa(double SpeedRatio) const
		{
			const double FlowM3s = AnalyticFlowM3s(SpeedRatio);
			return ResistancePaS2M6 * FlowM3s * FlowM3s;
		}
	};

	FHydraulicSolveResult SolveLoop(const FPumpLoop& Loop, double SpeedRatio,
		FHydraulicNetwork& OutNetwork)
	{
		OutNetwork.Reset();

		FHydraulicNode Reference;
		Reference.bFixedPressure = true;
		Reference.PressurePa = 2.0e5;
		const int32 Suction = OutNetwork.AddNode(Reference);
		const int32 Discharge = OutNetwork.AddNode(FHydraulicNode());

		FHydraulicPump Pump;
		Pump.FromNodeIndex = Suction;
		Pump.ToNodeIndex = Discharge;
		Pump.ShutoffHeadPa = Loop.ShutoffHeadPa;
		Pump.CurveCoeffPaS2M6 = Loop.CurveCoeffPaS2M6;
		Pump.Efficiency = Loop.Efficiency;
		Pump.SpeedRatio = SpeedRatio;
		OutNetwork.AddPump(Pump);

		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = Discharge;
		Pipe.ToNodeIndex = Suction;
		Pipe.ResistancePaS2M6 = Loop.ResistancePaS2M6;
		OutNetwork.AddPipe(Pipe);

		FHydraulicSolveSettings Settings;
		return OutNetwork.Solve(Settings);
	}
}

// 상사칙 해석해: 회전수비 r에서 유량은 r에 비례하고 양정은 r^2에 비례한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicPumpAffinityLawTest,
	"VRG.Hydraulics.Pump.AffinityLawAnalytic", VrgPumpSpeedTestFlags)

bool FHydraulicPumpAffinityLawTest::RunTest(const FString& Parameters)
{
	const FPumpLoop Loop;
	const double SpeedRatios[] = { 0.25, 0.5, 0.75, 1.0, 1.2 };

	double MaxFlowErrorNormalized = 0.0;
	double MaxHeadErrorNormalized = 0.0;
	bool bAllConverged = true;

	const double ReferenceFlowM3s = Loop.AnalyticFlowM3s(1.0);
	const double ReferenceHeadPa = Loop.AnalyticDeltaPPa(1.0);

	for (const double SpeedRatio : SpeedRatios)
	{
		FHydraulicNetwork Network;
		const FHydraulicSolveResult Result = SolveLoop(Loop, SpeedRatio, Network);
		bAllConverged = bAllConverged && Result.bConverged;

		const double FlowM3s = Network.GetPumpFlowM3s(0);
		const double DeltaPPa = Network.GetPumpDeltaPPa(0);

		const double AnalyticFlowM3s = Loop.AnalyticFlowM3s(SpeedRatio);
		const double AnalyticDeltaPPa = Loop.AnalyticDeltaPPa(SpeedRatio);

		// 정규화 기준은 정격(r=1) 운전점이다. 작은 r에서 0으로 나누지 않는다.
		const double FlowErrorNormalized = FMath::Abs(FlowM3s - AnalyticFlowM3s) / ReferenceFlowM3s;
		const double HeadErrorNormalized = FMath::Abs(DeltaPPa - AnalyticDeltaPPa) / ReferenceHeadPa;

		MaxFlowErrorNormalized = FMath::Max(MaxFlowErrorNormalized, FlowErrorNormalized);
		MaxHeadErrorNormalized = FMath::Max(MaxHeadErrorNormalized, HeadErrorNormalized);

		AddInfo(FString::Printf(
			TEXT("r=%.2f | 유량 %.6f L/s (해석해 %.6f, 정격비 %.4f) | 양정 %.2f kPa (해석해 %.2f, 정격비 %.4f)"),
			SpeedRatio,
			FlowM3s * 1000.0, AnalyticFlowM3s * 1000.0, FlowM3s / ReferenceFlowM3s,
			DeltaPPa / 1000.0, AnalyticDeltaPPa / 1000.0, DeltaPPa / ReferenceHeadPa));
	}

	AddInfo(FString::Printf(TEXT("정격 운전점 기준 정규화 오차: 유량 최대 %.3e / 양정 최대 %.3e"),
		MaxFlowErrorNormalized, MaxHeadErrorNormalized));

	TestTrue(TEXT("모든 회전수비에서 수렴해야 한다"), bAllConverged);
	TestTrue(TEXT("유량이 해석해와 일치해야 한다"), MaxFlowErrorNormalized < 1.0e-9);
	TestTrue(TEXT("양정이 해석해와 일치해야 한다"), MaxHeadErrorNormalized < 1.0e-9);

	// 상사칙 자체를 비율로 다시 확인한다. r=0.5면 유량 0.5배, 양정 0.25배.
	FHydraulicNetwork HalfNetwork;
	SolveLoop(Loop, 0.5, HalfNetwork);
	const double FlowRatio = HalfNetwork.GetPumpFlowM3s(0) / ReferenceFlowM3s;
	const double HeadRatio = HalfNetwork.GetPumpDeltaPPa(0) / ReferenceHeadPa;

	AddInfo(FString::Printf(TEXT("r=0.5 상사칙 확인: 유량비 %.12f (기대 0.5) / 양정비 %.12f (기대 0.25)"),
		FlowRatio, HeadRatio));

	TestTrue(TEXT("유량은 회전수비에 비례해야 한다"), FMath::Abs(FlowRatio - 0.5) < 1.0e-9);
	TestTrue(TEXT("양정은 회전수비의 제곱에 비례해야 한다"), FMath::Abs(HeadRatio - 0.25) < 1.0e-9);

	return true;
}

// D8: 전력은 운전점에서 구한다. P_rated * r^3 으로 만들지 않는다.
// 이 모델에는 P_rated가 아예 없으므로, 확인할 것은 "전력이 Δp * Flow / eta와 같은가"다.
// 단순 루프에서는 그 결과가 우연히 r^3에 비례하지만, 그것은 식이 아니라 결과다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicPumpSpeedPowerTest,
	"VRG.Hydraulics.Pump.SpeedPowerFromOperatingPoint", VrgPumpSpeedTestFlags)

bool FHydraulicPumpSpeedPowerTest::RunTest(const FString& Parameters)
{
	const FPumpLoop Loop;
	const double SpeedRatios[] = { 0.25, 0.5, 0.75, 1.0, 1.2 };

	double MaxDefinitionErrorNormalized = 0.0;

	FHydraulicNetwork RatedNetwork;
	SolveLoop(Loop, 1.0, RatedNetwork);
	const double RatedElectricalW = RatedNetwork.GetPumpElectricalInputPowerW(0);

	for (const double SpeedRatio : SpeedRatios)
	{
		FHydraulicNetwork Network;
		SolveLoop(Loop, SpeedRatio, Network);

		const double DeltaPPa = Network.GetPumpDeltaPPa(0);
		const double FlowM3s = Network.GetPumpFlowM3s(0);
		const double HydraulicW = Network.GetPumpHydraulicPowerW(0);
		const double ElectricalW = Network.GetPumpElectricalInputPowerW(0);

		// 정의 검증: 유체 동력 = Δp * Flow, 전기 입력 = 유체 동력 / 효율.
		const double DefinitionHydraulicW = DeltaPPa * FlowM3s;
		const double DefinitionElectricalW = DefinitionHydraulicW / Loop.Efficiency;

		const double ErrorNormalized =
			(FMath::Abs(HydraulicW - DefinitionHydraulicW) + FMath::Abs(ElectricalW - DefinitionElectricalW))
			/ FMath::Max(RatedElectricalW, 1.0);
		MaxDefinitionErrorNormalized = FMath::Max(MaxDefinitionErrorNormalized, ErrorNormalized);

		AddInfo(FString::Printf(
			TEXT("r=%.2f | 유체 동력 %.4f W = %.2f kPa x %.6f L/s | 전기 입력 %.4f W | 정격 전력 대비 %.6f (r^3=%.6f)"),
			SpeedRatio, HydraulicW, DeltaPPa / 1000.0, FlowM3s * 1000.0, ElectricalW,
			ElectricalW / RatedElectricalW, SpeedRatio * SpeedRatio * SpeedRatio));
	}

	AddInfo(FString::Printf(TEXT("정격 전기 입력 %.4f W 기준, 전력 정의식 정규화 오차 최대 %.3e"),
		RatedElectricalW, MaxDefinitionErrorNormalized));

	TestTrue(TEXT("전력이 운전점 정의식과 일치해야 한다"), MaxDefinitionErrorNormalized < 1.0e-12);

	return true;
}

// 회전수를 0으로 두면 양정이 사라지고 유량도 0이 돼야 한다. 특이해지면 안 된다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicPumpZeroSpeedTest,
	"VRG.Hydraulics.Pump.ZeroSpeedNoSingularity", VrgPumpSpeedTestFlags)

bool FHydraulicPumpZeroSpeedTest::RunTest(const FString& Parameters)
{
	const FPumpLoop Loop;

	FHydraulicNetwork Network;
	const FHydraulicSolveResult Result = SolveLoop(Loop, 0.0, Network);

	const double FlowM3s = Network.GetPumpFlowM3s(0);
	const double DeltaPPa = Network.GetPumpDeltaPPa(0);
	const double ElectricalW = Network.GetPumpElectricalInputPowerW(0);

	AddInfo(FString::Printf(TEXT("r=0 | 수렴 %s (반복 %d회, 정규화 잔차 %.3e)"),
		Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("유량 %.6e m3/s / 양정 %.6e Pa / 전기 입력 %.6e W"),
		FlowM3s, DeltaPPa, ElectricalW));

	TestTrue(TEXT("회전수 0에서도 수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("유량이 유한해야 한다"), FMath::IsFinite(FlowM3s));
	TestTrue(TEXT("회전수 0이면 유량이 0이어야 한다"), FMath::Abs(FlowM3s) < 1.0e-12);
	TestTrue(TEXT("회전수 0이면 전력이 0이어야 한다"), FMath::Abs(ElectricalW) < 1.0e-9);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
