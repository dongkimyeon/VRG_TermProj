// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/MultigridPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Simulation/Air/OctreeAirSolver.h"
#include "Simulation/Air/OctreeAdaptController.h"
#include "VrgAirGpu.h"
#include "AirSolver.generated.h"

// S3: 균일 격자 공기 해석.
//
// 비압축 Navier-Stokes를 투영법으로 푼다. 한 스텝은 다음 순서다:
//   1. 속도 이류 (반라그랑주)
//   2. 부력 (Boussinesq)
//   3. 벽 경계 적용 (벽면 법선 속도 0)
//   4. 압력 Poisson 풀이 -> 투영 (발산 제거)
//   5. 온도 이류 (보존형 플럭스 풍상차분)
//   6. 체적 열원 주입
//
// --- 온도 이류는 보존형이어야 한다 ---
// 처음에는 온도도 반라그랑주로 옮겼는데, 막힌 상자에서 10초 만에 열이 7.7% 늘었다.
// 반라그랑주는 안정하고 값이 튀지 않지만 적분을 보존하지 않는다.
// 벽 근처에서 거슬러 올라간 점이 도메인 밖으로 나가면 경계값을 물고 오는데,
// 그게 뜨거운 값을 복제해 열을 만들어 낸다.
//
// 그래서 온도는 면 플럭스 형태로 옮긴다:
//   T_new[i] = T[i] - dt/h * ( F_{i+1/2} - F_{i-1/2} + ... )
//   F_{i+1/2} = u * (u >= 0 ? T[i] : T[i+1])        (풍상차분)
// 면 플럭스를 한 번만 계산해 인접한 두 셀에 부호만 바꿔 적용한다 (규약 6).
// 그러면 내부 플럭스가 전부 상쇄되고 벽면은 u=0이라 플럭스가 0이므로,
// 총 열량이 반올림 수준까지 정확히 보존된다.
// 속도는 여전히 반라그랑주로 옮긴다. 운동량은 장부에 없으므로 보존 대상이 아니다.
//
// --- D1: 열은 체적 열원으로만 넣는다 ---
// 랙 발열을 셀에 W로 넣고, 그 셀의 온도를 HeatW*dt/(rho*cp*V)만큼 올린다.
// 배기 온도를 T_in + HeatW/(MassFlowKgs*Cp)로 덮어쓰지 않는다.
// 두 방식을 같이 쓰면 열이 중복 계상된다. 랙이 여러 부품으로 쪼개져 있어도
// 여기에 넣을 때는 반드시 합산해 하나의 열원으로 준다.
//
// --- D2: 부력은 절대온도로 ---
// Boussinesq 근사에서 이상기체의 열팽창계수는 1/T_ref다. 섭씨로 계산하면
// 부호와 크기가 모두 틀린다. 온도는 K로만 들고 있는다 (규약 2).
//
// --- 경계 ---
// 사방이 막힌 상자다. 벽면 법선 속도를 0으로 둔다.
// 방 안의 고체(랙 몸통 등)는 bSolidCell로 표시한다. 고체 셀에 닿는 면은
// 벽과 똑같이 다뤄서 공기가 지나가지 못한다.
//
// 열은 두 경로로 드나든다: 체적 열원으로 들어오고 냉각 코일(CRAH)로 나간다.
// 둘 다 장부에 적으므로 유입 - 유출 - 축적 = 0이 성립한다.
// 벽을 통한 급배기(질량이 드나드는 경계)는 아직 없다.
//
// --- Fan ducts (2026-09-25) ---
// Rack fans drive air through the solid rack body: intake faces on its front, exhaust faces on its rear, each
// with a prescribed normal velocity (FAirSolverInput::DuctFace*). Mass stays inside the box (every duct has
// equal intake and exhaust volume flow) and the enthalpy taken in at the front is handed back at the rear
// unchanged, so the ledger above still holds. Uniform path only; the octree path reports them as rejected.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않으므로 워커 스레드에서 돌릴 수 있다.

// 매 스텝 바깥에서 주어지는 경계 조건.
USTRUCT(BlueprintType)
struct FAirSolverInput
{
	GENERATED_BODY()

	// 셀별 체적 열원 [W]. 길이가 셀 수와 다르면 열원이 없는 것으로 본다.
	// D1: 랙이 부품으로 쪼개져 있어도 여기 넣을 때는 합산해 하나로 준다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	TArray<double> CellHeatW;

	// 중력 가속도 [m/s^2]. 부력이 작용할 방향이며 아래(-Z)가 음수다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double GravityZMs2 = -9.81;

	// 부력을 끄고 순수 이류만 보고 싶을 때 쓴다. 해석해 검증에서 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	bool bEnableBuoyancy = true;

	// 셀별 냉각 전도도 [W/K]. CRAH 코일이 공기에서 열을 빼 가는 몫이다.
	//   Heat_removed = UA * (T_cell - SupplyTempK)
	// 길이가 셀 수와 다르면 냉각이 없는 것으로 본다.
	//
	// 유량 기반 장치가 아니라 뭉뚱그린 코일이다. 라디에이터와 달리 포화하지 않으므로
	// UA를 크게 주면 그만큼 계속 빼 간다. 실제 CRAH 용량을 모사하려면 운전점에서
	// 역산해서 넣어야 한다 (열수지 장부는 어느 쪽이든 맞는다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	TArray<double> CellCoolingUAWK;

	// 냉각 코일의 공급 온도 [K]. 공기가 이 온도 아래로는 내려가지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double CoolingSupplyTempK = 291.15;

	// ---- Fan ducts: air driven through a solid body (rack through-flow) ----
	// A duct is a set of intake faces and exhaust faces on the boundary between fluid cells and a solid body.
	// Every duct face has a prescribed normal velocity [m/s, signed along +axis] that the projection keeps
	// (like a wall, but with a known flux): the fans push air into the body at the intake faces and out of it
	// at the exhaust faces. The caller gives equal volume flow on both sides of a duct, so the pressure
	// equation stays compatible.
	// Energy (rule 6): the enthalpy the intake faces carry out of the fluid is computed once per step and the
	// exhaust faces return exactly that enthalpy (exhaust temperature = flow-weighted intake temperature).
	// Heat added inside the body still enters only as a volumetric source (CellHeatW, D1); the duct never
	// sets an exhaust temperature from HeatW / (mdot * cp).
	// Flat SoA arrays of equal length (rule 7). Axis 0/1/2 = X/Y/Z face array; FaceIndex indexes that array;
	// FluidCell is the fluid cell on the face; Duct is the duct id (< NumDucts).
	TArray<uint8> DuctFaceAxis;
	TArray<int32> DuctFaceIndex;
	TArray<int32> DuctFaceFluidCell;
	TArray<int32> DuctFaceDuct;
	TArray<uint8> DuctFaceIsExhaust;
	TArray<double> DuctFaceVelocityMs;
	int32 NumDucts = 0;

	// Optional cooling coil inside a duct (CRAH unit, 2026-09-27). Length NumDucts, or empty for no coils.
	// UA 0 = plain duct. The coil sees the duct's mixed intake air and water at DuctCoilWaterTempK:
	//   eps = 1 - exp(-UA / (mdot * cp)),  T_exhaust = T_mix - eps * (T_mix - T_water)
	// Water-side capacity rate is taken as much larger than the air side (C_r -> 0): chilled-water flow limits
	// are not modelled. Like the radiator the coil saturates as UA grows; at zero flow it removes nothing (D2).
	// The removed heat is computed once from the same exhaust temperature the faces carry and booked as air
	// cooling (rule 6).
	TArray<double> DuctCoilUAWK;
	TArray<double> DuctCoilWaterTempK;

	// Optional fan electric input per duct [W] (length NumDucts, or empty). It all ends up as heat in the air the
	// duct delivers, added after the coil (draw-through fan): T_exhaust += P / (mdot * cp). Booked once as an
	// air-side input (GetDuctFanHeatJ). A duct carrying no flow adds nothing (its fan is not running).
	TArray<double> DuctFanHeatW;

	bool HasDucts() const { return DuctFaceIndex.Num() > 0; }
};

// 표시용 읽기 전용 사본. 렌더/UI는 이것만 본다 (규약 5).
USTRUCT(BlueprintType)
struct FAirSolverSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double SimTimeS = 0.0;

	// 셀 중심 온도 [K]. 평탄 배열이며 격자와 같은 인덱스 규칙을 쓴다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	TArray<double> TemperatureK;

	// 셀 중심 속도 크기 [m/s]. 면 속도를 셀 중심으로 평균한 값이다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	TArray<double> SpeedMs;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double MinTemperatureK = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double MaxTemperatureK = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double MaxSpeedMs = 0.0;

	// 투영 뒤 남은 최대 발산 [1/s]. 0에 가까워야 한다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double MaxAbsDivergencePerS = 0.0;

	// 발산을 (최대 속도 / 셀 크기)로 정규화한 무차원 값. 비압축 검사 기준.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double NormalizedDivergence = 0.0;

	// 총 유입 열 [J] 대비 열수지 잔차의 무차원 값. 보존 검사 기준.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double NormalizedEnergyResidual = 0.0;

	// 마지막 스텝에 냉각 코일이 빼 간 열 [W].
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double CoolingRemovedW = 0.0;

	// Part of CoolingRemovedW taken by duct coils (CRAH units) [W].
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double DuctCoilRemovedW = 0.0;

	// 고체가 막고 있지 않은 셀 수. 온도 통계는 이 셀들만 본다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	int32 NumFluidCells = 0;

	// D5: 압력 풀이가 수렴했는지. false면 속도장을 믿으면 안 된다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	bool bPressureConverged = true;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	int32 PressureIterations = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double PressureNormalizedResidual = 0.0;

	// CFL 수. 1을 넘으면 반라그랑주가 한 스텝에 한 셀을 넘어 건너뛴다.
	// 반라그랑주는 그래도 터지지 않지만 결과가 뭉개진다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double CourantNumber = 0.0;

	// Fan ducts of the last step: total volume flow through exhaust faces [m^3/s] and face count.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double DuctFlowM3s = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	int32 NumDuctFaces = 0;

	// True when duct faces were given but not applied (invalid arrays, or the octree path which does not
	// support ducts yet). The flow field then has no fan-driven through-flow: do not trust it for racks.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	bool bDuctsRejected = false;
};

// Chen & Xu (1998) zero-equation turbulence model for indoor air (2026-09-27; Q. Chen, W. Xu, "A zero-equation
// turbulence model for indoor airflow simulation", Energy and Buildings 28 (1998) 137-144):
//   nu_t = 0.03874 * |V| * l,   l = distance to the nearest wall (AirWallDistance)
// Momentum diffuses with nu_mol + nu_t (Laplacian form: the transpose term of the stress tensor is left out; tangential
// no-slip at walls and solids, ghost value -u half a cell away). Temperature diffuses with alpha_mol + nu_t / Pr_t as a
// face flux inside the conservative transport (rule 6; walls, solids and duct faces carry no diffusive flux).
// Both are explicit and sub-stepped for stability. Off, nothing of this runs and the solver is unchanged bit for bit.
// Uniform path only; the octree path ignores it.
struct FAirTurbulenceSettings
{
	bool bEnabled = false;
	double ChenXuCoefficient = 0.03874;
	// Air at 300 K (Incropera & DeWitt, Fundamentals of Heat and Mass Transfer, Table A.4): nu 15.89e-6, alpha 22.5e-6 m^2/s.
	double MolecularViscosityM2s = 15.89e-6;
	double MolecularDiffusivityM2s = 22.5e-6;
	// Turbulent Prandtl number, the common assumption for air (not from Chen & Xu).
	double TurbulentPrandtl = 0.9;
};

class VRG_TERMPROJ_API FAirSolver
{
public:
	void Initialize(const FAirGridParams& InGridParams, double InitialTempK);

	// 고정 스텝 한 번. 규약 4대로 바깥 시계가 주는 구간을 그대로 적분한다.
	void Step(const FAirSolverInput& Input, double DeltaTimeS);

	FAirSolverSnapshot MakeSnapshot(double SimTimeS) const;
	// Uniform root grid remains the coupling/render interface; octree leaves integrate the physics.
	bool ConfigureOctree(const FFixedOctreeSettings& Settings, const TArray<FBox>& BoxesM, FString& Error);
	// Manual refinement boxes (solver-local m). With automatic adaptation on, the automatic boxes are added to them.
	bool AdaptOctree(const TArray<FBox>& BoxesM, double DtS, FString& Error);
	// S5-D: automatic adaptation from temperature / velocity indicators (see FOctreeAdaptController), evaluated inside
	// Step() on the octree path. Invalid settings (no hysteresis) are refused and leave it off.
	bool SetOctreeAutoAdapt(bool bEnable, const FOctreeAdaptSettings& Settings, FString& Error);
	bool IsOctreeAutoAdapting() const { return bAutoAdapt; }
	const FOctreeAdaptController& GetOctreeAdaptController() const { return AdaptController; }
	int64 GetNumAutoAdaptFailures() const { return NumAutoAdaptFailures; }
	const FString& GetLastAutoAdaptError() const { return LastAutoAdaptError; }
	bool IsUsingOctree() const { return bUseOctree; }
	const FOctreeAirSolver& GetOctreeSolver() const { return OctreeSolver; }

	// ---- 파이프라인 단계 ----
	// Step()이 이 순서로 부른다. 단계 하나만 따로 검증하려고 밖에서 부를 수도 있다
	// (막힌 상자에서는 균일 관통류가 불가능해서, 이류만 보려면 투영을 건너뛰어야 한다).
	void AdvectVelocity(double DeltaTimeS);
	void ApplyBuoyancy(const FAirSolverInput& Input, double DeltaTimeS);
	void ApplyWallBoundary();
	void ProjectToDivergenceFree(double DeltaTimeS);
	void AdvectTemperature(double DeltaTimeS);
	void InjectHeat(const FAirSolverInput& Input, double DeltaTimeS);
	void ApplyCooling(const FAirSolverInput& Input, double DeltaTimeS);
	// Turbulence stages (only called when enabled): nu_t from the current velocity, then momentum diffusion.
	void UpdateTurbulentViscosity();
	void DiffuseMomentum(double DeltaTimeS);

	FAirTurbulenceSettings& GetMutableTurbulenceSettings() { return Turbulence; }
	const FAirTurbulenceSettings& GetTurbulenceSettings() const { return Turbulence; }
	const TArray<double>& GetWallDistanceM() const { return WallDistanceM; }
	// Per cell [m^2/s], 0 in solids; valid after UpdateTurbulentViscosity.
	const TArray<double>& GetTurbulentViscosityM2s() const { return TurbulentViscosityM2s; }
	double GetMaxTurbulentViscosityM2s() const { return LastMaxTurbulentViscosityM2s; }
	double GetMeanTurbulentViscosityM2s() const { return LastMeanTurbulentViscosityM2s; }
	int32 GetLastMomentumDiffusionSubsteps() const { return LastMomentumDiffusionSubsteps; }

	const FUniformAirGrid& GetGrid() const { return Grid; }
	// Any caller may edit the fields through this; the GPU copy is re-uploaded before the next GPU step.
	FUniformAirGrid& GetMutableGrid() { bGpuStateDirty = true; return Grid; }

	FPoissonSolveSettings& GetMutablePressureSettings() { return PressureSettings; }
	const FPoissonSolveResult& GetLastPressureResult() const { return LastPressureResult; }

	// 직전 스텝에서 압력 Poisson에 쓴 시간 [ms]. 계측용이다 (D10: 측정값).
	// 전역 누산기 대신 멤버에 담는 이유는 규약 5다 — 이 클래스는 워커 스레드에서
	// 돌 수 있어야 하므로 게임 스레드 전용 전역을 건드리면 안 된다.
	// 게임 스레드 쪽(액터)이 이 값을 읽어 프레임 누산기에 더한다.
	double GetLastPressureSolveMs() const { return LastPressureSolveMs; }

	// S7-4: 압력 스윕을 컴퓨트 셰이더로 돌린다.
	//
	// **켜면 이 솔버는 게임 스레드 전용이 된다.** GPU 경로가 렌더 명령을 밀고
	// 결과를 기다리기 때문이다. 워커 스레드에서 돌리려면 꺼야 한다.
	// 끄면 기존 CPU FP64 경로 그대로이며, 그쪽이 검증 기준이자 폴백이다.
	void SetUseGpuPressure(bool bInUse) { bUseGpuPressure = bInUse; }

	// 2-1: multigrid-preconditioned CG (CPU FP64, damped-Jacobi smoother). Default on.
	// When on it takes precedence over the GPU Jacobi path. Off = the S3 damped-Jacobi reference.
	void SetUseMultigridPressure(bool bInUse) { bUseMultigridPressure = bInUse; }
	// 4d (2026-09-27): the multigrid-CG pressure solve on the GPU in FP64 (FVrgAirGpuPressure), same algorithm and hierarchy
	// as the CPU one; falls back to the CPU when there is no GPU or the GPU solve did not run. Game thread only when on.
	void SetUseGpuMultigrid(bool bInUse) { bUseGpuMultigrid = bInUse; }
	// 4b/4c (2026-09-27): the whole uniform-grid step resident on the GPU in FP64 (FVrgAirGpuSolver). The CPU grid stays a
	// mirror (temperature and face velocities are downloaded every step), so coupling, rendering and the ledger are unchanged.
	// Falls back to the CPU step when there is no GPU or a stage fails. Game thread only when on.
	void SetUseGpuStep(bool bInUse) { bUseGpuStep = bInUse; }
	bool IsUsingGpuStep() const { return bUseGpuStep; }
	bool DidLastStepRunOnGpu() const { return bLastStepRanOnGpu; }
	bool IsUsingGpuMultigrid() const { return bUseGpuMultigrid; }
	bool IsUsingMultigridPressure() const { return bUseMultigridPressure; }
	// Pressure solves since Initialize and how many of them missed the target (D5).
	int64 GetNumPressureSolves() const { return NumPressureSolves; }
	int64 GetNumPressureFailures() const { return NumPressureFailures; }
	bool IsUsingGpuPressure() const { return bUseGpuPressure; }

	// 직전 압력 풀이가 실제로 GPU에서 돌았는지. GPU를 켜도 실패하면 CPU로 떨어진다.
	bool DidLastPressureRunOnGpu() const { return bLastPressureRanOnGpu; }

	// 지금 상태에서 CPU FP64 해와 GPU FP32 해를 따로 구해 비교한다.
	// 검증 규칙대로 비트 동일성이 아니라 허용 오차로 본다. 결과는 로그에 남긴다.
	// 원래 압력장을 건드리지 않는다.
	bool CompareGpuAndCpuPressure(double& OutNormalizedDifference, FString& OutSummary);

	// 고체 셀 표시를 갱신한 뒤 부른다. 압력 해와 버퍼를 다시 맞춘다.
	void OnSolidCellsChanged();

	// ---- 열수지 (규약: 축적량을 포함한다) ----
	double GetEnergyInJ() const { return EnergyInJ; }

	// 냉각 코일이 빼 간 누적 열 [J].
	double GetEnergyOutJ() const { return EnergyOutJ; }

	// 초기 상태 대비 공기에 쌓인 에너지 [J].
	double GetEnergyStoredJ() const;

	// 유입 - 유출 - 축적 [J]. 0에 가까워야 한다.
	double GetEnergyResidualJ() const;

	double GetNormalizedEnergyResidual() const;

	// True when the last step was given fan ducts it could not apply (see FAirSolverSnapshot::bDuctsRejected).
	bool WereDuctsRejected() const { return bLastDuctsRejected; }

	// Heat duct coils (CRAH units) removed in the last step [W]; included in the cooling ledger.
	double GetLastDuctCoilRemovedW() const { return LastDuctCoilRemovedW; }

	// Duct fan heat: last step [W] and cumulative [J]. Part of GetEnergyInJ (an input to the air, like CellHeatW).
	double GetLastDuctFanHeatW() const { return LastDuctFanHeatW; }
	double GetDuctFanHeatJ() const { return DuctFanHeatTotalJ; }

	// 마지막 스텝의 CFL 수.
	double GetCourantNumber() const { return LastCourantNumber; }

	// Wall time per pipeline stage, summed since the last reset [ms] (measurement, D10; members, rule 5).
	// Stages: 0 velocity advection, 1 buoyancy, 2 wall/ducts, 3 projection (incl. pressure), 4 temperature
	// transport, 5 heat + cooling, 6 turbulence (viscosity + momentum diffusion). Uniform path only.
	void ResetStageTimes() { for (double& T : StageMs) T = 0.0; StageSteps = 0; }
	FString DescribeStageTimes() const;

	// Temperature-transport sub-steps used in the last step (1 unless the flow got fast for the cell size).
	int32 GetLastTemperatureSubsteps() const { return LastTemperatureSubsteps; }

private:
	FOctreeAirSolver OctreeSolver;
	FFixedOctreeSettings OctreeSettings;
	TArray<FBox> OctreeBoxesM, OctreeUserBoxesM;
	bool bAutoAdapt = false;
	FOctreeAdaptSettings AdaptSettings;
	FOctreeAdaptController AdaptController;
	double AdaptElapsedS = 0.0;
	int64 NumAutoAdaptFailures = 0;
	FString LastAutoAdaptError;
	bool AdaptToBoxes(const TArray<FBox>& BoxesM, double DtS, FString& Error);
	bool bUseOctree = false, bOctreeNeedsImport = false;
	FUniformAirGrid Grid;
	FPoissonSolveSettings PressureSettings;
	FPoissonSolveResult LastPressureResult;
	double LastPressureSolveMs = 0.0;
	bool bUseGpuPressure = false;
	bool bUseMultigridPressure = true;
	bool bUseGpuMultigrid = false;
	TUniquePtr<FVrgAirGpuPressure> GpuMultigrid;
	bool bGpuMultigridFallbackLogged = false;
	bool bUseGpuStep = false, bLastStepRanOnGpu = false, bGpuStepFallbackLogged = false;
	bool bGpuStateDirty = true, bGpuTopologyDirty = true, bGpuHasWallDistance = false;
	TUniquePtr<FVrgAirGpuSolver> GpuStep;
	// The previous GPU step already computed this step's PreStep values (valid while nothing re-uploaded the state).
	bool bGpuPreparedValid = false, bGpuPreparedTurbulence = false;
	double GpuPreparedChenXu = 0.0, GpuPreparedMaxU = 0.0, GpuPreparedMaxNu = 0.0, GpuPreparedSumNu = 0.0;
	bool StepOnGpu(const FAirSolverInput& Input, double DeltaTimeS);
	int64 NumPressureSolves = 0, NumPressureFailures = 0;
	MultigridPoissonSolver::FUniformCache MultigridCache;
	bool bLastPressureRanOnGpu = false;

	// GPU 경로용 임시 버퍼. 매 스텝 재할당하지 않으려고 멤버로 둔다.
	TArray<float> GpuRightHandSide;
	TArray<float> GpuField;
	TArray<uint32> GpuFluidMask;

	// 압력 Poisson을 푼다. GPU 경로와 CPU 경로를 여기서 가른다.
	void SolvePressure();
	bool SolvePressureOnGpu();

	double InitialTemperatureK = 297.15;
	double EnergyInJ = 0.0;
	double EnergyOutJ = 0.0;
	double LastCoolingRemovedW = 0.0;
	double LastCourantNumber = 0.0;

	// 압력 풀이에 쓰는 버퍼. 매 스텝 재할당하지 않으려고 멤버로 들고 있다.
	TArray<double> DivergencePerS;
	TArray<double> PressureScratch;

	// 온도 이류의 면 플럭스 [K*m/s]. 매 스텝 재할당하지 않으려고 멤버로 들고 있다.
	TArray<double> TemperatureFluxX;
	TArray<double> TemperatureFluxY;
	TArray<double> TemperatureFluxZ;

	// 면 속도를 셀 중심으로 평균한 값 [m/s].
	FVector CellCenterVelocityMs(int32 X, int32 Y, int32 Z) const;

	// Ducts of the step in progress (set by Step, cleared after). ApplyWallBoundary re-imposes their face
	// velocities; AdvectTemperature applies their enthalpy transfer.
	const FAirSolverInput* ActiveDucts = nullptr;
	TArray<double> DuctIntakeEnthalpy, DuctExhaustSpeedSum;
	double LastDuctFlowM3s = 0.0;
	double DuctCoilRemovedJ = 0.0, LastDuctCoilRemovedW = 0.0;  // this step (sum over sub-steps) / its rate
	double DuctFanHeatStepJ = 0.0, LastDuctFanHeatW = 0.0, DuctFanHeatTotalJ = 0.0;
	int32 LastNumDuctFaces = 0;
	bool bLastDuctsRejected = false;
	int32 LastTemperatureSubsteps = 1;
	double StageMs[7] = {0, 0, 0, 0, 0, 0, 0};
	FAirTurbulenceSettings Turbulence;
	TArray<double> WallDistanceM, TurbulentViscosityM2s;
	bool bWallDistanceDirty = true;
	double LastMaxTurbulentViscosityM2s = 0.0, LastMeanTurbulentViscosityM2s = 0.0;
	int32 LastMomentumDiffusionSubsteps = 0;
	void DiffuseFaceComponent(int32 Axis, double DeltaTimeS);
	int64 StageSteps = 0;
	bool bOctreeDuctWarningLogged = false;
	TArray<double>& FluxArray(uint8 Axis) { return Axis == 0 ? TemperatureFluxX : Axis == 1 ? TemperatureFluxY : TemperatureFluxZ; }
	TArray<double>& VelocityArray(uint8 Axis) { return Axis == 0 ? Grid.VelocityXMs : Axis == 1 ? Grid.VelocityYMs : Grid.VelocityZMs; }

public:
	// Checks duct arrays against this grid (lengths, indices, fluid cells, finite velocities, every duct with
	// flow has both intake and exhaust faces). Empty error = valid.
	FString ValidateDucts(const FAirSolverInput& Input) const;
};
