// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgUndockTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// --- 수력: AHydraulicNetworkDebugActor와 같은 인로우 루프를 솔버 수준에서 세운다 ---
	// 액터 없이도 돌아야 하므로 그래프를 직접 만든다.
	// 이 값들은 AHydraulicNetworkDebugActor의 기본값과 같아야 한다.
	constexpr int32 RackCount = 6;
	constexpr int32 SledsPerRack = 12;
	constexpr double HeaderSegmentResistancePaS2M6 = 2.7e8;
	constexpr double RackBranchResistanceFullPaS2M6 = 1.4e11;
	constexpr double ReturnToCduResistancePaS2M6 = 5.4e8;

	struct FRowSolveOutput
	{
		FHydraulicSolveResult Result;
		double PumpFlowM3s = 0.0;
		// 길이 RackCount. 분기가 끊긴 랙은 0이다.
		TArray<double> RackBranchFlowM3s;
	};

	// DockedSleds[i] == 0 이면 그 랙의 분기와 랙 노드를 아예 만들지 않는다.
	FRowSolveOutput SolveRow(const TArray<int32>& DockedSleds)
	{
		FHydraulicNetwork Network;

		FHydraulicNode CduNode;
		CduNode.bFixedPressure = true;
		CduNode.PressurePa = 2.0e5;
		const int32 Cdu = Network.AddNode(CduNode);

		TArray<int32> SupplyNodes;
		TArray<int32> ReturnNodes;
		TArray<int32> RackNodes;
		for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
		{
			SupplyNodes.Add(Network.AddNode(FHydraulicNode()));
			ReturnNodes.Add(Network.AddNode(FHydraulicNode()));
			RackNodes.Add(DockedSleds[RackIndex] > 0 ? Network.AddNode(FHydraulicNode()) : INDEX_NONE);
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

		TArray<int32> BranchPipes;
		for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
		{
			if (RackIndex + 1 < RackCount)
			{
				AddPipe(SupplyNodes[RackIndex], SupplyNodes[RackIndex + 1], HeaderSegmentResistancePaS2M6);
			}

			if (DockedSleds[RackIndex] > 0)
			{
				// 2차 손실 병렬 합성: 동일 저항 n개를 병렬로 묶으면 R/n^2.
				const double ParallelScale = static_cast<double>(SledsPerRack) / static_cast<double>(DockedSleds[RackIndex]);
				const double BranchPaS2M6 = RackBranchResistanceFullPaS2M6 * ParallelScale * ParallelScale;

				BranchPipes.Add(AddPipe(SupplyNodes[RackIndex], RackNodes[RackIndex], BranchPaS2M6));
				AddPipe(RackNodes[RackIndex], ReturnNodes[RackIndex], BranchPaS2M6);
			}
			else
			{
				BranchPipes.Add(INDEX_NONE);
			}

			if (RackIndex + 1 < RackCount)
			{
				AddPipe(ReturnNodes[RackIndex + 1], ReturnNodes[RackIndex], HeaderSegmentResistancePaS2M6);
			}
		}

		AddPipe(ReturnNodes[0], Cdu, ReturnToCduResistancePaS2M6);

		FRowSolveOutput Output;
		FHydraulicSolveSettings Settings;
		Output.Result = Network.Solve(Settings);
		Output.PumpFlowM3s = Network.GetPumpFlowM3s(0);
		for (const int32 PipeIndex : BranchPipes)
		{
			Output.RackBranchFlowM3s.Add(PipeIndex == INDEX_NONE ? 0.0 : FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
		}
		return Output;
	}
}

// 유닛을 뽑으면 그 랙의 분기 저항이 올라가 유량이 줄고, 남은 랙이 더 받아야 한다.
// D4: 영향 범위를 위상표로 정하지 않고 네트워크 재계산 결과로 판정한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicSledUndockRedistributionTest,
	"VRG.Hydraulics.Network.SledUndockRedistributesFlow", VrgUndockTestFlags)

bool FHydraulicSledUndockRedistributionTest::RunTest(const FString& Parameters)
{
	TArray<int32> FullDocked;
	FullDocked.Init(SledsPerRack, RackCount);
	const FRowSolveOutput Baseline = SolveRow(FullDocked);

	// 랙 2번에서 유닛 12대 중 4대를 뽑는다.
	TArray<int32> PartialDocked = FullDocked;
	PartialDocked[2] = 8;
	const FRowSolveOutput Partial = SolveRow(PartialDocked);

	AddInfo(FString::Printf(TEXT("만재: 펌프 %.4f L/s | 분기 %.3f %.3f %.3f %.3f %.3f %.3f L/s"),
		Baseline.PumpFlowM3s * 1000.0,
		Baseline.RackBranchFlowM3s[0] * 1000.0, Baseline.RackBranchFlowM3s[1] * 1000.0,
		Baseline.RackBranchFlowM3s[2] * 1000.0, Baseline.RackBranchFlowM3s[3] * 1000.0,
		Baseline.RackBranchFlowM3s[4] * 1000.0, Baseline.RackBranchFlowM3s[5] * 1000.0));
	AddInfo(FString::Printf(TEXT("랙2 8/12대: 펌프 %.4f L/s | 분기 %.3f %.3f %.3f %.3f %.3f %.3f L/s"),
		Partial.PumpFlowM3s * 1000.0,
		Partial.RackBranchFlowM3s[0] * 1000.0, Partial.RackBranchFlowM3s[1] * 1000.0,
		Partial.RackBranchFlowM3s[2] * 1000.0, Partial.RackBranchFlowM3s[3] * 1000.0,
		Partial.RackBranchFlowM3s[4] * 1000.0, Partial.RackBranchFlowM3s[5] * 1000.0));

	TestTrue(TEXT("만재 구성이 수렴해야 한다"), Baseline.Result.bConverged);
	TestTrue(TEXT("일부 탈착 구성이 수렴해야 한다"), Partial.Result.bConverged);

	TestTrue(TEXT("유닛을 뽑은 랙은 분기 유량이 줄어야 한다"),
		Partial.RackBranchFlowM3s[2] < Baseline.RackBranchFlowM3s[2]);

	bool bOthersIncreased = true;
	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		if (RackIndex == 2)
		{
			continue;
		}
		if (!(Partial.RackBranchFlowM3s[RackIndex] > Baseline.RackBranchFlowM3s[RackIndex]))
		{
			bOthersIncreased = false;
		}
	}
	TestTrue(TEXT("남은 랙들은 분기 유량이 늘어야 한다"), bOthersIncreased);

	// 랙 3번을 통째로 비운다. 분기가 사라지므로 유량이 정확히 0이어야 한다.
	TArray<int32> EmptyRackDocked = FullDocked;
	EmptyRackDocked[3] = 0;
	const FRowSolveOutput EmptyRack = SolveRow(EmptyRackDocked);

	AddInfo(FString::Printf(TEXT("랙3 0/12대: 펌프 %.4f L/s | 분기 %.3f %.3f %.3f %.3f %.3f %.3f L/s"),
		EmptyRack.PumpFlowM3s * 1000.0,
		EmptyRack.RackBranchFlowM3s[0] * 1000.0, EmptyRack.RackBranchFlowM3s[1] * 1000.0,
		EmptyRack.RackBranchFlowM3s[2] * 1000.0, EmptyRack.RackBranchFlowM3s[3] * 1000.0,
		EmptyRack.RackBranchFlowM3s[4] * 1000.0, EmptyRack.RackBranchFlowM3s[5] * 1000.0));

	TestTrue(TEXT("랙을 통째로 비운 구성이 수렴해야 한다"), EmptyRack.Result.bConverged);
	TestEqual(TEXT("비운 랙의 분기 유량은 정확히 0이어야 한다"), EmptyRack.RackBranchFlowM3s[3], 0.0);

	// 질량 보존: 분기 유량의 합 = 펌프 유량.
	double BranchSumM3s = 0.0;
	for (const double FlowM3s : EmptyRack.RackBranchFlowM3s)
	{
		BranchSumM3s += FlowM3s;
	}
	const double NormalizedMassError = FMath::Abs(BranchSumM3s - EmptyRack.PumpFlowM3s)
		/ FMath::Max(EmptyRack.PumpFlowM3s, 1.0e-12);

	AddInfo(FString::Printf(TEXT("분기 합 %.9f vs 펌프 유량 %.9f m3/s, 정규화 오차 %.3e"),
		BranchSumM3s, EmptyRack.PumpFlowM3s, NormalizedMassError));
	TestTrue(TEXT("분기 유량의 합이 펌프 유량과 같아야 한다"), NormalizedMassError < 1.0e-9);

	// 타당성 안전망: 랙 하나가 빠졌다고 총 유량이 사라지거나 폭주하면 안 된다.
	const double PumpFlowRatio = EmptyRack.PumpFlowM3s / Baseline.PumpFlowM3s;
	AddInfo(FString::Printf(TEXT("랙 1대 격리 시 펌프 유량비 %.4f (만재 대비)"), PumpFlowRatio));
	TestTrue(TEXT("랙 1대를 빼도 펌프 유량이 만재의 0.7~1.0배 범위여야 한다"),
		PumpFlowRatio > 0.7 && PumpFlowRatio < 1.0);

	return true;
}

// [B] 빈 슬롯은 유동저항이 낮아 공기를 더 가져간다. 그만큼 남은 유닛의 유량이 줄고
// 온도가 오른다. 총 유량은 팬이 정하므로 보존된다.
// 실제로 blanking panel을 끼우는 이유가 이것이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackUndockedSlotAirBypassTest,
	"VRG.Thermal.Rack.UndockedSlotStealsAirFlow", VrgUndockTestFlags)

bool FRackUndockedSlotAirBypassTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params;
	const int32 NumSleds = Params.NumSleds;

	auto RunToSteadyState = [&Params, NumSleds](const TArray<bool>& SledDocked, FRackThermalModel& OutModel)
	{
		FRackThermalInput Input;
		Input.ComponentHeatW = FRackThermalModel::MakeUniformHeatW(NumSleds);
		Input.SledDocked = SledDocked;

		FRackThermalState Initial;
		Initial.CoolantTempK = 300.15;

		OutModel.Initialize(Params, Initial);
		for (int32 StepIndex = 0; StepIndex < 100000; ++StepIndex)
		{
			OutModel.Step(Input, 0.05);
		}
		return OutModel.MakeSnapshot(5.0e3);
	};

	TArray<bool> FullDocked;
	FullDocked.Init(true, NumSleds);

	FRackThermalModel FullModel;
	const FRackThermalSnapshot Full = RunToSteadyState(FullDocked, FullModel);

	// 유닛 1대를 뽑는다.
	TArray<bool> OneRemoved = FullDocked;
	const int32 RemovedSled = 5;
	OneRemoved[RemovedSled] = false;

	FRackThermalModel RemovedModel;
	const FRackThermalSnapshot Removed = RunToSteadyState(OneRemoved, RemovedModel);

	AddInfo(FString::Printf(TEXT("만재: 슬롯당 공기 %.5f kg/s (총 %.4f kg/s)"),
		Full.SlotAirMassFlowKgs[0], 1.2));
	AddInfo(FString::Printf(TEXT("유닛 %d 탈착: 빈 슬롯 %.5f kg/s / 남은 슬롯 %.5f kg/s"),
		RemovedSled, Removed.SlotAirMassFlowKgs[RemovedSled], Removed.SlotAirMassFlowKgs[0]));

	// 총 유량 보존.
	double FullSumKgs = 0.0;
	double RemovedSumKgs = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		FullSumKgs += Full.SlotAirMassFlowKgs[SledIndex];
		RemovedSumKgs += Removed.SlotAirMassFlowKgs[SledIndex];
	}
	const double NormalizedFlowError = FMath::Abs(RemovedSumKgs - FullSumKgs) / FullSumKgs;

	AddInfo(FString::Printf(TEXT("슬롯 유량 합: 만재 %.9f / 탈착 %.9f kg/s, 정규화 오차 %.3e"),
		FullSumKgs, RemovedSumKgs, NormalizedFlowError));
	TestTrue(TEXT("총 공기 유량이 보존돼야 한다"), NormalizedFlowError < 1.0e-12);

	// 빈 슬롯이 더 많이 가져간다.
	TestTrue(TEXT("빈 슬롯이 남은 유닛보다 공기를 더 가져가야 한다"),
		Removed.SlotAirMassFlowKgs[RemovedSled] > Removed.SlotAirMassFlowKgs[0]);

	// 남은 유닛은 유량이 줄어든다.
	TestTrue(TEXT("남은 유닛의 공기 유량이 줄어야 한다"),
		Removed.SlotAirMassFlowKgs[0] < Full.SlotAirMassFlowKgs[0]);

	// 남은 유닛의 공랭 부품 온도가 오른다. 액랭 부품은 영향이 거의 없어야 한다.
	const int32 RamIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Ram);
	const int32 VrmIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Vrm);
	const int32 GpuIndex = FRackThermalModel::NodeIndex(0, ERackComponent::Gpu);

	const double RamRiseK = Removed.ComponentTempK[RamIndex] - Full.ComponentTempK[RamIndex];
	const double VrmRiseK = Removed.ComponentTempK[VrmIndex] - Full.ComponentTempK[VrmIndex];
	const double GpuRiseK = Removed.ComponentTempK[GpuIndex] - Full.ComponentTempK[GpuIndex];

	AddInfo(FString::Printf(TEXT("남은 유닛 0: RAM %.3f -> %.3f C (+%.3f K) / VRM %.3f -> %.3f C (+%.3f K)"),
		Full.ComponentTempK[RamIndex] - 273.15, Removed.ComponentTempK[RamIndex] - 273.15, RamRiseK,
		Full.ComponentTempK[VrmIndex] - 273.15, Removed.ComponentTempK[VrmIndex] - 273.15, VrmRiseK));
	AddInfo(FString::Printf(TEXT("남은 유닛 0: GPU %.3f -> %.3f C (%+.3f K, 액랭이라 거의 영향 없음)"),
		Full.ComponentTempK[GpuIndex] - 273.15, Removed.ComponentTempK[GpuIndex] - 273.15, GpuRiseK));

	TestTrue(TEXT("남은 유닛의 RAM 온도가 올라야 한다"), RamRiseK > 0.0);
	TestTrue(TEXT("남은 유닛의 VRM 온도가 올라야 한다"), VrmRiseK > 0.0);
	TestTrue(TEXT("액랭 부품은 공기 유량 변화에 거의 영향받지 않아야 한다"), FMath::Abs(GpuRiseK) < 0.5);

	// 뽑은 유닛은 콜드플레이트도 강제 공랭도 끊긴다.
	const int32 RemovedGpuIndex = FRackThermalModel::NodeIndex(RemovedSled, ERackComponent::Gpu);
	TestEqual(TEXT("뽑은 유닛은 냉각수로 열을 내보내지 않아야 한다"),
		Removed.ComponentHeatToCoolantW[RemovedGpuIndex], 0.0);
	TestEqual(TEXT("뽑은 유닛은 강제 공랭으로 열을 내보내지 않아야 한다"),
		Removed.ComponentHeatToAirW[RemovedGpuIndex], 0.0);

	// 빈 슬롯의 배기는 입구 온도 그대로다 (바이패스).
	AddInfo(FString::Printf(TEXT("빈 슬롯 배기 %.4f C / 캐비닛 혼합 배기 %.4f C (만재 %.4f C)"),
		Removed.SledAirOutletTempK[RemovedSled] - 273.15,
		Removed.MixedAirOutletTempK - 273.15, Full.MixedAirOutletTempK - 273.15));
	TestEqual(TEXT("빈 슬롯 배기는 입구 온도와 같아야 한다"),
		Removed.SledAirOutletTempK[RemovedSled], 297.15);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
