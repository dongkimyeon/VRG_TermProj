// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// S1 랙 열모델 검증 (2026-09-27 재정립: 15 kW 하이브리드 균일 랙을 없애고 GB200 NVL72 기준으로 바꿨다).
//
// 해석해 3건(선형 램프, 지수 응답, 공랭 직렬)은 식을 닫힌 형태로 풀어야 하므로 제품이 아닌
// "합성 장비" 한 대(부품 몇 개짜리 슬롯 하나)로 돈다. 나머지 보존·정상 상태·경로 분담은 NVL72 프리셋으로 돈다.
namespace
{
	constexpr EAutomationTestFlags VrgThermalTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// 해석해용 합성 장비 (제품 아님). 슬롯 하나에 Parts를 공기 경로 순서대로 둔다.
	// bFans면 팬 한 대(전기 입력 0 W라 팬 열이 없다)를 달아 FanSpeedOverride로 풍량을 고정한다.
	FRackThermalParams MakeTestDevice(const TArray<FRackComponentParams>& Parts, bool bFans, double ResistancePaS2Kg2)
	{
		FRackDeviceType Device;
		Device.Name = TEXT("Analytic test device");
		Device.Components = Parts;
		Device.DockedResistancePaS2Kg2 = ResistancePaS2Kg2;
		if (bFans)
		{
			Device.Fans.NumFans = 1;
			Device.Fans.MaxFlowM3s = 1.0;
			Device.Fans.ShutoffPressurePa = 1000.0;
			Device.Fans.RatedInputW = 0.0;
		}
		FRackThermalParams Params;
		Params.DeviceTypes = {Device};
		Params.Slots.Add({0, 0.0, 0.04445});
		Params.NumSleds = 1;
		Params.CoolantHeatCapacityJK = 4.0e4;
		return Params;
	}

	FRackComponentParams MakePart(double HeatCapacityJK, double ToAirUAWK)
	{
		FRackComponentParams Part;
		Part.Name = TEXT("Part");
		Part.HeatCapacityJK = HeatCapacityJK;
		Part.ToCoolantUAWK = 0.0;
		Part.ToAirUAWK = ToAirUAWK;
		Part.ToAmbientUAWK = 0.0;
		return Part;
	}

	// Slot mass flow the fan override gives [kg/s].
	double TestFanFlowKgs(const FRackThermalParams& Params, double SpeedRatio)
	{
		return FRackThermalModel::FanOperatingMassFlowKgs(Params.DeviceTypes[0].Fans,
			Params.DeviceTypes[0].DockedResistancePaS2Kg2, Params.AirDensityKgM3, SpeedRatio);
	}

	// GB200 NVL72 at its design point (same as ADataCenterRoomActor / Gb200Nvl72Test): full load, 2.3 kg/s coolant.
	FRackThermalInput MakeNvl72Input(const FRackThermalParams& Params)
	{
		FRackThermalInput Input;
		Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
		Input.SledDocked.Init(true, Params.Slots.Num());
		Input.CoolantMassFlowKgs = 2.3;
		Input.CoolantInletTempK = 300.15;
		Input.AirInletTempK = 297.15;
		return Input;
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
	const FRackThermalParams Params = MakeTestDevice({MakePart(5.0e4, 0.0)}, false, 0.0);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;
	Initial.ComponentTempK.Init(300.15, 1);

	FRackThermalInput Input;
	Input.ComponentHeatW = {1.5e4};
	Input.SledDocked.Init(true, 1);
	Input.CoolantMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.01;
	const int32 NumSteps = 100000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double ChipTempK = Model.GetState().ComponentTempK[0];
	const double ElapsedS = DeltaTimeS * NumSteps;
	const double AnalyticChipTempK = 300.15 + 1.5e4 * ElapsedS / 5.0e4;
	const double AbsErrorK = FMath::Abs(ChipTempK - AnalyticChipTempK);

	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.9f K / 해석해 %.9f K / 절대 오차 %.3e K"),
		ElapsedS, ChipTempK, AnalyticChipTempK, AbsErrorK));

	// 기준: 측정값(상승분 300 K)에 여유를 둔 값.
	TestTrue(TEXT("선형 램프는 해석해와 반올림 수준으로 일치해야 한다"), AbsErrorK < 1.0e-8);

	return true;
}

// 해석해 2: 공기 경로만 남기면 1차 지수 응답이다. 풍량은 팬 회전수를 고정해 정한다.
// T(t) = T_air + HeatW/G + (T0 - T_air - HeatW/G) * exp(-G*t/C),  G = mdot*cp*eps
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalExponentialTest,
	"VRG.Thermal.Rack.ExponentialAnalytic", VrgThermalTestFlags)

bool FRackThermalExponentialTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = MakeTestDevice({MakePart(5.0e4, 150.0)}, true, 100.0);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;
	Initial.ComponentTempK.Init(300.15, 1);

	FRackThermalInput Input;
	Input.ComponentHeatW = {1.5e4};
	Input.SledDocked.Init(true, 1);
	Input.AirInletTempK = 297.15;
	Input.FanSpeedOverride = 1.0;
	Input.CoolantMassFlowKgs = 0.0;
	Input.CoolantInletTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.005;
	const int32 NumSteps = 60000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double MassFlowKgs = TestFanFlowKgs(Params, 1.0);
	const double AirCapacityRateWK = MassFlowKgs * Params.AirCpJKgK;
	const double ConductanceWK = AirCapacityRateWK * FRackThermalModel::AirEffectiveness(150.0, AirCapacityRateWK);

	const double ElapsedS = DeltaTimeS * NumSteps;
	const double SteadyTempK = Input.AirInletTempK + 1.5e4 / ConductanceWK;
	const double TimeConstantS = 5.0e4 / ConductanceWK;
	const double AnalyticChipTempK = SteadyTempK + (300.15 - SteadyTempK) * FMath::Exp(-ElapsedS / TimeConstantS);

	const double ChipTempK = Model.GetState().ComponentTempK[0];
	const double AbsErrorK = FMath::Abs(ChipTempK - AnalyticChipTempK);
	const double RiseK = FMath::Abs(AnalyticChipTempK - 300.15);

	AddInfo(FString::Printf(TEXT("팬 운전점 %.4f kg/s, 유효 전도도 %.4f W/K, 시정수 %.2f s"), MassFlowKgs, ConductanceWK, TimeConstantS));
	AddInfo(FString::Printf(TEXT("t=%.1f s | 수치 %.6f K / 해석해 %.6f K"), ElapsedS, ChipTempK, AnalyticChipTempK));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e K / 온도 상승분(%.3f K) 대비 정규화 오차 %.3e"),
		AbsErrorK, RiseK, AbsErrorK / RiseK));

	TestTrue(TEXT("팬 운전점이 0보다 커야 한다"), MassFlowKgs > 0.0);
	// 기준: 측정값에 여유를 둔 값. 명시적 오일러의 O(dt) 절단 오차가 지배한다.
	TestTrue(TEXT("지수 응답이 해석해와 일치해야 한다"), AbsErrorK < 1.0e-3);

	return true;
}

// 보존: 유입 - 유출 - 축적 = 0. 축적량을 반드시 포함한다. GB200 NVL72, 팬 자동 제어.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalEnergyConservationTest,
	"VRG.Thermal.Rack.EnergyConservation", VrgThermalTestFlags)

bool FRackThermalEnergyConservationTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const FRackThermalInput Input = MakeNvl72Input(Params);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.02;
	const int32 NumSteps = 30000;
	RunSteps(Model, Input, DeltaTimeS, NumSteps);

	const double NormalizedResidual = FMath::Abs(Model.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("장비 %d대 / 노드 %d개 / 마지막 스텝 발열 %.1f W"),
		Params.Slots.Num(), Model.GetNumNodes(), Model.GetLastTotalHeatW()));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J"),
		Model.GetEnergyInJ(), Model.GetEnergyOutJ(), Model.GetEnergyStoredJ()));
	AddInfo(FString::Printf(TEXT("잔차 %.6e J / 유입 기준 정규화 잔차 %.3e (무차원)"),
		Model.GetEnergyResidualJ(), NormalizedResidual));

	// 기준: 측정값(유입 기준 정규화, 무차원)에 여유를 둔 값. 반올림 수준이다.
	TestTrue(TEXT("정규화 열수지 잔차가 반올림 수준이어야 한다"), NormalizedResidual < 1.0e-10);

	return true;
}

// D2: 유량 0에서 특이해지지 않아야 한다. 나눗셈이 없으므로 냉각량이 0이 되고 열이 그대로 축적된다.
// 냉각수 유량 0, 팬 정지(풍량·팬 열 0), 케이스 자연대류도 끊어 완전 단열로 둔다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalZeroFlowTest,
	"VRG.Thermal.Rack.ZeroFlowNoSingularity", VrgThermalTestFlags)

bool FRackThermalZeroFlowTest::RunTest(const FString& Parameters)
{
	FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	for (FRackDeviceType& Device : Params.DeviceTypes)
		for (FRackComponentParams& Part : Device.Components) Part.ToAmbientUAWK = 0.0;

	FRackThermalInput Input = MakeNvl72Input(Params);
	Input.CoolantMassFlowKgs = 0.0;
	Input.FanSpeedOverride = 0.0;

	// 기대 발열 (전부하, 모델과 독립으로 계산): DC 부품 입력 합 + 전원 셸프 변환 손실 = 손실 비율 합 x DC 부하.
	// 팬이 멈춰 팬 부품은 0 W다.
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	FRackThermalModel::ResolveLayout(Params, Nodes, Starts);
	double DcW = 0.0, LossFraction = 0.0;
	for (int32 N = 0; N < Nodes.Num(); ++N)
	{
		if (Nodes[N].Kind == ERackPartKind::Fan) continue;
		if (Nodes[N].LossFractionOfRackLoad > 0.0) LossFraction += Nodes[N].LossFractionOfRackLoad;
		else DcW += Input.ComponentHeatW[N];
	}
	const double ExpectedHeatW = DcW * (1.0 + LossFraction);

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
	const double ExpectedStoredJ = ExpectedHeatW * ElapsedS;
	const double StoredJ = Model.GetEnergyStoredJ();
	const double NormalizedStoredError = FMath::Abs(StoredJ - ExpectedStoredJ) / ExpectedStoredJ;

	AddInfo(FString::Printf(TEXT("t=%.1f s | 발열 %.1f W (DC %.1f W + 손실 %.1f W) | 최고 부품 %.3f K / 냉각수 %.3f K"),
		ElapsedS, ExpectedHeatW, DcW, DcW * LossFraction, PreviousHottestK, Model.GetState().CoolantTempK));
	AddInfo(FString::Printf(TEXT("축적 %.6e J / 기대 %.6e J / 정규화 오차 %.3e"),
		StoredJ, ExpectedStoredJ, NormalizedStoredError));

	TestTrue(TEXT("모든 부품 온도가 유한해야 한다"), bFinite);
	TestTrue(TEXT("냉각수 온도가 유한해야 한다"), FMath::IsFinite(Model.GetState().CoolantTempK));
	TestTrue(TEXT("유량 0이면 온도가 단조 상승해야 한다"), bMonotonic);
	// 기준: 측정값(기대 축적 기준 정규화, 무차원)에 여유를 둔 값.
	TestTrue(TEXT("유량 0이면 발열이 전부 축적돼야 한다"), NormalizedStoredError < 1.0e-12);

	return true;
}

// 정상 상태에서 발열과 반출열이 균형을 이뤄야 한다. GB200 NVL72, 냉각수 입구 고정(개루프).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalSteadyStateTest,
	"VRG.Thermal.Rack.SteadyStateBalance", VrgThermalTestFlags)

bool FRackThermalSteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const FRackThermalInput Input = MakeNvl72Input(Params);

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

	double GpuMaxC = -1e9, CpuMaxC = -1e9;
	for (int32 N = 0; N < Model.GetNumNodes(); ++N)
	{
		const ERackPartKind Kind = Model.GetNodeParams(N).Kind;
		if (Kind == ERackPartKind::Gpu) GpuMaxC = FMath::Max(GpuMaxC, Snapshot.ComponentTempK[N] - 273.15);
		if (Kind == ERackPartKind::Cpu && Model.GetNodeParams(N).ToCoolantUAWK > 0.0) CpuMaxC = FMath::Max(CpuMaxC, Snapshot.ComponentTempK[N] - 273.15);
	}

	AddInfo(FString::Printf(TEXT("t=%.0f s | Blackwell 최고 %.2f C / Grace 최고 %.2f C"), Snapshot.SimTimeS, GpuMaxC, CpuMaxC));
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
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const FRackThermalInput Input = MakeNvl72Input(Params);
	FRackThermalState Initial;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	TestTrue(TEXT("작은 스텝은 안정해야 한다"), Model.IsStepStable(Input, 0.02));

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

// 공랭 직렬: 같은 부품을 네 개 직렬로 두면 뒤쪽일수록 뜨거워야 한다.
// 전부 입구 공기를 보게 짜면 네 온도가 같아지고, 그것이 냉각 과대평가다. (합성 장비, AirStage -1 = 직렬)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalSerialAirPathTest,
	"VRG.Thermal.Rack.SerialAirPathOrdering", VrgThermalTestFlags)

bool FRackThermalSerialAirPathTest::RunTest(const FString& Parameters)
{
	constexpr int32 NumParts = 4;
	TArray<FRackComponentParams> Parts;
	for (int32 I = 0; I < NumParts; ++I) Parts.Add(MakePart(500.0, 5.0));
	const FRackThermalParams Params = MakeTestDevice(Parts, true, 1.0e5);

	const double PerComponentHeatW = 100.0;
	FRackThermalInput Input;
	Input.ComponentHeatW.Init(PerComponentHeatW, NumParts);
	Input.SledDocked.Init(true, 1);
	Input.AirInletTempK = 297.15;
	Input.FanSpeedOverride = 1.0;
	Input.CoolantMassFlowKgs = 0.0;

	FRackThermalState Initial;
	Initial.CoolantTempK = 297.15;
	Initial.ComponentTempK.Init(297.15, NumParts);

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.1, 200000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(2.0e4);

	bool bStrictlyIncreasing = true;
	for (int32 Index = 0; Index < NumParts; ++Index)
	{
		AddInfo(FString::Printf(TEXT("체인 %d번 부품: %.3f C (반출 %.3f W)"),
			Index, Snapshot.ComponentTempK[Index] - 273.15, Snapshot.ComponentHeatToAirW[Index]));
		if (Index > 0 && !(Snapshot.ComponentTempK[Index] > Snapshot.ComponentTempK[Index - 1]))
		{
			bStrictlyIncreasing = false;
		}
	}

	const double FirstToLastRiseK = Snapshot.ComponentTempK[NumParts - 1] - Snapshot.ComponentTempK[0];
	AddInfo(FString::Printf(TEXT("팬 운전점 %.4f kg/s | 입구 %.2f C -> 배기 %.2f C | 체인 처음과 끝의 온도차 %.3f K"),
		TestFanFlowKgs(Params, 1.0), Input.AirInletTempK - 273.15, Snapshot.MixedAirOutletTempK - 273.15, FirstToLastRiseK));

	TestTrue(TEXT("뒤쪽 부품일수록 뜨거워야 한다 (앞 단이 데운 공기를 받는다)"), bStrictlyIncreasing);
	TestTrue(TEXT("직렬 효과가 유의미한 크기여야 한다"), FirstToLastRiseK > 1.0);

	// 정상 상태에서는 각 부품이 자기 발열만큼을 공기로 내보낸다 (다른 경로가 없다).
	double MaxDeviationW = 0.0;
	for (int32 Index = 0; Index < NumParts; ++Index)
	{
		MaxDeviationW = FMath::Max(MaxDeviationW, FMath::Abs(Snapshot.ComponentHeatToAirW[Index] - PerComponentHeatW));
	}
	AddInfo(FString::Printf(TEXT("부품별 반출열의 발열 대비 최대 편차 %.3e W (발열 %.0f W)"),
		MaxDeviationW, PerComponentHeatW));
	TestTrue(TEXT("정상 상태에서 부품별 반출열이 발열과 같아야 한다"), MaxDeviationW / PerComponentHeatW < 1.0e-6);

	return true;
}

// 공기 열수지: 구간별 반출열의 합과 공기 엔탈피 상승이 정확히 같아야 한다.
// effectiveness를 열유속과 출구 온도에 같은 값으로 쓰기 때문에 성립한다 (병렬 단은 혼합 출구로 같은 에너지).
// GB200 NVL72에서 컴퓨트 트레이 두 장을 뽑아 빈 슬롯이 섞인 경우에도 본다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalAirPathEnergyBalanceTest,
	"VRG.Thermal.Rack.AirPathEnergyBalance", VrgThermalTestFlags)

bool FRackThermalAirPathEnergyBalanceTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalInput Input = MakeNvl72Input(Params);

	int32 Undocked = 0;
	for (int32 Slot = 0; Slot < Params.Slots.Num() && Undocked < 2; ++Slot)
		if (Params.Slots[Slot].DeviceType == FRackThermalModel::Gb200DevCompute && Slot % 3 == 0) { Input.SledDocked[Slot] = false; ++Undocked; }

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.02, 100000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(2.0e3);

	// 슬롯별로 (질량유량 * Cp * 온도 상승)을 더한 값.
	double EnthalpyRiseW = 0.0;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
	{
		EnthalpyRiseW += Snapshot.SlotAirMassFlowKgs[Slot] * Params.AirCpJKgK
			* (Snapshot.SledAirOutletTempK[Slot] - Input.AirInletTempK);
	}

	const double AbsErrorW = FMath::Abs(EnthalpyRiseW - Snapshot.TotalHeatToAirW);
	const double NormalizedError = AbsErrorW / FMath::Max(FMath::Abs(Snapshot.TotalHeatToAirW), 1.0);

	AddInfo(FString::Printf(TEXT("뽑은 컴퓨트 트레이 %d장 | 구간 반출열 합 %.6f W / 공기 엔탈피 상승 %.6f W"),
		Undocked, Snapshot.TotalHeatToAirW, EnthalpyRiseW));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e W / 반출열 기준 정규화 오차 %.3e (무차원)"),
		AbsErrorW, NormalizedError));

	TestEqual(TEXT("컴퓨트 트레이 두 장을 뽑았어야 한다"), Undocked, 2);
	TestTrue(TEXT("구간 반출열의 합과 공기 엔탈피 상승이 일치해야 한다"), NormalizedError < 1.0e-12);

	return true;
}

// 냉각 경로 분담: 컴퓨트 트레이의 콜드플레이트 부품(Grace, Blackwell, LPDDR5X)은 냉각수로만,
// 공랭 부품(NIC, DPU, 드라이브, 전원, 팬)은 공기로만 열을 내보낸다. 랙 발열의 대부분은 냉각수가 맡는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackThermalLiquidAirSplitTest,
	"VRG.Thermal.Rack.LiquidAirSplit", VrgThermalTestFlags)

bool FRackThermalLiquidAirSplitTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const FRackThermalInput Input = MakeNvl72Input(Params);

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);
	RunSteps(Model, Input, 0.05, 100000);

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(5.0e3);

	int32 ComputeSlot = INDEX_NONE;
	for (int32 Slot = 0; Slot < Params.Slots.Num() && ComputeSlot == INDEX_NONE; ++Slot)
		if (Params.Slots[Slot].DeviceType == FRackThermalModel::Gb200DevCompute) ComputeSlot = Slot;

	double MaxLiquidPartToAirW = 0.0, MaxAirPartToCoolantW = 0.0;
	int32 LiquidParts = 0, AirParts = 0;
	for (int32 N = Model.GetSlotNodeStart(ComputeSlot); N < Model.GetSlotNodeStart(ComputeSlot) + Model.GetSlotNodeCount(ComputeSlot); ++N)
	{
		const FRackComponentParams& P = Model.GetNodeParams(N);
		if (P.ToCoolantUAWK > 0.0 && P.ToAirUAWK == 0.0) { ++LiquidParts; MaxLiquidPartToAirW = FMath::Max(MaxLiquidPartToAirW, FMath::Abs(Snapshot.ComponentHeatToAirW[N])); }
		if (P.ToAirUAWK > 0.0 && P.ToCoolantUAWK == 0.0) { ++AirParts; MaxAirPartToCoolantW = FMath::Max(MaxAirPartToCoolantW, FMath::Abs(Snapshot.ComponentHeatToCoolantW[N])); }
		AddInfo(FString::Printf(TEXT("%s: %.2f C, 냉각수로 %.1f W, 공기로 %.1f W"), *P.Name.ToString(),
			Snapshot.ComponentTempK[N] - 273.15, Snapshot.ComponentHeatToCoolantW[N], Snapshot.ComponentHeatToAirW[N]));
	}
	AddInfo(FString::Printf(TEXT("랙 합계: 냉각수 %.0f W (%.1f%%) / 공기 %.0f W / 자연대류 %.0f W (발열 %.0f W)"),
		Snapshot.TotalHeatToCoolantW, 100.0 * Snapshot.TotalHeatToCoolantW / Snapshot.TotalHeatW,
		Snapshot.TotalHeatToAirW, Snapshot.TotalHeatToAmbientW, Snapshot.TotalHeatW));

	TestTrue(TEXT("컴퓨트 트레이에 콜드플레이트 부품과 공랭 부품이 둘 다 있어야 한다"), LiquidParts >= 8 && AirParts >= 8);
	TestEqual(TEXT("콜드플레이트 부품은 공기로 열을 내보내지 않아야 한다"), MaxLiquidPartToAirW, 0.0);
	TestEqual(TEXT("공랭 부품은 냉각수로 열을 내보내지 않아야 한다"), MaxAirPartToCoolantW, 0.0);
	TestTrue(TEXT("콜드플레이트가 랙 발열의 대부분을 맡아야 한다"), Snapshot.TotalHeatToCoolantW > 0.7 * Snapshot.TotalHeatW);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
