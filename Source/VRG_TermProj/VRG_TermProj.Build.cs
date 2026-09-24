// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class VRG_TermProj : ModuleRules
{
	public VRG_TermProj(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Simulation/Core 같은 하위 폴더를 모듈 루트 기준 경로로 include 하기 위해 필요하다.
		// (의존 모듈 추가가 아니라 include 경로 설정이다.)
		PublicIncludePaths.Add(ModuleDirectory);

		// EnhancedInput은 디버그 키를 실제로 쓰기 시작한 시점에 추가했다.
		// RenderCore는 S7-0 계측에서 GGameThreadTime/GRenderThreadTime을 읽으려고 추가했다.
		// RHI는 GPU 프레임 시간 때문이다 — UE 5.8에는 GGPUFrameTime 전역이 없고
		// GRHIGPUFrameTimeHistory에서 꺼내야 한다.
		// UMG/Slate는 패치 평면도에 사용한다. Niagara는 아직 사용하지 않는다.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "RenderCore", "RHI", "VrgRender" });

		PrivateDependencyModuleNames.AddRange(new string[] { "Eigen", "UMG", "Slate", "SlateCore" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
