// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RadiatorModel.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgRadiatorTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	constexpr double WaterCpJKgK = 4182.0;
}

// 해석해: eps = 1 - exp(-UA/(m*Cp)). 식 자체를 직접 맞춰 본다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiatorEffectivenessAnalyticTest,
	"VRG.Thermal.Radiator.EffectivenessAnalytic", VrgRadiatorTestFlags)

bool FRadiatorEffectivenessAnalyticTest::RunTest(const FString& Parameters)
{
	FRadiatorParams Params;
	Params.UAWK = 1.6e3;
	Params.AmbientTempK = 295.15;

	const double MassFlowKgs = 0.82;
	const double CapacityRateWK = MassFlowKgs * WaterCpJKgK;
	const double InletTempK = 303.15;

	const double Eps = RadiatorModel::Effectiveness(Params.UAWK, CapacityRateWK);
	const double AnalyticEps = 1.0 - FMath::Exp(-Params.UAWK / CapacityRateWK);
	const double EpsErrorAbs = FMath::Abs(Eps - AnalyticEps);

	const double OutletTempK = RadiatorModel::OutletTempK(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
	const double AnalyticOutletTempK = InletTempK - AnalyticEps * (InletTempK - Params.AmbientTempK);
	const double OutletErrorAbs = FMath::Abs(OutletTempK - AnalyticOutletTempK);

	AddInfo(FString::Printf(TEXT("UA %.0f W/K / 열용량률 %.1f W/K / NTU %.4f"),
		Params.UAWK, CapacityRateWK, Params.UAWK / CapacityRateWK));
	AddInfo(FString::Printf(TEXT("eps 수치 %.12f / 해석해 %.12f / 절대 오차 %.3e"),
		Eps, AnalyticEps, EpsErrorAbs));
	AddInfo(FString::Printf(TEXT("입구 %.2f C -> 출구 %.2f C (외기 %.2f C, 접근온도 %.2f K)"),
		InletTempK - 273.15, OutletTempK - 273.15, Params.AmbientTempK - 273.15,
		OutletTempK - Params.AmbientTempK));

	TestTrue(TEXT("effectiveness가 해석해와 일치해야 한다"), EpsErrorAbs < 1.0e-15);
	TestTrue(TEXT("출구 온도가 해석해와 일치해야 한다"), OutletErrorAbs < 1.0e-12);

	return true;
}

// 열수지: 반출열 = 열용량률 * (입구 - 출구). 두 식이 같은 eps를 써야 성립한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiatorEnergyBalanceTest,
	"VRG.Thermal.Radiator.EnergyBalance", VrgRadiatorTestFlags)

bool FRadiatorEnergyBalanceTest::RunTest(const FString& Parameters)
{
	FRadiatorParams Params;
	Params.AmbientTempK = 295.15;

	double MaxNormalizedError = 0.0;

	// UA와 유량을 넓게 훑는다. 한 점에서만 맞는 식일 수도 있다.
	const double UAValuesWK[] = { 1.0e2, 8.0e2, 1.6e3, 5.0e3, 2.0e4 };
	const double MassFlowValuesKgs[] = { 0.05, 0.3, 0.82, 2.0 };

	for (const double UAWK : UAValuesWK)
	{
		for (const double MassFlowKgs : MassFlowValuesKgs)
		{
			Params.UAWK = UAWK;
			const double InletTempK = 308.15;
			const double CapacityRateWK = MassFlowKgs * WaterCpJKgK;

			const double OutletTempK = RadiatorModel::OutletTempK(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
			const double HeatRejectedW = RadiatorModel::HeatRejectedW(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
			const double EnthalpyDropW = CapacityRateWK * (InletTempK - OutletTempK);

			const double NormalizedError = FMath::Abs(HeatRejectedW - EnthalpyDropW)
				/ FMath::Max(FMath::Abs(HeatRejectedW), 1.0);
			MaxNormalizedError = FMath::Max(MaxNormalizedError, NormalizedError);
		}
	}

	AddInfo(FString::Printf(TEXT("UA 5종 x 유량 4종 = 20개 운전점, 반출열 기준 최대 정규화 오차 %.3e"),
		MaxNormalizedError));

	// 기준: 측정값 7.15e-14에 여유를 둔 값.
	// 하한을 정하는 것은 식의 정확도가 아니라 자릿수 소실이다. 온도를 K로 들고
	// 있으므로 (Tin - Tout)은 300 K간의 뺄셈이다. UA가 작고 유량이 큰 운전점에서는
	// 강하가 0.2 K 수준으로 작아져 상대 오차가 300*eps/0.2 배로 증폭된다.
	TestTrue(TEXT("반출열과 엔탈피 강하가 일치해야 한다"), MaxNormalizedError < 1.0e-12);

	return true;
}

// 크기를 키우면 효과가 포화해야 한다. 선형으로 좋아지면 epsilon-NTU가 아니다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiatorSaturationTest,
	"VRG.Thermal.Radiator.EffectivenessSaturates", VrgRadiatorTestFlags)

bool FRadiatorSaturationTest::RunTest(const FString& Parameters)
{
	FRadiatorParams Params;
	Params.AmbientTempK = 295.15;

	const double MassFlowKgs = 0.82;
	const double InletTempK = 308.15;
	const double CapacityRateWK = MassFlowKgs * WaterCpJKgK;
	const double MaxPossibleW = CapacityRateWK * (InletTempK - Params.AmbientTempK);

	AddInfo(FString::Printf(TEXT("열용량률 %.1f W/K, 이론 최대 반출 %.1f W (입구 %.2f C, 외기 %.2f C)"),
		CapacityRateWK, MaxPossibleW, InletTempK - 273.15, Params.AmbientTempK - 273.15));

	// 포화의 올바른 진술은 UA에 대한 오목성이다: 같은 크기만큼 UA를 더할 때
	// 얻는 반출열 증가분이 갈수록 줄어야 한다 (1 - exp(-UA/C)가 UA에 대해 오목).
	//
	// UA를 "두 배씩" 키우며 보면 증가분이 줄지 않는다. 그건 식이 틀려서가 아니라
	// 로그 축에서 보면 S자 곡선이라 변곡점 전까지는 증가분이 오히려 커지기 때문이다.
	// 선형 증분으로 봐야 포화를 제대로 검증한다.
	const double UAStepWK = 8.0e2;
	double PreviousHeatW = 0.0;
	double PreviousGainW = TNumericLimits<double>::Max();
	bool bGainShrinks = true;
	bool bNeverExceedsMax = true;

	for (int32 StepIndex = 1; StepIndex <= 16; ++StepIndex)
	{
		Params.UAWK = UAStepWK * StepIndex;
		const double HeatW = RadiatorModel::HeatRejectedW(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
		const double GainW = HeatW - PreviousHeatW;

		AddInfo(FString::Printf(TEXT("UA %8.0f W/K -> 반출 %8.1f W (직전 +%.0f W/K 대비 +%7.1f W, 최대의 %.1f%%)"),
			Params.UAWK, HeatW, UAStepWK, GainW, 100.0 * HeatW / MaxPossibleW));

		if (!(GainW < PreviousGainW))
		{
			bGainShrinks = false;
		}
		if (HeatW > MaxPossibleW * (1.0 + 1.0e-12))
		{
			bNeverExceedsMax = false;
		}

		PreviousGainW = GainW;
		PreviousHeatW = HeatW;
	}

	// 기준 운전점에서 UA를 두 배로 키우면 반출열이 두 배보다 훨씬 적게 는다.
	Params.UAWK = 1.6e3;
	const double BaseHeatW = RadiatorModel::HeatRejectedW(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
	Params.UAWK = 3.2e3;
	const double DoubledHeatW = RadiatorModel::HeatRejectedW(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
	const double Ratio = DoubledHeatW / BaseHeatW;

	AddInfo(FString::Printf(TEXT("UA 1600 -> 3200 W/K: 반출 %.1f -> %.1f W (배율 %.4f)"),
		BaseHeatW, DoubledHeatW, Ratio));

	TestTrue(TEXT("UA를 두 배로 키워도 반출열이 두 배가 되면 안 된다"), Ratio < 2.0);
	TestTrue(TEXT("같은 크기만큼 UA를 더할 때 반출열 증가분이 계속 줄어야 한다"), bGainShrinks);
	TestTrue(TEXT("반출열이 이론 최대를 넘으면 안 된다"), bNeverExceedsMax);

	// UA를 아주 크게 키우면 출구가 외기에 수렴한다.
	Params.UAWK = 1.0e7;
	const double HugeUAOutletTempK = RadiatorModel::OutletTempK(Params, InletTempK, MassFlowKgs, WaterCpJKgK);
	AddInfo(FString::Printf(TEXT("UA 1e7 W/K -> 출구 %.6f C (외기 %.6f C, 접근온도 %.3e K)"),
		HugeUAOutletTempK - 273.15, Params.AmbientTempK - 273.15,
		HugeUAOutletTempK - Params.AmbientTempK));

	TestTrue(TEXT("UA가 매우 크면 출구가 외기에 수렴해야 한다"),
		FMath::Abs(HugeUAOutletTempK - Params.AmbientTempK) < 1.0e-9);
	TestTrue(TEXT("출구 온도가 외기보다 낮아지면 안 된다"),
		HugeUAOutletTempK >= Params.AmbientTempK - 1.0e-12);

	return true;
}

// D2: 유량 0에서 특이해지지 않아야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiatorZeroFlowTest,
	"VRG.Thermal.Radiator.ZeroFlowNoSingularity", VrgRadiatorTestFlags)

bool FRadiatorZeroFlowTest::RunTest(const FString& Parameters)
{
	FRadiatorParams Params;
	Params.UAWK = 1.6e3;
	Params.AmbientTempK = 295.15;

	const double InletTempK = 308.15;
	const double OutletTempK = RadiatorModel::OutletTempK(Params, InletTempK, 0.0, WaterCpJKgK);
	const double HeatRejectedW = RadiatorModel::HeatRejectedW(Params, InletTempK, 0.0, WaterCpJKgK);

	AddInfo(FString::Printf(TEXT("유량 0 | 출구 %.6f C / 반출 %.6e W"),
		OutletTempK - 273.15, HeatRejectedW));

	TestTrue(TEXT("출구 온도가 유한해야 한다"), FMath::IsFinite(OutletTempK));
	TestTrue(TEXT("반출열이 유한해야 한다"), FMath::IsFinite(HeatRejectedW));
	TestEqual(TEXT("유량 0이면 반출열이 0이어야 한다"), HeatRejectedW, 0.0);

	// UA가 0이면 아무 일도 일어나지 않는다.
	Params.UAWK = 0.0;
	const double NoUAOutletTempK = RadiatorModel::OutletTempK(Params, InletTempK, 0.82, WaterCpJKgK);
	TestEqual(TEXT("UA 0이면 출구가 입구와 같아야 한다"), NoUAOutletTempK, InletTempK);

	return true;
}

// 라디에이터로 냉각수 루프를 닫았을 때, 랙 발열 전체가 결국 라디에이터와
// 공기로 빠져나가고 정상 상태 열수지가 맞아야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRadiatorClosedLoopSteadyStateTest,
	"VRG.Thermal.Radiator.ClosedLoopSteadyState", VrgRadiatorTestFlags)

bool FRadiatorClosedLoopSteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams RackParams;

	FRackThermalInput Input;
	Input.ComponentHeatW = FRackThermalModel::MakeUniformHeatW(RackParams.NumSleds);
	Input.SledDocked.Init(true, RackParams.NumSleds);

	FRadiatorParams Radiator;

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(RackParams, Initial);

	const double DeltaTimeS = 0.05;
	const int32 NumSteps = 200000;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		// 액터가 하는 것과 같은 방식으로 루프를 닫는다.
		Input.CoolantInletTempK = RadiatorModel::OutletTempK(
			Radiator, Model.GetState().CoolantTempK, Input.CoolantMassFlowKgs, RackParams.CoolantCpJKgK);
		Model.Step(Input, DeltaTimeS);
	}

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(DeltaTimeS * NumSteps);
	const double RadiatorHeatW = RadiatorModel::HeatRejectedW(
		Radiator, Snapshot.CoolantOutletTempK, Input.CoolantMassFlowKgs, RackParams.CoolantCpJKgK);

	// 정상 상태에서는 라디에이터가 버리는 열 = 냉각수가 랙에서 받아 나간 열.
	const double CoolantLoopMismatchW = FMath::Abs(RadiatorHeatW - Snapshot.HeatRemovedByCoolantW);
	const double NormalizedLoopMismatch = CoolantLoopMismatchW / FMath::Max(RadiatorHeatW, 1.0);

	// 랙 전체로 보면 발열 = 라디에이터 + 공기 + 자연대류.
	const double TotalRemovedW = RadiatorHeatW + Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW;
	const double NormalizedImbalance = FMath::Abs(TotalRemovedW - Snapshot.TotalHeatW) / Snapshot.TotalHeatW;

	AddInfo(FString::Printf(TEXT("t=%.0f s | 냉각수 랙 출구 %.2f C -> 라디에이터 출구 %.2f C (외기 %.2f C)"),
		Snapshot.SimTimeS, Snapshot.CoolantOutletTempK - 273.15,
		Input.CoolantInletTempK - 273.15, Radiator.AmbientTempK - 273.15));
	AddInfo(FString::Printf(TEXT("라디에이터 반출 %.1f W / 냉각수 반출 %.1f W / 정규화 차이 %.3e"),
		RadiatorHeatW, Snapshot.HeatRemovedByCoolantW, NormalizedLoopMismatch));
	AddInfo(FString::Printf(TEXT("라디에이터 %.1f W + 공기 %.1f W + 자연대류 %.1f W = %.1f W (발열 %.1f W), 정규화 불균형 %.3e"),
		RadiatorHeatW, Snapshot.TotalHeatToAirW, Snapshot.TotalHeatToAmbientW,
		TotalRemovedW, Snapshot.TotalHeatW, NormalizedImbalance));

	TestTrue(TEXT("스텝이 안정 조건을 만족해야 한다"), Snapshot.bStepStable);
	TestTrue(TEXT("정상 상태에서 라디에이터 반출 = 냉각수 반출이어야 한다"), NormalizedLoopMismatch < 1.0e-6);
	TestTrue(TEXT("정상 상태에서 발열과 반출열이 균형을 이뤄야 한다"), NormalizedImbalance < 1.0e-6);

	// 타당성 안전망: 닫힌 루프에서도 냉각수가 외기보다 뜨겁고 부품이 허용 온도 아래여야 한다.
	TestTrue(TEXT("냉각수가 외기보다 뜨거워야 한다"), Snapshot.CoolantOutletTempK > Radiator.AmbientTempK);
	TestTrue(TEXT("GPU가 허용 온도 90 C 아래여야 한다"),
		Model.GetComponentTempK(0, ERackComponent::Gpu) - 273.15 < 90.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
