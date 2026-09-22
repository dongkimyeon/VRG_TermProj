// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/FixedOctreeDebugActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// 렌더 화질 검사가 아니라 실제 액터 경계의 cm->m, 회전 AABB, 재구축·표시 경로 검사다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeDebugActorTest,
	"VRG.Air.Octree.Fixed.DebugActorRebuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FFixedOctreeDebugActorTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Transient world"), World)) { return false; }
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>();
	AFixedOctreeDebugActor* Preview = World->SpawnActor<AFixedOctreeDebugActor>();
	if (!Air || !Rack || !Preview)
	{
		AddError(TEXT("Could not spawn preview fixtures"));
		World->DestroyWorld(false);
		return false;
	}
	Air->GridParams.NumCellsX = 4;
	Air->GridParams.NumCellsY = 3;
	Air->GridParams.NumCellsZ = 2;
	Air->GridParams.CellSizeM = 1.0;
	Air->SetActorLocation(FVector(1000.0, -500.0, 200.0));
	Rack->RackSizeM = FVector(1.6, 0.2, 0.8);
	Rack->SetActorLocation(FVector(1075.0, -350.0, 210.0));
	Rack->SetActorRotation(FRotator(0.0, 90.0, 0.0));
	Preview->AirField = Air;
	Preview->RackPaddingM = 0.0;
	Preview->RefinementDepth = 1;
	Preview->MaxDrawnLeaves = 2; // 표시 상한이 토폴로지 상한이 아님을 검증한다.
	Preview->RebuildOctree();
	Preview->Tick(0.0f);
	// 회전 후 x=[0.65,0.85], y=[0.7,2.3], z=[0.1,0.9] m: 루트 3개와 겹친다.
	TestEqual(TEXT("Rotated rack refines three roots in translated domain"), Preview->ActiveLeafCount, 45);
	TestFalse(TEXT("Build summary available"), Preview->BuildStatus.IsEmpty());

	Air->GetMutableSolver().Initialize(Air->GridParams, 300.0);
	const TArray<double> Before = Air->GetMutableSolver().GetGrid().TemperatureK;
	Preview->RebuildOctree();
	Preview->Tick(0.0f);
	TestTrue(TEXT("Preview does not change uniform temperatures"), Before == Air->GetMutableSolver().GetGrid().TemperatureK);
	TestFalse(TEXT("Preview does not take over uniform clock"), Air->bDrivenExternally);
	TestTrue(TEXT("Live temperature copy reported"), Preview->BuildStatus.Contains(TEXT("ΔE=")));

	Rack->SetActorLocation(FVector(10000.0));
	Preview->RebuildOctree();
	Preview->Tick(0.0f);
	TestEqual(TEXT("Explicit rebuild removes old refinement"), Preview->ActiveLeafCount, 24);
	Preview->MaxLeaves = 1;
	Preview->RebuildOctree();
	Preview->Tick(0.0f);
	TestEqual(TEXT("Failed build clears stale preview"), Preview->ActiveLeafCount, 0);
	TestTrue(TEXT("Budget failure visible"), Preview->BuildStatus.Contains(TEXT("MaxLeaves")));
	World->DestroyWorld(false);
	return true;
}

#endif
