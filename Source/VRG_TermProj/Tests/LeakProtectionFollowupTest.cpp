// W5 follow-up (Tools/누수_실시간_계획.md "W5 후속", 2026-11-10): detector faults and false alarms, who acts and how long
// it takes, and the operator recovery. Fault kinds, rates and every delay except the 5 s BMS cycle (NVIDIA BMS integration
// heartbeat) are ESTIMATES; the tests check the logic and the timing those values give, not real hardware.

#include "Misc/AutomationTest.h"
#include "Simulation/Sensors/InstrumentBank.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Visualization/GjRackLayout.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgW5FFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FInstrumentPoint ContactPoint()
	{
		FInstrumentPoint P;
		P.Site = EInstrumentSite::TrayColdPlate; P.bWaterContact = true; P.Id = TEXT("test");
		P.Contact.WettingVolumeM3 = 2.0e-6; P.Contact.SamplePeriodS = 1.0; P.Contact.ResponseDelayS = 1.0; P.Contact.bCurrentWater = true;
		return P;
	}
	// Runs one detector for Seconds at 20 ms with the given water; returns its leak alarms.
	TArray<FLeakAlarm> RunDetector(FInstrumentBank& B, double WaterM3, double FromS, double Seconds)
	{
		TArray<FLeakAlarm> Out;
		const int32 N = FMath::RoundToInt32(Seconds / 0.02);
		for (int32 I = 1; I <= N; ++I) Out.Append(B.Advance(FromS + I * 0.02, 0.02, {}, {}, { WaterM3 }));
		return Out;
	}

	struct FW5FRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		ACoolingDistributionUnitActor* Cdu = nullptr;
		TArray<ARackThermalActor*> Racks;
		explicit FW5FRoom(const TCHAR* Before = nullptr)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16;
			Sim->LeakProtectionPolicy.bEnabled = true;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			Room->bUseGjConnections = true; Room->bShowSummary = false;
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
			Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
			if (Before) GEngine->Exec(World, Before);
			Sim->EnterSimulationMode();
			Racks = Room->GetRoutedRacks();
		}
		~FW5FRoom() { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		double Now() const { return Sim->GetSnapshot().SimTimeS; }
		int32 Internal(int32 Rack, int32 Tray, int32 Kind) const
		{
			const auto& S = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < S.Num(); ++I) if (S[I].RackIndex == Rack && S[I].Tray == Tray && S[I].InternalSite == Kind) return I;
			return INDEX_NONE;
		}
		int32 Detector(EInstrumentSite Site, int32 Rack, int32 Slot) const
		{
			const FInstrumentBank& B = Sim->GetInstruments();
			for (int32 I = 0; I < B.Num(); ++I) if (B.GetPoint(I).Site == Site && B.GetPoint(I).Key == Rack && B.GetPoint(I).Loop == Slot) return I;
			return INDEX_NONE;
		}
		bool Powered(int32 Rack, int32 Slot) const { const auto& P = Racks[Rack]->GetInput().SlotPowered; return P.IsEmpty() || P[Slot]; }
		double EventS(const TCHAR* Action, int32 Rack = INDEX_NONE) const
		{
			for (const auto& E : Sim->GetLeakProtectionEvents()) if (E.Action.Contains(Action) && (Rack == INDEX_NONE || E.Rack == Rack)) return E.TimeS;
			return -1.0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5FDetectorHealthTest, "VRG.Sensors.W5F.DetectorHealth", VrgW5FFlags)
bool FW5FDetectorHealthTest::RunTest(const FString& Parameters)
{
	auto Fresh = [](FInstrumentBank& B) { B.Reset(); B.AddPoint(ContactPoint(), {}); };
	FInstrumentBank B;
	// Healthy with water: one real alarm.
	Fresh(B);
	TArray<FLeakAlarm> A = RunDetector(B, 5e-6, 0.0, 4.0);
	TestTrue(TEXT("healthy + water: one real alarm"), A.Num() == 1 && !A[0].bFalse);
	// Open circuit: the supervision reports a fault at a poll, the water is never seen.
	Fresh(B); B.SetSensorHealth(0, ESensorHealth::OpenCircuit);
	A = RunDetector(B, 5e-6, 0.0, 4.0);
	TestTrue(TEXT("open circuit: no leak alarm, one fault alarm at a poll"), A.IsEmpty() && B.GetFaultLog().Num() == 1 && FMath::IsNearlyEqual(B.GetFaultLog()[0].TimeS, 1.0, 1e-6));
	// Stuck wet: alarms with no water, flagged false (truth only).
	Fresh(B); B.SetSensorHealth(0, ESensorHealth::StuckWet);
	A = RunDetector(B, 0.0, 0.0, 4.0);
	TestTrue(TEXT("stuck wet: a false alarm without water"), A.Num() == 1 && A[0].bFalse);
	// Stuck dry: misses real water, silently.
	Fresh(B); B.SetSensorHealth(0, ESensorHealth::StuckDry);
	A = RunDetector(B, 5e-6, 0.0, 4.0);
	TestTrue(TEXT("stuck dry: water missed, no fault alarm"), A.IsEmpty() && B.GetFaultLog().IsEmpty());
	// Repair (Ok): the still-wet detector alarms again after its debounce.
	B.SetSensorHealth(0, ESensorHealth::Ok);
	A = RunDetector(B, 5e-6, 4.0, 3.0);
	TestTrue(TEXT("repaired detector sees the water again"), A.Num() == 1 && !A[0].bFalse);
	// Spurious pulses: off by default; on, deterministic and flagged false. A pulse shorter than debounce + poll never alarms.
	Fresh(B);
	{ FFalseTriggerSpec Short; Short.PerDetectorHour = 3600.0; Short.PulseS = 1.5; B.SetFalseTriggers(Short); }
	TestTrue(TEXT("pulses shorter than debounce + poll never alarm"), RunDetector(B, 0.0, 0.0, 120.0).IsEmpty());
	Fresh(B); B.SetFalseTriggers(FFalseTriggerSpec());
	TestTrue(TEXT("no false triggers by default"), RunDetector(B, 0.0, 0.0, 600.0).IsEmpty());
	FInstrumentBank C, D;
	for (FInstrumentBank* X : { &C, &D })
	{
		X->Reset();
		for (int32 I = 0; I < 20; ++I) X->AddPoint(ContactPoint(), {});
		FFalseTriggerSpec Spec; Spec.PerDetectorHour = 60.0; X->SetFalseTriggers(Spec);   // default 3 s pulse
	}
	int32 FalseC = 0; TArray<double> TimesC, TimesD;
	for (int32 I = 1; I <= 30000; ++I)
	{
		TArray<double> Dry; Dry.Init(0.0, 20);
		for (const FLeakAlarm& L : C.Advance(I * 0.02, 0.02, {}, {}, Dry)) { FalseC += L.bFalse ? 1 : 0; TimesC.Add(L.TimeS); }
		for (const FLeakAlarm& L : D.Advance(I * 0.02, 0.02, {}, {}, Dry)) TimesD.Add(L.TimeS);
	}
	AddInfo(FString::Printf(TEXT("오경보 펄스 60 회/시간·감지기 20개·600 s(추정 시험값): 경보 %d건(전부 거짓 %d건, 기대 약 %.0f건 = 감지기 수 × 첫 펄스까지)"), TimesC.Num(), FalseC, 20.0));
	TestTrue(TEXT("pulses longer than the debounce raise false alarms"), TimesC.Num() > 0 && FalseC == TimesC.Num());
	TestTrue(TEXT("the same seed gives the same false alarms"), TimesC == TimesD);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5FActorDelaysTest, "VRG.Sensors.W5F.ActorDelays", VrgW5FFlags)
bool FW5FActorDelaysTest::RunTest(const FString& Parameters)
{
	// Two leaking trays (compute + switch): the switch tray by its BMC, the rack isolation by the BMS at its cycle, the
	// shelves after their delay and the valves at the end of their stroke (all ESTIMATED delays, BMS cycle 5 s).
	FW5FRoom R;
	const auto& Pol = R.Sim->LeakProtectionPolicy;
	R.Run(0.5);
	const int32 Compute = R.Internal(0, 5, 2), Switch = R.Internal(0, 10, 2);
	const int32 SwitchSlot = R.Sim->GetLiveLeakSites()[Switch].Slot;
	R.Sim->SetLiveLeak(Compute, 1.0e-3, TEXT("test"));
	R.Sim->SetLiveLeak(Switch, 1.0e-3, TEXT("test"));
	double SwitchAlarmS = -1.0, SwitchOffS = -1.0;
	for (int32 F = 0; F < 300 && R.EventS(TEXT("supply/return valves shut")) < 0.0; ++F)
	{
		R.Sim->Tick(.1f);
		if (SwitchAlarmS < 0.0) SwitchAlarmS = R.Sim->GetInstruments().GetLeakAlarmTimeS(R.Detector(EInstrumentSite::TrayColdPlate, 0, SwitchSlot));
		if (SwitchOffS < 0.0 && !R.Powered(0, SwitchSlot)) SwitchOffS = R.Now();
	}
	const double Requested = R.EventS(TEXT("rack isolation requested")), Shelves = R.EventS(TEXT("power shelf DC output off")), Valves = R.EventS(TEXT("supply/return valves shut"));
	const double Cycle = FMath::CeilToDouble(Requested / Pol.BmsPollS - 1e-9) * Pol.BmsPollS;
	AddInfo(FString::Printf(TEXT("스위치 트레이 경보 %.2f s → 차단 %.2f s(BMC %.1f s) | 랙 격리 요청 %.2f s → BMS 주기 %.2f s → 셸프 차단 %.2f s(+%.1f) → 밸브 %.2f s(+%.1f)"),
		SwitchAlarmS, SwitchOffS, Pol.TrayBmcDelayS, Requested, Cycle, Shelves, Pol.ShelfOffDelayS, Valves, Pol.ValveStrokeS));
	TestTrue(TEXT("the switch tray goes off after the BMC delay (within one frame)"), SwitchAlarmS > 0.0 && SwitchOffS >= SwitchAlarmS + Pol.TrayBmcDelayS - 1e-6 && SwitchOffS <= SwitchAlarmS + Pol.TrayBmcDelayS + 0.1 + 1e-6);
	TestTrue(TEXT("the shelves go off at the BMS cycle + their delay"), Shelves >= Cycle + Pol.ShelfOffDelayS - 1e-6 && Shelves <= Cycle + Pol.ShelfOffDelayS + 0.1 + 1e-6);
	TestTrue(TEXT("the valves shut at the end of their stroke, not before"), Valves >= Cycle + Pol.ValveStrokeS - 1e-6 && Valves <= Cycle + Pol.ValveStrokeS + 0.1 + 1e-6 && R.Room->IsRackValveClosed(R.Racks[0]));
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual) < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5FFaultsAndFalseTest, "VRG.Sensors.W5F.FaultsAndFalseAlarms", VrgW5FFlags)
bool FW5FFaultsAndFalseTest::RunTest(const FString& Parameters)
{
	FW5FRoom R;
	R.Run(0.5);
	// A detector fault only alerts (NICo: field check), even with the protection on.
	const int32 Slot = R.Sim->GetLiveLeakSites()[R.Internal(0, 0, 2)].Slot;
	const int32 Faulty = R.Detector(EInstrumentSite::TrayColdPlate, 0, Slot);
	TestTrue(TEXT("fault set"), R.Sim->SetSensorHealth(Faulty, ESensorHealth::OpenCircuit));
	R.Run(3.0);
	int32 ActionsOnRack0 = 0;
	for (const auto& E : R.Sim->GetLeakProtectionEvents()) ActionsOnRack0 += E.Rack == 0 ? 1 : 0;
	TestTrue(TEXT("sensor fault: alert only, nothing switched off"), R.EventS(TEXT("sensor fault")) > 0.0 && ActionsOnRack0 == 0 && R.Powered(0, Slot));
	// A stuck-wet detector cannot be told from a leak: the tray protection acts on it; the report knows it was false.
	const int32 OtherSlot = R.Sim->GetLiveLeakSites()[R.Internal(1, 0, 2)].Slot;
	const int32 Stuck = R.Detector(EInstrumentSite::TrayColdPlate, 1, OtherSlot);
	TestTrue(TEXT("stuck set"), R.Sim->SetSensorHealth(Stuck, ESensorHealth::StuckWet));
	R.Run(3.0);
	bool bFalse = false;
	for (const FLeakAlarm& A : R.Sim->GetInstruments().GetAlarmLog()) bFalse |= A.Point == Stuck && A.bFalse;
	TestTrue(TEXT("stuck wet: a false alarm starts the tray protection"), bFalse && R.EventS(TEXT("shutdown timer started"), 1) > 0.0);
	TestEqual(TEXT("no water anywhere"), R.Sim->GetRackInteriorCoupling().GetWater().ReceivedM3, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FW5FRecoveryTest, "VRG.Sensors.W5F.Recovery", VrgW5FFlags)
bool FW5FRecoveryTest::RunTest(const FString& Parameters)
{
	// W4 on with a short trip (test value) so the leaking compute tray trips by water before its 10 s shutdown timer;
	// two leaking trays then isolate the rack. Recovery must restore everything protection cut, but not that tray.
	FW5FRoom R(TEXT("vrg.Water.Impact 1 0.3 100"));
	auto& Pol = R.Sim->LeakProtectionPolicy;
	Pol.ValveStrokeS = 2.0; Pol.RecoveryPowerOnDelayS = 1.0;   // shorter for the test; the order is what is checked
	R.Run(0.5);
	const int32 Compute = R.Internal(0, 5, 2), Switch = R.Internal(0, 10, 2);
	const int32 ComputeSlot = R.Sim->GetLiveLeakSites()[Compute].Slot, SwitchSlot = R.Sim->GetLiveLeakSites()[Switch].Slot;
	R.Sim->SetLiveLeak(Compute, 1.0e-3, TEXT("test"));
	R.Sim->SetLiveLeak(Switch, 1.0e-3, TEXT("test"));
	R.Run(12.0);
	const double ItIsolated = R.Racks[0]->GetModel().GetLastTotalHeatW();
	TestTrue(TEXT("rack isolated"), R.Room->IsRackValveClosed(R.Racks[0]) && R.Racks[0]->GetModel().IsBusbarTripped() && ItIsolated == 0.0);
	const bool bWaterTripped = R.Sim->GetWaterRackPowerCuts().ContainsByPredicate([&](const auto& C) { return C.Slot == ComputeSlot; });
	TestTrue(TEXT("the leaking compute tray was tripped by water (W4) before its timer"), bWaterTripped);
	FString Reason;
	TestFalse(TEXT("recovery refused while the leak is open"), R.Sim->RecoverLeakProtection(0, INDEX_NONE, Reason));
	AddInfo(TEXT("거절 1: ") + Reason);
	R.Sim->SetLiveLeak(Compute, 0.0, TEXT("test")); R.Sim->SetLiveLeak(Switch, 0.0, TEXT("test"));
	R.Run(0.5);
	TestFalse(TEXT("recovery refused while alarms are latched"), R.Sim->RecoverLeakProtection(0, INDEX_NONE, Reason));
	AddInfo(TEXT("거절 2: ") + Reason);
	// The trays hold water (no drying model): resetting re-raises their alarms after the debounce; recover in between.
	R.Sim->ResetLeakAlarms(INDEX_NONE);
	const bool bOk = R.Sim->RecoverLeakProtection(0, INDEX_NONE, Reason);
	AddInfo(TEXT("복구: ") + Reason);
	TestTrue(TEXT("recovery accepted once repaired and reset"), bOk);
	// No drying model: the wet tray detectors alarm again after their debounce. With the automatic protection still on
	// they would isolate the rack again, so the operator recovers with it switched off (the queued recovery still runs).
	Pol.bEnabled = false;
	const double T0 = R.Now();
	R.Run(Pol.ValveStrokeS + Pol.RecoveryPowerOnDelayS + 0.5);
	const double Open = R.EventS(TEXT("supply/return valves open")), Power = R.EventS(TEXT("rack breaker closed"));
	AddInfo(FString::Printf(TEXT("복구 예약 %.2f s → 밸브 열림 %.2f s → 차단기·전원 %.2f s, 랙 0 발열 %.1f kW, 손상 트레이 전원 %d"),
		T0, Open, Power, R.Racks[0]->GetModel().GetLastTotalHeatW() * 1e-3, R.Powered(0, ComputeSlot) ? 1 : 0));
	TestTrue(TEXT("valves first, then breaker and power"), Open > 0.0 && Power > Open && !R.Room->IsRackValveClosed(R.Racks[0]));
	TestTrue(TEXT("the rack runs again (busbar reclosed)"), !R.Racks[0]->GetModel().IsBusbarTripped() && R.Racks[0]->GetModel().GetLastTotalHeatW() > 0.0);
	// Every slot: on unless the W4 water path cut it (both leaking trays were tripped by water here).
	int32 On = 0, KeptOff = 0, Wrong = 0;
	for (int32 S = 0; S < R.Racks[0]->Params.Slots.Num(); ++S)
	{
		const FString Id = TEXT("rack/") + VrgRackLifecycle::Identity(R.Racks[0]);
		const bool bWater = R.Sim->GetWaterRackPowerCuts().ContainsByPredicate([&](const auto& C) { return C.Slot == S && C.EquipmentId == Id; });
		if (bWater) { KeptOff += R.Powered(0, S) ? 0 : 1; Wrong += R.Powered(0, S) ? 1 : 0; }
		else { On += R.Powered(0, S) ? 1 : 0; Wrong += R.Powered(0, S) ? 0 : 1; }
	}
	AddInfo(FString::Printf(TEXT("복구 뒤 랙 0 슬롯: 켜짐 %d, W4 차단 유지 %d, 어긋남 %d (스위치 트레이 W4 차단 %d)"), On, KeptOff, Wrong,
		R.Sim->GetWaterRackPowerCuts().ContainsByPredicate([&](const auto& C) { return C.Slot == SwitchSlot; }) ? 1 : 0));
	TestEqual(TEXT("protection-cut slots on again, W4-cut slots kept off until R"), Wrong, 0);
	TestFalse(TEXT("the W4-tripped tray stays off until R"), R.Powered(0, ComputeSlot));
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual) < 1e-9);
	return true;
}

#endif
