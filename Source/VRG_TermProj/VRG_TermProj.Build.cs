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

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
