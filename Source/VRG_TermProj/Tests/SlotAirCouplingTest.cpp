#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Thermal/RadiatorModel.h"

#if WITH_DEV_AUTOMATION_TESTS
// Per-server air coupling: every slot takes its inlet from the air at its own height and hands its
// air-side heat to the exhaust cells at its own height. Conservation must stay exact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSlotAirCouplingTest, "VRG.Coupling.SlotAirHeatAtSlotHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSlotAirCouplingTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 4, NY = 4, NZ = 10;
	constexpr double H = .25;
	FAirGridParams GridParams; GridParams.NumCellsX = NX; GridParams.NumCellsY = NY; GridParams.NumCellsZ = NZ; GridParams.CellSizeM = H;
	FAirSolver Air; Air.Initialize(GridParams, 297.15);
	FUniformAirGrid& Grid = Air.GetMutableGrid();
	for (int32 Z = 0; Z < NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
		Grid.TemperatureK[Grid.CellIndex(X, Y, Z)] = 295.0 + 2.0 * Z;   // warmer toward the ceiling

	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalModel Model; FRackThermalState Initial; Initial.CoolantTempK = 300.15;
	Model.Initialize(Params, Initial);
	FRackThermalInput Input; Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.Slots.Num()); Input.CoolantMassFlowKgs = 2.3;
	FRadiatorParams Radiator; Radiator.UAWK = 1.1e4;
	// Warm the parts up a little so every slot actually sheds heat to air.
	for (int32 I = 0; I < 2000; ++I) Model.Step(Input, .05);

	// Rack occupies X = 1..2; inlet column X = 3 (front), exhaust column X = 0 (rear), Y = 1..2, all heights.
	FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs = 2.3;
	TArray<double> InZ, OutZ;
	for (int32 Z = 0; Z < NZ; ++Z) for (int32 Y = 1; Y <= 2; ++Y)
	{
		Link.InletCellIndices.Add(Grid.CellIndex(3, Y, Z)); InZ.Add((Z + .5) * H);
		Link.ExhaustCellIndices.Add(Grid.CellIndex(0, Y, Z)); OutZ.Add((Z + .5) * H);
	}
	auto Lists = [&](const TArray<int32>& Cells, const TArray<double>& Zs, TArray<int32>& Start, TArray<int32>& Out)
	{
		for (const FRackSlot& Slot : Params.Slots)
		{
			Start.Add(Out.Num());
			for (int32 K = 0; K < Cells.Num(); ++K)
				if (Zs[K] + H / 2 > Slot.BottomZM && Zs[K] - H / 2 < Slot.BottomZM + Slot.HeightM) Out.Add(Cells[K]);
		}
		Start.Add(Out.Num());
	};
	Lists(Link.InletCellIndices, InZ, Link.SlotInletStart, Link.SlotInletCells);
	Lists(Link.ExhaustCellIndices, OutZ, Link.SlotExhaustStart, Link.SlotExhaustCells);

	FCouplingBridge Bridge;
	Bridge.Initialize({{&Model, &Input, &Radiator}}, {Link});
	Bridge.SetAirSolver(&Air);
	const double Dt = .02;
	Bridge.Step(Dt, false, true);

	// 1) Per-slot inlet follows the air at each device's height.
	bool bMonotone = Input.SlotAirInletTempK.Num() == Params.Slots.Num();
	for (int32 S = 1; bMonotone && S < Params.Slots.Num(); ++S)
		bMonotone &= Input.SlotAirInletTempK[S] >= Input.SlotAirInletTempK[S - 1] - 1e-12;
	AddInfo(FString::Printf(TEXT("슬롯 흡기: 가장 아래 %.2f C / 가장 위 %.2f C (랙 평균 %.2f C)"),
		Input.SlotAirInletTempK[0] - 273.15, Input.SlotAirInletTempK.Last() - 273.15, Input.AirInletTempK - 273.15));
	TestTrue(TEXT("Every slot has its own inlet, warmer higher up"), bMonotone && Input.SlotAirInletTempK.Last() > Input.SlotAirInletTempK[0] + 1.0);

	// 2) Heat handed to the grid equals the slots' air-side heat (no double counting, nothing lost).
	const FRackThermalSnapshot Snap = Model.MakeSnapshot(0.0);
	double SlotSumW = 0.0; for (const double W : Snap.SlotHeatToAirW) SlotSumW += W;
	double CellSumW = 0.0; TArray<double> LayerW; LayerW.SetNumZeroed(NZ);
	for (int32 Z = 0; Z < NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
	{
		const double W = Bridge.AirSolverInput.CellHeatW[Grid.CellIndex(X, Y, Z)];
		CellSumW += W; LayerW[Z] += W;
	}
	const double ConservationError = FMath::Abs(CellSumW - SlotSumW) / SlotSumW;
	const double AirInError = FMath::Abs(Air.GetEnergyInJ() - SlotSumW * Dt) / (SlotSumW * Dt);
	AddInfo(FString::Printf(TEXT("슬롯 공기 열 합 %.2f W / 격자에 넣은 합 %.2f W / 정규화 차이 %.3e / 공기 유입 장부 차이 %.3e"),
		SlotSumW, CellSumW, ConservationError, AirInError));
	TestTrue(TEXT("Grid receives exactly the slots' air heat"), ConservationError < 1e-12 && AirInError < 1e-12);

	// 3) Height distribution: nothing above the top device (1.81 m), most heat where the devices are.
	FString Layers;
	for (int32 Z = 0; Z < NZ; ++Z) Layers += FString::Printf(TEXT(" z%d=%.0fW"), Z, LayerW[Z]);
	AddInfo(TEXT("층별 주입:") + Layers);
	TestTrue(TEXT("No heat above the top device"), LayerW[8] == 0.0 && LayerW[9] == 0.0);
	TestTrue(TEXT("Heat is not uniform over height (not lumped)"), FMath::Max(LayerW) > 2.0 * LayerW[7]);
	return true;
}
#endif
