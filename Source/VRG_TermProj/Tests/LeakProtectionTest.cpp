// Fill out your copyright notice in the Description page of Project Settings.

// W5-3 (Tools/누수_실시간_계획.md L5 (다), 2026-11-03): protective shutdown / isolation from the leak detectors, after the
// NVIDIA Mission Control / NICo response matrix. Off by default. The response is the existing slot power, busbar, rack
// valves and CDU power, then the network / thermal solve (D4). The compute-tray timer is an ESTIMATE.

#include "Misc/AutomationTest.h"
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
	constexpr EAutomationTestFlags VrgW53Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	struct FW53Room
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		ACoolingDistributionUnitActor* Cdu = nullptr;
		TArray<ARackThermalActor*> Racks;
		explicit FW53Room(bool bPolicy)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16;
			Sim->LeakProtectionPolicy.bEnabled = bPolicy;
			// The response logic at the decision step (2026-11-10: actor delays are tested in LeakProtectionFollowupTest).
			auto& Pol = Sim->LeakProtectionPolicy;
			Pol.TrayBmcDelayS = Pol.BmsPollS = Pol.ShelfOffDelayS = Pol.ValveStrokeS = Pol.CduStopDelayS = 0.0;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			Room->bUseGjConnections = true; Room->bShowSummary = false;
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
				Racks.Add(Rack);
			}
			Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
			Sim->EnterSimulationMode();
			Racks = Room->GetRoutedRacks();   // the simulation's rack order (live-site and detector rack indices)
		}
		~FW53Room() { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		int32 Internal(int32 Rack, int32 Tray, int32 Kind) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].RackIndex == Rack && S[I].Tray == Tray && S[I].InternalSite == Kind) return I;
			return INDEX_NONE;
		}
		int32 Labeled(const TCHAR* Label) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].Label == Label) return I;
			return INDEX_NONE;
		}
		bool Powered(int32 Rack, int32 Slot) const { const auto& P = Racks[Rack]->GetInput().SlotPowered; return P.IsEmpty() || P[Slot]; }
		double ColdPlateAlarmS(int32 Rack, int32 Slot) const
		{
			const FInstrumentBank& B = Sim->GetInstruments();
			for (int32 I = 0; I < B.Num(); ++I)
				if (B.GetPoint(I).Site == EInstrumentSite::TrayColdPlate && B.GetPoint(I).Key == Rack && B.GetPoint(I).Loop == Slot) return B.GetLeakAlarmTimeS(I);
			return -1.0;
		}
		int32 CountEvents(const TCHAR* Level) const { int32 N = 0; for (const auto& E : Sim->GetLeakProtectionEvents()) N += E.Level == Level ? 1 : 0; return N; }
		// Rack isolations / row shutdowns themselves (their request, not the shelf, valve or CDU actions that follow).
		int32 CountWhole(const TCHAR* Level) const { int32 N = 0; for (const auto& E : Sim->GetLeakProtectionEvents()) N += E.Level == Level && E.Slot == INDEX_NONE && E.Action.Contains(TEXT("requested")) ? 1 : 0; return N; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW53TrayTimerTest, "VRG.Sensors.W5.ProtectionTrayTimer", VrgW53Flags)
bool FW53TrayTimerTest::RunTest(const FString& Parameters)
{
	// Policy off: the detector alarms, nothing is switched off.
	{
		FW53Room R(false);
		R.Run(0.5);
		const int32 Site = R.Internal(0, 0, 2);
		const int32 Slot = R.Sim->GetLiveLeakSites()[Site].Slot;
		R.Sim->SetLiveLeak(Site, 1.0e-3, TEXT("test"));
		R.Run(15.0);
		TestTrue(TEXT("policy off: alarm, but no power cut and no event"), R.ColdPlateAlarmS(0, Slot) > 0.0 && R.Powered(0, Slot) && R.Sim->GetLeakProtectionEvents().IsEmpty());
	}
	FW53Room R(true);
	R.Run(0.5);
	// Tray 0 = the lowest compute tray: its water drains to the power shelves below (no tray detector there), so only one
	// tray leaks. A higher tray's water would wet the trays below it and the two-tray rule would isolate the rack.
	const int32 Site = R.Internal(0, 0, 2);
	const int32 Slot = R.Sim->GetLiveLeakSites()[Site].Slot;
	const double ItBefore = R.Racks[0]->GetModel().GetLastTotalHeatW();
	R.Sim->SetLiveLeak(Site, 1.0e-3, TEXT("test"));
	double AlarmS = -1.0, OffS = -1.0;
	bool bEarly = false;
	for (int32 F = 0; F < 200 && OffS < 0.0; ++F)
	{
		R.Sim->Tick(.1f);
		if (AlarmS < 0.0) AlarmS = R.ColdPlateAlarmS(0, Slot);
		if (!R.Powered(0, Slot)) OffS = R.Sim->GetSnapshot().SimTimeS;
		bEarly |= AlarmS > 0.0 && OffS > 0.0 && OffS < AlarmS + R.Sim->LeakProtectionPolicy.ComputeTrayShutdownDelayS - 1e-6;
	}
	R.Run(0.5);
	const double ItAfter = R.Racks[0]->GetModel().GetLastTotalHeatW();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("컴퓨트 트레이 냉각판 누수: 경보 %.2f s, 전원 차단 %.2f s(타이머 %.1f s), 랙 0 발열 %.1f -> %.1f kW, 랙 격리 %d, 전역 열수지 %.2e"),
		AlarmS, OffS, R.Sim->LeakProtectionPolicy.ComputeTrayShutdownDelayS, ItBefore * 1e-3, ItAfter * 1e-3, R.CountWhole(TEXT("rack")), Global));
	TestTrue(TEXT("the compute tray is cut after its shutdown timer, not before (within one frame)"),
		AlarmS > 0.0 && OffS > 0.0 && !bEarly && OffS <= AlarmS + R.Sim->LeakProtectionPolicy.ComputeTrayShutdownDelayS + 0.1 + 1e-6);
	TestTrue(TEXT("its heat leaves the rack; one leaking tray does not isolate the rack"), ItAfter < ItBefore - 1000.0 && R.CountWhole(TEXT("rack")) == 0);
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW53RackIsolationTest, "VRG.Sensors.W5.ProtectionRackIsolation", VrgW53Flags)
bool FW53RackIsolationTest::RunTest(const FString& Parameters)
{
	FW53Room R(true);
	R.Run(0.5);
	const int32 Compute = R.Internal(0, 5, 2), Switch = R.Internal(0, 10, 2);   // tray 10 = NVLink switch tray
	const int32 SwitchSlot = R.Sim->GetLiveLeakSites()[Switch].Slot;
	R.Sim->SetLiveLeak(Compute, 1.0e-3, TEXT("test"));
	R.Sim->SetLiveLeak(Switch, 1.0e-3, TEXT("test"));
	R.Run(5.0);
	const bool bSwitchOff = !R.Powered(0, SwitchSlot);
	const bool bValve = R.Room->IsRackValveClosed(R.Racks[0]), bOtherValve = R.Room->IsRackValveClosed(R.Racks[1]);
	const bool bTripped = R.Racks[0]->GetModel().IsBusbarTripped(), bOtherTripped = R.Racks[1]->GetModel().IsBusbarTripped();
	const double It0 = R.Racks[0]->GetModel().GetLastTotalHeatW(), It1 = R.Racks[1]->GetModel().GetLastTotalHeatW();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("트레이 2개 누수(컴퓨트+스위치): 스위치 즉시 차단 %d, 랙 0 밸브 %d·버스바 차단 %d·발열 %.1f W | 랙 1 밸브 %d·버스바 %d·발열 %.1f kW | 사건 트레이 %d·랙 %d, 전역 %.2e"),
		bSwitchOff ? 1 : 0, bValve ? 1 : 0, bTripped ? 1 : 0, It0, bOtherValve ? 1 : 0, bOtherTripped ? 1 : 0, It1 * 1e-3, R.CountEvents(TEXT("tray")), R.CountWhole(TEXT("rack")), Global));
	TestTrue(TEXT("the switch tray is powered off at once"), bSwitchOff);
	TestTrue(TEXT("two leaking trays isolate the rack: shelves off (busbar trips, no electrical heat), supply/return valves shut"), bValve && bTripped && It0 == 0.0);
	TestTrue(TEXT("the other rack keeps running"), !bOtherValve && !bOtherTripped && It1 > 0.0);
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW53RowShutdownTest, "VRG.Sensors.W5.ProtectionRowShutdown", VrgW53Flags)
bool FW53RowShutdownTest::RunTest(const FString& Parameters)
{
	FW53Room R(true);
	R.Run(0.5);
	R.Sim->SetLiveLeak(R.Labeled(TEXT("랙 0 공급 헤더")), 5.0e-3, TEXT("test"));
	R.Run(10.0);
	const bool bCduOff = !R.Cdu->IsPumpPowered() && !R.Room->GetNetwork().IsPumpPowered(R.Room->GetRowPumpIndex(0));
	const bool bAllTripped = R.Racks[0]->GetModel().IsBusbarTripped() && R.Racks[1]->GetModel().IsBusbarTripped();
	const double Flow = R.Room->GetNetwork().GetPumpFlowM3s(R.Room->GetRowPumpIndex(0));
	int32 AlertsWithAction = 0;
	for (const auto& E : R.Sim->GetLeakProtectionEvents()) AlertsWithAction += E.Level == TEXT("alert") && (E.Rack != INDEX_NONE || E.Row != INDEX_NONE) ? 1 : 0;
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("헤더 5 mm 누수 → 배관 로프 경보 → 줄 정지: CDU 정지 %d, 랙 두 대 버스바 차단 %d, 펌프 유량 %.4f L/s, 사건 줄 %d·알림 %d, 전역 %.2e"),
		bCduOff ? 1 : 0, bAllTripped ? 1 : 0, Flow * 1e3, R.CountWhole(TEXT("row")), R.CountEvents(TEXT("alert")), Global));
	TestTrue(TEXT("a room rope alarm shuts the row once: every rack off and the row CDU stopped"), R.CountWhole(TEXT("row")) == 1 && bCduOff && bAllTripped);
	TestTrue(TEXT("process-value alarms only alert"), R.CountEvents(TEXT("alert")) >= 1 && AlertsWithAction == 0);
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	return true;
}

#endif
