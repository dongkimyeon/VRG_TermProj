// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Core/VrgPerfCounters.h"

DEFINE_STAT(STAT_VrgAirSolverStep);
DEFINE_STAT(STAT_VrgPressureSolve);
DEFINE_STAT(STAT_VrgDebugDraw);
DEFINE_STAT(STAT_VrgCouplingStep);

namespace VrgPerf
{
	namespace
	{
		// 규약 5: 게임 스레드 전용이라 잠금이 없다. 워커에서 쓰면 안 된다.
		FFrameCosts GAccumulator;
		FFrameCosts GLastFrame;
	}

	FFrameCosts& Accumulator()
	{
		return GAccumulator;
	}

	const FFrameCosts& LastFrame()
	{
		return GLastFrame;
	}

	void EndFrame()
	{
		GLastFrame = GAccumulator;
		GAccumulator = FFrameCosts();
	}
}
