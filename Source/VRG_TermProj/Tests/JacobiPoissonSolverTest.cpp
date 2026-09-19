// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgAirTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	FAirGridParams MakeGridParams(int32 NumCellsX, int32 NumCellsY, int32 NumCellsZ, double CellSizeM)
	{
		FAirGridParams Params;
		Params.NumCellsX = NumCellsX;
		Params.NumCellsY = NumCellsY;
		Params.NumCellsZ = NumCellsZ;
		Params.CellSizeM = CellSizeM;
		return Params;
	}

	// 셀 중심에서 잰 cos(pi*x/Lx)*cos(pi*y/Ly)*cos(pi*z/Lz).
	//
	// 이 배열은 Neumann 경계 이산 Laplacian의 **정확한 고유벡터**다.
	// 셀 중심이 (i+0.5)h이고 벽 바깥을 거울상으로 두면
	//   cos(pi*(i+1+0.5)/N) + cos(pi*(i-1+0.5)/N) = 2*cos(pi/N)*cos(pi*(i+0.5)/N)
	// 가 경계에서도 성립하기 때문이다.
	// 따라서 고유값은 축마다 -(4/h^2)*sin^2(pi/(2N))이고, 이산화 오차가 0이다.
	// 해석해 비교에서 남는 오차는 오직 반복 솔버의 수렴 한계뿐이다.
	void FillCosineMode(const FUniformAirGrid& Grid, TArray<double>& OutField, double& OutEigenvalue)
	{
		const int32 NumCellsX = Grid.GetNumCellsX();
		const int32 NumCellsY = Grid.GetNumCellsY();
		const int32 NumCellsZ = Grid.GetNumCellsZ();
		const double CellSizeM = Grid.GetCellSizeM();

		OutField.SetNum(Grid.GetNumCells());

		for (int32 Z = 0; Z < NumCellsZ; ++Z)
		{
			for (int32 Y = 0; Y < NumCellsY; ++Y)
			{
				for (int32 X = 0; X < NumCellsX; ++X)
				{
					OutField[Grid.CellIndex(X, Y, Z)] =
						FMath::Cos(UE_DOUBLE_PI * (X + 0.5) / NumCellsX)
						* FMath::Cos(UE_DOUBLE_PI * (Y + 0.5) / NumCellsY)
						* FMath::Cos(UE_DOUBLE_PI * (Z + 0.5) / NumCellsZ);
				}
			}
		}

		auto AxisEigen = [CellSizeM](int32 NumCells) -> double
		{
			const double S = FMath::Sin(UE_DOUBLE_PI / (2.0 * NumCells));
			return -4.0 * S * S / (CellSizeM * CellSizeM);
		};

		OutEigenvalue = AxisEigen(NumCellsX) + AxisEigen(NumCellsY) + AxisEigen(NumCellsZ);
	}

	double MaxAbsDifference(const TArray<double>& A, const TArray<double>& B)
	{
		double MaxAbs = 0.0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			MaxAbs = FMath::Max(MaxAbs, FMath::Abs(A[Index] - B[Index]));
		}
		return MaxAbs;
	}
}

// 이산 Laplacian이 코사인 모드에 대해 정확히 고유값 배가 되는지.
// 이게 맞아야 아래 해석해 비교가 의미를 갖는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirLaplacianEigenmodeTest,
	"VRG.Air.Poisson.DiscreteEigenmodeExact", VrgAirTestFlags)

bool FAirLaplacianEigenmodeTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Grid;
	Grid.Initialize(MakeGridParams(16, 12, 20, 0.25));

	TArray<double> Mode;
	double Eigenvalue = 0.0;
	FillCosineMode(Grid, Mode, Eigenvalue);

	double MaxAbsError = 0.0;
	double MaxAbsExpected = 0.0;
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const double Actual = JacobiPoissonSolver::LaplacianAt(Grid, Mode, X, Y, Z);
				const double Expected = Eigenvalue * Mode[Grid.CellIndex(X, Y, Z)];
				MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(Actual - Expected));
				MaxAbsExpected = FMath::Max(MaxAbsExpected, FMath::Abs(Expected));
			}
		}
	}

	const double NormalizedError = MaxAbsError / MaxAbsExpected;
	AddInfo(FString::Printf(TEXT("격자 16x12x20, h=0.25 m | 고유값 %.9f 1/m^2"), Eigenvalue));
	AddInfo(FString::Printf(TEXT("절대 오차 %.3e / 기대 최대 %.6f 기준 정규화 오차 %.3e"),
		MaxAbsError, MaxAbsExpected, NormalizedError));

	TestTrue(TEXT("코사인 모드가 이산 Laplacian의 고유벡터여야 한다"), NormalizedError < 1.0e-14);

	return true;
}

// 해석해 비교: laplacian(p) = lambda*p 를 풀면 p가 나와야 한다.
// 이산화 오차가 0이므로 남는 오차는 반복 솔버의 수렴 한계뿐이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirPoissonAnalyticTest,
	"VRG.Air.Poisson.CosineModeAnalytic", VrgAirTestFlags)

bool FAirPoissonAnalyticTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Grid;
	Grid.Initialize(MakeGridParams(16, 16, 16, 0.25));

	TArray<double> Analytic;
	double Eigenvalue = 0.0;
	FillCosineMode(Grid, Analytic, Eigenvalue);

	TArray<double> RightHandSide;
	RightHandSide.SetNum(Grid.GetNumCells());
	for (int32 Index = 0; Index < RightHandSide.Num(); ++Index)
	{
		RightHandSide[Index] = Eigenvalue * Analytic[Index];
	}

	FPoissonSolveSettings Settings;
	Settings.NormalizedResidualTarget = 1.0e-8;
	Settings.MaxIterations = 20000;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result = JacobiPoissonSolver::Solve(Grid, RightHandSide, Solved, Scratch, Settings);

	// 해석해도 평균이 0이 아닐 수 있으므로 같은 기준으로 맞춘다.
	TArray<double> AnalyticZeroMean = Analytic;
	JacobiPoissonSolver::RemoveMean(AnalyticZeroMean);

	const double MaxAbsError = MaxAbsDifference(Solved, AnalyticZeroMean);

	double MaxAbsAnalytic = 0.0;
	for (const double Value : AnalyticZeroMean)
	{
		MaxAbsAnalytic = FMath::Max(MaxAbsAnalytic, FMath::Abs(Value));
	}
	const double NormalizedError = MaxAbsError / MaxAbsAnalytic;

	AddInfo(FString::Printf(TEXT("격자 16^3, h=0.25 m | %s (반복 %d회, 정규화 잔차 %.3e, 기준 %.6f)"),
		Result.bConverged ? TEXT("수렴") : TEXT("수렴 실패"),
		Result.IterationsUsed, Result.NormalizedResidual, Result.ReferenceScale));
	AddInfo(FString::Printf(TEXT("해석해 대비 절대 오차 %.3e / 해 최대 크기 %.6f 기준 정규화 오차 %.3e"),
		MaxAbsError, MaxAbsAnalytic, NormalizedError));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("해가 해석해와 일치해야 한다"), NormalizedError < 1.0e-6);

	return true;
}

// D5: 반복 상한에 걸리면 수렴 실패를 결과에 표시해야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirPoissonNonConvergenceTest,
	"VRG.Air.Poisson.NonConvergenceReported", VrgAirTestFlags)

bool FAirPoissonNonConvergenceTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Grid;
	Grid.Initialize(MakeGridParams(16, 16, 16, 0.25));

	TArray<double> Analytic;
	double Eigenvalue = 0.0;
	FillCosineMode(Grid, Analytic, Eigenvalue);

	TArray<double> RightHandSide;
	RightHandSide.SetNum(Grid.GetNumCells());
	for (int32 Index = 0; Index < RightHandSide.Num(); ++Index)
	{
		RightHandSide[Index] = Eigenvalue * Analytic[Index];
	}

	FPoissonSolveSettings Settings;
	Settings.NormalizedResidualTarget = 1.0e-12;
	Settings.MaxIterations = 3;
	Settings.ResidualCheckInterval = 1;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result = JacobiPoissonSolver::Solve(Grid, RightHandSide, Solved, Scratch, Settings);

	AddInfo(FString::Printf(TEXT("반복 상한 %d회 | 수렴 %s, 정규화 잔차 %.3e"),
		Settings.MaxIterations, Result.bConverged ? TEXT("성공") : TEXT("실패"), Result.NormalizedResidual));

	TestFalse(TEXT("반복 상한에 걸리면 수렴 실패로 보고돼야 한다"), Result.bConverged);
	TestEqual(TEXT("반복 상한까지 돌아야 한다"), Result.IterationsUsed, Settings.MaxIterations);
	TestTrue(TEXT("남은 잔차가 보고돼야 한다"), Result.NormalizedResidual > Settings.NormalizedResidualTarget);

	return true;
}

// 전체 Neumann에서 sum(B) != 0이면 해가 없다. 그 사실을 표시해야 한다.
// 표시하지 않으면 호출자가 "수렴 실패"를 솔버 성능 문제로 오해한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirPoissonIncompatibleRhsTest,
	"VRG.Air.Poisson.IncompatibleRightHandSideReported", VrgAirTestFlags)

bool FAirPoissonIncompatibleRhsTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Grid;
	Grid.Initialize(MakeGridParams(8, 8, 8, 0.25));

	// 전부 같은 값이면 sum(B)가 0이 아니다. 상수 함수는 Neumann Poisson의 해가 없다.
	TArray<double> RightHandSide;
	RightHandSide.Init(1.0, Grid.GetNumCells());

	FPoissonSolveSettings Settings;
	Settings.MaxIterations = 200;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result = JacobiPoissonSolver::Solve(Grid, RightHandSide, Solved, Scratch, Settings);

	AddInfo(FString::Printf(TEXT("우변 평균 %.6f (기준 %.6f) | 비적합 표시 %s | 수렴 %s"),
		Result.MeanRightHandSide, Result.ReferenceScale,
		Result.bIncompatibleRightHandSide ? TEXT("있음") : TEXT("없음"),
		Result.bConverged ? TEXT("성공") : TEXT("실패")));

	TestTrue(TEXT("sum(B) != 0이면 비적합으로 표시돼야 한다"), Result.bIncompatibleRightHandSide);
	TestFalse(TEXT("비적합 우변은 수렴하지 않아야 한다"), Result.bConverged);

	// 반대로 평균을 빼면 적합해지고 수렴해야 한다.
	double Sum = 0.0;
	for (const double Value : RightHandSide) { Sum += Value; }
	const double Mean = Sum / RightHandSide.Num();
	for (double& Value : RightHandSide) { Value -= Mean; }

	TArray<double> CompatibleSolved;
	const FPoissonSolveResult CompatibleResult =
		JacobiPoissonSolver::Solve(Grid, RightHandSide, CompatibleSolved, Scratch, Settings);

	AddInfo(FString::Printf(TEXT("평균을 뺀 뒤: 비적합 표시 %s | 수렴 %s"),
		CompatibleResult.bIncompatibleRightHandSide ? TEXT("있음") : TEXT("없음"),
		CompatibleResult.bConverged ? TEXT("성공") : TEXT("실패")));

	TestFalse(TEXT("평균을 빼면 비적합 표시가 사라져야 한다"), CompatibleResult.bIncompatibleRightHandSide);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
