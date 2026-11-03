// Fill out your copyright notice in the Description page of Project Settings.

// W5-1 (Tools/누수_실시간_계획.md L5 (나), 2026-11-03): contact leak detectors judged by the computed water, never by the
// solve's leak flow. NVIDIA's three detection levels: tray (cold plate + inner manifold detectors), rack (ropes / spot
// sensors along the piping and in the rack), data centre (CDU spot sensors, ropes along the facility piping).
//  - Bank: a current-water detector alarms after its debounce only while wet; drying first cancels it; the alarm latches;
//    a reset with water still there raises it again.
//  - Room ropes under the pipes alarm only where the floor water (W3) actually reached their cells, never before.
//  - A cold-plate leak wets that tray's cold-plate detector and not its inner-manifold detector; a connector leak wets the
//    inner-manifold detector.
// Thresholds and debounce are ESTIMATES (DataCenterSimulationInstruments.cpp ContactSpec).

#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/InstrumentBank.h"
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
	constexpr EAutomationTestFlags VrgW5Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	struct FW5Room
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		FW5Room()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16;
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
		~FW5Room() { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		int32 SiteByLabel(const TCHAR* Label) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].Label == Label) return I;
			return INDEX_NONE;
		}
		int32 InternalSite(int32 Rack, int32 Tray, int32 Kind) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].RackIndex == Rack && S[I].Tray == Tray && S[I].InternalSite == Kind) return I;
			return INDEX_NONE;
		}
		int32 TrayPoint(EInstrumentSite Site, int32 Rack, int32 Slot) const
		{
			const FInstrumentBank& B = Sim->GetInstruments();
			for (int32 I = 0; I < B.Num(); ++I) if (B.GetPoint(I).Site == Site && B.GetPoint(I).Key == Rack && B.GetPoint(I).Loop == Slot) return I;
			return INDEX_NONE;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5ContactBankTest, "VRG.Sensors.W5.CurrentWaterDebounceAndReset", VrgW5Flags)
bool FW5ContactBankTest::RunTest(const FString& Parameters)
{
	FInstrumentBank Bank;
	Bank.Reset();
	FInstrumentPoint P;
	P.bWaterContact = true; P.Contact.WettingVolumeM3 = 5e-6; P.Contact.bCurrentWater = true; P.Contact.SamplePeriodS = 1.0; P.Contact.ResponseDelayS = 1.0;
	Bank.AddPoint(P, {});
	const double Dt = 0.1;
	auto Run = [&](double From, double To, double WaterM3) { for (double T = From + Dt; T <= To + 1e-9; T += Dt) Bank.Advance(T, Dt, {}, {}, { WaterM3 }); };
	Run(0.0, 0.5, 6e-6);    // wet for 0.5 s ...
	Run(0.5, 2.0, 1e-6);    // ... then dries below the threshold before the debounce ended
	const bool bNoFlicker = !Bank.IsLeakAlarm(0);
	Run(2.0, 5.0, 6e-6);    // wet from 2.1 s: alarm at the first poll >= 3.1 s
	const bool bAlarm = Bank.IsLeakAlarm(0);
	const double AlarmS = Bank.GetLeakAlarmTimeS(0);
	Run(5.0, 6.0, 0.0);     // dries: the alarm stays latched
	const bool bLatched = Bank.IsLeakAlarm(0);
	const int32 Cleared = Bank.ResetLeakAlarm(INDEX_NONE);
	Run(6.0, 9.0, 0.0);     // reset while dry: no alarm
	const bool bQuietAfterReset = !Bank.IsLeakAlarm(0);
	Run(9.0, 12.0, 6e-6);   // wet again: alarm again
	const bool bAgain = Bank.IsLeakAlarm(0);
	Bank.ResetLeakAlarm(0);
	Run(12.0, 14.5, 6e-6);  // reset while still wet: raised again after the debounce
	AddInfo(FString::Printf(TEXT("경보 %.2f s(젖음 2.1 s, 지연 1 s, 판독 1 s), 해제 %d건, 젖은 채 해제 뒤 %.2f s"), AlarmS, Cleared, Bank.GetLeakAlarmTimeS(0)));
	TestTrue(TEXT("drying before the debounce raises nothing"), bNoFlicker);
	TestTrue(TEXT("wet past the debounce raises the alarm at a poll (3.1 s <= t < 4.1 s)"), bAlarm && AlarmS >= 3.1 - 1e-9 && AlarmS < 4.1);
	TestTrue(TEXT("the alarm latches when the water goes"), bLatched && Cleared == 1);
	TestTrue(TEXT("reset while dry stays quiet; new water alarms again"), bQuietAfterReset && bAgain);
	TestTrue(TEXT("reset while still wet re-raises after the debounce"), Bank.IsLeakAlarm(0) && Bank.GetLeakAlarmTimeS(0) >= 13.0 - 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5RoomRopeTest, "VRG.Sensors.W5.RoomRopesReadFloorWater", VrgW5Flags)
bool FW5RoomRopeTest::RunTest(const FString& Parameters)
{
	FW5Room R;
	R.Run(0.5);
	const FInstrumentBank& Bank = R.Sim->GetInstruments();
	const int32 Before = Bank.GetAlarmLog().Num();
	const int32 Site = R.SiteByLabel(TEXT("랙 0 공급 헤더"));
	TestTrue(TEXT("header site opened"), R.Sim->SetLiveLeak(Site, 5.0e-3, TEXT("test")));
	R.Run(8.0);
	int32 Ropes = 0, RopeAlarms = 0, WetRopes = 0, BadAlarms = 0;
	double First = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Bank.Num(); ++I)
	{
		const FInstrumentPoint& P = Bank.GetPoint(I);
		if (P.Site != EInstrumentSite::RoomPipe) continue;
		++Ropes;
		WetRopes += Bank.GetContactWaterM3(I) >= P.Contact.WettingVolumeM3 ? 1 : 0;
	}
	for (const FLeakAlarm& A : Bank.GetAlarmLog())
	{
		const FInstrumentPoint& P = Bank.GetPoint(A.Point);
		if (P.Site != EInstrumentSite::RoomPipe) continue;
		++RopeAlarms;
		First = FMath::Min(First, A.TimeS);
		// The alarm comes from water on its cells: at or above the threshold, after its threshold time + debounce.
		BadAlarms += A.WaterM3 >= P.Contact.WettingVolumeM3 && A.ThresholdS >= 0.5 && A.TimeS + 1e-9 >= A.ThresholdS + P.Contact.ResponseDelayS ? 0 : 1;
	}
	const FFloorWaterTotals& F = R.Sim->GetFloorWater().GetTotals();
	AddInfo(FString::Printf(TEXT("5 mm 헤더 누수 8 s: 바닥 물 %.3f L(배수 %.3f L), 배관 로프 %d개 중 지금 젖음 %d, 경보 %d(첫 %.2f s), 잘못된 경보 %d, 누수 전 경보 %d"),
		F.StoredM3 * 1e3, F.DrainedM3 * 1e3, Ropes, WetRopes, RopeAlarms, First, BadAlarms, Before));
	TestTrue(TEXT("no detector alarm before the leak"), Before == 0);
	TestTrue(TEXT("ropes under the pipes exist"), Ropes > 0);
	TestTrue(TEXT("the floor water reached at least one rope and it alarmed"), RopeAlarms > 0 && F.StoredM3 + F.DrainedM3 > 0.0);
	TestTrue(TEXT("every rope alarm comes from water on its cells (threshold, after the leak, after the debounce)"), BadAlarms == 0);
	TestTrue(TEXT("dry ropes stay quiet (not every rope alarmed)"), RopeAlarms < Ropes);
	// Operator reset: ropes still wet raise again; the count of latched alarms after a short wait = ropes still wet.
	R.Sim->ResetLeakAlarms(INDEX_NONE);
	R.Run(2.5);
	int32 Relatched = 0, StillWet = 0;
	for (int32 I = 0; I < Bank.Num(); ++I)
		if (Bank.GetPoint(I).Site == EInstrumentSite::RoomPipe)
		{
			Relatched += Bank.IsLeakAlarm(I) ? 1 : 0;
			StillWet += Bank.GetContactWaterM3(I) >= Bank.GetPoint(I).Contact.WettingVolumeM3 ? 1 : 0;
		}
	AddInfo(FString::Printf(TEXT("해제 2.5 s 뒤: 다시 울림 %d, 지금 젖음 %d"), Relatched, StillWet));
	TestTrue(TEXT("after a reset the ropes still wet alarm again"), Relatched > 0 && Relatched <= RopeAlarms);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5TraySensorTest, "VRG.Sensors.W5.TrayColdPlateAndManifold", VrgW5Flags)
bool FW5TraySensorTest::RunTest(const FString& Parameters)
{
	FW5Room R;
	R.Run(0.5);
	const FInstrumentBank& Bank = R.Sim->GetInstruments();
	const int32 Plate = R.InternalSite(0, 5, 2), Connector = R.InternalSite(0, 10, 1);
	if (!TestTrue(TEXT("internal sites exist"), Plate != INDEX_NONE && Connector != INDEX_NONE)) return false;
	const int32 PlateSlot = R.Sim->GetLiveLeakSites()[Plate].Slot, ConnSlot = R.Sim->GetLiveLeakSites()[Connector].Slot;
	const int32 PlateCold = R.TrayPoint(EInstrumentSite::TrayColdPlate, 0, PlateSlot), PlateMan = R.TrayPoint(EInstrumentSite::TrayManifold, 0, PlateSlot);
	const int32 ConnMan = R.TrayPoint(EInstrumentSite::TrayManifold, 0, ConnSlot);
	int32 TrayPoints = 0;
	for (int32 I = 0; I < Bank.Num(); ++I) TrayPoints += Bank.GetPoint(I).Site == EInstrumentSite::TrayColdPlate || Bank.GetPoint(I).Site == EInstrumentSite::TrayManifold ? 1 : 0;
	TestTrue(TEXT("two detectors per liquid tray (2 racks x 27 trays x 2)"), TrayPoints == 2 * 27 * 2 && PlateCold != INDEX_NONE && PlateMan != INDEX_NONE && ConnMan != INDEX_NONE);
	TestTrue(TEXT("plate leak opened"), R.Sim->SetLiveLeak(Plate, 1.0e-3, TEXT("test")));
	R.Run(4.0);
	const bool bPlateCold = Bank.IsLeakAlarm(PlateCold), bPlateMan = Bank.IsLeakAlarm(PlateMan);
	TestTrue(TEXT("connector leak opened"), R.Sim->SetLiveLeak(Connector, 1.0e-3, TEXT("test")));
	R.Run(4.0);
	const bool bConnMan = Bank.IsLeakAlarm(ConnMan);
	AddInfo(FString::Printf(TEXT("냉각판 누수(트레이 5): 냉각판 센서 %d(%.2f s, %.3f mL), 같은 트레이 매니폴드 센서 %d | 커넥터 누수(트레이 10): 매니폴드 센서 %d(%.2f s, %.3f mL)"),
		bPlateCold ? 1 : 0, Bank.GetLeakAlarmTimeS(PlateCold), Bank.GetContactWaterM3(PlateCold) * 1e6, bPlateMan ? 1 : 0,
		bConnMan ? 1 : 0, Bank.GetLeakAlarmTimeS(ConnMan), Bank.GetContactWaterM3(ConnMan) * 1e6));
	TestTrue(TEXT("a cold-plate leak wets that tray's cold-plate detector, not its inner-manifold detector"), bPlateCold && !bPlateMan);
	TestTrue(TEXT("a connector leak wets the tray's inner-manifold detector"), bConnMan);
	return true;
}

#endif
