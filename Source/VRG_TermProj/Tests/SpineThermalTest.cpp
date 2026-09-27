#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Coupling/CouplingBridge.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpineThermalTest, "VRG.Coupling.SpineAirOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSpineThermalTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams Params = FRackThermalModel::MakeQuantumSpineParams({1.1, 1.14445, 1.1889, 1.23335});
	FRackThermalInput Input;
	Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	Input.CoolantMassFlowKgs = 0.0;
	FRackThermalModel Model; Model.Initialize(Params, FRackThermalState());
	FAirGridParams GP; GP.NumCellsX = 2; GP.NumCellsY = 2; GP.NumCellsZ = 2; GP.CellSizeM = 1.0;
	FAirSolver Air; Air.Initialize(GP, 297.15);
	FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs = 0.0;
	Link.InletCellIndices = {0, 1, 2, 3}; Link.ExhaustCellIndices = {4, 5, 6, 7};
	FCouplingBridge Bridge; Bridge.Initialize({{&Model, &Input, nullptr}}, {Link}); Bridge.SetAirSolver(&Air);
	Bridge.AirSolverInput.CellCoolingUAWK.Init(4 * 747.0 / (10.0 * 8), 8);
	Bridge.AirSolverInput.CoolingSupplyTempK = 291.15;
	for (int32 I = 0; I < 20000; ++I) Bridge.Step(.02, false, true);
	const auto Snap = Bridge.MakeSnapshot(400.0);
	const auto Thermal = Model.MakeSnapshot(400.0);
	double MaxC = -273.15; for (double T : Thermal.ComponentTempK) MaxC = FMath::Max(MaxC, T - 273.15);
	AddInfo(FString::Printf(TEXT("Spine: IT %.6f W, CRAH %.6f W, residual %.12e (energy-in J normalization), max part %.3f C, inlet %.3f C"),
		Snap.TotalItHeatW, Snap.TotalAirCoolingRemovedW, Snap.NormalizedEnergyResidual, MaxC, Input.AirInletTempK - 273.15));
	// Fan parts carry the computed fan input (affinity law at the controlled speed), the rest is the preset.
	const double FanW = Thermal.TotalFanPowerW;
	AddInfo(FString::Printf(TEXT("Spine fans: %.3f W total, speed %.3f"), FanW, Thermal.SlotFanSpeedRatio.Num() ? Thermal.SlotFanSpeedRatio[0] : -1.0));
	TestTrue(TEXT("IT = 4 x 597 W electronics + computed fan input"), FMath::Abs(Snap.TotalItHeatW - (4 * 597.0 + FanW)) < 1e-9 * Snap.TotalItHeatW);
	TestTrue(TEXT("Below the 4 x 747 W QM9700 typical reference (fans included there)"), Snap.TotalItHeatW <= 2988.0 && FanW > 0.0);
	TestEqual(TEXT("No hydraulic flow"), Input.CoolantMassFlowKgs, 0.0);
	TestEqual(TEXT("No radiator heat rejection"), Snap.TotalRadiatorRejectedW, 0.0);
	TestEqual(TEXT("No liquid slots"), FRackThermalModel::CountLiquidSlots(Params), 0);
	// Measured 1.50e-13 after 400 s; margin accounts for summation/platform rounding.
	TestTrue(TEXT("Global energy closure / cumulative IT input [dimensionless]"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-11);
	TestTrue(TEXT("CRAH approaches the 2.988 kW load after warmup"), Snap.TotalAirCoolingRemovedW > .98 * Snap.TotalItHeatW && Snap.TotalAirCoolingRemovedW <= Snap.TotalItHeatW);
	TestTrue(TEXT("Finite ledger and physically plausible component temperature"), FMath::IsFinite(Snap.NormalizedEnergyResidual) && MaxC > 25 && MaxC < 100);
	return true;
}
#endif
