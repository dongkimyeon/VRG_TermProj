// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

// W3 (2026-10-27): the floor water step on the GPU (Shaders/Private/VrgFloorWater.usf), the twin of FFloorWater::Step.
// The CPU path stays the reference; the two are compared with a tolerance, not for bit equality. FP64 fields (the
// ledger is about mm of water over m^2: FP32 would lose it). State stays resident between steps; the host sends the
// cells its deposits changed and reads the whole state back after each step (the floods, totals and drawing read it).
// Game thread only; it pushes render commands and waits (rule 5: no solver structure is touched by the render thread).

struct FVrgFloorGpuConfig
{
	int32 Nx = 0, Ny = 0;
	double CellM = 0.025, GravityMs2 = 9.81, DryDepthM = 1e-7, PinDepthM = 0.0, ManningN = 0.0, Cfl = 0.45;
	int32 MaxSubsteps = 4000;
	TArray<double> BedM;      // Nx * Ny
	TArray<int32> Solid;      // Nx * Ny, 1 = wall
};

class VRGRENDER_API FVrgFloorWaterGpu
{
public:
	FVrgFloorWaterGpu();
	~FVrgFloorWaterGpu();

	// Same availability as the air FP64 path (a real SM6 GPU; false under -NullRHI).
	static bool IsAvailable();

	bool Configure(const FVrgFloorGpuConfig& Config);
	bool IsConfigured() const;
	void Reset();
	// Replaces the resident state.
	bool Upload(const TArray<double>& H, const TArray<double>& Hu, const TArray<double>& Hv);
	// One step of DtS: first sets the given cells' depths (the host's deposits), then CFL substeps covering DtS exactly,
	// then reads back the state, the depth created by clipping (sum over cells) and the number of substeps.
	bool Step(double DtS, const TArray<int32>& SetCells, const TArray<double>& SetDepths,
		TArray<double>& OutH, TArray<double>& OutHu, TArray<double>& OutHv, double& OutCreatedDepthSum, int32& OutSubsteps);

	struct FImpl;

private:
	TUniquePtr<FImpl> Impl;
};
