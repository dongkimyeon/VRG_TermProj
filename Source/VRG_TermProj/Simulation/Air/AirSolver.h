// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"
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
// 지금은 사방이 막힌 상자다. 벽면 법선 속도를 0으로 두므로 질량과 에너지가
// 정확히 보존되고, 그래서 보존 검사가 의미를 갖는다.
// 방 급배기(CRAH)는 S4에서 붙인다.
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
};

class VRG_TERMPROJ_API FAirSolver
{
public:
	void Initialize(const FAirGridParams& InGridParams, double InitialTempK);

	// 고정 스텝 한 번. 규약 4대로 바깥 시계가 주는 구간을 그대로 적분한다.
	void Step(const FAirSolverInput& Input, double DeltaTimeS);

	FAirSolverSnapshot MakeSnapshot(double SimTimeS) const;

	// ---- 파이프라인 단계 ----
	// Step()이 이 순서로 부른다. 단계 하나만 따로 검증하려고 밖에서 부를 수도 있다
	// (막힌 상자에서는 균일 관통류가 불가능해서, 이류만 보려면 투영을 건너뛰어야 한다).
	void AdvectVelocity(double DeltaTimeS);
	void ApplyBuoyancy(const FAirSolverInput& Input, double DeltaTimeS);
	void ApplyWallBoundary();
	void ProjectToDivergenceFree(double DeltaTimeS);
	void AdvectTemperature(double DeltaTimeS);
	void InjectHeat(const FAirSolverInput& Input, double DeltaTimeS);

	const FUniformAirGrid& GetGrid() const { return Grid; }
	FUniformAirGrid& GetMutableGrid() { return Grid; }

	FPoissonSolveSettings& GetMutablePressureSettings() { return PressureSettings; }
	const FPoissonSolveResult& GetLastPressureResult() const { return LastPressureResult; }

	// ---- 열수지 (규약: 축적량을 포함한다) ----
	double GetEnergyInJ() const { return EnergyInJ; }

	// 초기 상태 대비 공기에 쌓인 에너지 [J].
	double GetEnergyStoredJ() const;

	// 유입 - 축적 [J]. 닫힌 상자라 유출이 없으므로 0에 가까워야 한다.
	double GetEnergyResidualJ() const;

	double GetNormalizedEnergyResidual() const;

	// 마지막 스텝의 CFL 수.
	double GetCourantNumber() const { return LastCourantNumber; }

private:
	FUniformAirGrid Grid;
	FPoissonSolveSettings PressureSettings;
	FPoissonSolveResult LastPressureResult;

	double InitialTemperatureK = 297.15;
	double EnergyInJ = 0.0;
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
};
