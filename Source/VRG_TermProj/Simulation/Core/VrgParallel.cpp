// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Core/VrgParallel.h"

#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarVrgSimParallel(
	TEXT("vrg.Sim.Parallel"), 1,
	TEXT("솔버 루프를 여러 코어로 나눠 돈다. 0 = 한 코어(비교용). 블록 분할이 고정이라 결과는 같다."),
	ECVF_Default);

bool VrgParallel::IsEnabled()
{
	return CVarVrgSimParallel.GetValueOnAnyThread() != 0;
}
