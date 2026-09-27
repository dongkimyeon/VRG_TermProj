// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/WallDistance.h"
#include "Simulation/Air/UniformAirGrid.h"

namespace
{
	constexpr double Far = TNumericLimits<double>::Max();

	// Exact 1D squared distance transform: D[q] = min over p with finite F[p] of (q - p)^2 + F[p].
	// Only finite entries enter the lower envelope (mixing a huge sentinel into the intersection formula loses precision).
	void Transform1D(const TArray<double>& F, TArray<double>& D, TArray<int32>& V, TArray<double>& Z)
	{
		const int32 N = F.Num();
		V.SetNumUninitialized(N);
		Z.SetNumUninitialized(N + 1);
		int32 K = -1;
		for (int32 Q = 0; Q < N; ++Q)
		{
			if (F[Q] == Far) continue;
			double S = -Far;
			while (K >= 0)
			{
				const int32 P = V[K];
				S = ((F[Q] + double(Q) * Q) - (F[P] + double(P) * P)) / (2.0 * (Q - P));
				if (S > Z[K]) break;
				--K;
			}
			++K;
			V[K] = Q;
			Z[K] = K == 0 ? -Far : S;
			Z[K + 1] = Far;
		}
		D.SetNumUninitialized(N);
		if (K < 0)
		{
			for (int32 Q = 0; Q < N; ++Q) D[Q] = Far;
			return;
		}
		int32 J = 0;
		for (int32 Q = 0; Q < N; ++Q)
		{
			while (Z[J + 1] < Q) ++J;
			const double Dq = double(Q - V[J]);
			D[Q] = Dq * Dq + F[V[J]];
		}
	}
}

void AirWallDistance::Compute(const FUniformAirGrid& Grid, TArray<double>& OutDistanceM)
{
	const int32 NX = Grid.GetNumCellsX(), NY = Grid.GetNumCellsY(), NZ = Grid.GetNumCellsZ();
	const int32 PX = NX + 2, PY = NY + 2, PZ = NZ + 2;
	auto P = [&](int32 X, int32 Y, int32 Z) { return X + PX * (Y + PY * Z); };

	// Padded grid: the ring and the solid cells are walls (squared distance 0), fluid starts far.
	TArray<double> D2;
	D2.Init(0.0, PX * PY * PZ);
	for (int32 Z = 0; Z < NZ; ++Z)
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
				D2[P(X + 1, Y + 1, Z + 1)] = Grid.IsSolidCell(X, Y, Z) ? 0.0 : Far;

	TArray<double> Line, Out, Zs;
	TArray<int32> Vs;
	auto Pass = [&](int32 Len, int32 NA, int32 NB, TFunctionRef<int32(int32, int32, int32)> Index)
	{
		Line.SetNumUninitialized(Len);
		for (int32 B = 0; B < NB; ++B)
			for (int32 A = 0; A < NA; ++A)
			{
				for (int32 I = 0; I < Len; ++I) Line[I] = D2[Index(I, A, B)];
				Transform1D(Line, Out, Vs, Zs);
				for (int32 I = 0; I < Len; ++I) D2[Index(I, A, B)] = Out[I];
			}
	};
	Pass(PX, PY, PZ, [&](int32 I, int32 A, int32 B) { return P(I, A, B); });
	Pass(PY, PX, PZ, [&](int32 I, int32 A, int32 B) { return P(A, I, B); });
	Pass(PZ, PX, PY, [&](int32 I, int32 A, int32 B) { return P(A, B, I); });

	const double H = Grid.GetCellSizeM();
	OutDistanceM.SetNumUninitialized(Grid.GetNumCells());
	for (int32 Z = 0; Z < NZ; ++Z)
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
				OutDistanceM[Grid.CellIndex(X, Y, Z)] = Grid.IsSolidCell(X, Y, Z) ? 0.0
					: (FMath::Sqrt(D2[P(X + 1, Y + 1, Z + 1)]) - 0.5) * H;
}
