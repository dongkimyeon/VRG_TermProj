// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Thermal/RadiatorModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgGb200TestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// ADataCenterRoomActor가 띄우는 랙과 같은 운전점이어야 한다.
	// 이 값이 바뀌면 여기도 같이 바꿔야 테스트가 실제 구성을 검증한다.
	constexpr double CoolantMassFlowKgs = 2.3;
	constexpr double TotalAirMassFlowKgs = 0.6;
	constexpr double RadiatorUAWK = 1.1e4;
}

// GB200 NVL72 프리셋의 정상 상태가 타당한 범위에 있어야 한다.
//
// 보존 검사는 장부가 맞는지만 본다. 파라미터가 1000배 틀려도 통과한다.
// 그래서 물리 법칙이 아니라 상식 범위를 따로 건다 (CLAUDE.md의 S2 교훈).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGb200Nvl72SteadyStateTest,
	"VRG.Thermal.Gb200Nvl72.SteadyStatePlausible", VrgGb200TestFlags)

bool FGb200Nvl72SteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();

	FRackThermalInput Input;
	Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.NumSleds);
	Input.CoolantMassFlowKgs = CoolantMassFlowKgs;
	Input.TotalAirMassFlowKgs = TotalAirMassFlowKgs;

	FRadiatorParams Radiator;
	Radiator.UAWK = RadiatorUAWK;

	FRackThermalState Initial;
	Initial.CoolantTempK = 300.15;

	FRackThermalModel Model;
	Model.Initialize(Params, Initial);

	const double DeltaTimeS = 0.05;
	const int32 NumSteps = 200000;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Input.CoolantInletTempK = RadiatorModel::OutletTempK(
			Radiator, Model.GetState().CoolantTempK, Input.CoolantMassFlowKgs, Params.CoolantCpJKgK);
		Model.Step(Input, DeltaTimeS);
	}

	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(DeltaTimeS * NumSteps);

	const double CpuC = Model.GetComponentTempK(0, ERackComponent::Cpu) - 273.15;
	const double GpuC = Model.GetComponentTempK(0, ERackComponent::Gpu) - 273.15;
	const double RamC = Model.GetComponentTempK(0, ERackComponent::Ram) - 273.15;
	const double VrmC = Model.GetComponentTempK(0, ERackComponent::Vrm) - 273.15;

	const double CoolantDeltaTK = Snapshot.CoolantOutletTempK - Input.CoolantInletTempK;
	const double LiquidFraction = Snapshot.TotalHeatToCoolantW / Snapshot.TotalHeatW;

	AddInfo(FString::Printf(TEXT("컴퓨트 트레이 %d장 / 노드 %d개 / 랙 발열 %.1f kW"),
		Params.NumSleds, Model.GetNumNodes(), Snapshot.TotalHeatW / 1000.0));
	AddInfo(FString::Printf(TEXT("트레이 0: Grace %.1f C / Blackwell %.1f C / RAM-NIC %.1f C / VRM %.1f C"),
		CpuC, GpuC, RamC, VrmC));
	AddInfo(FString::Printf(TEXT("냉각수 %.2f -> %.2f C (ΔT %.2f K, 유량 %.2f kg/s) / 캐비닛 배기 %.2f C"),
		Input.CoolantInletTempK - 273.15, Snapshot.CoolantOutletTempK - 273.15,
		CoolantDeltaTK, Input.CoolantMassFlowKgs, Snapshot.MixedAirOutletTempK - 273.15));
	AddInfo(FString::Printf(TEXT("반출: 냉각수 %.1f kW (%.1f%%) / 공기 %.1f kW / 자연대류 %.2f kW"),
		Snapshot.HeatRemovedByCoolantW / 1000.0, LiquidFraction * 100.0,
		Snapshot.TotalHeatToAirW / 1000.0, Snapshot.TotalHeatToAmbientW / 1000.0));

	const double RemovedW = Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW + Snapshot.HeatRemovedByCoolantW;
	const double NormalizedImbalance = FMath::Abs(RemovedW - Snapshot.TotalHeatW) / Snapshot.TotalHeatW;
	AddInfo(FString::Printf(TEXT("발열 기준 정규화 불균형 %.3e (무차원)"), NormalizedImbalance));

	TestTrue(TEXT("스텝이 안정 조건을 만족해야 한다"), Snapshot.bStepStable);
	TestTrue(TEXT("정상 상태에서 발열과 반출열이 균형을 이뤄야 한다"), NormalizedImbalance < 1.0e-6);

	// --- 타당성 안전망 (물리 법칙이 아니라 상식 범위다) ---
	TestTrue(TEXT("랙 발열이 90~120 kW 범위여야 한다 (컴퓨트 트레이 18장분)"),
		Snapshot.TotalHeatW > 9.0e4 && Snapshot.TotalHeatW < 1.2e5);
	TestTrue(TEXT("NVL72는 액랭이 발열의 85% 이상을 맡아야 한다"), LiquidFraction > 0.85);
	TestTrue(TEXT("냉각수 ΔT가 5~15 K 범위여야 한다"), CoolantDeltaTK > 5.0 && CoolantDeltaTK < 15.0);

	TestTrue(TEXT("Blackwell이 허용 온도 90 C 아래여야 한다"), GpuC < 90.0);
	TestTrue(TEXT("Grace가 허용 온도 90 C 아래여야 한다"), CpuC < 90.0);
	TestTrue(TEXT("RAM/NIC이 허용 온도 85 C 아래여야 한다"), RamC < 85.0);
	TestTrue(TEXT("VRM이 허용 온도 125 C 아래여야 한다"), VrmC < 125.0);

	// 공랭 직렬 순서가 유지되는지. 뒤쪽(VRM)이 더 뜨거워야 한다.
	TestTrue(TEXT("공랭 체인 뒤쪽 부품이 더 뜨거워야 한다"), VrmC > RamC);

	return true;
}

// 방 액터의 수력 기본값이 GB200 랙의 필요 유량을 실제로 대야 한다.
// 값을 감으로 넣으면 보존 검사는 통과하면서 유량이 10배 틀릴 수 있다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGb200Nvl72RowHydraulicsTest,
	"VRG.Hydraulics.Gb200Nvl72.RowOperatingPointPlausible", VrgGb200TestFlags)

bool FGb200Nvl72RowHydraulicsTest::RunTest(const FString& Parameters)
{
	// ADataCenterRoomActor의 기본값과 같아야 한다.
	const int32 RacksPerRow = 8;
	FInRowLoopSpec Spec;
	Spec.NumRacks = RacksPerRow;
	Spec.SledsPerRack = FRackThermalModel::Gb200Nvl72ComputeTrays;
	Spec.HeaderSegmentResistancePaS2M6 = 6.0e6;
	Spec.RackBranchResistancePaS2M6 = 1.7e10;
	Spec.ReturnToCduResistancePaS2M6 = 1.5e7;

	// 펌프도 방 액터 값으로 맞춰야 한다. FInRowLoopSpec의 기본 펌프는
	// 15 kW 하이브리드 랙(AHydraulicNetworkDebugActor)용이라 NVL72 유량을 못 댄다.
	Spec.PumpShutoffHeadPa = 4.0e5;
	Spec.PumpCurveCoeffPaS2M6 = 6.0e8;
	Spec.PumpEfficiency = 0.7;

	FHydraulicNetwork Network;
	const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, Spec);

	FHydraulicSolveSettings Settings;
	const FHydraulicSolveResult Result = Network.Solve(Settings);

	const double PumpFlowM3s = Network.GetPumpFlowM3s(Handles.PumpIndex);
	const double PumpHeadPa = Network.GetPumpDeltaPPa(Handles.PumpIndex);
	const double PumpElectricalW = Network.GetPumpElectricalInputPowerW(Handles.PumpIndex);

	double MinBranchM3s = TNumericLimits<double>::Max();
	double MaxBranchM3s = 0.0;
	double BranchSumM3s = 0.0;
	for (const int32 PipeIndex : Handles.RackBranchPipeIndices)
	{
		const double FlowM3s = FMath::Abs(Network.GetPipeFlowM3s(PipeIndex));
		MinBranchM3s = FMath::Min(MinBranchM3s, FlowM3s);
		MaxBranchM3s = FMath::Max(MaxBranchM3s, FlowM3s);
		BranchSumM3s += FlowM3s;
	}

	// 랙 1대의 액랭 부하와 그 유량이 내는 냉각수 ΔT.
	const TArray<double> TrayHeatW = FRackThermalModel::Gb200Nvl72TrayHeatW();
	const double LiquidPerTrayW = TrayHeatW[static_cast<int32>(ERackComponent::Cpu)]
		+ TrayHeatW[static_cast<int32>(ERackComponent::Gpu)];
	const double LiquidPerRackW = LiquidPerTrayW * FRackThermalModel::Gb200Nvl72ComputeTrays;

	// 물 밀도 1000 kg/m^3, 비열 4182 J/(kg*K).
	const double MinBranchDeltaTK = LiquidPerRackW / (MaxBranchM3s * 1000.0 * 4182.0);
	const double MaxBranchDeltaTK = LiquidPerRackW / (MinBranchM3s * 1000.0 * 4182.0);

	const double ItLoadW = LiquidPerRackW * RacksPerRow;
	const double PumpFractionOfIt = PumpElectricalW / ItLoadW;

	AddInfo(FString::Printf(TEXT("펌프 %.2f L/s / 양정 %.1f kPa / 전기 입력 %.2f kW / Newton %d회"),
		PumpFlowM3s * 1000.0, PumpHeadPa / 1000.0, PumpElectricalW / 1000.0, Result.IterationsUsed));
	AddInfo(FString::Printf(TEXT("랙 분기 %.3f ~ %.3f L/s (편차 %.1f%%)"),
		MinBranchM3s * 1000.0, MaxBranchM3s * 1000.0,
		(1.0 - MinBranchM3s / MaxBranchM3s) * 100.0));
	AddInfo(FString::Printf(TEXT("랙 1대 액랭 부하 %.1f kW -> 냉각수 ΔT %.2f ~ %.2f K"),
		LiquidPerRackW / 1000.0, MinBranchDeltaTK, MaxBranchDeltaTK));
	AddInfo(FString::Printf(TEXT("행 IT 부하 %.0f kW 대비 펌프 전력 %.2f%%"),
		ItLoadW / 1000.0, PumpFractionOfIt * 100.0));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);

	const double NormalizedMassError = FMath::Abs(BranchSumM3s - PumpFlowM3s) / PumpFlowM3s;
	AddInfo(FString::Printf(TEXT("분기 합 - 펌프 유량 정규화 오차 %.3e"), NormalizedMassError));
	TestTrue(TEXT("분기 유량의 합이 펌프 유량과 같아야 한다"), NormalizedMassError < 1.0e-9);

	// --- 타당성 안전망 ---
	TestTrue(TEXT("랙마다 필요한 냉각수 ΔT가 5~15 K 범위여야 한다"),
		MinBranchDeltaTK > 5.0 && MaxBranchDeltaTK < 15.0);
	TestTrue(TEXT("펌프 전력이 IT 부하의 5% 미만이어야 한다"), PumpFractionOfIt < 0.05);
	TestTrue(TEXT("랙별 분기 유량 편차가 15% 미만이어야 한다"),
		(1.0 - MinBranchM3s / MaxBranchM3s) < 0.15);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
