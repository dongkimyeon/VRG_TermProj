// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

// S7-1: 전역 셰이더와 렌더 패스만 담는 모듈.
//
// 왜 모듈을 따로 만들었나: 전역 셰이더 맵은 엔진 PreInit에서 컴파일된다.
// 게임 모듈(LoadingPhase Default)의 StartupModule에서 셰이더 디렉터리를 매핑하면
// 그때는 이미 늦어서 "Shader source directory not found"로 죽는다.
// 그래서 이 모듈만 PostConfigInit으로 먼저 올려 디렉터리를 매핑하고,
// 전역 셰이더 클래스도 같이 여기에 둔다.
public class VrgRender : ModuleRules
{
	public VrgRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"RenderCore", "RHI", "Renderer", "Projects",
		});
	}
}
