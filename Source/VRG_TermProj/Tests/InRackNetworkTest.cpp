#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRackNetwork.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Water/RackInteriorWater.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgInRackMeasurement, "VRG.Hydraulics.InRack.Measurement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FVrgInRackMeasurement::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalInput Input; Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	FInRackSpec Spec = InRackNetwork::FromThermal(Params, Input);
	TestEqual(TEXT("액랭 트레이 27개"), Spec.Slots.Num(), 27);
	FHydraulicNetwork Network;
	FHydraulicNode Supply; Supply.bFixedPressure = true; Supply.PressurePa = 4.0e5;
	FHydraulicNode Return = Supply; Return.PressurePa = Supply.PressurePa - 0.98 * Spec.EquivalentResistancePaS2M6 * FMath::Square(Spec.DesignFlowM3s);
	const int32 A = Network.AddNode(Supply), B = Network.AddNode(Return);
	const FInRackHandles H = InRackNetwork::Add(Network, Spec, A, B);
	TestEqual(TEXT("지점 135개"), H.Leaks.Num(), 135);
	const auto Baseline = Network.Solve(FHydraulicSolveSettings());
	TestTrue(TEXT("닫힌 망 수렴"), Baseline.bConverged);
	double BaselineSum = 0.0;
	for (const int32 Pipe : H.InletPipes) BaselineSum += Network.GetPipeFlowM3s(Pipe);
	AddInfo(FString::Printf(TEXT("닫힌 유량 %.12g m3/s, 설계 %.12g, 차 %.12g, 노드 잔차 %.12g m3/s"),
		BaselineSum, Spec.DesignFlowM3s, BaselineSum - Spec.DesignFlowM3s, H.MaxBalanceErrorM3s(Network)));
	const int32 Tray = 13;
	const double Before = Network.GetPipeFlowM3s(H.InletPipes[Tray]);
	const int32 NodesBefore = Network.GetNumNodes(), PipesBefore = Network.GetNumPipes();
	Network.SetLeakHoleDiameterM(H.Leaks[Tray * 5 + 2], 0.003);
	TestTrue(TEXT("플레이트 누수 망 수렴"), Network.Solve(FHydraulicSolveSettings()).bConverged);
	const double Inflow = Network.GetPipeFlowM3s(H.InletPipes[Tray]);
	const double Outflow = Network.GetPipeFlowM3s(H.OutletPipes[Tray]);
	const double Leaked = Network.GetLeakFlowM3s(H.Leaks[Tray * 5 + 2]);
	AddInfo(FString::Printf(TEXT("트레이 %d 누수 전 %.12g, 입구 %.12g, 출구 %.12g, 누수 %.12g m3/s, 잔차 %.12g m3/s"),
		Tray, Before, Inflow, Outflow, Leaked, H.MaxBalanceErrorM3s(Network)));
	TestEqual(TEXT("노드 수 불변"), Network.GetNumNodes(), NodesBefore);
	TestEqual(TEXT("배관 수 불변"), Network.GetNumPipes(), PipesBefore);
	TestTrue(TEXT("하류 유량 감소"), Outflow < Before);
	H.SetDocked(Network, Tray, false);
	TestTrue(TEXT("탈착 수렴"), Network.Solve(FHydraulicSolveSettings()).bConverged);
	TestEqual(TEXT("탈착 입구 차단"), Network.GetPipeFlowM3s(H.InletPipes[Tray]), 0.0);
	TestEqual(TEXT("탈착 출구 차단"), Network.GetPipeFlowM3s(H.OutletPipes[Tray]), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgRackInteriorWaterMeasurement, "VRG.Water.RackInterior.Measurement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FVrgRackInteriorWaterMeasurement::RunTest(const FString& Parameters)
{
	FRackInteriorWater A, B;
	A.Initialize({5}, {0.1, 0.2, 0.3, 0.4, 0.5}); B = A;
	A.AddWater(0, 3, 0.002, false, 0.0); B.AddWater(0, 3, 0.002, false, 0.0);
	for (int32 S = 0; S < 1000; ++S) { A.Step(0.02, (S + 1) * 0.02); B.Step(0.02, (S + 1) * 0.02); }
	TestEqual(TEXT("위 슬롯 마름"), A.FirstWetS[4], -1.0);
	TestTrue(TEXT("바닥 도착"), A.FloorM3[0] > 0.0);
	TestTrue(TEXT("결정성 보유량"), A.HeldM3 == B.HeldM3);
	TestTrue(TEXT("결정성 바닥"), A.FloorM3 == B.FloorM3);
	AddInfo(FString::Printf(TEXT("슬롯 물: 주입 %.12g, 바닥 %.12g, 보존 잔차 %.12g m3"), A.ReceivedM3, A.FloorM3[0], A.BalanceErrorM3()));
	A.AddWater(0, 3, 0.001, true, 20.0);
	for (int32 S = 0; S < 1000; ++S) A.Step(0.02, 20.0 + (S + 1) * 0.02);
	TestTrue(TEXT("뒤쪽 노출 기록"), A.RearExposureM3[0] > 0.0);
	TestEqual(TEXT("뒤쪽 누수 위 슬롯 마름"), A.FirstWetS[4], -1.0);
	AddInfo(FString::Printf(TEXT("뒤쪽 물: 주입 %.12g, 뒤 바닥 %.12g, 보존 잔차 %.12g m3"), A.ReceivedM3, A.RearFloorM3[0], A.BalanceErrorM3()));
	return true;
}
#endif
