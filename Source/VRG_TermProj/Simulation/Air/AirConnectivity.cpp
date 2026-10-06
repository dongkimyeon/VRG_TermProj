// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/AirConnectivity.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

int32 VrgAirConnectivity::LabelAirRegions(const FUniformAirGrid& Grid, TArray<int32>& OutLabel)
{
	const int32 NX = Grid.GetNumCellsX(), NY = Grid.GetNumCellsY(), NZ = Grid.GetNumCellsZ();
	OutLabel.Init(INDEX_NONE, Grid.GetNumCells());
	int32 NumRegions = 0;
	TArray<int32> Stack;
	for (int32 Z = 0; Z < NZ; ++Z)
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
			{
				const int32 Seed = Grid.CellIndex(X, Y, Z);
				if (OutLabel[Seed] != INDEX_NONE || !Grid.IsFluidCell(X, Y, Z)) continue;
				OutLabel[Seed] = NumRegions;
				Stack.Reset();
				Stack.Add(Seed);
				while (Stack.Num() > 0)
				{
					const int32 C = Stack.Pop(EAllowShrinking::No);
					const int32 CX = C % NX, CY = (C / NX) % NY, CZ = C / (NX * NY);
					const int32 N[6][3] = { {CX - 1, CY, CZ}, {CX + 1, CY, CZ}, {CX, CY - 1, CZ}, {CX, CY + 1, CZ}, {CX, CY, CZ - 1}, {CX, CY, CZ + 1} };
					for (const auto& P : N)
					{
						if (!Grid.IsFluidCell(P[0], P[1], P[2])) continue;
						const int32 I = Grid.CellIndex(P[0], P[1], P[2]);
						if (OutLabel[I] != INDEX_NONE) continue;
						OutLabel[I] = NumRegions;
						Stack.Add(I);
					}
				}
				++NumRegions;
			}
	return NumRegions;
}

int32 VrgAirConnectivity::CountSplitDucts(const FUniformAirGrid& Grid, const FAirSolverInput& Input)
{
	if (Input.NumDucts <= 0 || Input.DuctFaceFluidCell.Num() != Input.DuctFaceDuct.Num()) return 0;
	TArray<int32> Label;
	LabelAirRegions(Grid, Label);
	TArray<int32> RegionOfDuct;
	TArray<bool> bSplit;
	RegionOfDuct.Init(INDEX_NONE, Input.NumDucts);
	bSplit.Init(false, Input.NumDucts);
	for (int32 F = 0; F < Input.DuctFaceDuct.Num(); ++F)
	{
		const int32 D = Input.DuctFaceDuct[F], Cell = Input.DuctFaceFluidCell[F];
		if (!RegionOfDuct.IsValidIndex(D) || !Label.IsValidIndex(Cell)) continue;
		const int32 Region = Label[Cell];
		if (RegionOfDuct[D] == INDEX_NONE) RegionOfDuct[D] = Region;
		else if (RegionOfDuct[D] != Region) bSplit[D] = true;
	}
	int32 Count = 0;
	for (const bool b : bSplit) Count += b ? 1 : 0;
	return Count;
}
