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

	// 부품 노드 하나만 남긴 설정. 해석해 비교용이라 유닛도 1대다.
	// 나머지 부품은 전도도·발열이 모두 0이라 초기 온도에 그대로 머문다.
	FRackThermalParams MakeSingleNodeParams()
	{
		FRackThermalParams Params;
		Params.NumSleds = 1;
		Params.Components.SetNum(NumRackComponents);

		for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
		{
			FRackComponentParams& Component = Params.Components[ComponentIndex];
			Component.Name = FName(*FString::Printf(TEXT("C%d"), ComponentIndex));
			Component.HeatCapacityJK = 1.0;
			Component.ToCoolantUAWK = 0.0;
			Component.ToAirUAWK = 0.0;
			Component.ToAmbientUAWK = 0.0;
		}

		FRackComponentParams& Chip = Params.Components[static_cast<int32>(ERackComponent::Cpu)];
		Chip.HeatCapacityJK = 5.0e4;
		Chip.ToAirUAWK = 150.0;

		Params.CoolantHeatCapacityJK = 4.0e4;
		return Params;
	}

	// 단일 노드 설정에 맞춘 발열 배열. CPU 자리에만 넣는다.
	TArray<double> MakeSingleNodeHeatW(double HeatW)
	{
		TArray<double> ComponentHeatW;
		ComponentHeatW.Init(0.0, NumRackComponents);
		ComponentHeatW[static_cast<int32>(ERackComponent::Cpu)] = HeatW;
		return ComponentHeatW;
	}

	// 기본 랙 구성이되 케이스 자연대류를 끊은 것. 단열 경계 검증용.
	FRackThermalParams MakeNoAmbientParams()
	{
		FRackThermalParams Params;
		Params.Components = FRackThermalModel::DefaultComponents();
		for (FRackComponentParams& Component : Params.Components)
		{
			Component.ToAmbientUAWK = 0.0;
		}
		return Params;
	}

	FRackThermalInput MakeDefaultInput(const FRackThermalParams& Params)
	{
		FRackThermalInput Input;
		Input.ComponentHeatW = FRackThermalModel::MakeUniformHeatW(Params.NumSleds);
		Input.SledDocked.Init(true, Params.NumSleds);
		return Input;
	}

	void RunSteps(FRackThermalModel& Model, const FRackThermalInput& Input, double DeltaTimeS, int32 NumSteps)
	{
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			Model.Step(Input, DeltaTimeS);
		}
	}

	double TotalHeatW(const FRackThermalInput& Input)
	{
		double SumW = 0.0;
		for (const double HeatW : Input.ComponentHeatW)
		{
			SumW += HeatW;
		}
		return SumW;
	}
}

// 해석해 1: 냉각 경로를 모두 끊으면 C*dT/dt = HeatW 이므로 온도는 정확히 선형으로 오른다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalLinearRampTest,
	"VRG.Thermal.Rack.LinearRampAnalytic", VrgThermalTestFlags)

bool FRackThermalLinearRampTest::RunTest(const FString& Parameters)
{
	FRackThermalParams Params = MakeSingleNodeParams();
	Params.Components[static_cast<int32>(ERackComponent::Cpu)].ToAirUAWK = 0.0;

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;
	Initial.ComponentTempK.Init(300.15, NumRackComponents);

	FRackThermalInput Input;
	Input.ComponentHeatW = MakeSingleNodeHeatW(1.5e4);
	Input.SledDocked.Init(true, 1);
	Input.CoolantMassFlowKgs = 0.0;
	Input.TotalAirMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 100000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double ChipTempK = Model.GetComponentTempK(0, ERackComponent::Cpu);
	const double ElapsedS = DeltaTimeS * NumSteps;
	const double HeatCapacityJK = Params.Components[static_cast<int32>(ERackComponent::Cpu)].HeatCapacityJK;
	const double AnalyticChipTempK = 300.15 + 1.5e4 * ElapsedS / HeatCapacityJK;
	const double AbsErrorK = FMath::Abs(ChipTempK - AnalyticChipTempK);

	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.9f K / 해석해 %.9f K / 절대 오차 %.3e K"),
		ElapsedS, ChipTempK, AnalyticChipTempK, AbsErrorK));

	// 기준: 측정값(상승분 300 K)에 여유를 둔 값.
	TestTrue(TEXT("선형 램프는 해석해와 반올림 수준으로 일치해야 한다"), AbsErrorK < 1.0e-8);

	return true;
}

// 해석해 2: 공기 경로만 남기면 1차 지수 응답이다.
// T(t) = T_air + HeatW/G + (T0 - T_air - HeatW/G) * exp(-G*t/C)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalExponentialTest,
	"VRG.Thermal.Rack.ExponentialAnalytic", VrgThermalTestFlags)

bool FRackThermalExponentialTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = MakeSingleNodeParams();

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;
	Initial.ComponentTempK.Init(300.15, NumRackComponents);

	FRackThermalInput Input;
	Input.ComponentHeatW = MakeSingleNodeHeatW(1.5e4);
	Input.SledDocked.Init(true, 1);
	Input.AirInletTempK = 297.15;
	Input.TotalAirMassFlowKgs = 1.2;
	Input.CoolantMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.005;
	const int32 NumSteps = 60000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const FRackComponentParams& Chip = Params.Components[static_cast<int32>(ERackComponent::Cpu)];
	const double AirCapacityRateWK = Input.TotalAirMassFlowKgs * Params.AirCpJKgK;
	const double ConductanceWK = AirCapacityRateWK
		* FRackThermalModel::AirEffectiveness(Chip.ToAirUAWK, AirCapacityRateWK);

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double SteadyTempK = Input.AirInletTempK + 1.5e4 / ConductanceWK;
	const double TimeConstantS = Chip.HeatCapacityJK / ConductanceWK;
	const double AnalyticChipTempK = SteadyTempK + (300.15 - SteadyTempK) * FMath::Exp(-ElapsedS / TimeConstantS);

	const double ChipTempK = Model.GetComponentTempK(0, ERackComponent::Cpu);
	const double AbsErrorK = FMath::Abs(ChipTempK - AnalyticChipTempK);
	const double RiseK = FMath::Abs(AnalyticChipTempK - 300.15);
	const double NormalizedError = AbsErrorK / RiseK;

	AddInfo(FString::Printf(TEXT("유효 전도도 %.4f W/K, 시정수 %.2f s"), ConductanceWK, TimeConstantS));
	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.6f K / 해석해 %.6f K"), ElapsedS, ChipTempK, AnalyticChipTempK));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e K / 온도 상승분(%.3f K) 대비 정규화 오차 %.3e"),
		AbsErrorK, RiseK, NormalizedError));

	// 기준: 측정값에 여유를 둔 값. 명시적 오일러의 O(dt) 절단 오차가 지배한다.
	TestTrue(TEXT("지수 응답이 해석해와 일치해야 한다"), AbsErrorK < 1.0e-3);

	return true;
}

// 보존: 유입 - 유출 - 축적 = 0. 축적량을 반드시 포함한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalEnergyConservationTest,
	"VRG.Thermal.Rack.EnergyConservation", VrgThermalTestFlags)

bool FRackThermalEnergyConservationTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	const FRackThermalInput Input = MakeDefaultInput(Params);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 60000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double NormalizedResidual = FMath::Abs(Model.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("유닛 %d대 / 노드 %d개 / 총 발열 %.0f W"),
		Params.NumSleds, Model.GetNumNodes(), TotalHeatW(Input)));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J"),
		Model.GetEnergyInJ(), Model.GetEnergyOutJ(), Model.GetEnergyStoredJ()));
	AddInfo(FString::Printf(TEXT("잔차 %.6e J / 유입 기준 정규화 잔차 %.3e (무차원)"),
		Model.GetEnergyResidualJ(), NormalizedResidual));

	// 기준: 측정값 1.67e-12 (유입 9.0e6 J 기준 정규화, 무차원)에 여유를 둔 값.
	// 노드가 49개로 늘면서 S1(2노드, 1.54e-14)보다 커졌다. 절대온도 K에 미세 증분을
	// 더할 때 생기는 절단 오차가 지배하며, 여전히 반올림 수준이다.
	TestTrue(TEXT("정규화 열수지 잔차가 반올림 수준이어야 한다"), NormalizedResidual < 1.0e-10);

	return true;
}

// D2: 유량 0에서 특이해지지 않아야 한다.
// 나눗셈이 없으므로 냉각량이 0이 되고 열이 그대로 축적된다.
// 케이스 자연대류까지 끊어 완전 단열로 둔다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalZeroFlowTest,
	"VRG.Thermal.Rack.ZeroFlowNoSingularity", VrgThermalTestFlags)

bool FRackThermalZeroFlowTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = MakeNoAmbientParams();

	FRackThermalInput Input = MakeDefaultInput(Params);
	Input.CoolantMassFlowKgs = 0.0;
	Input.TotalAirMassFlowKgs = 0.0;

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 30000;

	double PreviousHottestK = 0.0;
	bool bMonotonic = true;
	bool bFinite = true;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Model.Step(Input, DeltaTimeS);

		double HottestK = 0.0;
		for (const double TempK : Model.GetState().ComponentTempK)
		{
			HottestK = FMath::Max(HottestK, TempK);
			bFinite = bFinite && FMath::IsFinite(TempK);
		}
		if (HottestK < PreviousHottestK)
		{
			bMonotonic = false;
		}
		PreviousHottestK = HottestK;
	}

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double ExpectedStoredJ = TotalHeatW(Input) * ElapsedS;
	const double StoredJ = Model.GetEnergyStoredJ();
	const double NormalizedStoredError = FMath::Abs(StoredJ - ExpectedStoredJ) / ExpectedStoredJ;

	AddInfo(FString::Printf(TEXT("t=%.1f s | 최고 부품 %.3f K / 냉각수 %.3f K"),
		ElapsedS, PreviousHottestK, Model.GetState().CoolantTempK));
	AddInfo(FString::Printf(TEXT("축적 %.6e J / 기대 %.6e J / 정규화 오차 %.3e"),
		StoredJ, ExpectedStoredJ, NormalizedStoredError));

	TestTrue(TEXT("모든 부품 온도가 유한해야 한다"), bFinite);
	TestTrue(TEXT("냉각수 온도가 유한해야 한다"), FMath::IsFinite(Model.GetState().CoolantTempK));
	TestTrue(TEXT("유량 0이면 온도가 단조 상승해야 한다"), bMonotonic);
	// 기준: 측정값(기대 축적 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("유량 0이면 발열이 전부 축적돼야 한다"), NormalizedStoredError < 1.0e-12);

	return true;
}

// 정상 상태에서 발열과 반출열이 균형을 이뤄야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalSteadyStateTest,
	"VRG.Thermal.Rack.SteadyStateBalance", VrgThermalTestFlags)

bool FRackThermalSteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	const FRackThermalInput Input = MakeDefaultInput(Params);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.05;
	const int32 NumSteps = 100000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(DeltaTimeS * NumSteps);
	const double RemovedW = Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW + Snapshot.HeatRemovedByCoolantW;
	const double NormalizedImbalance = FMath::Abs(RemovedW - Snapshot.TotalHeatW) / Snapshot.TotalHeatW;

	AddInfo(FString::Printf(TEXT("t=%.0f s | CPU %.2f C / GPU %.2f C / RAM %.2f C / VRM %.2f C (유닛 0)"),
		Snapshot.SimTimeS,
		Model.GetComponentTempK(0, ERackComponent::Cpu) - 273.15,
		Model.GetComponentTempK(0, ERackComponent::Gpu) - 273.15,
		Model.GetComponentTempK(0, ERackComponent::Ram) - 273.15,
		Model.GetComponentTempK(0, ERackComponent::Vrm) - 273.15));
	AddInfo(FString::Printf(TEXT("냉각수 출구 %.2f C / 캐비닛 배기 %.2f C"),
		Snapshot.CoolantOutletTempK - 273.15, Snapshot.MixedAirOutletTempK - 273.15));
	AddInfo(FString::Printf(TEXT("냉각수 반출 %.2f W / 공기 반출 %.2f W / 자연대류 %.2f W / 합 %.2f W (발열 %.2f W)"),
		Snapshot.HeatRemovedByCoolantW, Snapshot.TotalHeatToAirW, Snapshot.TotalHeatToAmbientW,
		RemovedW, Snapshot.TotalHeatW));
	AddInfo(FString::Printf(TEXT("발열 기준 정규화 불균형 %.3e (무차원)"), NormalizedImbalance));

	TestTrue(TEXT("스텝이 안정 조건을 만족해야 한다"), Snapshot.bStepStable);
	// 기준: 측정값(발열 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("정상 상태에서 발열과 반출열이 균형을 이뤄야 한다"), NormalizedImbalance < 1.0e-9);

	return true;
}

// D5 취지: 안정 조건 위반을 결과에 표시해야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalStabilityFlagTest,
	"VRG.Thermal.Rack.StabilityFlagReported", VrgThermalTestFlags)

bool FRackThermalStabilityFlagTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	const FRackThermalInput Input = MakeDefaultInput(Params);
	FRackThermalState Initial;

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

// --- 여기부터 다노드 확장 검증 ---

// 공랭 직렬: 같은 부품을 네 개 직렬로 두면 뒤쪽일수록 뜨거워야 한다.
// 전부 AirInletTempK를 보게 짜면 네 온도가 같아지고, 그것이 냉각 과대평가다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalSerialAirPathTest,
	"VRG.Thermal.Rack.SerialAirPathOrdering", VrgThermalTestFlags)

bool FRackThermalSerialAirPathTest::RunTest(const FString& Parameters)
{
	// 부품 네 개를 완전히 동일하게 둔다. 차이는 오직 체인 상의 위치뿐이다.
	FRackThermalParams Params;
	Params.NumSleds = 1;
	Params.Components.SetNum(NumRackComponents);
	for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
	{
		FRackComponentParams& Component = Params.Components[ComponentIndex];
		Component.Name = FName(*FString::Printf(TEXT("C%d"), ComponentIndex));
		Component.HeatCapacityJK = 500.0;
		Component.ToCoolantUAWK = 0.0;
		Component.ToAirUAWK = 5.0;
		Component.ToAmbientUAWK = 0.0;
	}

	const double PerComponentHeatW = 100.0;
	FRackThermalInput Input;
	Input.ComponentHeatW.Init(PerComponentHeatW, NumRackComponents);
	Input.SledDocked.Init(true, 1);
	Input.AirInletTempK = 297.15;
	Input.TotalAirMassFlowKgs = 0.1;
	Input.CoolantMassFlowKgs = 0.0;

	FRackThermalState Initial;
	Initial.CoolantTempK = 297.15;
	Initial.ComponentTempK.Init(297.15, NumRackComponents);

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.1, 200000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(2.0e4);

	bool bStrictlyIncreasing = true;
	for (int32 Index = 1; Index < NumRackComponents; ++Index)
	{
		AddInfo(FString::Printf(TEXT("체인 %d번 부품: %.3f C (반출 %.3f W)"),
			Index - 1,
			Snapshot.ComponentTempK[Index - 1] - 273.15,
			Snapshot.ComponentHeatToAirW[Index - 1]));

		if (!(Snapshot.ComponentTempK[Index] > Snapshot.ComponentTempK[Index - 1]))
		{
			bStrictlyIncreasing = false;
		}
	}
	AddInfo(FString::Printf(TEXT("체인 %d번 부품: %.3f C (반출 %.3f W)"),
		NumRackComponents - 1,
		Snapshot.ComponentTempK[NumRackComponents - 1] - 273.15,
		Snapshot.ComponentHeatToAirW[NumRackComponents - 1]));

	const double FirstToLastRiseK = Snapshot.ComponentTempK[NumRackComponents - 1] - Snapshot.ComponentTempK[0];
	AddInfo(FString::Printf(TEXT("입구 %.2f C -> 배기 %.2f C | 체인 처음과 끝의 온도차 %.3f K"),
		Input.AirInletTempK - 273.15, Snapshot.MixedAirOutletTempK - 273.15, FirstToLastRiseK));

	TestTrue(TEXT("뒤쪽 부품일수록 뜨거워야 한다 (앞 단이 데운 공기를 받는다)"), bStrictlyIncreasing);
	TestTrue(TEXT("직렬 효과가 유의미한 크기여야 한다"), FirstToLastRiseK > 1.0);

	// 정상 상태에서는 각 부품이 자기 발열만큼을 공기로 내보낸다 (다른 경로가 없다).
	double MaxDeviationW = 0.0;
	for (int32 Index = 0; Index < NumRackComponents; ++Index)
	{
		MaxDeviationW = FMath::Max(MaxDeviationW,
			FMath::Abs(Snapshot.ComponentHeatToAirW[Index] - PerComponentHeatW));
	}
	AddInfo(FString::Printf(TEXT("부품별 반출열의 발열 대비 최대 편차 %.3e W (발열 %.0f W)"),
		MaxDeviationW, PerComponentHeatW));
	TestTrue(TEXT("정상 상태에서 부품별 반출열이 발열과 같아야 한다"),
		MaxDeviationW / PerComponentHeatW < 1.0e-6);

	return true;
}

// 공기 열수지: 구간별 반출열의 합과 공기 엔탈피 상승이 정확히 같아야 한다.
// effectiveness를 열유속과 출구 온도에 같은 값으로 쓰기 때문에 성립한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalAirPathEnergyBalanceTest,
	"VRG.Thermal.Rack.AirPathEnergyBalance", VrgThermalTestFlags)

bool FRackThermalAirPathEnergyBalanceTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	FRackThermalInput Input = MakeDefaultInput(Params);

	// 도킹 상태를 섞어 빈 슬롯이 섞인 경우에도 장부가 맞는지 본다.
	Input.SledDocked[2] = false;
	Input.SledDocked[7] = false;

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.02, 100000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(2.0e3);

	// 슬롯별로 (질량유량 * Cp * 온도 상승)을 더한 값.
	double EnthalpyRiseW = 0.0;
	for (int32 SledIndex = 0; SledIndex < Params.NumSleds; ++SledIndex)
	{
		EnthalpyRiseW += Snapshot.SlotAirMassFlowKgs[SledIndex] * Params.AirCpJKgK
			* (Snapshot.SledAirOutletTempK[SledIndex] - Input.AirInletTempK);
	}

	const double AbsErrorW = FMath::Abs(EnthalpyRiseW - Snapshot.TotalHeatToAirW);
	const double NormalizedError = AbsErrorW / FMath::Max(FMath::Abs(Snapshot.TotalHeatToAirW), 1.0);

	AddInfo(FString::Printf(TEXT("구간 반출열 합 %.6f W / 공기 엔탈피 상승 %.6f W"),
		Snapshot.TotalHeatToAirW, EnthalpyRiseW));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e W / 반출열 기준 정규화 오차 %.3e (무차원)"),
		AbsErrorW, NormalizedError));

	TestTrue(TEXT("구간 반출열의 합과 공기 엔탈피 상승이 일치해야 한다"), NormalizedError < 1.0e-12);

	return true;
}

// 냉각 경로 분담: 콜드플레이트는 CPU·GPU만, 공기는 RAM·VRM만 맡는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalLiquidAirSplitTest,
	"VRG.Thermal.Rack.LiquidAirSplit", VrgThermalTestFlags)

bool FRackThermalLiquidAirSplitTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	const FRackThermalInput Input = MakeDefaultInput(Params);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.05, 100000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(5.0e3);

	const int32 CpuIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Cpu);
	const int32 GpuIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Gpu);
	const int32 RamIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Ram);
	const int32 VrmIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Vrm);

	AddInfo(FString::Printf(TEXT("유닛 0 CPU: %.2f C, 냉각수로 %.1f W, 공기로 %.3f W"),
		Snapshot.ComponentTempK[CpuIndex] - 273.15,
		Snapshot.ComponentHeatToCoolantW[CpuIndex], Snapshot.ComponentHeatToAirW[CpuIndex]));
	AddInfo(FString::Printf(TEXT("유닛 0 GPU: %.2f C, 냉각수로 %.1f W, 공기로 %.3f W"),
		Snapshot.ComponentTempK[GpuIndex] - 273.15,
		Snapshot.ComponentHeatToCoolantW[GpuIndex], Snapshot.ComponentHeatToAirW[GpuIndex]));
	AddInfo(FString::Printf(TEXT("유닛 0 RAM: %.2f C, 냉각수로 %.3f W, 공기로 %.1f W"),
		Snapshot.ComponentTempK[RamIndex] - 273.15,
		Snapshot.ComponentHeatToCoolantW[RamIndex], Snapshot.ComponentHeatToAirW[RamIndex]));
	AddInfo(FString::Printf(TEXT("유닛 0 VRM: %.2f C, 냉각수로 %.3f W, 공기로 %.1f W"),
		Snapshot.ComponentTempK[VrmIndex] - 273.15,
		Snapshot.ComponentHeatToCoolantW[VrmIndex], Snapshot.ComponentHeatToAirW[VrmIndex]));
	AddInfo(FString::Printf(TEXT("랙 합계: 냉각수 %.0f W / 공기 %.0f W / 자연대류 %.0f W (발열 %.0f W)"),
		Snapshot.TotalHeatToCoolantW, Snapshot.TotalHeatToAirW,
		Snapshot.TotalHeatToAmbientW, Snapshot.TotalHeatW));

	TestEqual(TEXT("CPU는 공기로 열을 내보내지 않아야 한다"), Snapshot.ComponentHeatToAirW[CpuIndex], 0.0);
	TestEqual(TEXT("GPU는 공기로 열을 내보내지 않아야 한다"), Snapshot.ComponentHeatToAirW[GpuIndex], 0.0);
	TestEqual(TEXT("RAM은 냉각수로 열을 내보내지 않아야 한다"), Snapshot.ComponentHeatToCoolantW[RamIndex], 0.0);
	TestEqual(TEXT("VRM은 냉각수로 열을 내보내지 않아야 한다"), Snapshot.ComponentHeatToCoolantW[VrmIndex], 0.0);

	TestTrue(TEXT("콜드플레이트가 랙 발열의 대부분을 맡아야 한다"),
		Snapshot.TotalHeatToCoolantW > 0.7 * Snapshot.TotalHeatW);
	TestTrue(TEXT("RAM은 허용 온도 85 C 아래여야 한다"),
		Snapshot.ComponentTempK[RamIndex] - 273.15 < 85.0);
	TestTrue(TEXT("VRM은 허용 온도 125 C 아래여야 한다"),
		Snapshot.ComponentTempK[VrmIndex] - 273.15 < 125.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
