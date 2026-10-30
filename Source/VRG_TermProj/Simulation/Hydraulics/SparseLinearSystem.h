#pragma once
#include "CoreMinimal.h"

struct FHydraulicMatrixEntry
{
	int32 Row, Col;
	double Value;
};

struct FSparseSolveCosts
{
 double AssemblyMs = 0, AnalyzeMs = 0, FactorMs = 0, BacksolveMs = 0;
 int32 AnalyzeCalls = 0, ReuseHits = 0;
};

// Duplicate triplets are summed. Rhs is replaced only on a successful finite solve.
bool SolveSparseLinearSystem(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size, FSparseSolveCosts* Costs = nullptr);


// Owns symbolic analysis only; numerical values/factorization are always refreshed.
// Copies start empty so network rollback/snapshots never share mutable solver workspaces.
class FSparseLinearWorkspace
{
public:
 FSparseLinearWorkspace();
 ~FSparseLinearWorkspace();
 FSparseLinearWorkspace(const FSparseLinearWorkspace&);
 FSparseLinearWorkspace& operator=(const FSparseLinearWorkspace& Other);
 FSparseLinearWorkspace(FSparseLinearWorkspace&&) noexcept;
 FSparseLinearWorkspace& operator=(FSparseLinearWorkspace&&) noexcept;
 bool Solve(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size,
   FSparseSolveCosts* Costs = nullptr, bool bReuseAnalysis = true);
private:
 struct FImpl;
 TUniquePtr<FImpl> Impl;
};
