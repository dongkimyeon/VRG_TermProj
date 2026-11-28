#pragma once

// 2026-11-28 소음 표시(새 UI): 소음 수신점 결과를 화면에 그리기 위해 한 프레임에 한 번 모은 값. 계산은 하지 않고
// ADataCenterSimulationActor의 수신점 묶음(N4·N5 결과)을 읽기만 한다(규약 5). 표시 전용.
#include "CoreMinimal.h"

namespace VrgNoise { struct FNoiseTrack; }

struct FVrgNoiseView
{
	bool bShow = false;                  // vrg.Noise.Show (N 키)
	bool bAny = false;                   // 수신점이 하나라도 있음
	int32 Receivers = 0;                 // 모든 묶음의 수신점 수
	int32 OnScreen = 0;
	TOptional<double> MaxDb;             // 최신 LpA 중 최고(값이 있는 점만)
	int32 PartialPoints = 0;             // 음원 일부 누락으로 부분 합계인 점
	const VrgNoise::FNoiseTrack* Selected = nullptr;   // 화면 중앙에 가장 가까운 점(48 px 안)
	FVector2D SelectedScreen = FVector2D::ZeroVector;
	FString SelectedGroup;               // 실내 / 실외 수신점 / 부지 경계
	double SourceTimeS = 0.0;
	// 화면에 보이는 점: 위치(뷰포트 픽셀)와 표시 색(sRGB).
	TArray<TPair<FVector2D, FColor>> Points;
};
