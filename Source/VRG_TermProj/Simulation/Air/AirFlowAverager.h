// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"

// Time average of the air field for display (2026-10-27 streamline view). The flow here keeps moving from step to step
// (buoyancy and fan jets; CLAUDE.md: compare time averages), so a streamline of one instant changes shape every second.
// This keeps the last WindowS seconds as blocks of BlockS seconds: each block is the integral of the face velocities
// and the cell temperatures over the steps it covers (sum of field x dt, the steps' own dt on the shared clock, rule 4);
// the average is the sum of the kept blocks over their total time. Exact for any step size; the window moves in whole
// blocks. Plain C++, game thread (it reads the solver's grid between steps).

class VRG_TERMPROJ_API FAirFlowAverager
{
public:
	double WindowS = 30.0;
	double BlockS = 1.0;

	void Reset();
	// Adds one step of the field. A grid of another size starts the average over.
	void Add(const FUniformAirGrid& Grid, double DtS);
	// Seconds the average covers now (whole blocks only; 0 before the first block closes).
	double GetCoveredS() const { return CoveredS; }
	// The averaged field in a grid of the same size and solids (velocities, temperatures). False before the first block.
	bool Build(const FUniformAirGrid& Like, FUniformAirGrid& Out) const;

private:
	struct FBlock { TArray<double> Vx, Vy, Vz, T; double DurationS = 0.0; };
	TArray<FBlock> Blocks;       // oldest first
	FBlock Open;                 // the block being filled
	FIntVector Dims = FIntVector::ZeroValue;
	double CoveredS = 0.0;
	void StartOpen(const FUniformAirGrid& Grid);
};
