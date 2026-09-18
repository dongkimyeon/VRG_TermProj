// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// 규약 4: 모든 도메인·센서·이벤트가 공유하는 단일 시계.
// 실시간 경과를 고정 스텝으로 쪼개고, 스텝에 못 미치는 남은 시간은 버리지 않고
// 누적기에 남겨 다음 프레임으로 넘긴다. 도메인마다 갱신 주기가 달라도
// 이 시계가 주는 같은 시간 구간을 적분한다.
//
// 프레임 급락 시 무한 서브스텝을 막기 위한 상한이 있다. 상한에 걸려 버린 시간은
// 모든 도메인에 동일하게 적용되며, DiscardedTimeS로 계속 노출되므로
// 결과에 표시할 수 있다 (규약: 검증되지 않은 결과를 감추지 않는다).
class FSimClock
{
public:
	explicit FSimClock(double InFixedStepS = 0.01);

	void Reset(double InFixedStepS);

	// RealDeltaS를 누적하고 이번 프레임에 실행해야 할 고정 스텝 수를 돌려준다.
	// 반환값만큼 Step을 적분한 뒤 매번 CompleteStep()을 호출해야 한다.
	int32 BeginFrame(double RealDeltaS);

	// 한 스텝을 실제로 적분한 뒤 호출한다. SimTime이 고정 스텝만큼 전진한다.
	void CompleteStep();

	double GetSimTimeS() const { return SimTimeS; }
	double GetFixedStepS() const { return FixedStepS; }

	// 아직 한 스텝을 못 채워 누적기에 남아 있는 시간. 다음 프레임으로 넘어간다.
	double GetPendingTimeS() const { return AccumulatorS; }

	// 서브스텝 상한에 걸려 버린 누적 시간. 0이 아니면 결과에 표시해야 한다.
	double GetDiscardedTimeS() const { return DiscardedTimeS; }
	bool WasClampedLastFrame() const { return bClampedLastFrame; }

	void SetMaxSubStepsPerFrame(int32 InMaxSubSteps);
	int32 GetMaxSubStepsPerFrame() const { return MaxSubStepsPerFrame; }

private:
	double SimTimeS = 0.0;
	double FixedStepS = 0.01;
	double AccumulatorS = 0.0;
	double DiscardedTimeS = 0.0;
	int32 MaxSubStepsPerFrame = 32;
	bool bClampedLastFrame = false;
};
