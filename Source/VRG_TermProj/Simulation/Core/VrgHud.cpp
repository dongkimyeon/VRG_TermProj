// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Core/VrgHud.h"
#include "HAL/IConsoleManager.h"

namespace
{
	int32 GVrgHudDebug = 0;
	FAutoConsoleVariableRef CVarVrgHudDebug(TEXT("vrg.Hud.Debug"), GVrgHudDebug,
		TEXT("화면 디버그 문구(요약·라벨·성능 오버레이). 0=끔(기본, 단축키 안내만), 1=켬"));
}

bool VrgHud::ShowDebugText()
{
	return GVrgHudDebug != 0;
}
