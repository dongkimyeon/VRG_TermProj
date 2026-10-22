// Fill out your copyright notice in the Description page of Project Settings.

// 2026-10-22 user report ("배치 모드 드래그 중 렉, 놓을 때 한 번만 계산"): measured on L_Test the drag frames were light and
// every hitch was the room's pipe / cable rebuild (~50 ms), fired 0.4 s after a release - in the middle of the next drag
// when equipment was moved in quick succession. While the controller holds the rebuild nothing is rebuilt, and the
// release rebuilds once at once; with no hold the settle rule is unchanged.

#include "Misc/AutomationTest.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRebuildHoldTest, "VRG.Interaction.Layout.RebuildOnceAfterRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRebuildHoldTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	Room->bUseGjConnections = true;
	Room->LayoutSettleS = 0.0;   // the settle wait itself is not under test
	Room->bShowSummary = false;
	TArray<ARackThermalActor*> Racks;
	for (const double Y : { 0.0, 66.0 })
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Racks.Add(Rack);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	Room->ConsumeRebuiltFlag();
	auto Ticks = [&](int32 N) { int32 Rebuilds = 0; for (int32 I = 0; I < N; ++I) { Room->Tick(0.f); Rebuilds += Room->ConsumeRebuiltFlag() ? 1 : 0; } return Rebuilds; };

	// Held (drag / grab live): the moved rack changes nothing however long it waits.
	Room->SetLayoutRebuildHeld(true);
	Racks[1]->SetActorLocation(FVector(0, 166, 0));
	const int32 WhileHeld = Ticks(5);
	// Release: one rebuild on the first tick, then none.
	Room->SetLayoutRebuildHeld(false);
	const int32 AtRelease = Ticks(1);
	const int32 AfterRelease = Ticks(5);
	// A release without a change rebuilds nothing.
	Room->SetLayoutRebuildHeld(true); Ticks(2);
	Room->SetLayoutRebuildHeld(false);
	const int32 EmptyRelease = Ticks(3);
	// No hold (console / script move): the settle rule still rebuilds once.
	Racks[1]->SetActorLocation(FVector(0, 266, 0));
	const int32 Unheld = Ticks(5);
	AddInfo(FString::Printf(TEXT("재구축 횟수: 잡은 동안 %d, 놓은 직후 %d, 그 뒤 %d, 바뀐 것 없이 놓음 %d, 잡지 않은 이동 %d"),
		WhileHeld, AtRelease, AfterRelease, EmptyRelease, Unheld));
	TestEqual(TEXT("no rebuild while held"), WhileHeld, 0);
	TestEqual(TEXT("one rebuild at the release tick"), AtRelease, 1);
	TestEqual(TEXT("none after it"), AfterRelease, 0);
	TestEqual(TEXT("release without a change: none"), EmptyRelease, 0);
	TestEqual(TEXT("unheld move: the settle rule rebuilds once"), Unheld, 1);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
