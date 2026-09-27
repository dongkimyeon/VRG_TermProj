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
	constexpr double RadiatorUAWK = 1.1e4;

	// Design liquid-side heat of the preset [W]: each part's heat times its coolant share.
	double LiquidDesignHeatW(const FRackThermalParams& Params)
	{
		TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
		FRackThermalModel::ResolveLayout(Params, Nodes, Starts);
		double W = 0.0;
		for (const FRackComponentParams& N : Nodes)
			if (N.ToCoolantUAWK > 0.0) W += N.DefaultHeatW * N.ToCoolantUAWK / (N.ToCoolantUAWK + N.ToAirUAWK);
		return W;
	}
}

// GB200 NVL72 프리셋(장비·칩 단위)의 정상 상태가 타당한 범위에 있어야 한다.
//
// 보존 검사는 장부가 맞는지만 본다. 파라미터가 1000배 틀려도 통과한다.
// 그래서 물리 법칙이 아니라 상식 범위를 따로 건다 (CLAUDE.md의 S2 교훈).
// 범위의 근거는 RackThermalModel.cpp의 프리셋 주석(NVIDIA / Lenovo / SemiAnalysis 공개 자료)이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGb200Nvl72SteadyStateTest,
	"VRG.Thermal.Gb200Nvl72.SteadyStatePlausible", VrgGb200TestFlags)

bool FGb200Nvl72SteadyStateTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();

	// Layout: 37 devices, 1U each, inside the 48U mounting range, no two in the same U.
	int32 Counts[4] = {0, 0, 0, 0};
	TSet<int32> UsedU;
	bool bInside = true;
	for (const FRackSlot& Slot : Params.Slots)
	{
		++Counts[Slot.DeviceType];
		UsedU.Add(FMath::RoundToInt32((Slot.BottomZM - FRackThermalModel::Gb200SlotZ0M) / FRackThermalModel::Gb200UnitM));
		bInside &= Slot.BottomZM >= .081 - 1e-3 && Slot.BottomZM + Slot.HeightM <= 2.186 + 1e-3;
	}
	TestEqual(TEXT("18 compute trays"), Counts[0], 18);
	TestEqual(TEXT("9 NVLink switch trays"), Counts[1], 9);
	TestEqual(TEXT("8 power shelves"), Counts[2], 8);
	TestEqual(TEXT("2 management switches"), Counts[3], 2);
	TestEqual(TEXT("One device per U"), UsedU.Num(), Params.Slots.Num());
	TestTrue(TEXT("Every device inside the mounting range"), bInside);

	FRackThermalInput Input;
	Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.Slots.Num());
	Input.CoolantMassFlowKgs = CoolantMassFlowKgs;

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

	// Hottest part of each kind, and the DC load / PSU loss split.
	TMap<ERackPartKind, double> MaxC;
	double DcW = 0.0, PsuW = 0.0;
	TArray<double> E1sC;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
		for (int32 N = Model.GetSlotNodeStart(Slot); N < Model.GetSlotNodeStart(Slot) + Model.GetSlotNodeCount(Slot); ++N)
		{
			const FRackComponentParams& P = Model.GetNodeParams(N);
			const double C = Snapshot.ComponentTempK[N] - 273.15;
			MaxC.FindOrAdd(P.Kind, -1e9) = FMath::Max(MaxC.FindOrAdd(P.Kind, -1e9), C);
			// Actual heat of the step (fan parts carry the computed fan input, not their nominal 20 W).
			if (P.LossFractionOfRackLoad > 0.0) PsuW += P.LossFractionOfRackLoad; else DcW += Snapshot.ComponentHeatW[N];
			if (Slot == 6 && P.Kind == ERackPartKind::Storage && P.Name.ToString().StartsWith(TEXT("E1.S"))) E1sC.Add(C);
		}
	const double PsuLossW = PsuW * DcW;  // sum of loss fractions times the DC load
	const double CoolantDeltaTK = Snapshot.CoolantOutletTempK - Input.CoolantInletTempK;
	const double LiquidFraction = Snapshot.TotalHeatToCoolantW / Snapshot.TotalHeatW;
	const double AirFraction = (Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW) / Snapshot.TotalHeatW;

	AddInfo(FString::Printf(TEXT("장비 %d대 / 노드 %d개 / 랙 %.2f kW = DC %.2f kW + PSU 손실 %.3f kW"),
		Params.Slots.Num(), Model.GetNumNodes(), Snapshot.TotalHeatW / 1000.0, DcW / 1000.0, PsuLossW / 1000.0));
	AddInfo(FString::Printf(TEXT("최고 온도 C: Blackwell %.1f / Grace %.1f / LPDDR5X %.1f / NVSwitch %.1f / NIC %.1f / DPU %.1f / E1.S %.1f / 전원 %.1f / 팬 %.1f"),
		MaxC[ERackPartKind::Gpu], MaxC[ERackPartKind::Cpu], MaxC[ERackPartKind::Memory], MaxC[ERackPartKind::Switch],
		MaxC[ERackPartKind::Nic], MaxC[ERackPartKind::Dpu], MaxC[ERackPartKind::Storage], MaxC[ERackPartKind::Power], MaxC[ERackPartKind::Fan]));
	AddInfo(FString::Printf(TEXT("냉각수 %.2f -> %.2f C (ΔT %.2f K, 유량 %.2f kg/s) / 캐비닛 배기 %.2f C"),
		Input.CoolantInletTempK - 273.15, Snapshot.CoolantOutletTempK - 273.15,
		CoolantDeltaTK, Input.CoolantMassFlowKgs, Snapshot.MixedAirOutletTempK - 273.15));
	AddInfo(FString::Printf(TEXT("반출: 냉각수 %.1f kW (%.1f%%) / 공기 %.1f kW + 자연대류 %.2f kW (%.1f%%)"),
		Snapshot.HeatRemovedByCoolantW / 1000.0, LiquidFraction * 100.0,
		Snapshot.TotalHeatToAirW / 1000.0, Snapshot.TotalHeatToAmbientW / 1000.0, AirFraction * 100.0));

	const double RemovedW = Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW + Snapshot.HeatRemovedByCoolantW;
	const double NormalizedImbalance = FMath::Abs(RemovedW - Snapshot.TotalHeatW) / Snapshot.TotalHeatW;
	double SlotAirSumW = 0.0;
	for (const double W : Snapshot.SlotHeatToAirW) SlotAirSumW += W;
	const double SlotAirError = FMath::Abs(SlotAirSumW - Snapshot.TotalHeatToAirW - Snapshot.TotalHeatToAmbientW) / Snapshot.TotalHeatW;
	// Power-shelf fans are powered inside the PSU and their heat lands on the PSU parts, outside the DC load.
	double ShelfFanW = 0.0;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot) if (Params.Slots[Slot].DeviceType == 2) ShelfFanW += Snapshot.SlotFanPowerW[Slot];
	const double PsuError = FMath::Abs(Snapshot.TotalHeatW - DcW - DcW * (1.0 / .975 - 1.0) - ShelfFanW) / Snapshot.TotalHeatW;
	AddInfo(FString::Printf(TEXT("정상 상태 불균형 %.3e / 슬롯별 공기 열 합 오차 %.3e / PSU 손실 = DC x (1/0.975-1) 오차 %.3e (발열 기준, 무차원)"),
		NormalizedImbalance, SlotAirError, PsuError));

	TestTrue(TEXT("스텝이 안정 조건을 만족해야 한다"), Snapshot.bStepStable);
	TestTrue(TEXT("정상 상태에서 발열과 반출열이 균형을 이뤄야 한다"), NormalizedImbalance < 1.0e-6);
	TestTrue(TEXT("슬롯별 공기 열의 합이 랙 공기 열과 같아야 한다"), SlotAirError < 1.0e-12);
	TestTrue(TEXT("전원 셸프 열 = DC 부하의 변환 손실"), PsuError < 1.0e-12);

	// --- 타당성 안전망 (물리 법칙이 아니라 공개 자료 범위다) ---
	// Fans: every fan-equipped slot inside [min, 1]; fan power a small share of the rack (plausibility).
	double MinR = 1.0, MaxR = 0.0;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
		if (FRackThermalModel::FansOf(Params, Slot)) { MinR = FMath::Min(MinR, Snapshot.SlotFanSpeedRatio[Slot]); MaxR = FMath::Max(MaxR, Snapshot.SlotFanSpeedRatio[Slot]); }
	double RackAirKgs = 0.0; for (const double M : Snapshot.SlotAirMassFlowKgs) RackAirKgs += M;
	AddInfo(FString::Printf(TEXT("팬 모드 %s | 팬 속도 %.3f ~ %.3f | 팬 전력 %.3f kW (%.2f%%) | 랙 풍량 %.3f kg/s"),
		Snapshot.bFanMode ? TEXT("켬") : TEXT("끔"), MinR, MaxR, Snapshot.TotalFanPowerW / 1000.0,
		Snapshot.TotalFanPowerW / Snapshot.TotalHeatW * 100.0, RackAirKgs));
	TestTrue(TEXT("GB200 preset runs its own fans"), Snapshot.bFanMode && MinR >= .3 - 1e-12 && MaxR <= 1.0);
	TestTrue(TEXT("Fan power below 2% of rack heat"), Snapshot.TotalFanPowerW > 0.0 && Snapshot.TotalFanPowerW < .02 * Snapshot.TotalHeatW);

	TestTrue(TEXT("랙 발열이 NVIDIA 공개치(약 120 kW)의 ±5% 안이어야 한다"),
		Snapshot.TotalHeatW > 1.14e5 && Snapshot.TotalHeatW < 1.26e5);
	TestTrue(TEXT("공기 몫이 공개 범위(랙 약 10% ~ 트레이 약 15%) 근처여야 한다 (8~17%)"), AirFraction > .08 && AirFraction < .17);
	TestTrue(TEXT("냉각수 ΔT가 5~15 K 범위여야 한다"), CoolantDeltaTK > 5.0 && CoolantDeltaTK < 15.0);
	TestTrue(TEXT("Blackwell < 90 C"), MaxC[ERackPartKind::Gpu] < 90.0);
	TestTrue(TEXT("Grace < 90 C"), MaxC[ERackPartKind::Cpu] < 90.0);
	TestTrue(TEXT("NVSwitch < 90 C"), MaxC[ERackPartKind::Switch] < 90.0);
	TestTrue(TEXT("NIC / DPU < 95 C"), MaxC[ERackPartKind::Nic] < 95.0 && MaxC[ERackPartKind::Dpu] < 95.0);
	TestTrue(TEXT("E1.S NVMe < 70 C (throttle region)"), MaxC[ERackPartKind::Storage] < 70.0);
	TestTrue(TEXT("PSU / PDB < 100 C"), MaxC[ERackPartKind::Power] < 100.0);

	// Parallel air stage: the four identical front drives see the same inlet air, so they are equal.
	// In a series chain each would be hotter than the one before it.
	double E1sSpread = 0.0;
	for (const double C : E1sC) E1sSpread = FMath::Max(E1sSpread, FMath::Abs(C - E1sC[0]));
	AddInfo(FString::Printf(TEXT("첫 컴퓨트 트레이 E1.S 4개 온도 차 %.3e K (병렬 공기 경로)"), E1sSpread));
	TestTrue(TEXT("Parallel drives share one inlet"), E1sC.Num() == 4 && E1sSpread < 1e-9);

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
	Spec.SledsPerRack = FRackThermalModel::CountLiquidSlots(FRackThermalModel::MakeGb200Nvl72Params());
	Spec.HeaderSegmentResistancePaS2M6 = 6.0e6;
	Spec.RackBranchResistancePaS2M6 = 1.7e10;
	Spec.ReturnToCduResistancePaS2M6 = 1.5e7;

	// 펌프도 방 액터(CDU) 값으로 맞춘다. FInRowLoopSpec 기본값과 같다.
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
	const double LiquidPerRackW = LiquidDesignHeatW(FRackThermalModel::MakeGb200Nvl72Params());

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
