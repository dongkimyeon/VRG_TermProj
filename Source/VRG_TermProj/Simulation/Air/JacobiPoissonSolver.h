// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "JacobiPoissonSolver.generated.h"

class FUniformAirGrid;

// 셀 중심 격자 위의 Poisson 방정식을 푼다.
//
//   laplacian(X) = B        (벽에서 dX/dn = 0, 즉 Neumann 경계)
//
// --- D6: Jacobi로 통일한다 ---
// Red-Black Gauss-Seidel을 쓰지 않는다. 균일 격자에서는 2색으로 인접 셀을 나눌 수
// 있지만 옥트리(S5)에서는 나눌 수 없어서, 지금 RBGS로 짜면 S5에서 통째로 다시 짜야 한다.
// Jacobi는 읽기/쓰기 버퍼가 분리돼 있어 격자 구조와 무관하게 같은 코드가 돈다
// (규약 7의 핑퐁과도 맞는다).
//
// --- 완화(damped) Jacobi를 써야 한다 ---
// 순수 Jacobi는 이 문제에서 수렴하지 않는다. 격자 그래프가 이분 그래프이기 때문이다.
// 반복 행렬 M = D^-1 * (L+U)는 행 합이 1인 비음수 행렬이라 고유값 1(상수 모드)을 갖고,
// 이분 그래프이므로 **고유값 -1도 정확히 갖는다**. 그 고유벡터는 체커보드
// (셀 좌표 합의 홀짝으로 부호가 갈리는) 모드이고, |mu| = 1이라 진동만 하고 줄지 않는다.
// 셀 차수가 경계에서 6,5,4,3으로 달라져도 마찬가지다
// (이웃이 전부 반대 홀짝이라 (Mv)_i = -v_i가 그대로 성립한다).
//
// 실제로 겪었다: 16^3 격자에서 2만 번 돌려도 정규화 잔차가 2.7e-4에서 멈췄다.
// 그때 해 자체의 오차는 2.5e-6이었다. 잔차가 오차의 약 96배(= 6/h^2)라는 것이
// 남은 오차가 고주파라는 증거다.
//
// 완화 계수 omega를 끼우면 고유값이 1 - omega*(1 - mu)로 옮겨 간다.
// omega = 2/3이면 체커보드 모드가 -1 -> -1/3이 되어 잘 줄고, 가장 매끄러운 모드는
// 거의 그대로 남는다 (느리지만 수렴한다).
//
// --- D5: 종료 조건 ---
// 정규화 잔차 목표와 최대 반복 수를 함께 쓴다. "Jacobi 20~40회" 같은 고정 횟수를
// 종료 조건으로 삼지 않는다. 수렴 실패는 결과에 표시하며, 호출자가 bConverged를
// 보지 않고 쓰면 안 된다.
//
// --- Neumann 경계의 특이성 ---
// 벽이 전부 Neumann이면 해가 상수만큼 자유롭다 (X와 X+c가 둘 다 해다).
// 그래서 매 반복 뒤 평균을 빼서 상수를 고정한다. 그렇게 해야 압력이 떠내려가지 않는다.
// 또한 sum(B)가 0이 아니면 해가 아예 없다. 비압축 투영에서는 닫힌 상자의 총 발산이
// 0이라 자동으로 만족하지만, 아니면 결과에 표시한다.
//
// 이 파일은 UObject를 만들지 않는다. 워커 스레드에서 불러도 된다.

USTRUCT(BlueprintType)
struct FPoissonSolveSettings
{
	GENERATED_BODY()

	// 무차원 정규화 잔차 목표. 정규화 기준은 결과 구조체에 함께 실린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double NormalizedResidualTarget = 1.0e-6;

	// D5: 목표에 못 미쳐도 여기서 멈춘다. 멈춘 사실은 결과에 표시된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1"))
	int32 MaxIterations = 400;

	// 잔차를 매 반복 재는 것은 비싸다. 이 간격마다 잰다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1"))
	int32 ResidualCheckInterval = 8;

	// 완화 계수. 1.0이면 순수 Jacobi이고 이 문제에서는 수렴하지 않는다 (위 주석 참고).
	// 2/3은 3차원 Laplacian에서 고주파를 가장 잘 죽이는 값으로 알려져 있다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	double Relaxation = 2.0 / 3.0;
};

USTRUCT(BlueprintType)
struct FPoissonSolveResult
{
	GENERATED_BODY()

	// D5: 이 값을 보지 않고 해를 쓰면 안 된다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	bool bConverged = false;

	UPROPERTY(BlueprintReadOnly, Category = "Air")
	int32 IterationsUsed = 0;

	// 무차원. 잔차의 최대 크기를 ReferenceScale로 나눈 값이다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double NormalizedResidual = 0.0;

	// 정규화 기준을 함께 노출한다 (검증 규칙: 정규화 기준과 단위를 함께 적는다).
	// 우변 B의 최대 크기이며 단위는 B와 같다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double ReferenceScale = 0.0;

	// 전체 Neumann에서 sum(B) != 0이면 해가 없다. 그럴 때 표시한다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	bool bIncompatibleRightHandSide = false;

	// sum(B)를 셀 수로 나눈 값. 위 플래그의 근거를 같이 보여준다.
	UPROPERTY(BlueprintReadOnly, Category = "Air")
	double MeanRightHandSide = 0.0;
};

namespace JacobiPoissonSolver
{
	// Neumann 경계에서의 이산 Laplacian [X의 단위 / m^2].
	// 벽 바깥 이웃은 거울상으로 본다 (dX/dn = 0). 그래서 경계 셀은 계수가 줄어든다.
	VRG_TERMPROJ_API double LaplacianAt(const FUniformAirGrid& Grid, const TArray<double>& Field,
		int32 X, int32 Y, int32 Z);

	// laplacian(OutX) = B를 푼다. OutX는 초기 추정값으로도 쓰인다 (이전 해를 넣으면 빨리 는다).
	// Scratch는 핑퐁용 두 번째 버퍼다. 호출자가 들고 있으면 매 스텝 재할당하지 않는다.
	VRG_TERMPROJ_API FPoissonSolveResult Solve(const FUniformAirGrid& Grid,
		const TArray<double>& RightHandSide, TArray<double>& OutField, TArray<double>& Scratch,
		const FPoissonSolveSettings& Settings);

	// 잔차 max|laplacian(X) - B|를 잰다. 검증용으로 따로 쓸 수 있게 열어 둔다.
	VRG_TERMPROJ_API double MaxAbsResidual(const FUniformAirGrid& Grid, const TArray<double>& Field,
		const TArray<double>& RightHandSide);

	// 전체 Neumann이라 해가 상수만큼 자유롭다. 평균을 0으로 맞춘다.
	VRG_TERMPROJ_API void RemoveMean(TArray<double>& Field);

	// 위와 같지만 고체 셀을 빼고 평균을 낸다.
	// 고체 셀은 방정식에 들어가지 않으므로 평균에도 넣으면 안 된다.
	VRG_TERMPROJ_API void RemoveFluidMean(const FUniformAirGrid& Grid, TArray<double>& Field);
}
