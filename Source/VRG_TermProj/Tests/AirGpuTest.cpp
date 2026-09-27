// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "VrgAirGpu.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/MultigridPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Simulation/Air/AirSolver.h"
#include <cmath>

#if WITH_DEV_AUTOMATION_TESTS

// GPU tests (VRG.Gpu.*) need a real RHI. The regular suite runs with -NullRHI, where they note the skip and pass;
// run them separately without -NullRHI (command in CLAUDE.md).
namespace
{
	constexpr auto AirGpuTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams GpuGridParams(int32 X, int32 Y, int32 Z, double H)
	{
		FAirGridParams P; P.NumCellsX = X; P.NumCellsY = Y; P.NumCellsZ = Z; P.CellSizeM = H; return P;
	}

	// Two rows of rack-like solid boxes with gaps (as VRG.Air.Poisson.MultigridSolidsAndScaling).
	void GpuAddRackRows(FUniformAirGrid& Grid)
	{
		const int32 NX = Grid.GetNumCellsX(), NY = Grid.GetNumCellsY(), NZ = Grid.GetNumCellsZ();
		for (int32 Z = 0; Z < NZ * 3 / 5; ++Z)
			for (int32 Y = NY / 6; Y < NY * 5 / 6; ++Y)
				for (int32 X = 0; X < NX; ++X)
				{
					const bool bRowA = X >= NX / 5 && X < NX / 5 + 3, bRowB = X >= NX * 4 / 5 - 3 && X < NX * 4 / 5;
					if ((bRowA || bRowB) && (Y % 4) != 3) Grid.bSolidCell[Grid.CellIndex(X, Y, Z)] = true;
				}
	}

	// B = L(Ref) with the S3 operator: always compatible.
	void GpuManufacture(const FUniformAirGrid& Grid, TArray<double>& Ref, TArray<double>& B)
	{
		Ref.Init(0.0, Grid.GetNumCells()); B.Init(0.0, Grid.GetNumCells());
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) Ref[Grid.CellIndex(X, Y, Z)] = FMath::Sin(.37 * X + .11 * Y) * FMath::Cos(.23 * Z - .5 * Y) + .01 * X * Z;
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) B[Grid.CellIndex(X, Y, Z)] = JacobiPoissonSolver::LaplacianAt(Grid, Ref, X, Y, Z);
	}

	bool SkipWithoutGpu(FAutomationTestBase& Test)
	{
		if (VrgAirGpu::IsAvailable()) return false;
		Test.AddInfo(TEXT("GPU 없음(NullRHI 등) - 건너뜀. GPU 테스트는 -NullRHI 없이 따로 돌린다."));
		return true;
	}
}

// 4a (2026-09-27): FP64 arithmetic in a compute shader gives the CPU's answer (see the fma note below).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirGpuDoubleProbeTest, "VRG.Gpu.Air.DoubleArithmeticMatchesCpu", AirGpuTestFlags)
bool FAirGpuDoubleProbeTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 N = 4096;
	constexpr double C = 297.15, D = 3.0000000001;
	TArray<double> A, B, Out;
	FRandomStream Rng(1234);
	for (int32 I = 0; I < N; ++I)
	{
		A.Add(Rng.FRandRange(-1e3, 1e3) + Rng.FRand() * 1e-9);
		B.Add(Rng.FRandRange(-2.0, 2.0) + Rng.FRand() * 1e-12);
	}
	FString Error;
	if (!TestTrue(TEXT("Probe ran"), VrgAirGpu::ProbeDoubleArithmetic(A, B, C, D, Out, Error)))
	{
		AddError(Error);
		return false;
	}
	// The shader compiler contracts A*B + C into one fused multiply-add (one rounding instead of two). With the CPU doing
	// the same (std::fma) every value must be identical: that shows FP64 add / mul / div are IEEE on the GPU and the only
	// difference from plain C++ is that contraction - a rounding-level difference, not a precision loss.
	double MaxDiff = 0.0;
	int32 NumDifferent = 0, NumDifferentFma = 0;
	for (int32 I = 0; I < N; ++I)
	{
		const double Quotient = (A[I] * B[I] + C) / D;
		const double QuotientFma = std::fma(A[I], B[I], C) / D;
		const double Diff = FMath::Abs(Out[I] - (Quotient - double(int32(Quotient))));
		MaxDiff = FMath::Max(MaxDiff, Diff);
		NumDifferent += Diff != 0.0;
		NumDifferentFma += Out[I] != QuotientFma - double(int32(QuotientFma));
	}
	AddInfo(FString::Printf(TEXT("FP64 셰이더 대 CPU: %d개 중 다른 값 %d개(최대 %.3e, 몫 크기 약 700) | CPU도 fma로 계산하면 다른 값 %d개"),
		N, NumDifferent, MaxDiff, NumDifferentFma));
	TestEqual(TEXT("Identical to the CPU computing with fma"), NumDifferentFma, 0);
	TestTrue(TEXT("Plain CPU differs only at rounding level"), MaxDiff < 1e-12);
	return true;
}

// 4d (2026-09-27): GPU MG-PCG (FP64) against the CPU MG-PCG on the same hierarchy and input. Not bit identity (the GPU
// sums in a different order and fuses multiply-adds): the same iteration count within one, both below the target, and the
// solutions close relative to their size. Also the D5 flags: zero / incompatible right-hand side, iteration cap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirGpuPressureTest, "VRG.Gpu.Air.PressureMatchesCpu", AirGpuTestFlags)
bool FAirGpuPressureTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	struct FCase { int32 Scale; double Target; };
	for (const FCase Case : { FCase{ 1, 1e-10 }, FCase{ 2, 1e-10 }, FCase{ 2, 1e-6 } })
	{
		FUniformAirGrid Grid; Grid.Initialize(GpuGridParams(20 * Case.Scale, 30 * Case.Scale, 10 * Case.Scale, .25 / Case.Scale));
		GpuAddRackRows(Grid);
		TArray<double> Ref, B; GpuManufacture(Grid, Ref, B);
		FPoissonSolveSettings S; S.NormalizedResidualTarget = Case.Target; S.MaxIterations = 100;
		MultigridPoissonSolver::FUniformCache Cache;
		TArray<double> XCpu;
		const FPoissonSolveResult RCpu = MultigridPoissonSolver::Solve(Grid, B, XCpu, Cache, S);
		FVrgMgHierarchy H; MultigridPoissonSolver::ExportHierarchy(Grid, Cache, H);
		FVrgAirGpuPressure Gpu;
		if (!TestTrue(TEXT("GPU hierarchy configured"), Gpu.Configure(H))) return false;
		FVrgGpuPoissonSettings G; G.NormalizedResidualTarget = Case.Target; G.MaxIterations = 100; G.Omega = S.Relaxation;
		TArray<double> XGpu; XGpu.Init(0.0, Grid.GetNumCells());
		const FVrgGpuPoissonResult RGpu = Gpu.Solve(B, XGpu, G);
		double Diff = 0.0, Mx = 0.0;
		for (int32 I = 0; I < XCpu.Num(); ++I)
			if (!Grid.bSolidCell[I]) { Diff = FMath::Max(Diff, FMath::Abs(XGpu[I] - XCpu[I])); Mx = FMath::Max(Mx, FMath::Abs(XCpu[I])); }
		const double ResidualS3 = JacobiPoissonSolver::MaxAbsResidual(Grid, XGpu, B) / FMath::Max(RGpu.ReferenceScale, 1e-300);
		AddInfo(FString::Printf(TEXT("%dx%dx%d 유체 %d셀 %d층 목표 %.0e | CPU %d회 잔차 %.3e | GPU %d회 잔차 %.3e (제출 %d번, S3 연산자로 잰 잔차 %.3e) | 해 차 %.3e (CPU 해 최대 %.4f 기준)"),
			Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ(), H.Levels[0].NumCells, H.Levels.Num(), Case.Target,
			RCpu.IterationsUsed, RCpu.NormalizedResidual, RGpu.IterationsUsed, RGpu.NormalizedResidual, RGpu.Submissions, ResidualS3, Diff / Mx, Mx));
		TestTrue(TEXT("GPU ran"), RGpu.bRan);
		TestTrue(TEXT("Both converge"), RCpu.bConverged && RGpu.bConverged);
		TestTrue(TEXT("Same iteration count within one"), FMath::Abs(RCpu.IterationsUsed - RGpu.IterationsUsed) <= 1);
		TestTrue(TEXT("GPU residual below the target, also by the S3 operator"), RGpu.NormalizedResidual <= Case.Target && ResidualS3 <= 10.0 * Case.Target);
		TestTrue(TEXT("Solutions agree"), Diff / Mx < 1e3 * Case.Target);
	}

	// D5 flags on a small grid.
	FUniformAirGrid Grid; Grid.Initialize(GpuGridParams(20, 30, 10, .25));
	GpuAddRackRows(Grid);
	TArray<double> Ref, B; GpuManufacture(Grid, Ref, B);
	MultigridPoissonSolver::FUniformCache Cache;
	MultigridPoissonSolver::UpdateCache(Grid, Cache);
	FVrgMgHierarchy H; MultigridPoissonSolver::ExportHierarchy(Grid, Cache, H);
	FVrgAirGpuPressure Gpu;
	Gpu.Configure(H);
	FVrgGpuPoissonSettings G; G.NormalizedResidualTarget = 1e-10;
	TArray<double> X; X.Init(1.0, Grid.GetNumCells());
	TArray<double> Zero; Zero.Init(0.0, Grid.GetNumCells());
	const FVrgGpuPoissonResult RZero = Gpu.Solve(Zero, X, G);
	double MaxX = 0.0; for (int32 I = 0; I < X.Num(); ++I) if (!Grid.bSolidCell[I]) MaxX = FMath::Max(MaxX, FMath::Abs(X[I]));
	TestTrue(TEXT("Zero right-hand side: converged, field zero"), RZero.bRan && RZero.bConverged && MaxX == 0.0);
	TArray<double> Bad = B; for (int32 I = 0; I < Bad.Num(); ++I) if (!Grid.bSolidCell[I]) Bad[I] += 1.0;
	X.Init(0.0, Grid.GetNumCells());
	const FVrgGpuPoissonResult RBad = Gpu.Solve(Bad, X, G);
	TestTrue(TEXT("Incompatible right-hand side flagged, not converged"), RBad.bRan && RBad.bIncompatibleRightHandSide && !RBad.bConverged);
	G.MaxIterations = 2;
	const FVrgGpuPoissonResult RCap = Gpu.Solve(B, X, G);
	TestTrue(TEXT("Iteration cap reported as failure"), RCap.bRan && !RCap.bConverged && RCap.IterationsUsed == 2);
	AddInfo(FString::Printf(TEXT("D5: 영 우변 수렴 %d / 부적합 %d (잔차 %.3e) / 상한 2회 수렴 %d, %d회"),
		RZero.bConverged, RBad.bIncompatibleRightHandSide, RBad.NormalizedResidual, RCap.bConverged, RCap.IterationsUsed));
	return true;
}

// 4d: wall time of one pressure solve at the 0.125 m L_Test size (measurement only, D10: not a guarantee).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirGpuPressureTimingTest, "VRG.Gpu.Air.PressureTiming", AirGpuTestFlags)
bool FAirGpuPressureTimingTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	FUniformAirGrid Grid; Grid.Initialize(GpuGridParams(108, 82, 30, .125));
	GpuAddRackRows(Grid);
	TArray<double> Ref, B; GpuManufacture(Grid, Ref, B);
	FPoissonSolveSettings S; S.NormalizedResidualTarget = 1e-6; S.MaxIterations = 100;
	MultigridPoissonSolver::FUniformCache Cache;
	MultigridPoissonSolver::UpdateCache(Grid, Cache);
	FVrgMgHierarchy H; MultigridPoissonSolver::ExportHierarchy(Grid, Cache, H);
	FVrgAirGpuPressure Gpu;
	if (!TestTrue(TEXT("Configured"), Gpu.Configure(H))) return false;
	FVrgGpuPoissonSettings G; G.NormalizedResidualTarget = 1e-6; G.MaxIterations = 100; G.Omega = S.Relaxation;
	double CpuMs = 0.0, GpuMs = 0.0;
	FPoissonSolveResult RCpu; FVrgGpuPoissonResult RGpu;
	for (int32 Rep = 0; Rep < 3; ++Rep)  // the first round pays warm-up; keep the best
	{
		TArray<double> XCpu, XGpu; XCpu.Init(0.0, Grid.GetNumCells()); XGpu.Init(0.0, Grid.GetNumCells());
		const double T0 = FPlatformTime::Seconds();
		RCpu = MultigridPoissonSolver::Solve(Grid, B, XCpu, Cache, S);
		const double T1 = FPlatformTime::Seconds();
		RGpu = Gpu.Solve(B, XGpu, G);
		const double T2 = FPlatformTime::Seconds();
		CpuMs = Rep == 0 ? (T1 - T0) * 1e3 : FMath::Min(CpuMs, (T1 - T0) * 1e3);
		GpuMs = Rep == 0 ? (T2 - T1) * 1e3 : FMath::Min(GpuMs, (T2 - T1) * 1e3);
	}
	AddInfo(FString::Printf(TEXT("108x82x30 = %d셀(유체 %d, %d층), 0에서 시작, 목표 1e-6 | CPU %d회 %.2f ms | GPU %d회 %.2f ms (제출 %d번) | 측정값, 보장 아님"),
		Grid.GetNumCells(), H.Levels[0].NumCells, H.Levels.Num(), RCpu.IterationsUsed, CpuMs, RGpu.IterationsUsed, GpuMs, RGpu.Submissions));
	TestTrue(TEXT("Both converge"), RCpu.bConverged && RGpu.bConverged);
	return true;
}

// 4b/4c (2026-09-27): the whole step on the GPU against the CPU step, same input. Solids, a fan duct through a body with a
// coil and fan heat, heat sources, cell coils, buoyancy; turbulence off and on. Rounding-level differences grow through a
// buoyant flow, so the comparison is a tolerance measured here; the GPU run's own ledger must close like the CPU's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirGpuStepTest, "VRG.Gpu.Air.StepMatchesCpu", AirGpuTestFlags)
bool FAirGpuStepTest::RunTest(const FString& Parameters)
{
	if (SkipWithoutGpu(*this)) return true;
	constexpr int32 NX = 40, NY = 24, NZ = 12, Steps = 150;
	constexpr double H = .25, Q = .6;
	struct FRun { TArray<double> T, U, V, W; double Residual = 0; int32 PressureIters = 0; bool bGpu = false; double CoolW = 0, CoilW = 0; };
	auto Run = [&](bool bGpu, bool bTurbulence)
	{
		FRun Out;
		FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;
		FAirSolver Air; Air.Initialize(GP, 297.15);
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-8;
		Air.GetMutableTurbulenceSettings().bEnabled = bTurbulence;
		Air.SetUseGpuStep(bGpu);
		FUniformAirGrid& G = Air.GetMutableGrid();
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 4; Y <= 7; ++Y) for (int32 X = 10; X <= 11; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 12; Y <= 20; ++Y) G.bSolidCell[G.CellIndex(25, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		FAirSolverInput In; In.CellHeatW.Init(0.0, G.GetNumCells()); In.CellCoolingUAWK.Init(0.0, G.GetNumCells());
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 12; Y <= 20; ++Y) In.CellHeatW[G.CellIndex(26, Y, Z)] = 60.0;
		for (int32 Y = 2; Y < 22; ++Y) for (int32 X = 30; X < 38; ++X) In.CellCoolingUAWK[G.CellIndex(X, Y, NZ - 1)] = 25.0;
		In.CoolingSupplyTempK = 291.15;
		In.NumDucts = 1; In.DuctCoilUAWK = {800.0}; In.DuctCoilWaterTempK = {291.15}; In.DuctFanHeatW = {300.0};
		for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 4; Y <= 7; ++Y)
		{
			const double V = Q / (36 * H * H);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(10, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(9, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(0); In.DuctFaceVelocityMs.Add(V);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(12, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(12, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(1); In.DuctFaceVelocityMs.Add(V);
		}
		for (int32 S = 0; S < Steps; ++S)
		{
			Air.Step(In, .02);
			Out.PressureIters += Air.GetLastPressureResult().IterationsUsed;
		}
		Out.bGpu = Air.DidLastStepRunOnGpu();
		Out.T = Air.GetGrid().TemperatureK; Out.U = Air.GetGrid().VelocityXMs; Out.V = Air.GetGrid().VelocityYMs; Out.W = Air.GetGrid().VelocityZMs;
		Out.Residual = Air.GetNormalizedEnergyResidual();
		Out.CoilW = Air.GetLastDuctCoilRemovedW();
		return Out;
	};
	auto MaxDiff = [](const TArray<double>& A, const TArray<double>& B)
	{
		double M = 0.0;
		for (int32 I = 0; I < A.Num(); ++I) M = FMath::Max(M, FMath::Abs(A[I] - B[I]));
		return M;
	};
	for (const bool bTurbulence : { false, true })
	{
		const FRun Cpu = Run(false, bTurbulence), Gpu = Run(true, bTurbulence);
		const double DT = MaxDiff(Cpu.T, Gpu.T);
		const double DV = FMath::Max3(MaxDiff(Cpu.U, Gpu.U), MaxDiff(Cpu.V, Gpu.V), MaxDiff(Cpu.W, Gpu.W));
		double TMin = 1e9, TMax = 0; for (const double T : Cpu.T) { TMin = FMath::Min(TMin, T); TMax = FMath::Max(TMax, T); }
		double VMax = 0; for (const double V : Cpu.W) VMax = FMath::Max(VMax, FMath::Abs(V));
		AddInfo(FString::Printf(TEXT("난류 %d, %d스텝 | GPU로 돌았나 %d | 온도 최대 차 %.3e K (온도 폭 %.2f K) | 속도 최대 차 %.3e m/s (최대 %.3f) | 압력 반복 합 CPU %d / GPU %d | 열수지 CPU %.3e / GPU %.3e | 덕트 코일 CPU %.4f / GPU %.4f W"),
			bTurbulence, Steps, Gpu.bGpu, DT, TMax - TMin, DV, VMax, Cpu.PressureIters, Gpu.PressureIters, Cpu.Residual, Gpu.Residual, Cpu.CoilW, Gpu.CoilW));
		TestTrue(TEXT("Ran on the GPU"), Gpu.bGpu);
		TestTrue(TEXT("GPU ledger closes"), FMath::Abs(Gpu.Residual) < 1e-12);
		TestTrue(TEXT("Temperature agrees"), DT < 1e-6);
		TestTrue(TEXT("Velocity agrees"), DV < 1e-6);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
