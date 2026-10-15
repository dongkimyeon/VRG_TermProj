// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

// 4단계 (2026-09-27): 공기 솔버를 GPU로 옮긴다. 계획은 docs/기록/확인절차/인계_2026-09-27.md "4단계 세부 계획".
//
// --- 정밀도: FP64 ---
// S7-4의 압력 Jacobi는 FP32였다. 이제는 FP64다. 300 K 근처 온도를 FP32로 두면 분해능이 약 3e-5 K라 스텝당 작은
// 온도 변화가 잘려 보존 자체가 깨진다. 커널은 메모리 대역폭 제한이라 FP64 처리량이 1/64이어도 비용은 대역폭 2배 수준이다.
// 확인(4a): 셰이더 FP64 덧셈·곱셈·나눗셈은 IEEE이고, 셰이더 컴파일러가 A*B+C를 FMA 한 번으로 합친다(반올림 한 번 차이).
// HLSL은 double sqrt/exp/floor를 지원하지 않으므로 그런 계산은 CPU(덕트별 ε)나 FP32(모델 계수)로 한다.
//
// --- 규약 5 ---
// 게임 스레드에서 부른다. 렌더 명령을 밀고 끝날 때까지 기다린다. 솔버 자료구조·액터를 렌더 스레드가 직접 보지 않는다.
namespace VrgAirGpu
{
	// 실제 GPU(NullRHI 아님, FP64 커널을 컴파일하는 SM6 이상)가 있는가. 자동화 테스트는 -NullRHI로 돌므로 그때는 false다.
	VRGRENDER_API bool IsAvailable();

	// 4a: FP64 연산이 셰이더에서 CPU와 같은 결과를 내는지 확인한다. Out[i] = frac_trunc((A[i]*B[i] + C) / D).
	VRGRENDER_API bool ProbeDoubleArithmetic(const TArray<double>& A, const TArray<double>& B, double C, double D,
		TArray<double>& Out, FString& OutError);
}

// ---- 4d: pressure Poisson, multigrid-preconditioned CG (the GPU twin of FMultigridPoisson, EMode::ConjugateGradient) ----

// One level of the hierarchy in compact (fluid-only) indexing, as FMultigridPoisson built it on the CPU.
struct FVrgMgLevelData
{
	int32 NumCells = 0;
	TArray<int32> RowStart;    // NumCells + 1
	TArray<int32> Col;
	TArray<double> Trans, Diag;
	TArray<int32> Parent;      // cell -> coarse cell (empty on the coarsest level)
	TArray<int32> ChildStart;  // NumCells + 1, children on the finer level (empty on level 0)
	TArray<int32> Child;
};

struct FVrgMgHierarchy
{
	TArray<FVrgMgLevelData> Levels;
	TArray<double> Volume0;    // level-0 cell volumes [m^3]
	TArray<int32> GridOf;      // level-0 compact cell -> grid cell
	int32 NumGridCells = 0;
	int32 PreSweeps = 2, PostSweeps = 2, CoarsestSweeps = 60;
};

struct FVrgGpuPoissonSettings
{
	double NormalizedResidualTarget = 1e-6;
	int32 MaxIterations = 100;
	double Omega = 2.0 / 3.0;
	// CG iterations queued per submission; the host reads the status once per chunk.
	int32 ChunkIterations = 8;
};

struct FVrgGpuPoissonResult
{
	bool bRan = false;            // false: GPU unavailable / not configured / bad input (caller falls back to the CPU)
	bool bConverged = false;
	bool bIncompatibleRightHandSide = false;
	int32 IterationsUsed = 0;
	int32 Submissions = 0;
	double NormalizedResidual = 0.0;
	double ReferenceScale = 0.0;
	double MeanRightHandSide = 0.0;
};

class VRGRENDER_API FVrgAirGpuPressure
{
public:
	FVrgAirGpuPressure();
	~FVrgAirGpuPressure();

	// Uploads the hierarchy (topology change only). Returns false without a GPU or with inconsistent data.
	bool Configure(const FVrgMgHierarchy& Hierarchy);
	bool IsConfigured() const;
	void Reset();

	// Grid-sized right-hand side and field (solid cells untouched), same contract as MultigridPoissonSolver::Solve.
	FVrgGpuPoissonResult Solve(const TArray<double>& RightHandSideGrid, TArray<double>& InOutFieldGrid,
		const FVrgGpuPoissonSettings& Settings);

	// Opaque GPU state (pooled buffers); declared public only so the graph helpers in the .cpp can name it.
	struct FImpl;
	FImpl& GetImpl() { return *Impl; }

private:
	TUniquePtr<FImpl> Impl;
};

// ---- 4b/4c: the whole uniform-grid air step resident on the GPU (FAirSolver drives it stage by stage) ----

struct FVrgAirGpuGrid
{
	FIntVector Dims = FIntVector::ZeroValue;
	double CellSizeM = 0.25, DensityKgM3 = 1.184, SpecificHeatJKgK = 1005.0, ReferenceTempK = 297.15;
	TArray<int32> Solid;           // per cell, 1 = solid
	TArray<double> WallDistanceM;  // per cell; empty = zeros (turbulence off)
	int32 NumFluidCells = 0;
};

// Fan ducts in GPU form (FAirSolverInput's duct faces plus what the CPU derives per step).
struct FVrgAirGpuDucts
{
	int32 NumDucts = 0;
	TArray<int32> Packed;          // per face: axis | (exhaust << 2)
	TArray<int32> Face, FluidCell, Duct;
	TArray<double> Velocity;       // per face [m/s]
	TArray<int32> IntakeStart;     // NumDucts + 1: intake faces of each duct, in input order
	TArray<int32> IntakeFaces;
	TArray<double> Params;         // 6 per duct: exhaust speed sum, eps, water T, fan dT, capacity W/K, coil flag
	bool HasFaces() const { return Face.Num() > 0; }
};

struct FVrgAirGpuVelocityInput
{
	double DtS = 0.02;
	double GravityZMs2 = -9.81;
	bool bBuoyancy = true;
	bool bTurbulence = false;
	double MolecularViscosityM2s = 0.0;
	int32 MomentumSubsteps = 1;
	const FVrgAirGpuDucts* Ducts = nullptr;
	FVrgGpuPoissonSettings Pressure;
};

struct FVrgAirGpuTransportInput
{
	double DtS = 0.02;
	int32 TemperatureSubsteps = 1;
	bool bDiffusion = false;
	double MolecularDiffusivityM2s = 0.0, InverseTurbulentPrandtl = 1.0;
	const FVrgAirGpuDucts* Ducts = nullptr;
	TArray<int32> HeatCell;  TArray<double> HeatW;   // non-zero fluid cells only
	TArray<int32> CoolCell;  TArray<double> CoolUAWK; // UA > 0 fluid cells only
	double CoolingSupplyTempK = 291.15;
	// Also compute what the next step's PreStep would (nu_t and maxima of the final state) and read it back with the
	// fields, so the next step needs no start-of-step sync. Same kernels on the same velocity, so the same values.
	bool bPrepareNext = false;
	bool bTurbulenceNext = false;
	double ChenXuNext = 0.0;
};

struct FVrgAirGpuTransportOutput
{
	double CellCoolingRemovedW = 0.0;   // sum of the applied cell-coil rates
	double DuctCoilRemovedJ = 0.0;      // over the step's sub-steps
	// With bPrepareNext: the next step's PreStep results.
	double NextMaxAbsVelocityMs = 0.0, NextMaxNuTM2s = 0.0, NextSumNuTM2s = 0.0;
};

class VRGRENDER_API FVrgAirGpuSolver
{
public:
	FVrgAirGpuSolver();
	~FVrgAirGpuSolver();

	// Grid, solids, wall distance and the pressure hierarchy. Topology changes only.
	bool Configure(const FVrgAirGpuGrid& Grid, const FVrgMgHierarchy& Hierarchy);
	bool IsConfigured() const;
	void Reset();

	// Replaces the resident fields (after a reset or any CPU-side edit).
	bool UploadState(const TArray<double>& TemperatureK, const TArray<double>& VelocityX, const TArray<double>& VelocityY,
		const TArray<double>& VelocityZ, const TArray<double>& PressurePa);

	// Stage A (sync): nu_t from the current velocity when bTurbulence, and the maxima the host needs before the step.
	bool PreStep(bool bTurbulence, double ChenXuCoefficient, double& OutMaxAbsVelocityMs, double& OutMaxNuTM2s, double& OutSumNuTM2s);

	// Stage B (sync): advection, buoyancy, [diffusion], walls/ducts, projection. Pressure result and max |u| after it.
	bool Velocity(const FVrgAirGpuVelocityInput& Input, FVrgGpuPoissonResult& OutPressure, double& OutMaxAbsVelocityMs);

	// Stage C (sync): temperature sub-steps, heat, cooling; downloads temperature and face velocities.
	bool Transport(const FVrgAirGpuTransportInput& Input, FVrgAirGpuTransportOutput& Out, TArray<double>& OutTemperatureK,
		TArray<double>& OutVelocityX, TArray<double>& OutVelocityY, TArray<double>& OutVelocityZ);

	struct FImpl;

private:
	TUniquePtr<FImpl> Impl;
};
