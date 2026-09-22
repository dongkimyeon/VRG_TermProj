// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/JacobiPoissonSolver.h"

class FFixedOctreeGrid;

// S5-B: 고정 옥트리 위의 비균일 Poisson 연산자와 완화 Jacobi 솔버.
//
//   laplacian(X) = B        (벽·고체에서 dX/dn = 0, 즉 Neumann 경계)
//
// --- 왜 유한체적인가 ---
// 균일 격자에서 쓰던 "이웃 합 - 개수*중심" 꼴은 셀 크기가 하나일 때만 성립한다.
// 옥트리는 면마다 면적과 중심 거리가 다르므로 면 플럭스를 모아서 쓴다.
//
//   (L X)_i = (1/V_i) * sum_f  T_f * (X_other - X_i),   T_f = Area_f / Distance_f
//
// T_f는 면의 전달계수 [m]이고 면마다 한 번만 계산해 양쪽 셀에 부호만 바꿔 쓴다 (규약 6).
// 그래서 sum_i V_i * (L X)_i 가 항등적으로 0이다 (안쪽 플럭스가 쌍으로 상쇄된다).
// 이게 보존의 근거이고, 테스트가 재는 것도 이 장부다.
//
// 균일 격자(깊이 0)에서는 T_f = h^2/h = h, V = h^3이라
// (1/h^3)*h*sum(X_j - X_i) = (1/h^2)*sum(X_j - X_i)로 기존 균일 연산자와 **같은 식**이 된다.
// 자동화 테스트가 이 동일성을 직접 잰다.
//
// --- 정확도에 대한 정직한 진술 ---
// 두점 플럭스 근사(TPFA)는 두 셀 중심을 잇는 선분이 면 중심을 지날 때만 2차다.
// 비정합 면(깊이가 다른 두 리프)에서는 굵은 셀 중심이 면 중심에서 옆으로 벗어나 있어
// **1차**다. 면 방향으로 기울기가 있는 장에서 그 오차가 남는다.
// 그래서 "선형장은 정확히 조화롭다" 같은 패치 테스트를 걸지 않았다 — 실제로 성립하지 않는다.
// 이 근사를 개선하려면 비정합 면에 다점 플럭스나 보간 스텐실이 필요하고, 그건 별도 작업이다.
//
// --- D6 / D7 / D5 ---
// D6: 완화 Jacobi. 읽기/쓰기 버퍼가 분리돼 있어 격자 구조와 무관하다.
//     옥트리는 2색으로 인접 리프를 나눌 수 없으므로 Red-Black은 애초에 못 쓴다.
// D7: 이웃을 비트 연산으로 찾지 않는다. 구축 때 만든 면·CSR 인덱스만 읽는다.
// D5: 정규화 잔차 목표 + 최대 반복 수를 함께 쓴다. 수렴 실패를 결과에 표시한다.
//
// --- Neumann 특이성 ---
// 전부 Neumann이라 해가 상수만큼 자유롭다. 매 반복 뒤 **체적 가중** 평균을 뺀다.
// 셀 크기가 다르므로 단순 평균을 빼면 잘은 영역에 가중치가 쏠린다.
// 같은 이유로 해가 존재할 조건도 sum(B)=0이 아니라 sum(V_i * B_i)=0이다.
//
// UObject를 만들지 않는다. 워커 스레드에서 불러도 된다.
namespace OctreePoissonSolver
{
	// 리프 하나에서의 이산 Laplacian [X의 단위 / m^2].
	// 고체 리프에 닿는 면과 도메인 벽면은 플럭스가 0이라 항이 통째로 빠진다.
	VRG_TERMPROJ_API double LaplacianAt(const FFixedOctreeGrid& Tree, const TArray<double>& Field,
		int32 LeafIndex);

	// 잔차 max|laplacian(X) - B|. 고체 리프는 풀 방정식이 없으므로 뺀다.
	VRG_TERMPROJ_API double MaxAbsResidual(const FFixedOctreeGrid& Tree, const TArray<double>& Field,
		const TArray<double>& RightHandSide);

	// 공기 리프의 체적 가중 평균을 0으로 맞춘다.
	VRG_TERMPROJ_API void RemoveFluidVolumeMean(const FFixedOctreeGrid& Tree, TArray<double>& Field);

	// sum_i V_i * (L X)_i. 안쪽 면이 쌍으로 상쇄되므로 반올림 수준이어야 한다.
	// 검증 규칙대로 "장부가 어긋나지 않는다"를 재는 값이지 물리 정확도가 아니다.
	VRG_TERMPROJ_API double VolumeWeightedLaplacianSum(const FFixedOctreeGrid& Tree,
		const TArray<double>& Field);

	// laplacian(OutField) = B를 푼다. OutField는 초기 추정값으로도 쓰인다.
	// Scratch는 핑퐁용 두 번째 버퍼다 (규약 7: in-place 갱신을 하지 않는다).
	VRG_TERMPROJ_API FPoissonSolveResult Solve(const FFixedOctreeGrid& Tree,
		const TArray<double>& RightHandSide, TArray<double>& OutField, TArray<double>& Scratch,
		const FPoissonSolveSettings& Settings);
}
