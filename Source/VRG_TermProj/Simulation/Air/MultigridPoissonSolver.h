#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/JacobiPoissonSolver.h"

class FUniformAirGrid;

// Geometric (aggregation) multigrid for the pressure Poisson equation.
//
//   (1/V_i) * sum_j T_ij * (X_j - X_i) = B_i        (all-Neumann, T_ij = face area / centre distance)
//
// This is the same finite-volume form as the uniform MAC operator (T = h, V = h^3) and the octree TPFA
// operator (S5-B), so one hierarchy code serves both. Only cells that take part in the equation (fluid
// cells) are stored, in a compact linear index; faces are a CSR list built once per topology (D7).
//
// --- Why (2-1) ---
// Damped Jacobi removes a smooth error mode in O(N^(2/3)) sweeps. With racks blocked as solids the
// L_Test pressure stalled at a normalized residual of ~1e-4 after the 400-sweep cap every step.
// Coarse levels carry the smooth error, so the work to a fixed residual stops growing with the grid.
//
// --- D6 ---
// The smoother on every level is damped Jacobi with split read/write buffers (no Red-Black ordering),
// so the code runs unchanged on the octree and maps to the GPU the same way as before.
// Coarse levels: 2x2x2 aggregation of fluid cells; a coarse face keeps sum(T_f * d_f) / D_IJ over the
// open fine faces it covers (a blocked fine face contributes nothing), which is the rediscretized
// operator on the coarse grid, not the (too stiff) Galerkin product for constant prolongation.
//
// --- D5 ---
// Stops on the same normalized residual as JacobiPoissonSolver (max |L X - B| / max |B| over fluid
// cells) or on MaxIterations (counted in V-cycles / Krylov iterations), and reports failure.
//
// Plain C++. No UObject; safe on a worker thread.
class VRG_TERMPROJ_API FMultigridPoisson
{
public:
	enum class EMode : uint8 { VCycle, ConjugateGradient };

	// Fine level. Cells are compact (fluid only). Rows are symmetric: face (i,j) appears in both rows.
	// BoxMin/BoxSize are integer cell extents in finest units, used only to decide 2x2x2 aggregation.
	struct FLevelInput
	{
		TArray<double> Volume;
		TArray<FVector> Centre;
		TArray<FIntVector> BoxMin;
		TArray<int32> BoxSize;
		TArray<int32> RowStart;   // NumCells + 1
		TArray<int32> Col;
		TArray<double> Trans;
	};

	void Build(FLevelInput&& Fine);
	bool IsBuilt() const { return Levels.Num() > 0; }
	int32 GetNumLevels() const { return Levels.Num(); }
	int32 GetNumCells(int32 Level = 0) const { return Levels.IsValidIndex(Level) ? Levels[Level].Volume.Num() : 0; }

	// Solves in compact indexing. X is the initial guess and the result.
	FPoissonSolveResult Solve(const TArray<double>& B, TArray<double>& X, const FPoissonSolveSettings& Settings,
		EMode Mode = EMode::ConjugateGradient);

	// (L X)_i in compact indexing. For verification.
	void ApplyLaplacian(const TArray<double>& X, TArray<double>& Out) const;

private:
	struct FLevel
	{
		TArray<double> Volume, Diag;
		TArray<FVector> Centre;
		TArray<FIntVector> BoxMin;
		TArray<int32> BoxSize;
		TArray<int32> RowStart, Col;
		TArray<double> Trans;
		TArray<int32> Parent;      // cell -> coarse cell on the next level
		int32 AggregateUnit = 1;   // aggregation block edge of this level, finest units
		// Work buffers (A-form: A x = f, A = D - offdiag, f = -V*b).
		TArray<double> X, F, R, Tmp;
	};
	TArray<FLevel> Levels;

	void ApplyA(const FLevel& L, const TArray<double>& X, TArray<double>& Out) const;
	void Smooth(FLevel& L, int32 Sweeps, double Omega);
	void VCycle(int32 LevelIndex, double Omega);
	static void RemoveMean(TArray<double>& X);
};

namespace MultigridPoissonSolver
{
	// Uniform-grid entry with the same contract as JacobiPoissonSolver::Solve (full-size arrays; solid
	// cells are left untouched). Cache is rebuilt only when the grid dimensions or the solid mask change.
	struct VRG_TERMPROJ_API FUniformCache
	{
		FMultigridPoisson Solver;
		TArray<int32> CompactOf;   // grid cell -> compact fluid index or INDEX_NONE
		TArray<int32> GridOf;      // compact -> grid cell
		TArray<double> B, X;
		FIntVector Dims = FIntVector::ZeroValue;
		double CellSizeM = 0.0;
		uint32 SolidHash = 0;
	};

	VRG_TERMPROJ_API FPoissonSolveResult Solve(const FUniformAirGrid& Grid, const TArray<double>& RightHandSide,
		TArray<double>& OutField, FUniformCache& Cache, const FPoissonSolveSettings& Settings,
		FMultigridPoisson::EMode Mode = FMultigridPoisson::EMode::ConjugateGradient);
}
