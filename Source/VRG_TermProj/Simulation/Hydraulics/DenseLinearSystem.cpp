// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/DenseLinearSystem.h"

bool SolveDenseLinearSystem(TArray<double>& MatrixRowMajor, TArray<double>& RhsAndSolution, int32 N)
{
	if (N <= 0 || MatrixRowMajor.Num() != N * N || RhsAndSolution.Num() != N)
	{
		return false;
	}

	double* A = MatrixRowMajor.GetData();
	double* B = RhsAndSolution.GetData();

	for (int32 Col = 0; Col < N; ++Col)
	{
		// 부분 피벗: 이 열에서 절댓값이 가장 큰 행을 고른다.
		int32 PivotRow = Col;
		double PivotMagnitude = FMath::Abs(A[Col * N + Col]);
		for (int32 Row = Col + 1; Row < N; ++Row)
		{
			const double Magnitude = FMath::Abs(A[Row * N + Col]);
			if (Magnitude > PivotMagnitude)
			{
				PivotMagnitude = Magnitude;
				PivotRow = Row;
			}
		}

		if (PivotMagnitude <= UE_DOUBLE_SMALL_NUMBER)
		{
			return false;
		}

		if (PivotRow != Col)
		{
			for (int32 Index = 0; Index < N; ++Index)
			{
				Swap(A[PivotRow * N + Index], A[Col * N + Index]);
			}
			Swap(B[PivotRow], B[Col]);
		}

		const double Pivot = A[Col * N + Col];
		for (int32 Row = Col + 1; Row < N; ++Row)
		{
			const double Factor = A[Row * N + Col] / Pivot;
			if (Factor == 0.0)
			{
				continue;
			}
			for (int32 Index = Col; Index < N; ++Index)
			{
				A[Row * N + Index] -= Factor * A[Col * N + Index];
			}
			B[Row] -= Factor * B[Col];
		}
	}

	// 후진 대입
	for (int32 Row = N - 1; Row >= 0; --Row)
	{
		double Sum = B[Row];
		for (int32 Index = Row + 1; Index < N; ++Index)
		{
			Sum -= A[Row * N + Index] * B[Index];
		}
		B[Row] = Sum / A[Row * N + Row];
	}

	return true;
}
