// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// D3: 노드 200개 미만이면 밀행렬 LU로 충분하다.
// 부분 피벗 가우스 소거로 A*x = b를 푼다. 외부 선형대수 라이브러리를 쓰지 않는다.
//
// MatrixRowMajor: N*N 행 우선 배열. 호출 중에 파괴적으로 변경된다.
// RhsAndSolution: 길이 N. 입력은 b, 성공하면 x로 덮어쓴다.
// 반환값: 피벗이 0에 가까워 풀 수 없으면 false (특이 행렬).
bool SolveDenseLinearSystem(TArray<double>& MatrixRowMajor, TArray<double>& RhsAndSolution, int32 N);
