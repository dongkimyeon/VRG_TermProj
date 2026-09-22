// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// S7-4: 압력 Poisson의 완화 Jacobi 스윕을 컴퓨트 셰이더로 돌린다.
//
// --- 왜 이것만 GPU로 가나 ---
// S7-3 측정에서 프레임 133.75 ms 중 압력 Poisson이 101.32 ms로 75.8%였다.
// 수력 Newton(노드 50개)이나 랙 열모델(노드 1152개)은 GPU로 올리면
// 커널 실행 비용이 계산보다 커진다. 여기만 옮긴다.
//
// --- CPU FP64 경로는 지운 것이 아니다 ---
// `JacobiPoissonSolver`가 기준이자 폴백이다. GPU를 못 쓰거나 꺼 두면 그쪽으로 돈다.
// 두 해를 **허용 오차로** 비교하는 것이 검증이다. 비트 동일성은 검증이 아니다.
//
// --- 정밀도 ---
// GPU는 FP32다. RTX 4080(Ada)의 FP64는 FP32의 1/64라 배정밀도로 올릴 이유가 없다.
// 그래서 압력 해의 오차 기준만 완화되고, 열수지·수력·랙 장부는 CPU FP64 그대로다.
// 보존이 유지되는 이유는 값의 정밀도가 아니라 플럭스를 한 번만 계산해
// 양쪽에 부호만 바꿔 쓰는 구조(규약 6)에 있다.
//
// --- CPU 경로와 다른 점 (의도한 것) ---
// CPU는 매 반복 뒤 공기 셀 평균을 뺀다. GPU는 **마지막에 한 번만** 뺀다.
// 상수를 더해도 Laplacian이 0이라 잔차가 변하지 않으므로, 매 반복 빼는 것은
// 수치 위생이지 수렴에 필요한 일이 아니다. 반복마다 리덕션 패스를 두 번 더 도는 것보다
// 마지막에 한 번 빼는 쪽이 싸다. 평균 빼기와 잔차 계산은 결과를 내려받은 뒤 CPU에서 한다
// (어차피 투영 단계가 CPU라 해를 내려받아야 한다).
//
// --- 규약 5 ---
// 이 함수는 게임 스레드에서 부른다. 내부에서 렌더 명령을 밀고 결과를 기다린다.
// 솔버 자료구조나 액터를 렌더 스레드가 직접 보지 않는다.

struct FVrgPressureGpuRequest
{
	FIntVector GridSize = FIntVector::ZeroValue;
	float CellSizeM = 0.25f;
	float Omega = 2.0f / 3.0f;
	int32 Iterations = 64;
};

namespace VrgPressureGpu
{
	// 이 플랫폼/기능 수준에서 컴퓨트 경로를 쓸 수 있는가.
	VRGRENDER_API bool IsAvailable();

	// InOutX는 초기 추정값으로 들어가고 결과로 덮인다. 길이는 셀 수와 같아야 한다.
	// FluidMask는 셀당 1(공기)/0(고체)이다.
	// 실패하면 false를 돌려주고 InOutX를 건드리지 않는다. 호출자는 CPU 경로로 돌아가야 한다.
	VRGRENDER_API bool RunJacobiSweeps(const FVrgPressureGpuRequest& Request,
		const TArray<float>& RightHandSide, const TArray<uint32>& FluidMask, TArray<float>& InOutX);
}
