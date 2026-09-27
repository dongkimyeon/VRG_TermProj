// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/VrgParallel.h"

#if WITH_DEV_AUTOMATION_TESTS

// Multi-core solver loops (2026-09-27): the same run on one core and on many agrees within tolerance (numbers are
// compared with a tolerance, verification rule), and repeated parallel runs agree with each other (a race would
// make them drift). Neither is a proof of freedom from races; the disjoint-write construction of VrgParallel is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParallelSolverTest, "VRG.Air.Parallel.SerialParallelAgree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FParallelSolverTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Sim.Parallel"));
	if (!TestNotNull(TEXT("vrg.Sim.Parallel exists"), CVar)) return false;
	const int32 Saved = CVar->GetInt();

	// Large enough that the MG fine level and every row loop split into several blocks.
	constexpr int32 NX = 40, NY = 24, NZ = 12;
	constexpr double H = .25, Q = .6;
	auto Run = [&](bool bParallel, TArray<double>& OutT, TArray<double>& OutU, double& OutResidual)
	{
		CVar->Set(bParallel ? 1 : 0, ECVF_SetByCode);
		FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;
		FAirSolver Air; Air.Initialize(GP, 297.15);
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-8;
		FUniformAirGrid& G = Air.GetMutableGrid();
		// A body with a duct and a coil (as a CRAH), another heated wall of solids.
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 4; Y <= 7; ++Y) for (int32 X = 10; X <= 11; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 12; Y <= 20; ++Y) G.bSolidCell[G.CellIndex(25, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		FAirSolverInput In; In.CellHeatW.Init(0.0, G.GetNumCells());
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 12; Y <= 20; ++Y) In.CellHeatW[G.CellIndex(26, Y, Z)] = 60.0;
		In.NumDucts = 1; In.DuctCoilUAWK = {800.0}; In.DuctCoilWaterTempK = {291.15}; In.DuctFanHeatW = {300.0};
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 4; Y <= 7; ++Y)
		{
			const double V = Q / (36 * H * H);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(10, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(9, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(0); In.DuctFaceVelocityMs.Add(V);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(12, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(12, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(1); In.DuctFaceVelocityMs.Add(V);
		}
		for (int32 S = 0; S < 150; ++S) Air.Step(In, .02);
		OutT = Air.GetGrid().TemperatureK;
		OutU = Air.GetGrid().VelocityXMs;
		OutResidual = Air.GetNormalizedEnergyResidual();
	};
	auto MaxDiff = [](const TArray<double>& A, const TArray<double>& B)
	{
		double M = 0.0;
		for (int32 I = 0; I < A.Num(); ++I) M = FMath::Max(M, FMath::Abs(A[I] - B[I]));
		return M;
	};

	TArray<double> T0, U0, T1, U1, T2, U2; double R0 = 0, R1 = 0, R2 = 0;
	Run(false, T0, U0, R0);
	Run(true, T1, U1, R1);
	Run(true, T2, U2, R2);
	CVar->Set(Saved, ECVF_SetByCode);

	const double DT = MaxDiff(T0, T1), DU = MaxDiff(U0, U1), DT2 = MaxDiff(T1, T2), DU2 = MaxDiff(U1, U2);
	AddInfo(FString::Printf(TEXT("직렬 대 병렬: 온도 최대 차 %.3e K, 속도 %.3e m/s | 병렬 두 번: %.3e K, %.3e m/s | 열수지 %.3e / %.3e / %.3e | 셀 %d"),
		DT, DU, DT2, DU2, R0, R1, R2, NX * NY * NZ));
	TestTrue(TEXT("Serial and parallel agree (temperature)"), DT < 1e-9);
	TestTrue(TEXT("Serial and parallel agree (velocity)"), DU < 1e-9);
	TestTrue(TEXT("Repeated parallel runs agree"), DT2 < 1e-12 && DU2 < 1e-12);
	TestTrue(TEXT("Ledger closes in every run"), FMath::Abs(R0) < 1e-12 && FMath::Abs(R1) < 1e-12 && FMath::Abs(R2) < 1e-12);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
