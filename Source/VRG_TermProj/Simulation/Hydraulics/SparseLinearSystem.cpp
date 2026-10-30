#include "Simulation/Hydraulics/SparseLinearSystem.h"
#include "Simulation/Core/ElapsedTimer.h"
THIRD_PARTY_INCLUDES_START
#include <Eigen/SparseLU>
THIRD_PARTY_INCLUDES_END

struct FSparseLinearWorkspace::FImpl
{
 using FMatrix = Eigen::SparseMatrix<double>;
 static_assert(sizeof(FMatrix::StorageIndex) == sizeof(int32));
 Eigen::SparseLU<FMatrix> Solver;
 TArray<int32> Outer, Inner;
 int32 Size = 0;
 bool bAnalyzed = false;
};

FSparseLinearWorkspace::FSparseLinearWorkspace() = default;
FSparseLinearWorkspace::~FSparseLinearWorkspace() = default;
FSparseLinearWorkspace::FSparseLinearWorkspace(const FSparseLinearWorkspace&) {}
FSparseLinearWorkspace& FSparseLinearWorkspace::operator=(const FSparseLinearWorkspace& Other)
{
 if (this != &Other) Impl.Reset();
 return *this;
}
FSparseLinearWorkspace::FSparseLinearWorkspace(FSparseLinearWorkspace&&) noexcept = default;
FSparseLinearWorkspace& FSparseLinearWorkspace::operator=(FSparseLinearWorkspace&&) noexcept = default;

bool SolveSparseLinearSystem(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size, FSparseSolveCosts* Costs)
{
 FSparseLinearWorkspace Fresh;
 return Fresh.Solve(Entries, Rhs, Size, Costs, false); // preserved fresh-analysis reference
}

bool FSparseLinearWorkspace::Solve(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size,
 FSparseSolveCosts* Costs, bool bReuseAnalysis)
{
 FSparseSolveCosts Local;
 if (!Costs) Costs = &Local;
 if (Size <= 0 || Rhs.Num() != Size) return false;
 FImpl::FMatrix Matrix(Size, Size);
 {
  FElapsedTimer Time(Costs->AssemblyMs);
  std::vector<Eigen::Triplet<double>> Triplets;
  Triplets.reserve(Entries.Num());
  for (const auto& E : Entries)
  {
   if (E.Row < 0 || E.Row >= Size || E.Col < 0 || E.Col >= Size || !FMath::IsFinite(E.Value)) return false;
   Triplets.emplace_back(E.Row, E.Col, E.Value);
  }
  Matrix.setFromTriplets(Triplets.begin(), Triplets.end());
  Matrix.makeCompressed();
 }
 if (!Impl) Impl = MakeUnique<FImpl>();
 auto& P = *Impl;
 {
  FElapsedTimer Time(Costs->AnalyzeMs); // includes exact structure comparison/copy overhead
  const int32 Nonzeros = Matrix.nonZeros();
  const bool bSame = bReuseAnalysis && P.bAnalyzed && P.Size == Size && P.Outer.Num() == Size + 1 && P.Inner.Num() == Nonzeros
   && FMemory::Memcmp(P.Outer.GetData(), Matrix.outerIndexPtr(), (Size + 1) * sizeof(int32)) == 0
   && (Nonzeros == 0 || FMemory::Memcmp(P.Inner.GetData(), Matrix.innerIndexPtr(), Nonzeros * sizeof(int32)) == 0);
  if (!bSame)
  {
   P.Solver.analyzePattern(Matrix);
   P.Size = Size; P.Outer.SetNumUninitialized(Size + 1); P.Inner.SetNumUninitialized(Nonzeros);
   FMemory::Memcpy(P.Outer.GetData(), Matrix.outerIndexPtr(), (Size + 1) * sizeof(int32));
   if (Nonzeros) FMemory::Memcpy(P.Inner.GetData(), Matrix.innerIndexPtr(), Nonzeros * sizeof(int32));
   P.bAnalyzed = true; ++Costs->AnalyzeCalls;
  }
  else ++Costs->ReuseHits;
 }
 { FElapsedTimer Time(Costs->FactorMs); P.Solver.factorize(Matrix); }
 if (P.Solver.info() != Eigen::Success) return false;
 Eigen::Map<const Eigen::VectorXd> Input(Rhs.GetData(), Size);
 Eigen::VectorXd Solution;
 { FElapsedTimer Time(Costs->BacksolveMs); Solution = P.Solver.solve(Input); }
 if (P.Solver.info() != Eigen::Success || !Solution.allFinite()) return false;
 for (int32 I = 0; I < Size; ++I) Rhs[I] = Solution[I];
 return true;
}
