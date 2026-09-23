#include "Misc/AutomationTest.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/MultigridPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags MgFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams GridParams(int32 X, int32 Y, int32 Z, double H)
	{
		FAirGridParams P; P.NumCellsX = X; P.NumCellsY = Y; P.NumCellsZ = Z; P.CellSizeM = H; return P;
	}

	// Rack-row like obstacles: two rows of solid boxes with a gap, leaving the front/back layers open.
	void AddRackRows(FUniformAirGrid& Grid)
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

	// Manufactured right-hand side B = L(Xref) with the reference S3 operator: always compatible.
	void Manufacture(const FUniformAirGrid& Grid, TArray<double>& Ref, TArray<double>& B)
	{
		Ref.Init(0.0, Grid.GetNumCells()); B.Init(0.0, Grid.GetNumCells());
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) Ref[Grid.CellIndex(X, Y, Z)] = FMath::Sin(.37 * X + .11 * Y) * FMath::Cos(.23 * Z - .5 * Y) + .01 * X * Z;
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) B[Grid.CellIndex(X, Y, Z)] = JacobiPoissonSolver::LaplacianAt(Grid, Ref, X, Y, Z);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultigridPoissonTest, "VRG.Air.Poisson.MultigridSolidsAndScaling", MgFlags)
bool FMultigridPoissonTest::RunTest(const FString& Parameters)
{
	FPoissonSolveSettings S; S.NormalizedResidualTarget = 1.0e-10; S.MaxIterations = 200;

	// 1) Analytic: cell-centred cosine is an exact eigenvector of the Neumann operator (no discretization error).
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(16, 16, 16, .25));
		TArray<double> A, B; A.SetNum(Grid.GetNumCells()); B.SetNum(Grid.GetNumCells());
		const double Ev = -3.0 * 4.0 / (.25 * .25) * FMath::Square(FMath::Sin(UE_DOUBLE_PI / 32.0));
		for (int32 Z = 0; Z < 16; ++Z) for (int32 Y = 0; Y < 16; ++Y) for (int32 X = 0; X < 16; ++X)
		{
			const int32 I = Grid.CellIndex(X, Y, Z);
			A[I] = FMath::Cos(UE_DOUBLE_PI * (X + .5) / 16) * FMath::Cos(UE_DOUBLE_PI * (Y + .5) / 16) * FMath::Cos(UE_DOUBLE_PI * (Z + .5) / 16);
			B[I] = Ev * A[I];
		}
		JacobiPoissonSolver::RemoveMean(A);
		for (const FMultigridPoisson::EMode Mode : {FMultigridPoisson::EMode::VCycle, FMultigridPoisson::EMode::ConjugateGradient})
		{
			MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
			const FPoissonSolveResult R = MultigridPoissonSolver::Solve(Grid, B, X, Cache, S, Mode);
			double Err = 0.0, Mx = 0.0;
			for (int32 I = 0; I < A.Num(); ++I) { Err = FMath::Max(Err, FMath::Abs(X[I] - A[I])); Mx = FMath::Max(Mx, FMath::Abs(A[I])); }
			AddInfo(FString::Printf(TEXT("16^3 코사인 해석해 | %s | %d층 | %s %d회 | 정규화 잔차 %.3e | 해 최대 %.4f 기준 오차 %.3e"),
				Mode == FMultigridPoisson::EMode::VCycle ? TEXT("V-cycle") : TEXT("MG-PCG"), Cache.Solver.GetNumLevels(),
				R.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), R.IterationsUsed, R.NormalizedResidual, Mx, Err / Mx));
			TestTrue(TEXT("Analytic mode converges"), R.bConverged);
			TestTrue(TEXT("Analytic mode error"), Err / Mx < 1e-8);
		}
	}

	// 2) Rack-row solids: residual checked with the independent S3 operator, not the MG operator itself.
	//    3) The iteration count must not grow with the grid (the reason for multigrid).
	int32 ItersSmall = 0, ItersLarge = 0;
	for (const int32 Scale : {1, 2})
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(20 * Scale, 30 * Scale, 10 * Scale, .25 / Scale));
		AddRackRows(Grid);
		TArray<double> Ref, B; Manufacture(Grid, Ref, B);
		MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
		const FPoissonSolveResult R = MultigridPoissonSolver::Solve(Grid, B, X, Cache, S);
		const double ResidualS3 = JacobiPoissonSolver::MaxAbsResidual(Grid, X, B) / R.ReferenceScale;
		JacobiPoissonSolver::RemoveFluidMean(Grid, Ref); JacobiPoissonSolver::RemoveFluidMean(Grid, X);
		double Err = 0.0, Mx = 0.0;
		for (int32 I = 0; I < X.Num(); ++I) if (!Grid.bSolidCell[I]) { Err = FMath::Max(Err, FMath::Abs(X[I] - Ref[I])); Mx = FMath::Max(Mx, FMath::Abs(Ref[I])); }
		AddInfo(FString::Printf(TEXT("랙 줄 고체 %dx%dx%d | 유체 %d셀 %d층 | MG-PCG %s %d회 | S3 연산자로 잰 정규화 잔차 %.3e | 제조해 최대 %.4f 기준 오차 %.3e"),
			Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ(), Cache.Solver.GetNumCells(), Cache.Solver.GetNumLevels(),
			R.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), R.IterationsUsed, ResidualS3, Mx, Err / Mx));
		TestTrue(TEXT("Solid case converges"), R.bConverged);
		TestTrue(TEXT("Residual agrees with the S3 operator"), ResidualS3 < 1e-9);
		TestTrue(TEXT("Manufactured solution recovered"), Err / Mx < 1e-6);
		(Scale == 1 ? ItersSmall : ItersLarge) = R.IterationsUsed;

		// Jacobi reference on the small grid only, to record the gap (it hits its cap).
		if (Scale == 1)
		{
			FPoissonSolveSettings J = S; J.MaxIterations = 4000; J.NormalizedResidualTarget = 1e-6;
			TArray<double> XJ, Scratch;
			const FPoissonSolveResult RJ = JacobiPoissonSolver::Solve(Grid, B, XJ, Scratch, J);
			AddInfo(FString::Printf(TEXT("같은 문제 완화 Jacobi: 목표 1e-6에 %s %d회, 정규화 잔차 %.3e"),
				RJ.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), RJ.IterationsUsed, RJ.NormalizedResidual));
		}
	}
	TestTrue(TEXT("Iterations roughly grid independent (8x cells <= 2x iterations)"), ItersLarge <= 2 * FMath::Max(ItersSmall, 1));

	// D5: a cap that is too small is reported, not hidden.
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(20, 30, 10, .25)); AddRackRows(Grid);
		TArray<double> Ref, B; Manufacture(Grid, Ref, B);
		FPoissonSolveSettings Tight = S; Tight.MaxIterations = 2;
		MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
		TestFalse(TEXT("Cap reached is reported"), MultigridPoissonSolver::Solve(Grid, B, X, Cache, Tight).bConverged);
	}
	return true;
}
#endif
