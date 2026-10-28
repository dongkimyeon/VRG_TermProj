#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/TrayCoolantModel.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Hydraulics/InRackNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgTrayCoolantMeasurement, "VRG.Water.RackInterior.ThermalMeasurement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FVrgTrayCoolantMeasurement::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalInput RackInput; RackInput.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	const FInRackSpec Spec = InRackNetwork::FromThermal(Params, RackInput);
	FTrayCoolantModel Model;
	TestTrue(TEXT("초기화"), Model.Initialize(Spec.DesignFraction, 92000.0, 300.15));
	double SumCapacity = Model.GetReturnCapacityJK();
	for (double C : Model.GetTrayCapacityJK()) SumCapacity += C;
	AddInfo(FString::Printf(TEXT("열용량 합 %.12g J/K"), SumCapacity));
	TestTrue(TEXT("총 열용량 유지"), FMath::Abs(SumCapacity - 92000.0) <= 1e-8);
	FTrayCoolantInput Input;
	Input.SupplyTempK = Input.ReturnBoundaryTempK = 300.15;
	Input.LeakMassFlowKgs.Init(0.0, 27); Input.PartsConductanceWK.Init(0.0, 27);
	const double DesignHeatW = FRackThermalModel::DesignHeatToCoolantW(Params, RackInput);
	for (double F : Spec.DesignFraction)
	{
		Input.InletMassFlowKgs.Add(0.0023 * 997.0 * F);
		Input.OutletMassFlowKgs.Add(Input.InletMassFlowKgs.Last());
		Input.HeatFromPartsW.Add(DesignHeatW * F);
	}
	Input.ReturnMassFlowKgs = 0.0023 * 997.0;
	bool Stable = true;
	for (int32 S = 0; S < 20000; ++S) Stable &= Model.Step(Input, 0.02);
	TestTrue(TEXT("400 s 안정"), Stable);
	double MinDelta = 1e30, MaxDelta = -1e30;
	for (double T : Model.GetTrayTempK()) { MinDelta = FMath::Min(MinDelta, T - Input.SupplyTempK); MaxDelta = FMath::Max(MaxDelta, T - Input.SupplyTempK); }
	TestTrue(TEXT("트레이 설계 ΔT 5~15 K 타당성"), MinDelta > 5.0 && MaxDelta < 15.0);
	AddInfo(FString::Printf(TEXT("열 정방향: ΔT %.12g~%.12g K, 잔차 %.12g J, 입력 정규화 %.12g, 질량 %.12g kg/s"),
		MinDelta, MaxDelta, Model.GetResidualJ(), Model.GetResidualJ() / Model.GetHeatInJ(), Model.GetLastMassResidualKgs()));
	// 최초 측정 정방향 1.314e-12, 역류 1.781e-12(누적 부품 열 입력 J 기준) 뒤 정한 여유.
	TestTrue(TEXT("정방향 열수지 <= 1e-10"), FMath::Abs(Model.GetResidualJ()) / Model.GetHeatInJ() <= 1e-10);
	TestTrue(TEXT("정방향 질량 잔차 <= 1e-12 kg/s"), Model.GetLastMassResidualKgs() <= 1e-12);
	// 하나의 트레이는 회수에서 역류해 중간 누수로 나간다. 두 입구의 수온을 함께 사용해야 한다.
	Input.OutletMassFlowKgs[13] = -0.01;
	Input.LeakMassFlowKgs[13] = Input.InletMassFlowKgs[13] + 0.01;
	Input.ReturnMassFlowKgs = 0.0;
	for (double F : Input.OutletMassFlowKgs) Input.ReturnMassFlowKgs += F;
	for (int32 S = 0; S < 5000; ++S) Stable &= Model.Step(Input, 0.02);
	TestTrue(TEXT("역류 안정"), Stable);
	AddInfo(FString::Printf(TEXT("열 역류: 잔차 %.12g J, 입력 정규화 %.12g, 질량 %.12g kg/s, 누수 열 %.12g W"),
		Model.GetResidualJ(), Model.GetResidualJ() / Model.GetHeatInJ(), Model.GetLastMassResidualKgs(), Model.GetLastLeakHeatW()));
	TestTrue(TEXT("역류 열수지 <= 1e-10"), FMath::Abs(Model.GetResidualJ()) / Model.GetHeatInJ() <= 1e-10);
	TestTrue(TEXT("역류 질량 잔차 <= 1e-12 kg/s"), Model.GetLastMassResidualKgs() <= 1e-12);
	TestTrue(TEXT("누수 열 유출"), Model.GetLastLeakHeatW() > 0.0);
	const TArray<double> Before = Model.GetTrayTempK();
	Input.InletMassFlowKgs.Init(0.0, 27); Input.OutletMassFlowKgs.Init(0.0, 27); Input.LeakMassFlowKgs.Init(0.0, 27); Input.ReturnMassFlowKgs = 0.0;
	TestTrue(TEXT("유량 0 안정"), Model.Step(Input, 0.02));
	double WorstZeroErrorK = 0.0;
	for (int32 I = 0; I < 27; ++I)
	{
		const double ExpectedK = Before[I] + 0.02 * Input.HeatFromPartsW[I] / Model.GetTrayCapacityJK()[I];
		WorstZeroErrorK = FMath::Max(WorstZeroErrorK, FMath::Abs(Model.GetTrayTempK()[I] - ExpectedK));
		TestTrue(TEXT("냉각 상실 시 축적"), Model.GetTrayTempK()[I] > Before[I]);
	}
	AddInfo(FString::Printf(TEXT("유량 0 해석해 차 %.12g K"), WorstZeroErrorK));
	TestTrue(TEXT("유량 0 해석해 <= 1e-10 K"), WorstZeroErrorK <= 1e-10);
	const double EnergyBefore = Model.GetHeatInJ();
	const TArray<double> StableState = Model.GetTrayTempK();
	Input.PartsConductanceWK.Init(1e12, 27);
	TestFalse(TEXT("불안정 스텝 거절"), Model.Step(Input, 1.0));
	TestTrue(TEXT("거절 상태 보존"), StableState == Model.GetTrayTempK());
	TestEqual(TEXT("거절 장부 보존"), Model.GetHeatInJ(), EnergyBefore);
	return true;
}
#endif
