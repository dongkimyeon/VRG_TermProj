// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/OctreePoissonSolver.h"
#include "Simulation/Air/FixedOctreeGrid.h"

namespace
{
	// 리프 하나에 붙은 유효한 면을 모은 결과.
	// Diagonal은 sum(T_f) [m], FluxSum은 sum(T_f * X_other) [X단위*m]다.
	// 벽과 고체 쪽 면은 플럭스가 0이라 둘 다에서 빠진다 (Neumann).
	struct FOctreeNeighborSum
	{
		double FluxSum = 0.0;
		double Diagonal = 0.0;
	};

	FOctreeNeighborSum GatherFaces(const FFixedOctreeGrid& Tree, const TArray<double>& Field,
		int32 LeafIndex)
	{
		FOctreeNeighborSum Result;
		const TArray<FFixedOctreeFace>& Faces = Tree.GetFaces();
		const TArray<int32>& Start = Tree.GetLeafFaceStart();
		const TArray<int32>& Indices = Tree.GetLeafFaceIndices();

		for (int32 Slot = Start[LeafIndex]; Slot < Start[LeafIndex + 1]; ++Slot)
		{
			const FFixedOctreeFace& Face = Faces[Indices[Slot]];
			const int32 Other = Face.LeafLow == LeafIndex ? Face.LeafHigh : Face.LeafLow;

			// 도메인 벽이거나 고체에 닿은 면. 벽 바깥과 똑같이 다뤄야
			// 그 면으로 압력 기울기가 생기지 않는다.
			if (Other == INDEX_NONE || !Tree.IsLeafFluid(Other))
			{
				continue;
			}

			// 규약 6: 전달계수는 면이 들고 있는 면적/거리에서 한 번만 나온다.
			// 양쪽 리프가 같은 T_f를 읽으므로 두 번 계산하지 않는다.
			const double Transmissibility = Face.AreaM2 / Face.DistanceM;
			Result.FluxSum += Transmissibility * Field[Other];
			Result.Diagonal += Transmissibility;
		}
		return Result;
	}
}

double OctreePoissonSolver::LaplacianAt(const FFixedOctreeGrid& Tree, const TArray<double>& Field,
	int32 LeafIndex)
{
	const FOctreeNeighborSum Neighbors = GatherFaces(Tree, Field, LeafIndex);
	return (Neighbors.FluxSum - Neighbors.Diagonal * Field[LeafIndex]) / Tree.LeafVolumeM3(LeafIndex);
}

double OctreePoissonSolver::MaxAbsResidual(const FFixedOctreeGrid& Tree, const TArray<double>& Field,
	const TArray<double>& RightHandSide)
{
	double MaxAbs = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		// 고체 리프에는 풀 방정식이 없다. 잔차에 넣으면 못 줄이는 값이 섞인다.
		if (!Tree.IsLeafFluid(Leaf))
		{
			continue;
		}
		const double Residual = LaplacianAt(Tree, Field, Leaf) - RightHandSide[Leaf];
		MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Residual));
	}
	return MaxAbs;
}

void OctreePoissonSolver::RemoveFluidVolumeMean(const FFixedOctreeGrid& Tree, TArray<double>& Field)
{
	// 셀 크기가 섞여 있으므로 단순 평균을 쓰면 잘은 영역에 가중치가 쏠린다.
	// 고체 리프는 방정식에 들어가지 않으므로 평균에도 넣지 않는다.
	double WeightedSum = 0.0;
	double TotalVolumeM3 = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (!Tree.IsLeafFluid(Leaf))
		{
			continue;
		}
		const double VolumeM3 = Tree.LeafVolumeM3(Leaf);
		WeightedSum += VolumeM3 * Field[Leaf];
		TotalVolumeM3 += VolumeM3;
	}
	if (TotalVolumeM3 <= 0.0)
	{
		return;
	}
	const double Mean = WeightedSum / TotalVolumeM3;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (Tree.IsLeafFluid(Leaf))
		{
			Field[Leaf] -= Mean;
		}
	}
}

double OctreePoissonSolver::VolumeWeightedLaplacianSum(const FFixedOctreeGrid& Tree,
	const TArray<double>& Field)
{
	double Sum = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (Tree.IsLeafFluid(Leaf))
		{
			Sum += Tree.LeafVolumeM3(Leaf) * LaplacianAt(Tree, Field, Leaf);
		}
	}
	return Sum;
}

FPoissonSolveResult OctreePoissonSolver::Solve(const FFixedOctreeGrid& Tree,
	const TArray<double>& RightHandSide, TArray<double>& OutField, TArray<double>& Scratch,
	const FPoissonSolveSettings& Settings)
{
	FPoissonSolveResult Result;

	const int32 NumLeaves = Tree.GetNumLeaves();
	if (NumLeaves == 0 || RightHandSide.Num() != NumLeaves
		|| Tree.GetLeafFaceStart().Num() != NumLeaves + 1)
	{
		return Result;
	}

	if (OutField.Num() != NumLeaves)
	{
		OutField.Init(0.0, NumLeaves);
	}
	Scratch.SetNumUninitialized(NumLeaves, EAllowShrinking::No);

	// 정규화 기준은 우변의 최대 크기다. 단위가 B와 같으므로 잔차를 나누면 무차원이 된다.
	double ReferenceScale = 0.0;
	double WeightedSum = 0.0;
	double TotalVolumeM3 = 0.0;
	for (int32 Leaf = 0; Leaf < NumLeaves; ++Leaf)
	{
		if (!Tree.IsLeafFluid(Leaf))
		{
			continue;
		}
		const double VolumeM3 = Tree.LeafVolumeM3(Leaf);
		ReferenceScale = FMath::Max(ReferenceScale, FMath::Abs(RightHandSide[Leaf]));
		WeightedSum += VolumeM3 * RightHandSide[Leaf];
		TotalVolumeM3 += VolumeM3;
	}
	Result.ReferenceScale = ReferenceScale;
	// 균일 솔버는 셀 수로 나눈 평균을 싣지만, 여기서는 셀 크기가 다르므로 체적 가중이다.
	Result.MeanRightHandSide = TotalVolumeM3 > 0.0 ? WeightedSum / TotalVolumeM3 : 0.0;

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

	// 전체 Neumann에서는 sum(V_i * B_i)가 0이어야 해가 존재한다.
	// 0이 아니면 아무리 반복해도 잔차가 그만큼 남는다. 그 사실을 표시한다 (D5).
	Result.bIncompatibleRightHandSide =
		FMath::Abs(Result.MeanRightHandSide) > 1.0e-10 * ReferenceScale;

	const int32 CheckInterval = FMath::Max(1, Settings.ResidualCheckInterval);
	const int32 MaxIterations = FMath::Max(1, Settings.MaxIterations);

	// 완화 계수. 1.0(순수 Jacobi)은 체커보드 모드를 못 죽여 수렴하지 않는다.
	const double Omega = FMath::Clamp(Settings.Relaxation, 0.05, 1.0);

	for (int32 Iteration = 1; Iteration <= MaxIterations; ++Iteration)
	{
		// D6: Jacobi. 읽기(OutField)와 쓰기(Scratch)가 분리돼 있어
		// 리프 순회 순서가 결과에 영향을 주지 않는다. 옥트리에서도 같은 코드가 돈다.
		for (int32 Leaf = 0; Leaf < NumLeaves; ++Leaf)
		{
			const FOctreeNeighborSum Neighbors = GatherFaces(Tree, OutField, Leaf);

			// 고체 리프, 그리고 고체·벽에 둘러싸여 이웃이 하나도 없는 리프는 풀 것이 없다.
			if (!Tree.IsLeafFluid(Leaf) || Neighbors.Diagonal <= 0.0)
			{
				Scratch[Leaf] = OutField[Leaf];
				continue;
			}

			// (FluxSum - Diagonal*center)/V = B 를 center에 대해 푼 값.
			const double JacobiUpdate =
				(Neighbors.FluxSum - Tree.LeafVolumeM3(Leaf) * RightHandSide[Leaf]) / Neighbors.Diagonal;

			// 완화: 한 번에 갈아치우지 않고 omega만큼만 옮긴다.
			Scratch[Leaf] = FMath::Lerp(OutField[Leaf], JacobiUpdate, Omega);
		}

		Swap(OutField, Scratch);

		// 전체 Neumann이라 해가 상수만큼 자유롭다. 매번 체적 가중 평균을 빼서 떠내려가지 않게 한다.
		RemoveFluidVolumeMean(Tree, OutField);

		Result.IterationsUsed = Iteration;

		if (Iteration % CheckInterval == 0 || Iteration == MaxIterations)
		{
			Result.NormalizedResidual = MaxAbsResidual(Tree, OutField, RightHandSide) / ReferenceScale;
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
