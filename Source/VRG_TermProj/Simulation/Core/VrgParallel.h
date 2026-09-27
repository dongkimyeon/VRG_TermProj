// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Async/ParallelFor.h"

// Parallel loops for the solvers (2026-09-27).
//
// Work is split into FIXED blocks, so which thread runs a block never changes a result:
//  - ForRange bodies write only the indices of their own block (rule 7: read and write buffers are separate).
//  - Sum adds the block partials in block order, whatever order the blocks ran in.
// The block partition does not depend on the thread count, so the serial path (vrg.Sim.Parallel 0) gives the same
// numbers as the parallel one. Matching numbers are not a proof of freedom from races (verification rule); the
// disjoint-write construction is, and it is kept simple on purpose.
// Plain C++, no UObjects: usable from worker threads (rule 5).
namespace VrgParallel
{
	// Console vrg.Sim.Parallel 0|1 (default 1).
	VRG_TERMPROJ_API bool IsEnabled();

	template <typename FBody>
	void ForRange(int32 N, int32 BlockSize, FBody&& Body)
	{
		if (N <= 0) return;
		BlockSize = FMath::Max(BlockSize, 1);
		const int32 NumBlocks = (N + BlockSize - 1) / BlockSize;
		auto Run = [&](int32 B)
		{
			const int32 Begin = B * BlockSize;
			Body(Begin, FMath::Min(Begin + BlockSize, N));
		};
		if (NumBlocks == 1 || !IsEnabled())
		{
			for (int32 B = 0; B < NumBlocks; ++B) Run(B);
			return;
		}
		ParallelFor(NumBlocks, Run);
	}

	// Deterministic sum of Term(Begin, End) over the blocks of [0, N).
	template <typename FTerm>
	double Sum(int32 N, int32 BlockSize, FTerm&& Term)
	{
		if (N <= 0) return 0.0;
		BlockSize = FMath::Max(BlockSize, 1);
		const int32 NumBlocks = (N + BlockSize - 1) / BlockSize;
		TArray<double, TInlineAllocator<128>> Partial;
		Partial.SetNumZeroed(NumBlocks);
		ForRange(NumBlocks, 1, [&](int32 B0, int32 B1)
		{
			for (int32 B = B0; B < B1; ++B)
			{
				const int32 Begin = B * BlockSize;
				Partial[B] = Term(Begin, FMath::Min(Begin + BlockSize, N));
			}
		});
		double S = 0.0;
		for (const double P : Partial) S += P;
		return S;
	}

	// Deterministic max of Term(Begin, End) over the blocks of [0, N) (max is order independent anyway).
	template <typename FTerm>
	double Max(int32 N, int32 BlockSize, FTerm&& Term)
	{
		if (N <= 0) return 0.0;
		BlockSize = FMath::Max(BlockSize, 1);
		const int32 NumBlocks = (N + BlockSize - 1) / BlockSize;
		TArray<double, TInlineAllocator<128>> Partial;
		Partial.SetNumZeroed(NumBlocks);
		ForRange(NumBlocks, 1, [&](int32 B0, int32 B1)
		{
			for (int32 B = B0; B < B1; ++B)
			{
				const int32 Begin = B * BlockSize;
				Partial[B] = Term(Begin, FMath::Min(Begin + BlockSize, N));
			}
		});
		double M = Partial[0];
		for (const double P : Partial) M = FMath::Max(M, P);
		return M;
	}

	// Body(Y, Z) for every row Y in [Y0, Y1), Z in [Z0, Z1) of a grid, in parallel blocks of rows.
	template <typename FBody>
	void ForEachRow(int32 Y0, int32 Y1, int32 Z0, int32 Z1, FBody&& Body)
	{
		const int32 NY = Y1 - Y0, NZ = Z1 - Z0;
		if (NY <= 0 || NZ <= 0) return;
		ForRange(NY * NZ, 8, [&](int32 Begin, int32 End)
		{
			for (int32 R = Begin; R < End; ++R) Body(Y0 + R % NY, Z0 + R / NY);
		});
	}
}
