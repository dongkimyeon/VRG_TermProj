// Fill out your copyright notice in the Description page of Project Settings.

// 2026-11-03 (Tools/실사양정합_계획.md P4): the GB200 NVL72 power shelves feed one shared rack busbar (NVIDIA DGX GB200
// user guide: eight 33 kW shelves, N+N; Flex / Vertiv / HPE shelf documents). A shelf going off cuts no tray: the live
// shelves carry the same DC load and share the whole conversion loss. Only when the live capacity falls below the load
// (or no shelf is live) does the busbar trip and the whole rack lose power (the trip rule is an ESTIMATE).

#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackBusbarTest, "VRG.Thermal.Power.BusbarSharedShelves", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackBusbarTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	const int32 Count = Params.Slots.Num();
	TArray<int32> Shelves;
	for (int32 S = 0; S < Count; ++S) if (Params.Slots[S].DeviceType == FRackThermalModel::Gb200DevPowerShelf) Shelves.Add(S);
	FRackThermalInput In;
	In.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	In.SledDocked.Init(true, Count);
	In.SlotPowered.Init(true, Count);
	FRackThermalState Initial;
	Initial.CoolantTempK = In.CoolantInletTempK;
	Initial.ComponentTempK.Init(In.CoolantInletTempK, FRackThermalModel::CountNodes(Params));

	FRackThermalModel All;
	All.Initialize(Params, Initial);
	All.Step(In, .02); All.Step(In, .02);
	const double DcW = All.GetLastDcLoadW(), LossW = All.GetLastConversionLossW();
	TestTrue(TEXT("eight shelves, 264 kW, not tripped"), Shelves.Num() == 8 && All.GetLiveShelves() == 8 && FMath::IsNearlyEqual(All.GetBusbarCapacityW(), 264000.0) && !All.IsBusbarTripped());

	double WorstDcRel = 0.0, WorstLossRel = 0.0;
	bool bNoneTripped = true, bOffShelfCold = true;
	for (int32 Off = 1; Off <= 4; ++Off)
	{
		FRackThermalInput I = In;
		for (int32 K = 0; K < Off; ++K) I.SlotPowered[Shelves[K]] = false;
		FRackThermalModel M;
		M.Initialize(Params, Initial);
		M.Step(I, .02); M.Step(I, .02);
		WorstDcRel = FMath::Max(WorstDcRel, FMath::Abs(M.GetLastDcLoadW() - DcW) / DcW);
		WorstLossRel = FMath::Max(WorstLossRel, FMath::Abs(M.GetLastConversionLossW() - LossW) / LossW);
		bNoneTripped &= !M.IsBusbarTripped() && M.GetLiveShelves() == 8 - Off;
		const FRackThermalSnapshot S = M.MakeSnapshot(.04);
		for (int32 N = M.GetSlotNodeStart(Shelves[0]); N < M.GetSlotNodeStart(Shelves[0]) + M.GetSlotNodeCount(Shelves[0]); ++N) bOffShelfCold &= S.ComponentHeatW[N] == 0.0;
	}
	AddInfo(FString::Printf(TEXT("DC 부하 %.1f kW, 변환 손실 %.1f W | 셸프 1~4개 꺼짐: DC 부하 최대 상대 차 %.2e, 손실 최대 상대 차 %.2e, 차단 없음 %d"),
		DcW * 1e-3, LossW, WorstDcRel, WorstLossRel, bNoneTripped ? 1 : 0));
	TestTrue(TEXT("N+N: up to four shelves off cut no tray; same DC load and the same total loss on the live shelves (relative < 1e-12)"),
		bNoneTripped && WorstDcRel < 1e-12 && WorstLossRel < 1e-12 && bOffShelfCold);

	// Five shelves off: 3 x 33 = 99 kW < the DC load -> the busbar trips on the step after the overload is seen.
	FRackThermalInput Five = In;
	for (int32 K = 0; K < 5; ++K) Five.SlotPowered[Shelves[K]] = false;
	FRackThermalModel M;
	M.Initialize(Params, Initial);
	M.Step(In, .02);
	M.Step(Five, .02);
	const bool bTripped = M.IsBusbarTripped();
	M.Step(Five, .02);
	const double HeatAfter = M.GetLastTotalHeatW(), FanAfter = M.GetLastTotalFanPowerW();
	// Latched: putting the shelves back does not re-energize the rack until a new run (Initialize).
	M.Step(In, .02);
	const bool bLatched = M.IsBusbarTripped() && M.GetLastTotalHeatW() == 0.0;
	M.Initialize(Params, Initial);
	M.Step(In, .02);
	const double Norm = FMath::Max(1.0, FMath::Abs(M.GetEnergyInJ()));
	AddInfo(FString::Printf(TEXT("셸프 5개 꺼짐: 용량 %.0f kW < 부하 %.1f kW -> 차단 %d, 차단 뒤 전기 열 %.3f W, 팬 %.3f W, 유지 %d, 새 실행 뒤 차단 %d"),
		99.0, DcW * 1e-3, bTripped ? 1 : 0, HeatAfter, FanAfter, bLatched ? 1 : 0, M.IsBusbarTripped() ? 1 : 0));
	TestTrue(TEXT("overload trips the busbar: no electrical heat, no fans, latched; a new run resets it"), bTripped && HeatAfter == 0.0 && FanAfter == 0.0 && bLatched && !M.IsBusbarTripped());
	TestTrue(TEXT("rack energy ledger finite"), FMath::IsFinite(M.GetEnergyInJ() / Norm));
	return true;
}

#endif
