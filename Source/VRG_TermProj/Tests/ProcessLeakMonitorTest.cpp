// Fill out your copyright notice in the Description page of Project Settings.

// W5-2 (Tools/누수_실시간_계획.md L5 (가), 2026-11-03): process-value leak monitoring from instrument readings only.
//  - Monitor alone: noise below the thresholds raises nothing; each signal alarms once it has held past its threshold.
//  - Room: no leak for 60 s -> no process alarm; a 5 mm leak -> supply / return mismatch and vessel level; a 1 mm leak is
//    below the flow meters' resolution (no mismatch alarm) and is found only by the level drop, later.
//  - Facility: a 10 mm FWS leak -> its make-up water meter.
// Thresholds are ESTIMATES (ProcessLeakMonitor.h); the tests check the monitor reads the readings as stated.

#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/ProcessLeakMonitor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgW52Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	struct FW52Room
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		explicit FW52Room(bool bPlant)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16; Sim->bFacilityPlant = bPlant;
			Sim->PlantThermalSpec.AmbientK = 298.15;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			Room->bUseGjConnections = true; Room->bShowSummary = false;
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
			World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
			Sim->EnterSimulationMode();
		}
		~FW52Room() { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		int32 Site(const TCHAR* Label) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].Label == Label) return I;
			return INDEX_NONE;
		}
		int32 Loop(const TCHAR* Prefix) const
		{
			const FProcessLeakMonitor& M = Sim->GetProcessLeakMonitor();
			for (int32 L = 0; L < M.NumLoops(); ++L) if (M.GetLoopName(L).StartsWith(Prefix)) return L;
			return INDEX_NONE;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProcessMonitorUnitTest, "VRG.Sensors.W5.ProcessMonitorThresholds", VrgW52Flags)
bool FProcessMonitorUnitTest::RunTest(const FString& Parameters)
{
	FProcessLeakMonitor M;
	M.Reset({ TEXT("loop") }, { 5.0e-3 });
	const FProcessLeakSettings& S = M.GetSettings();
	FRandomStream Noise(7);
	// 120 s of noise inside the thresholds: level +-0.05 L, flows +-0.5 %, pressure +-500 Pa.
	for (int32 T = 1; T <= 120; ++T)
	{
		FProcessLeakReading R;
		R.LevelM3 = 2.4e-3 + 0.05e-3 * Noise.FRandRange(-1.f, 1.f);
		R.SupplyM3s = 5e-3 * (1.0 + 0.005 * Noise.FRandRange(-1.f, 1.f)); R.ReturnM3s = 5e-3 * (1.0 + 0.005 * Noise.FRandRange(-1.f, 1.f));
		R.PressurePa = 2.0e5 + 500.0 * Noise.FRandRange(-1.f, 1.f); R.MakeupM3s = 0.0;
		M.Step(T, { R });
	}
	const bool bQuiet = M.GetAlarmLog().IsEmpty();
	// From 121 s the level falls 0.02 L/s: the drop passes 0.2 L after 10 s, then holds 3 s.
	double LevelAlarm = -1.0;
	for (int32 T = 121; T <= 160; ++T)
	{
		FProcessLeakReading R;
		R.LevelM3 = 2.4e-3 - 0.02e-3 * (T - 120);
		R.SupplyM3s = 5e-3; R.ReturnM3s = 5e-3 * (T >= 140 ? 0.95 : 1.0); R.PressurePa = 2.0e5; R.MakeupM3s = T >= 150 ? 5e-6 : 0.0;
		for (const FProcessLeakAlarm& A : M.Step(T, { R })) if (A.Signal == EProcessLeakSignal::LevelDrop) LevelAlarm = A.TimeS;
	}
	AddInfo(FString::Printf(TEXT("잡음 120 s 경보 %d | 수위 경보 %.0f s, 불일치 %.0f s, 보충수 %.0f s, 압력 %d"), bQuiet ? 0 : 1, LevelAlarm,
		M.GetAlarmTimeS(0, EProcessLeakSignal::FlowMismatch), M.GetAlarmTimeS(0, EProcessLeakSignal::Makeup), M.IsAlarm(0, EProcessLeakSignal::PressureDrop) ? 1 : 0));
	TestTrue(TEXT("noise inside the thresholds raises nothing"), bQuiet);
	// The window's highest reading includes the earlier noise (up to +0.05 L): the drop passes 0.2 L between 128 and 131 s.
	TestTrue(TEXT("level drop alarms once 0.2 L is gone from the window's highest reading and held 3 s (131..134 s)"), LevelAlarm >= 131.0 && LevelAlarm <= 134.0);
	TestTrue(TEXT("5 % mismatch alarms after its hold (140 + 3 s)"), FMath::IsNearlyEqual(M.GetAlarmTimeS(0, EProcessLeakSignal::FlowMismatch), 143.0));
	TestTrue(TEXT("make-up flow alarms after its hold (150 + 3 s)"), FMath::IsNearlyEqual(M.GetAlarmTimeS(0, EProcessLeakSignal::Makeup), 153.0));
	TestFalse(TEXT("no pressure alarm without a pressure drop"), M.IsAlarm(0, EProcessLeakSignal::PressureDrop));
	TestTrue(TEXT("reset clears every latch"), M.ResetAlarms() == 3 && !M.IsAlarm(0, EProcessLeakSignal::LevelDrop));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProcessMonitorRoomTest, "VRG.Sensors.W5.ProcessMonitorRoomLeaks", VrgW52Flags)
bool FProcessMonitorRoomTest::RunTest(const FString& Parameters)
{
	// No leak: 60 s of readings, no process alarm.
	int32 QuietAlarms = -1;
	{
		FW52Room R(false);
		R.Run(60.0);
		QuietAlarms = R.Sim->GetProcessLeakMonitor().GetAlarmLog().Num();
	}
	auto RunLeak = [&](double HoleM, double Seconds, double& OutMismatchS, double& OutLevelS, double& OutStartS)
	{
		FW52Room R(false);
		R.Run(2.0);
		OutStartS = R.Sim->GetSnapshot().SimTimeS;
		R.Sim->SetLiveLeak(R.Site(TEXT("랙 0 공급 헤더")), HoleM, TEXT("test"));
		R.Run(Seconds);
		const int32 L = R.Loop(TEXT("CDU"));
		OutMismatchS = R.Sim->GetProcessLeakMonitor().GetAlarmTimeS(L, EProcessLeakSignal::FlowMismatch);
		OutLevelS = R.Sim->GetProcessLeakMonitor().GetAlarmTimeS(L, EProcessLeakSignal::LevelDrop);
	};
	double BigMis = -1, BigLevel = -1, BigStart = 0, SmallMis = -1, SmallLevel = -1, SmallStart = 0;
	RunLeak(5.0e-3, 6.0, BigMis, BigLevel, BigStart);
	RunLeak(1.0e-3, 60.0, SmallMis, SmallLevel, SmallStart);
	AddInfo(FString::Printf(TEXT("누수 없음 60 s: 공정값 경보 %d | 5 mm(%.1f s 시작): 불일치 %.1f s, 수위 %.1f s | 1 mm(%.1f s 시작): 불일치 %.1f s, 수위 %.1f s"),
		QuietAlarms, BigStart, BigMis, BigLevel, SmallStart, SmallMis, SmallLevel));
	TestTrue(TEXT("no leak, no process alarm (60 s)"), QuietAlarms == 0);
	TestTrue(TEXT("5 mm: supply/return mismatch and vessel level both alarm after the leak"), BigMis > BigStart && BigLevel > BigStart);
	TestTrue(TEXT("1 mm: below the flow meters' resolution, no mismatch alarm"), SmallMis < 0.0);
	TestTrue(TEXT("1 mm: found by the level drop, later than the 5 mm leak"), SmallLevel > SmallStart && SmallLevel - SmallStart > BigLevel - BigStart);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProcessMonitorPlantTest, "VRG.Sensors.W5.ProcessMonitorFacilityMakeup", VrgW52Flags)
bool FProcessMonitorPlantTest::RunTest(const FString& Parameters)
{
	FW52Room R(true);
	R.Run(3.0);
	const int32 Fws = R.Loop(TEXT("FWS"));
	const bool bQuietBefore = Fws != INDEX_NONE && !R.Sim->GetProcessLeakMonitor().IsAlarm(Fws, EProcessLeakSignal::Makeup);
	const double StartS = R.Sim->GetSnapshot().SimTimeS;
	TestTrue(TEXT("facility leak added"), R.Sim->ApplyPlantFault({ TEXT("leak"), TEXT("fws"), TEXT("branchsupply"), TEXT("0"), TEXT("10") }));
	R.Run(8.0);
	const double AlarmS = R.Sim->GetProcessLeakMonitor().GetAlarmTimeS(Fws, EProcessLeakSignal::Makeup);
	AddInfo(FString::Printf(TEXT("FWS 10 mm 누수 %.1f s 시작: 보충수 경보 %.1f s"), StartS, AlarmS));
	TestTrue(TEXT("FWS loop monitored, quiet before the leak"), bQuietBefore);
	TestTrue(TEXT("the make-up water meter finds the facility leak within 6 s"), AlarmS > StartS && AlarmS <= StartS + 6.0);
	return true;
}

#endif
