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
};

// Duplicate triplets are summed. Rhs is replaced only on a successful finite solve.
bool SolveSparseLinearSystem(const TArray<FHydraulicMatrixEntry>& Entries, TArray<double>& Rhs, int32 Size, FSparseSolveCosts* Costs = nullptr);
