// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

// S7-1: 전역 셰이더 디렉터리 매핑만 하는 모듈.
//
// 전역 셰이더 맵은 엔진 PreInit에서 컴파일된다. 그때 `/Vrg`가 매핑돼 있지 않으면
// 셰이더 소스를 못 찾고 죽는다. 그래서 이 모듈은 LoadingPhase가 PostConfigInit이고,
// 게임 모듈(Default)보다 먼저 올라온다. uproject의 모듈 순서도 그래서 이 모듈이 앞이다.
class FVrgRenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const FString ShaderDirectory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Vrg"), ShaderDirectory);
	}
};

IMPLEMENT_MODULE(FVrgRenderModule, VrgRender);
