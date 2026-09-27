#include "Simulation/Air/MultigridPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Simulation/Core/VrgParallel.h"

namespace
{
	constexpr int32 PreSweeps = 2, PostSweeps = 2, CoarsestSweeps = 60, MaxCoarsestCells = 64;
	// Cells per parallel block (VrgParallel): levels smaller than one block run serially.
	constexpr int32 ParBlock = 2048;

	// Hot loops use raw pointers: Development builds range-check every TArray index.
	double Dot(const TArray<double>& A, const TArray<double>& B)
	{
		const double* PA = A.GetData(); const double* PB = B.GetData();
		return VrgParallel::Sum(A.Num(), ParBlock, [&](int32 Begin, int32 End)
		{
			double S = 0.0;
			for (int32 I = Begin; I < End; ++I) S += PA[I] * PB[I];
			return S;
		});
	}
}

void FMultigridPoisson::RemoveMean(TArray<double>& X)
{
	// The constant vector is the null space of every level's operator (all-Neumann).
	if (X.IsEmpty()) return;
	double* PX = X.GetData();
	const double S = VrgParallel::Sum(X.Num(), ParBlock, [&](int32 Begin, int32 End)
	{
		double Part = 0.0;
		for (int32 I = Begin; I < End; ++I) Part += PX[I];
		return Part;
	}) / X.Num();
	VrgParallel::ForRange(X.Num(), ParBlock, [&](int32 Begin, int32 End) { for (int32 I = Begin; I < End; ++I) PX[I] -= S; });
}

void FMultigridPoisson::Build(FLevelInput&& Fine)
{
	Levels.Reset();
	FLevel& L0 = Levels.AddDefaulted_GetRef();
	L0.Volume = MoveTemp(Fine.Volume); L0.Centre = MoveTemp(Fine.Centre);
	L0.BoxMin = MoveTemp(Fine.BoxMin); L0.BoxSize = MoveTemp(Fine.BoxSize);
	L0.RowStart = MoveTemp(Fine.RowStart); L0.Col = MoveTemp(Fine.Col); L0.Trans = MoveTemp(Fine.Trans);

	for (;;)
	{
		FLevel& F = Levels.Last();
		const int32 N = F.Volume.Num();
		F.Diag.SetNumZeroed(N);
		for (int32 I = 0; I < N; ++I)
			for (int32 K = F.RowStart[I]; K < F.RowStart[I + 1]; ++K) F.Diag[I] += F.Trans[K];
		for (TArray<double>* Buf : {&F.X, &F.F, &F.R, &F.Tmp}) Buf->SetNumZeroed(N);
		if (N <= MaxCoarsestCells) break;

		// 2x2x2 aggregation. Cells already at least as large as the block pass through unchanged.
		// Only cells CONNECTED inside the block (through open faces) merge: a block straddling a thin wall
		// becomes two coarse cells, otherwise the coarse correction would leak through solids.
		const int32 Block = F.AggregateUnit * 2;
		TArray<FIntVector> Keys; Keys.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
			Keys[I] = F.BoxSize[I] >= Block ? FIntVector(-1 - I, 0, 0)
				: FIntVector(F.BoxMin[I].X / Block, F.BoxMin[I].Y / Block, F.BoxMin[I].Z / Block);
		TArray<int32> Root; Root.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I) Root[I] = I;
		auto Find = [&Root](int32 I) { while (Root[I] != I) { Root[I] = Root[Root[I]]; I = Root[I]; } return I; };
		for (int32 I = 0; I < N; ++I)
			for (int32 K = F.RowStart[I]; K < F.RowStart[I + 1]; ++K)
				if (F.Trans[K] > 0.0 && Keys[F.Col[K]] == Keys[I]) { const int32 A = Find(I), B = Find(F.Col[K]); if (A != B) Root[FMath::Max(A, B)] = FMath::Min(A, B); }
		F.Parent.SetNumUninitialized(N);
		TArray<int32> RootToCoarse; RootToCoarse.Init(INDEX_NONE, N);
		int32 NumCoarse = 0;
		for (int32 I = 0; I < N; ++I)
		{
			int32& C = RootToCoarse[Find(I)];
			if (C == INDEX_NONE) C = NumCoarse++;
			F.Parent[I] = C;
		}
		if (NumCoarse > int32(.8 * N)) { F.Parent.Reset(); break; }  // stalled: this is the coarsest level

		FLevel C;
		C.AggregateUnit = Block;
		C.Volume.SetNumZeroed(NumCoarse); C.Centre.SetNumZeroed(NumCoarse);
		C.BoxMin.Init(FIntVector(MAX_int32), NumCoarse); C.BoxSize.SetNumZeroed(NumCoarse);
		for (int32 I = 0; I < N; ++I)
		{
			const int32 P = F.Parent[I];
			C.Volume[P] += F.Volume[I];
			C.Centre[P] += F.Centre[I] * F.Volume[I];
			C.BoxMin[P] = FIntVector(FMath::Min(C.BoxMin[P].X, F.BoxMin[I].X), FMath::Min(C.BoxMin[P].Y, F.BoxMin[I].Y),
				FMath::Min(C.BoxMin[P].Z, F.BoxMin[I].Z));
			C.BoxSize[P] = FMath::Max(Block, F.BoxSize[I]);
		}
		for (int32 P = 0; P < NumCoarse; ++P) C.Centre[P] /= C.Volume[P];
		// Coarse face: sum over covered open fine faces of T_f * d_f, divided by the coarse centre distance.
		TArray<TMap<int32, double>> Rows; Rows.SetNum(NumCoarse);
		for (int32 I = 0; I < N; ++I)
			for (int32 K = F.RowStart[I]; K < F.RowStart[I + 1]; ++K)
			{
				const int32 J = F.Col[K], Pa = F.Parent[I], Pb = F.Parent[J];
				if (Pa == Pb) continue;
				Rows[Pa].FindOrAdd(Pb, 0.0) += F.Trans[K] * FVector::Distance(F.Centre[I], F.Centre[J]);
			}
		C.RowStart.SetNumUninitialized(NumCoarse + 1); C.RowStart[0] = 0;
		for (int32 P = 0; P < NumCoarse; ++P)
		{
			for (const TPair<int32, double>& E : Rows[P])
			{
				C.Col.Add(E.Key);
				C.Trans.Add(E.Value / FMath::Max(FVector::Distance(C.Centre[P], C.Centre[E.Key]), UE_DOUBLE_SMALL_NUMBER));
			}
			C.RowStart[P + 1] = C.Col.Num();
		}
		Levels.Add(MoveTemp(C));
	}
}

void FMultigridPoisson::ApplyA(const FLevel& L, const TArray<double>& X, TArray<double>& Out) const
{
	// A = D - offdiag (positive semidefinite). A X = -V * (L X).
	const int32 N = L.Volume.Num();
	const int32* RS = L.RowStart.GetData(); const int32* C = L.Col.GetData();
	const double* T = L.Trans.GetData(); const double* D = L.Diag.GetData(); const double* PX = X.GetData();
	double* O = Out.GetData();
	VrgParallel::ForRange(N, ParBlock, [&](int32 Begin, int32 End)
	{
		for (int32 I = Begin; I < End; ++I)
		{
			double S = D[I] * PX[I];
			for (int32 K = RS[I], E = RS[I + 1]; K < E; ++K) S -= T[K] * PX[C[K]];
			O[I] = S;
		}
	});
}

void FMultigridPoisson::ApplyLaplacian(const TArray<double>& X, TArray<double>& Out) const
{
	if (Levels.IsEmpty()) return;
	const FLevel& L = Levels[0];
	Out.SetNumUninitialized(L.Volume.Num());
	ApplyA(L, X, Out);
	for (int32 I = 0; I < Out.Num(); ++I) Out[I] = -Out[I] / L.Volume[I];
}

void FMultigridPoisson::Smooth(FLevel& L, int32 Sweeps, double Omega)
{
	// D6: damped Jacobi. Reads X, writes Tmp, then swaps (rule 7 ping-pong; order independent).
	const int32 N = L.Volume.Num();
	const int32* RS = L.RowStart.GetData(); const int32* C = L.Col.GetData();
	const double* T = L.Trans.GetData(); const double* D = L.Diag.GetData(); const double* F = L.F.GetData();
	for (int32 S = 0; S < Sweeps; ++S)
	{
		const double* X = L.X.GetData(); double* W = L.Tmp.GetData();
		VrgParallel::ForRange(N, ParBlock, [&](int32 Begin, int32 End)
		{
			for (int32 I = Begin; I < End; ++I)
			{
				if (D[I] <= 0.0) { W[I] = X[I]; continue; }  // isolated cell: nothing to solve
				double Off = 0.0;
				for (int32 K = RS[I], E = RS[I + 1]; K < E; ++K) Off += T[K] * X[C[K]];
				W[I] = X[I] + Omega * ((F[I] + Off) / D[I] - X[I]);
			}
		});
		Swap(L.X, L.Tmp);
	}
}

void FMultigridPoisson::VCycle(int32 Li, double Omega)
{
	FLevel& L = Levels[Li];
	if (Li == Levels.Num() - 1)
	{
		Smooth(L, CoarsestSweeps, Omega);
		RemoveMean(L.X);
		return;
	}
	Smooth(L, PreSweeps, Omega);
	ApplyA(L, L.X, L.R);
	FLevel& C = Levels[Li + 1];
	for (double& V : C.F) V = 0.0;
	for (double& V : C.X) V = 0.0;
	for (int32 I = 0; I < L.Volume.Num(); ++I) C.F[L.Parent[I]] += L.F[I] - L.R[I];  // restrict residual (sum)
	VCycle(Li + 1, Omega);
	{
		// Constant prolongation: each fine cell reads its parent (a gather, parallel safe). The restriction above
		// scatters into shared parents and stays serial.
		double* LX = L.X.GetData(); const double* CX = C.X.GetData(); const int32* Parent = L.Parent.GetData();
		VrgParallel::ForRange(L.Volume.Num(), ParBlock, [&](int32 Begin, int32 End) { for (int32 I = Begin; I < End; ++I) LX[I] += CX[Parent[I]]; });
	}
	Smooth(L, PostSweeps, Omega);
}

FPoissonSolveResult FMultigridPoisson::Solve(const TArray<double>& B, TArray<double>& X,
	const FPoissonSolveSettings& Settings, EMode Mode)
{
	FPoissonSolveResult Result;
	if (Levels.IsEmpty()) return Result;
	FLevel& L0 = Levels[0];
	const int32 N = L0.Volume.Num();
	if (B.Num() != N) return Result;
	if (X.Num() != N) X.Init(0.0, N);

	double Ref = 0.0, SumB = 0.0, TotalVolume = 0.0;
	for (int32 I = 0; I < N; ++I) { Ref = FMath::Max(Ref, FMath::Abs(B[I])); SumB += L0.Volume[I] * B[I]; TotalVolume += L0.Volume[I]; }
	Result.ReferenceScale = Ref;
	// Nonuniform Neumann compatibility is sum(V*B)=0, not sum(B)=0.
	Result.MeanRightHandSide = TotalVolume > 0 ? SumB / TotalVolume : 0.0;
	if (Ref <= UE_DOUBLE_SMALL_NUMBER) { for (double& V : X) V = 0.0; Result.bConverged = true; return Result; }
	Result.bIncompatibleRightHandSide = FMath::Abs(Result.MeanRightHandSide) > 1.0e-10 * Ref;
	if (Result.bIncompatibleRightHandSide)
	{
		Result.NormalizedResidual = FMath::Abs(Result.MeanRightHandSide) / Ref;
		return Result; // D5: never report a projected, different RHS as the requested solution.
	}

	// A-form right-hand side f = -V*B; remove only the compatible round-off remainder.
	// Incompatible input was rejected above.
	TArray<double> F; F.SetNumUninitialized(N);
	double SumF = 0.0, SumV = 0.0;
	for (int32 I = 0; I < N; ++I) { F[I] = -L0.Volume[I] * B[I]; SumF += F[I]; SumV += L0.Volume[I]; }
	for (int32 I = 0; I < N; ++I) F[I] -= L0.Volume[I] * SumF / SumV;

	const double Omega = FMath::Clamp(Settings.Relaxation, .05, 1.0);
	const int32 MaxIt = FMath::Max(1, Settings.MaxIterations);
	TArray<double> R, Z, P, AppliedDirection; R.SetNumUninitialized(N);
	auto Residual = [&]()  // normalized like JacobiPoissonSolver: max |L X - B| / max |B|
	{
		const double* PR = R.GetData(); const double* V = L0.Volume.GetData();
		return VrgParallel::Max(N, ParBlock, [&](int32 Begin, int32 End)
		{
			double M = 0.0;
			for (int32 I = Begin; I < End; ++I) M = FMath::Max(M, FMath::Abs(PR[I] / V[I]));
			return M;
		}) / Ref;
	};
	auto Precondition = [&](const TArray<double>& In, TArray<double>& Out)
	{
		L0.F = In; for (double& V : L0.X) V = 0.0;
		VCycle(0, Omega);
		Out = L0.X; RemoveMean(Out);
	};

	ApplyA(L0, X, R);
	for (int32 I = 0; I < N; ++I) R[I] = F[I] - R[I];
	Result.NormalizedResidual = Residual();
	if (Result.NormalizedResidual <= Settings.NormalizedResidualTarget) { Result.bConverged = true; return Result; }

	if (Mode == EMode::VCycle)
	{
		for (int32 It = 1; It <= MaxIt; ++It)
		{
			Precondition(R, Z);
			for (int32 I = 0; I < N; ++I) X[I] += Z[I];
			RemoveMean(X);
			ApplyA(L0, X, R);
			for (int32 I = 0; I < N; ++I) R[I] = F[I] - R[I];
			Result.IterationsUsed = It;
			Result.NormalizedResidual = Residual();
			if (Result.NormalizedResidual <= Settings.NormalizedResidualTarget) { Result.bConverged = true; return Result; }
		}
		return Result;
	}

	// Conjugate gradient preconditioned by one symmetric V-cycle (same pre/post Jacobi sweeps).
	AppliedDirection.SetNumUninitialized(N);
	Precondition(R, Z);
	P = Z;
	double RZ = Dot(R, Z);
	for (int32 It = 1; It <= MaxIt; ++It)
	{
		ApplyA(L0, P, AppliedDirection);
		const double PQ = Dot(P, AppliedDirection);
		if (PQ <= 0.0) break;  // breakdown (only at round-off): report the current residual
		const double Alpha = RZ / PQ;
		{
			double* PX = X.GetData(); double* PR = R.GetData(); const double* PP = P.GetData(); const double* PQv = AppliedDirection.GetData();
			VrgParallel::ForRange(N, ParBlock, [&](int32 Begin, int32 End)
			{
				for (int32 I = Begin; I < End; ++I) { PX[I] += Alpha * PP[I]; PR[I] -= Alpha * PQv[I]; }
			});
		}
		Result.IterationsUsed = It;
		Result.NormalizedResidual = Residual();
		if (Result.NormalizedResidual <= Settings.NormalizedResidualTarget) { Result.bConverged = true; break; }
		Precondition(R, Z);
		const double RZNew = Dot(R, Z);
		const double Beta = RZNew / RZ;
		RZ = RZNew;
		{
			double* PP = P.GetData(); const double* PZ = Z.GetData();
			VrgParallel::ForRange(N, ParBlock, [&](int32 Begin, int32 End) { for (int32 I = Begin; I < End; ++I) PP[I] = PZ[I] + Beta * PP[I]; });
		}
	}
	RemoveMean(X);
	return Result;
}

FPoissonSolveResult MultigridPoissonSolver::Solve(const FUniformAirGrid& Grid, const TArray<double>& RightHandSide,
	TArray<double>& OutField, FUniformCache& Cache, const FPoissonSolveSettings& Settings, FMultigridPoisson::EMode Mode)
{
	const int32 NumCells = Grid.GetNumCells();
	if (NumCells == 0 || RightHandSide.Num() != NumCells) return FPoissonSolveResult();
	if (OutField.Num() != NumCells) OutField.Init(0.0, NumCells);
	const FIntVector Dims(Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ());
	const double H = Grid.GetCellSizeM();

	uint32 Hash = 0;
	for (int32 I = 0; I < NumCells; ++I) Hash = HashCombineFast(Hash, Grid.bSolidCell.IsValidIndex(I) && Grid.bSolidCell[I] ? 0x9e37u + I : 1u);
	if (!Cache.Solver.IsBuilt() || Cache.Dims != Dims || Cache.CellSizeM != H || Cache.SolidHash != Hash)
	{
		Cache.Dims = Dims; Cache.CellSizeM = H; Cache.SolidHash = Hash;
		Cache.CompactOf.Init(INDEX_NONE, NumCells); Cache.GridOf.Reset();
		for (int32 Z = 0; Z < Dims.Z; ++Z) for (int32 Y = 0; Y < Dims.Y; ++Y) for (int32 X = 0; X < Dims.X; ++X)
			if (Grid.IsFluidCell(X, Y, Z)) { const int32 G = Grid.CellIndex(X, Y, Z); Cache.CompactOf[G] = Cache.GridOf.Add(G); }
		FMultigridPoisson::FLevelInput In;
		const int32 N = Cache.GridOf.Num();
		In.Volume.Init(H * H * H, N); In.BoxSize.Init(1, N);
		In.RowStart.Reserve(N + 1); In.RowStart.Add(0);
		for (int32 C = 0; C < N; ++C)
		{
			const int32 G = Cache.GridOf[C];
			const int32 X = G % Dims.X, Y = (G / Dims.X) % Dims.Y, Z = G / (Dims.X * Dims.Y);
			check(Grid.CellIndex(X, Y, Z) == G);
			In.Centre.Add(FVector(X + .5, Y + .5, Z + .5) * H); In.BoxMin.Add(FIntVector(X, Y, Z));
			const FIntVector Nb[6] = {{X - 1, Y, Z}, {X + 1, Y, Z}, {X, Y - 1, Z}, {X, Y + 1, Z}, {X, Y, Z - 1}, {X, Y, Z + 1}};
			for (const FIntVector& V : Nb)
				if (Grid.IsFluidCell(V.X, V.Y, V.Z)) { In.Col.Add(Cache.CompactOf[Grid.CellIndex(V.X, V.Y, V.Z)]); In.Trans.Add(H); }
			In.RowStart.Add(In.Col.Num());
		}
		Cache.Solver.Build(MoveTemp(In));
	}
	const int32 N = Cache.GridOf.Num();
	Cache.B.SetNumUninitialized(N); Cache.X.SetNumUninitialized(N);
	for (int32 C = 0; C < N; ++C) { Cache.B[C] = RightHandSide[Cache.GridOf[C]]; Cache.X[C] = OutField[Cache.GridOf[C]]; }
	const FPoissonSolveResult Result = Cache.Solver.Solve(Cache.B, Cache.X, Settings, Mode);
	for (int32 C = 0; C < N; ++C) OutField[Cache.GridOf[C]] = Cache.X[C];
	return Result;
}
