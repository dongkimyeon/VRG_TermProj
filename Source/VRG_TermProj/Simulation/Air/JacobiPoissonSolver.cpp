// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

namespace
{
	// 셀 하나에서 실제로 이웃이 있는 방향의 개수와 이웃 값의 합.
	// 벽 쪽은 거울상이라 (X_ghost - X_center)가 0이 되어 항이 통째로 사라진다.
	// 그래서 경계 셀은 대각 계수가 6이 아니라 이웃 개수가 된다.
	struct FNeighborSum
	{
		double Sum = 0.0;
		int32 Count = 0;
	};

	FNeighborSum GatherNeighbors(const FUniformAirGrid& Grid, const TArray<double>& Field,
		int32 X, int32 Y, int32 Z)
	{
		FNeighborSum Result;

		// 고체 셀은 이웃으로 치지 않는다. 벽 바깥과 똑같이 다뤄야
		// 그 면으로 압력 기울기가 생기지 않고, 따라서 속도도 밀려 들어가지 않는다.
		auto Accumulate = [&](int32 NX, int32 NY, int32 NZ)
		{
			if (Grid.IsFluidCell(NX, NY, NZ))
			{
				Result.Sum += Field[Grid.CellIndex(NX, NY, NZ)];
				++Result.Count;
			}
		};

		Accumulate(X - 1, Y, Z);
		Accumulate(X + 1, Y, Z);
		Accumulate(X, Y - 1, Z);
		Accumulate(X, Y + 1, Z);
		Accumulate(X, Y, Z - 1);
		Accumulate(X, Y, Z + 1);

		return Result;
	}
}

double JacobiPoissonSolver::LaplacianAt(const FUniformAirGrid& Grid, const TArray<double>& Field,
	int32 X, int32 Y, int32 Z)
{
	const double CellSizeM = Grid.GetCellSizeM();
	const double InvCellSizeSquared = 1.0 / (CellSizeM * CellSizeM);

	const FNeighborSum Neighbors = GatherNeighbors(Grid, Field, X, Y, Z);
	const double Center = Field[Grid.CellIndex(X, Y, Z)];

	return (Neighbors.Sum - Neighbors.Count * Center) * InvCellSizeSquared;
}

void JacobiPoissonSolver::RemoveMean(TArray<double>& Field)
{
	if (Field.Num() == 0)
	{
		return;
	}

	double Sum = 0.0;
	for (const double Value : Field)
	{
		Sum += Value;
	}
	const double Mean = Sum / Field.Num();

	for (double& Value : Field)
	{
		Value -= Mean;
	}
}

void JacobiPoissonSolver::RemoveFluidMean(const FUniformAirGrid& Grid, TArray<double>& Field)
{
	// 고체 셀은 방정식에 들어가지 않으므로 평균에도 넣지 않는다.
	// 고체를 섞어 평균을 내면 유체 쪽 해가 고체 셀 값에 끌려다닌다.
	double Sum = 0.0;
	int32 Count = 0;
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				Sum += Field[Grid.CellIndex(X, Y, Z)];
				++Count;
			}
		}
	}

	if (Count == 0)
	{
		return;
	}
	const double Mean = Sum / Count;

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				Field[Grid.CellIndex(X, Y, Z)] -= Mean;
			}
		}
	}
}

double JacobiPoissonSolver::MaxAbsResidual(const FUniformAirGrid& Grid, const TArray<double>& Field,
	const TArray<double>& RightHandSide)
{
	double MaxAbs = 0.0;
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				// 고체 셀에는 풀 방정식이 없다. 잔차에 넣으면 못 줄이는 값이 섞인다.
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				const int32 Index = Grid.CellIndex(X, Y, Z);
				const double Residual = LaplacianAt(Grid, Field, X, Y, Z) - RightHandSide[Index];
				MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Residual));
			}
		}
	}
	return MaxAbs;
}

FPoissonSolveResult JacobiPoissonSolver::Solve(const FUniformAirGrid& Grid,
	const TArray<double>& RightHandSide, TArray<double>& OutField, TArray<double>& Scratch,
	const FPoissonSolveSettings& Settings)
{
	FPoissonSolveResult Result;

	const int32 NumCells = Grid.GetNumCells();
	if (NumCells == 0 || RightHandSide.Num() != NumCells)
	{
		return Result;
	}

	if (OutField.Num() != NumCells)
	{
		OutField.Init(0.0, NumCells);
	}
	Scratch.SetNumUninitialized(NumCells, EAllowShrinking::No);

	// 정규화 기준은 우변의 최대 크기다. 단위가 B와 같으므로 잔차를 나누면 무차원이 된다.
	double ReferenceScale = 0.0;
	double SumRightHandSide = 0.0;
	int32 NumFluidCells = 0;
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				const double Value = RightHandSide[Grid.CellIndex(X, Y, Z)];
				ReferenceScale = FMath::Max(ReferenceScale, FMath::Abs(Value));
				SumRightHandSide += Value;
				++NumFluidCells;
			}
		}
	}
	Result.ReferenceScale = ReferenceScale;
	Result.MeanRightHandSide = NumFluidCells > 0 ? SumRightHandSide / NumFluidCells : 0.0;

	// 우변이 통째로 0이면 해는 상수다. 반복할 것이 없다.
	if (ReferenceScale <= UE_DOUBLE_SMALL_NUMBER)
	{
		for (double& Value : OutField)
		{
			Value = 0.0;
		}
		Result.bConverged = true;
		return Result;
	}

	// 전체 Neumann에서는 sum(B)가 0이어야 해가 존재한다.
	// 0이 아니면 아무리 반복해도 잔차가 평균만큼 남는다. 그 사실을 표시한다.
	Result.bIncompatibleRightHandSide =
		FMath::Abs(Result.MeanRightHandSide) > 1.0e-10 * ReferenceScale;

	const double CellSizeSquared = Grid.GetCellSizeM() * Grid.GetCellSizeM();
	const int32 CheckInterval = FMath::Max(1, Settings.ResidualCheckInterval);
	const int32 MaxIterations = FMath::Max(1, Settings.MaxIterations);

	// 완화 계수. 1.0(순수 Jacobi)은 체커보드 모드를 못 죽여 수렴하지 않는다.
	const double Omega = FMath::Clamp(Settings.Relaxation, 0.05, 1.0);

	for (int32 Iteration = 1; Iteration <= MaxIterations; ++Iteration)
	{
		// D6: Jacobi. 읽기(OutField)와 쓰기(Scratch)가 완전히 분리돼 있어
		// 셀 순회 순서가 결과에 영향을 주지 않는다. 격자 구조가 바뀌어도 같은 코드가 돈다.
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		{
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			{
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					const int32 Index = Grid.CellIndex(X, Y, Z);

					// 고체 셀은 풀지 않고 그대로 둔다.
					if (!Grid.IsFluidCell(X, Y, Z))
					{
						Scratch[Index] = OutField[Index];
						continue;
					}

					const FNeighborSum Neighbors = GatherNeighbors(Grid, OutField, X, Y, Z);

					// 이웃이 없는 셀(1x1x1 격자, 또는 고체에 둘러싸인 셀)은 풀 것이 없다.
					if (Neighbors.Count == 0)
					{
						Scratch[Index] = OutField[Index];
						continue;
					}

					// (sum(neighbors) - count*center)/h^2 = B 를 center에 대해 푼 값.
					const double JacobiUpdate = (Neighbors.Sum - CellSizeSquared * RightHandSide[Index])
						/ Neighbors.Count;

					// 완화: 한 번에 갈아치우지 않고 omega만큼만 옮긴다.
					Scratch[Index] = FMath::Lerp(OutField[Index], JacobiUpdate, Omega);
				}
			}
		}

		Swap(OutField, Scratch);

		// 전체 Neumann이라 해가 상수만큼 자유롭다. 매번 평균을 빼서 떠내려가지 않게 한다.
		// 고체 셀은 방정식에 없으므로 평균에서 뺀다.
		RemoveFluidMean(Grid, OutField);

		Result.IterationsUsed = Iteration;

		if (Iteration % CheckInterval == 0 || Iteration == MaxIterations)
		{
			Result.NormalizedResidual = MaxAbsResidual(Grid, OutField, RightHandSide) / ReferenceScale;
			if (Result.NormalizedResidual <= Settings.NormalizedResidualTarget)
			{
				Result.bConverged = true;
				return Result;
			}
		}
	}

	// D5: 목표에 못 미친 채 반복 상한에 걸렸다. 숨기지 않는다.
	Result.bConverged = false;
	return Result;
}
