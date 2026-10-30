#include "Simulation/Hydraulics/SparseLinearSystem.h"
#include "Simulation/Core/ElapsedTimer.h"
THIRD_PARTY_INCLUDES_START
#include <Eigen/SparseLU>
THIRD_PARTY_INCLUDES_END

bool SolveSparseLinearSystem(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size, FSparseSolveCosts* Costs)
{
 FSparseSolveCosts Local; if (!Costs) Costs = &Local;
	if (Size <= 0 || Rhs.Num() != Size) return false;
 Eigen::SparseMatrix<double> Matrix(Size, Size);
 { FElapsedTimer Time(Costs->AssemblyMs);
 std::vector<Eigen::Triplet<double>> Triplets;
	Triplets.reserve(Entries.Num());
	for (const FHydraulicMatrixEntry& Entry : Entries)
	{
		if (Entry.Row < 0 || Entry.Row >= Size || Entry.Col < 0 || Entry.Col >= Size || !FMath::IsFinite(Entry.Value)) return false;
		Triplets.emplace_back(Entry.Row, Entry.Col, Entry.Value);
	}
	Matrix.setFromTriplets(Triplets.begin(), Triplets.end());
	Matrix.makeCompressed();
 }
	Eigen::SparseLU<Eigen::SparseMatrix<double>> Solver;
	{ FElapsedTimer Time(Costs->AnalyzeMs); Solver.analyzePattern(Matrix); }
 { FElapsedTimer Time(Costs->FactorMs); Solver.factorize(Matrix); }
	if (Solver.info() != Eigen::Success) return false;
	Eigen::Map<const Eigen::VectorXd> Input(Rhs.GetData(), Size);
	Eigen::VectorXd Solution;
 { FElapsedTimer Time(Costs->BacksolveMs); Solution = Solver.solve(Input); }
	if (Solver.info() != Eigen::Success || !Solution.allFinite()) return false;
	for (int32 Index = 0; Index < Size; ++Index) Rhs[Index] = Solution[Index];
	return true;
}
