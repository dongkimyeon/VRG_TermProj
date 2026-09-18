// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgThermalTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// 두 노드를 완전히 끊어 칩 노드만 남긴 설정. 해석해 비교용.
	FRackThermalParams MakeChipOnlyParams()
	{
		FRackThermalParams Params;
		Params.ChipHeatCapacityJK = 5.0e4;
		Params.CoolantHeatCapacityJK = 4.0e4;
		Params.ChipToCoolantUAWK = 0.0;
		Params.ChipToAirUAWK = 150.0;
		return Params;
	}

	void RunSteps(FRackThermalModel& Model, const FRackThermalInput& Input, double DeltaTimeS, int32 NumSteps)
	{
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			Model.Step(Input, DeltaTimeS);
		}
	}
}

// 해석해 1: 냉각 경로를 모두 끊으면 C*dT/dt = HeatW 이므로 온도는 정확히 선형으로 오른다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalLinearRampTest,
	"VRG.Thermal.Rack.LinearRampAnalytic", VrgThermalTestFlags)

bool FRackThermalLinearRampTest::RunTest(const FString& Parameters)
{
	FRackThermalParams Params = MakeChipOnlyParams();
	Params.ChipToAirUAWK = 0.0;

	FRackThermalState Initial;
	Initial.ChipTempK = 300.15;
	Initial.CoolantTempK = 300.15;

	FRackThermalInput Input;
	Input.HeatW = 1.5e4;
	Input.CoolantMassFlowKgs = 0.0;
	Input.AirMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 100000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double AnalyticChipTempK = Initial.ChipTempK + Input.HeatW * ElapsedS / Params.ChipHeatCapacityJK;
	const double AbsErrorK = FMath::Abs(Model.GetState().ChipTempK - AnalyticChipTempK);

	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.9f K / 해석해 %.9f K / 절대 오차 %.3e K"),
		ElapsedS, Model.GetState().ChipTempK, AnalyticChipTempK, AbsErrorK));

	// 기준: 측정값 2.6e-10 K (t=1000 s, 상승분 300 K)에 여유를 둔 값.
	TestTrue(TEXT("선형 램프는 해석해와 반올림 수준으로 일치해야 한다"), AbsErrorK < 1.0e-8);

	return true;
}

// 해석해 2: 공기 경로만 남기면 1차 지수 응답이다.
// T(t) = T_air + HeatW/G + (T0 - T_air - HeatW/G) * exp(-G*t/C)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalExponentialTest,
	"VRG.Thermal.Rack.ExponentialAnalytic", VrgThermalTestFlags)

bool FRackThermalExponentialTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = MakeChipOnlyParams();

	FRackThermalState Initial;
	Initial.ChipTempK = 300.15;
	Initial.CoolantTempK = 300.15;

	FRackThermalInput Input;
	Input.HeatW = 1.5e4;
	Input.AirInletTempK = 297.15;
	Input.AirMassFlowKgs = 1.2;
	Input.CoolantMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.005;
	const int32 NumSteps = 60000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double ConductanceWK = FRackThermalModel::EffectiveAirConductanceWK(Params, Input.AirMassFlowKgs);
	const double SteadyTempK = Input.AirInletTempK + Input.HeatW / ConductanceWK;
	const double TimeConstantS = Params.ChipHeatCapacityJK / ConductanceWK;
	const double AnalyticChipTempK = SteadyTempK
		+ (Initial.ChipTempK - SteadyTempK) * FMath::Exp(-ElapsedS / TimeConstantS);

	const double AbsErrorK = FMath::Abs(Model.GetState().ChipTempK - AnalyticChipTempK);
	const double RiseK = FMath::Abs(AnalyticChipTempK - Initial.ChipTempK);
	const double NormalizedError = AbsErrorK / RiseK;

	AddInfo(FString::Printf(TEXT("유효 전도도 %.4f W/K, 시정수 %.2f s"), ConductanceWK, TimeConstantS));
	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.6f K / 해석해 %.6f K"),
		ElapsedS, Model.GetState().ChipTempK, AnalyticChipTempK));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e K / 온도 상승분(%.3f K) 대비 정규화 오차 %.3e"),
		AbsErrorK, RiseK, NormalizedError));

	// 기준: 측정값 2.65e-4 K (상승분 59.0 K 대비 4.5e-6)에 여유를 둔 값.
	// 명시적 오일러의 O(dt) 절단 오차가 지배한다.
	TestTrue(TEXT("지수 응답이 해석해와 일치해야 한다"), AbsErrorK < 1.0e-3);

	return true;
}

// 보존: 유입 - 유출 - 축적 = 0. 축적량을 반드시 포함한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalEnergyConservationTest,
	"VRG.Thermal.Rack.EnergyConservation", VrgThermalTestFlags)

bool FRackThermalEnergyConservationTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;

	FRackThermalState Initial;
	Initial.ChipTempK = 300.15;
	Initial.CoolantTempK = 300.15;

	FRackThermalInput Input;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 60000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double NormalizedResidual = FMath::Abs(Model.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J"),
		Model.GetEnergyInJ(), Model.GetEnergyOutJ(), Model.GetEnergyStoredJ()));
	AddInfo(FString::Printf(TEXT("잔차 %.6e J / 유입 기준 정규화 잔차 %.3e (무차원)"),
		Model.GetEnergyResidualJ(), NormalizedResidual));

	// 기준: 측정값 1.54e-14 (유입 9.0e6 J 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("정규화 열수지 잔차가 반올림 수준이어야 한다"), NormalizedResidual < 1.0e-12);

	return true;
}

// D2: 유량 0에서 특이해지지 않아야 한다.
// 나눗셈이 없으므로 냉각량이 0이 되고 열이 그대로 축적된다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalZeroFlowTest,
	"VRG.Thermal.Rack.ZeroFlowNoSingularity", VrgThermalTestFlags)

bool FRackThermalZeroFlowTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;

	FRackThermalState Initial;
	Initial.ChipTempK = 300.15;
	Initial.CoolantTempK = 300.15;

	FRackThermalInput Input;
	Input.HeatW = 1.5e4;
	Input.CoolantMassFlowKgs = 0.0;
	Input.AirMassFlowKgs = 0.0;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 30000;

	double PreviousChipTempK = Initial.ChipTempK;
	bool bMonotonic = true;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Model.Step(Input, DeltaTimeS);
		const double ChipTempK = Model.GetState().ChipTempK;
		if (ChipTempK < PreviousChipTempK)
		{
			bMonotonic = false;
		}
		PreviousChipTempK = ChipTempK;
	}

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double ExpectedStoredJ = Input.HeatW * ElapsedS;
	const double StoredJ = Model.GetEnergyStoredJ();
	const double NormalizedStoredError = FMath::Abs(StoredJ - ExpectedStoredJ) / ExpectedStoredJ;

	AddInfo(FString::Printf(TEXT("t=%.1f s | 칩 %.3f K / 냉각수 %.3f K"),
		ElapsedS, Model.GetState().ChipTempK, Model.GetState().CoolantTempK));
	AddInfo(FString::Printf(TEXT("축적 %.6e J / 기대 %.6e J / 정규화 오차 %.3e"),
		StoredJ, ExpectedStoredJ, NormalizedStoredError));

	TestTrue(TEXT("칩 온도가 유한해야 한다"), FMath::IsFinite(Model.GetState().ChipTempK));
	TestTrue(TEXT("냉각수 온도가 유한해야 한다"), FMath::IsFinite(Model.GetState().CoolantTempK));
	TestTrue(TEXT("유량 0이면 온도가 단조 상승해야 한다"), bMonotonic);
	// 기준: 측정값 1.28e-14 (기대 축적 4.5e6 J 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("유량 0이면 발열이 전부 축적돼야 한다"), NormalizedStoredError < 1.0e-12);

	return true;
}

// 정상 상태에서 발열과 반출열이 균형을 이뤄야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalSteadyStateTest,
	"VRG.Thermal.Rack.SteadyStateBalance", VrgThermalTestFlags)

bool FRackThermalSteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;

	FRackThermalState Initial;
	Initial.ChipTempK = 300.15;
	Initial.CoolantTempK = 300.15;

	FRackThermalInput Input;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.05;
	const int32 NumSteps = 100000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(DeltaTimeS * NumSteps);
	const double RemovedW = Snapshot.HeatChipToAirW + Snapshot.HeatRemovedByCoolantW;
	const double NormalizedImbalance = FMath::Abs(RemovedW - Input.HeatW) / Input.HeatW;

	AddInfo(FString::Printf(TEXT("t=%.0f s | 칩 %.3f K (%.2f C) / 냉각수 출구 %.3f K (%.2f C)"),
		Snapshot.SimTimeS, Snapshot.ChipTempK, Snapshot.ChipTempK - 273.15,
		Snapshot.CoolantOutletTempK, Snapshot.CoolantOutletTempK - 273.15));
	AddInfo(FString::Printf(TEXT("공기 반출 %.2f W / 냉각수 반출 %.2f W / 합 %.2f W (발열 %.2f W)"),
		Snapshot.HeatChipToAirW, Snapshot.HeatRemovedByCoolantW, RemovedW, Input.HeatW));
	AddInfo(FString::Printf(TEXT("발열 기준 정규화 불균형 %.3e (무차원)"), NormalizedImbalance));

	TestTrue(TEXT("스텝이 안정 조건을 만족해야 한다"), Snapshot.bStepStable);
	// 기준: 측정값 3.4e-12 (발열 15000 W 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("정상 상태에서 발열과 반출열이 균형을 이뤄야 한다"), NormalizedImbalance < 1.0e-9);

	return true;
}

// D5 취지: 안정 조건 위반을 결과에 표시해야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalStabilityFlagTest,
	"VRG.Thermal.Rack.StabilityFlagReported", VrgThermalTestFlags)

bool FRackThermalStabilityFlagTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	FRackThermalState Initial;
	FRackThermalInput Input;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	TestTrue(TEXT("작은 스텝은 안정해야 한다"), Model.IsStepStable(Input, 0.01));

	// 가장 빠른 시정수보다 훨씬 큰 스텝은 단조 조건을 위반한다.
	const double HugeStepS = 1000.0;
	TestFalse(TEXT("과도하게 큰 스텝은 불안정으로 보고돼야 한다"), Model.IsStepStable(Input, HugeStepS));

	Model.Step(Input, HugeStepS);
	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(HugeStepS);

	AddInfo(FString::Printf(TEXT("dt=%.0f s 스텝 후 bStepStable=%s"),
		HugeStepS, Snapshot.bStepStable ? TEXT("true") : TEXT("false")));

	TestFalse(TEXT("스냅샷에 불안정이 표시돼야 한다"), Snapshot.bStepStable);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
