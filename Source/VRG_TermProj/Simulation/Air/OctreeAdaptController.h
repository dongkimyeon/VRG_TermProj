// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "OctreeAdaptController.generated.h"

// S5-D: temperature / velocity driven adaptation of the octree (2026-09-27).
//
// Decides, every IntervalS of simulated time, which root cells (the uniform grid cells the octree is built on)
// are refined. S5-C does the actual split / merge with conservative transfer and re-projection; this class only
// chooses. It reads the uniform root export (temperature K, face velocities m/s, solid mask) and nothing else.
//
// Indicators per fluid root cell (SI):
//   |grad T| [K/m]   central differences over fluid neighbours (one-sided next to walls and solids)
//   |u|      [m/s]   cell-centre speed (face velocities averaged)
//
// Anti-chattering (the S5-D requirement):
//  1. Hysteresis: refine when an indicator rises above its Refine threshold, merge only when BOTH fall below their
//     Coarsen thresholds (Coarsen < Refine). A value oscillating between the two thresholds changes nothing.
//  2. Hold time: a root keeps a new state at least MinHoldS before it may change back.
//  3. Fixed evaluation interval: topology changes happen at most once per IntervalS, not every step.
// Budget: at most MaxRefinedRoots refined (0 = no cap); when over, the largest normalised indicators win. The budget
// overrides the hold time (it is a hard limit).
//
// Plain C++, no UObject references: usable from worker threads with the solver.
USTRUCT(BlueprintType)
struct FOctreeAdaptSettings
{
	GENERATED_BODY()

	// Temperature gradient [K/m]: refine above, allow merging below the coarsen value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double RefineGradientKPerM = 8.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double CoarsenGradientKPerM = 4.0;

	// Air speed [m/s], same hysteresis.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double RefineSpeedMs = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double CoarsenSpeedMs = 0.5;

	// Minimum time a root keeps a new state [s].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double MinHoldS = 2.0;

	// Re-evaluation interval [s of simulated time].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0.0"))
	double IntervalS = 0.5;

	// At most this many refined roots (0 = no cap).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree|Adapt", meta = (ClampMin = "0"))
	int32 MaxRefinedRoots = 0;

	// Empty when usable; otherwise why not (Coarsen must be below Refine for the hysteresis to exist).
	FString Validate() const;
};

class VRG_TERMPROJ_API FOctreeAdaptController
{
public:
	void Reset(int32 NumRoots);

	// Evaluates at most once per Settings.IntervalS of NowS. Returns true when it proposes a different refined set;
	// then OutBoxesM holds solver-local metre AABBs covering the proposed roots (runs merged along X). Nothing is
	// adopted until Commit(): the caller commits only after the octree really adapted, and discards on failure
	// (e.g. leaf budget), so the controller never believes in a topology that does not exist.
	bool Update(const FUniformAirGrid& Grid, const FOctreeAdaptSettings& Settings, double NowS, TArray<FBox>& OutBoxesM);
	void Commit();
	void Discard() { bPending = false; }

	// Indicators of the last evaluation (fluid roots; 0 for solids).
	const TArray<double>& GetLastGradientKPerM() const { return GradientKPerM; }
	const TArray<double>& GetLastSpeedMs() const { return SpeedMs; }
	bool IsRefined(int32 Root) const { return Refined.IsValidIndex(Root) && Refined[Root] != 0; }
	int32 GetNumRefined() const { return NumRefined; }
	// Evaluations that changed the refined set, and root state flips in total, since Reset.
	int64 GetNumTopologyChanges() const { return NumTopologyChanges; }
	int64 GetNumRootFlips() const { return NumRootFlips; }

	static void MakeBoxes(const FUniformAirGrid& Grid, const TArray<uint8>& Flags, TArray<FBox>& OutBoxesM);

private:
	TArray<uint8> Refined, PendingRefined;
	bool bPending = false;
	double PendingS = 0.0;
	TArray<double> LastChangeS;
	TArray<double> GradientKPerM, SpeedMs;
	double LastEvalS = TNumericLimits<double>::Lowest();
	int32 NumRefined = 0;
	int64 NumTopologyChanges = 0, NumRootFlips = 0;

	void MeasureIndicators(const FUniformAirGrid& Grid);
};
