// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Water/LeakSpray.h"
#include "Templates/UniquePtr.h"

class FVrgFloorWaterGpu;

// W3 (Tools/누수_실시간_계획.md L3, 2026-10-27): the water on the room floor, as shallow water (Saint-Venant).
//
//   dh/dt + d(hu)/dx + d(hv)/dy = sources (what the spray delivered this step)
//   d(hu)/dt + d(hu^2 + g h^2 / 2)/dx + d(huv)/dy = -g h dB/dx - friction
//   d(hv)/dt + d(huv)/dx + d(hv^2 + g h^2 / 2)/dy = -g h dB/dy - friction
//
//  - Finite volumes on a uniform floor grid (default 2.5 cm). First-order HLL fluxes with the hydrostatic
//    reconstruction of Audusse et al. (2004, SIAM J. Sci. Comput. 25(6), 2050): non-negative depths under the CFL
//    limit and exactly well balanced (a lake at rest stays at rest, also around dry bumps). Each face flux is computed
//    once and applied to both cells with opposite signs (rule 6); fields are SoA arrays written into a second set
//    (ping-pong, rule 7). One integrated step is split into equal CFL substeps covering exactly that interval (rule 4).
//  - Bed friction: Manning, implicit in the velocity (stable as h -> 0). Epoxy floor n = 0.010 s/m^(1/3), ESTIMATE
//    (smooth concrete 0.011-0.013 in Chow 1959; a coated floor is taken a little smoother).
//  - Contact line: a film does not spread forever. The equilibrium puddle thickness is h_eq = 2 l_c sin(theta / 2),
//    l_c = sqrt(sigma / (rho g)) (de Gennes, Brochard-Wyart & Quere 2004, "Capillarity and Wetting Phenomena").
//    Epoxy contact angle 70 deg, ESTIMATE (published values for epoxy coatings scatter around 60-90 deg).
//    A wet cell thinner than h_eq does not advance into a dry cell: that face is a wall (pinned contact line).
//  - Walls, barriers and the room edge are impermeable. Equipment stands on the floor with a gap underneath: water
//    flows under it, and the depth under its footprint is its flood depth (input of W4).
//  - Bed slope 0 by default (2026-10-27 user decision: no drain, level floor, epoxy). No drain.
//  - Ledger: received = water on the floor (exact to round-off; clipped negative depths are counted and reported).
// Plain C++: no actor access (rule 5), SI units (rule 1).

struct VRG_TERMPROJ_API FFloorWaterSettings
{
	double CellM = 0.025;
	double ManningN = 0.010;                                 // s/m^(1/3), ESTIMATE (epoxy)
	double ContactAngleRad = FMath::DegreesToRadians(70.0);  // ESTIMATE (epoxy)
	bool bContactLinePinning = true;
	double Cfl = 0.45;                                       // of the 2D first-order limit 0.5
	double DryDepthM = 1.0e-7;                               // below this a cell has no velocity
	double GravityMs2 = 9.81;
	double WaterDensityKgM3 = 997.0;
	double SurfaceTensionNm = 0.072;
	FVector2D BedSlope = FVector2D::ZeroVector;              // dB/dx, dB/dy (level floor)
	int32 MaxSubsteps = 4000;
	// GPU path (VrgFloorWater.usf, the same scheme; the CPU path is the reference). 0 = CPU (default), 1 = GPU whenever
	// available, -1 = automatic: the GPU when the wet box covers at least GpuMinCells cells (small puddles stay on the CPU,
	// which only touches the wet box, while the GPU steps the whole floor and reads it back each step).
	// CPU by default: measured 2026-10-27 on the RTX 4060 laptop, a whole wet room costs the same on both (about 9-10 ms
	// a step; FP64 on this GPU and the per-step read-back), so the GPU buys nothing here yet. vrg.Water.Gpu switches it.
	int32 GpuMode = 0;
	int32 GpuMinCells = 20000;   // ESTIMATE (used only by the automatic mode)
};

// What the floor water did to a box standing on it (input of W4).
struct VRG_TERMPROJ_API FFloorFlood
{
	FString Name;
	TArray<int32> Cells;          // floor cells under its footprint
	double FirstWetS = -1.0;
	double MeanDepthM = 0.0, MaxDepthM = 0.0, PeakDepthM = 0.0;   // now, now, highest so far
};

struct VRG_TERMPROJ_API FFloorWaterTotals
{
	double ReceivedM3 = 0.0, StoredM3 = 0.0, ClippedM3 = 0.0;
	double Residual = 0.0;        // |received - stored - clipped| / received
	int32 WetCells = 0, Substeps = 0, LastSubsteps = 0;
	int32 GpuSteps = 0;           // steps taken on the GPU
	bool bLastStepGpu = false;
	double MaxSpeedMs = 0.0, MaxDepthM = 0.0;
};

class VRG_TERMPROJ_API FFloorWater
{
public:
	FFloorWater();
	~FFloorWater();
	FFloorWater(const FFloorWater&) = delete;
	FFloorWater& operator=(const FFloorWater&) = delete;
	// Whether a step would run on the GPU now (mode, availability, wet box size).
	bool WantsGpu() const;
	// A floor grid: origin (min corner, metres), cells, floor height. Everything dry and open, level bed.
	void Reset(const FVector2D& OriginM, int32 Nx, int32 Ny, double FloorZM, const FFloorWaterSettings& InSettings);
	// The room floor from the spray geometry: walls / barriers become solid cells, equipment footprints are recorded.
	bool ResetForRoom(const FSprayGeometry& Geometry, const FFloorWaterSettings& InSettings);

	// ---- set-up (tests, scenarios) ----
	void SetSolid(int32 I, int32 J, bool bSolid);
	void SetBed(int32 I, int32 J, double BedM);
	void SetState(int32 I, int32 J, double DepthM, double Hu = 0.0, double Hv = 0.0);   // not counted as received
	void AddFlood(const FString& Name, const TArray<int32>& Cells);

	// Water arriving at a point (world metres). False when the point is off the grid (the caller keeps it).
	bool AddWater(const FVector& PointM, double VolumeM3);
	// One integrated step [s] ending at NowS + DtS.
	void Step(double DtS, double NowS);

	// h_eq = 2 l_c sin(theta / 2)
	static double EquilibriumDepthM(const FFloorWaterSettings& S);

	int32 GetNx() const { return Nx; }
	int32 GetNy() const { return Ny; }
	double GetCellM() const { return Settings.CellM; }
	FVector2D GetOriginM() const { return Origin; }
	double GetFloorZM() const { return FloorZ; }
	int32 Index(int32 I, int32 J) const { return J * Nx + I; }
	const TArray<double>& GetDepthM() const { return H; }
	const TArray<double>& GetHu() const { return HU; }
	const TArray<double>& GetHv() const { return HV; }
	const TArray<double>& GetBedM() const { return B; }
	const TArray<uint8>& GetSolid() const { return Solid; }
	const FFloorWaterTotals& GetTotals() const { return Totals; }
	const TArray<FFloorFlood>& GetFloods() const { return Floods; }
	const FFloorWaterSettings& GetSettings() const { return Settings; }
	bool HasWater() const { return Totals.ReceivedM3 > 0.0 || Totals.StoredM3 > 0.0; }
	// Bounding box of the wet cells [I0, I1] x [J0, J1] (empty when I0 > I1).
	FIntRect GetWetBox() const { return FIntRect(WetI0, WetJ0, WetI1, WetJ1); }

private:
	FFloorWaterSettings Settings;
	FVector2D Origin = FVector2D::ZeroVector;
	double FloorZ = 0.0;
	int32 Nx = 0, Ny = 0;
	TArray<double> H, HU, HV, NH, NHU, NHV, B;
	TArray<uint8> Solid;
	// Face fluxes: x faces (Nx + 1) x Ny, y faces Nx x (Ny + 1). Mass, normal momentum seen by the low / high cell
	// (different only by the bed-slope term of the reconstruction), transverse momentum.
	TArray<double> FxH, FxNL, FxNH, FxT, FyH, FyNL, FyNH, FyT;
	TArray<FFloorFlood> Floods;
	FFloorWaterTotals Totals;
	int32 WetI0 = 1, WetJ0 = 1, WetI1 = 0, WetJ1 = 0;
	// GPU twin: configured for this grid / holding this state? Cells changed by deposits since its last step.
	TUniquePtr<FVrgFloorWaterGpu> Gpu;
	bool bGpuConfigured = false, bGpuStateCurrent = false;
	TArray<int32> GpuPendingCells;
	bool StepGpu(double DtS);

	void Substep(double DtS, int32 I0, int32 I1, int32 J0, int32 J1);
	double MaxWaveSpeed(int32 I0, int32 I1, int32 J0, int32 J1) const;
	void UpdateWetBox(int32 I0, int32 I1, int32 J0, int32 J1);
	void UpdateTotals(double NowS);
};
