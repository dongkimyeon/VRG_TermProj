// Fill out your copyright notice in the Description page of Project Settings.

// W5-3 (Tools/누수_실시간_계획.md L5 (다), 2026-11-03): protective shutdown and isolation from the leak detectors.
// The policy, its sources and its estimates are on ADataCenterSimulationActor::FLeakProtectionPolicy. Decisions are made
// after the instruments of a step and act on the inputs of a later step (slot power, rack valves, CDU power), so both
// step loops (Tick and RunSteps, which share EvalAfterStep) behave the same.
//
// W5 follow-up (2026-11-10): who acts and how long it takes. A tray detector is read by the tray BMC (its own delay, then
// the compute shutdown timer or the switch out-of-band power off); rack- and row-level decisions go through the BMS,
// which acts at its next cycle; the shelf DC output, the valve stroke and the CDU stop each take their own time. Every
// action is queued with its due time and applied at the first step end at or after it. Sensor faults alert only (NICo:
// field check). Recovery (vrg.Leak.Recover) undoes only what protection did, in reverse order, and never re-powers what
// the W4 water path tripped or damaged (those stay off until the R restart, user decision 2026-11-05).

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GVrgLeakAutoIsolate(TEXT("vrg.Leak.AutoIsolate"),
		TEXT("누수 감지 보호 차단·격리 정책(기본 0=끔). vrg.Leak.AutoIsolate 0|1 [컴퓨트 트레이 셧다운 지연 s]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World) return;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->LeakProtectionPolicy.bEnabled = Args.Num() > 0 && FCString::Atoi(*Args[0]) != 0;
				if (Args.Num() > 1) It->LeakProtectionPolicy.ComputeTrayShutdownDelayS = FMath::Max(0.0, FCString::Atod(*Args[1]));
				const auto& P = It->LeakProtectionPolicy;
				UE_LOG(LogTemp, Display, TEXT("[Protect] 누수 보호 정책 %s (추정: 트레이 BMC %.1f s, 컴퓨트 셧다운 %.1f s, BMS 주기 %.1f s, 셸프 차단 %.1f s, 밸브 행정 %.1f s, CDU 정지 %.1f s; 랙 격리 누수 트레이 %d개)"),
					P.bEnabled ? TEXT("켬") : TEXT("끔"), P.TrayBmcDelayS, P.ComputeTrayShutdownDelayS, P.BmsPollS, P.ShelfOffDelayS, P.ValveStrokeS, P.CduStopDelayS, P.RackIsolationTrays);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GVrgLeakRecover(TEXT("vrg.Leak.Recover"),
		TEXT("보호 차단 복구(경보 해제·누수 수리 뒤). 밸브 열기 → CDU 재가동 → 버스바 재투입·전원. W4 트립·손상은 R 재시작까지 그대로. vrg.Leak.Recover all|rack <번호>|row <번호>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World) return;
			int32 Rack = INDEX_NONE, Row = INDEX_NONE;
			if (Args.Num() >= 2 && Args[0].Equals(TEXT("rack"), ESearchCase::IgnoreCase)) Rack = FCString::Atoi(*Args[1]);
			else if (Args.Num() >= 2 && Args[0].Equals(TEXT("row"), ESearchCase::IgnoreCase)) Row = FCString::Atoi(*Args[1]);
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				FString Reason;
				const bool bOk = It->RecoverLeakProtection(Rack, Row, Reason);
				UE_LOG(LogTemp, Display, TEXT("[Protect] 복구 %s: %s"), bOk ? TEXT("예약") : TEXT("거절"), *Reason);
			}
		}));

	bool IsShelfSlot(const ARackThermalActor* Rack, int32 Slot)
	{
		const int32 Type = Rack->Params.Slots.IsValidIndex(Slot) ? Rack->Params.Slots[Slot].DeviceType : INDEX_NONE;
		if (!Rack->Params.DeviceTypes.IsValidIndex(Type)) return false;
		for (const FRackComponentParams& P : Rack->Params.DeviceTypes[Type].Components) if (P.LossFractionOfRackLoad > 0.0) return true;
		return false;
	}
}

void ADataCenterSimulationActor::ResetLeakProtection()
{
	ProtectionAlarmCursor = ProtectionProcessCursor = ProtectionFaultCursor = 0;
	ProtectionLeakingTrays.Reset();
	ProtectionTimers.Reset();
	ProtectionIsolatedRacks.Reset();
	ProtectionShutRows.Reset();
	ProtectionCutSlots.Reset();
	ProtectionPending.Reset();
	LeakProtectionEvents.Reset();
}

double ADataCenterSimulationActor::NextBmsCycleS(double NowS) const
{
	// The BMS acts at its next cycle at or after the alarm (fixed phase from t = 0). 0 = at once.
	const double P = LeakProtectionPolicy.BmsPollS;
	return P > 0.0 ? FMath::CeilToDouble(NowS / P - 1e-9) * P : NowS;
}

void ADataCenterSimulationActor::QueueProtection(double DueS, EProtectionAction Action, int32 Rack, int32 Slot, int32 Row, const FString& Level, const FString& Trigger)
{
	FPendingProtection& P = ProtectionPending.AddDefaulted_GetRef();
	P.DueS = DueS; P.Action = Action; P.Rack = Rack; P.Slot = Slot; P.Row = Row; P.Level = Level; P.Trigger = Trigger;
}

void ADataCenterSimulationActor::ProtectSlotOff(int32 Rack, int32 Slot, const FString& Level, const FString& Trigger)
{
	ARackThermalActor* R = Racks.IsValidIndex(Rack) ? Racks[Rack].Get() : nullptr;
	if (!R) return;
	FRackThermalInput& In = R->GetMutableInput();
	if (In.SlotPowered.IsEmpty()) In.SlotPowered.Init(true, FRackThermalModel::NumSlotsOf(R->Params));
	if (!In.SlotPowered.IsValidIndex(Slot) || !In.SlotPowered[Slot]) return;
	In.SlotPowered[Slot] = false;
	ProtectionCutSlots.Add(FIntPoint(Rack, Slot));
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), Level, TEXT("slot power off"), Trigger, Rack, Slot, INDEX_NONE });
	UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s %s: 랙 %d 슬롯 %d 전원 차단 (원인: 보호 동작, 감지 %s)"), Clock.GetSimTimeS(), *Level, Rack, Slot, *Trigger);
}

void ADataCenterSimulationActor::ProtectRackIsolate(int32 Rack, const FString& Trigger)
{
	ARackThermalActor* R = Racks.IsValidIndex(Rack) ? Racks[Rack].Get() : nullptr;
	if (!R || ProtectionIsolatedRacks.Contains(Rack)) return;
	ProtectionIsolatedRacks.Add(Rack);
	// The BMS decides at its next cycle: power shelf DC output off, then the supply and return valves at the end of their stroke.
	const double Decide = NextBmsCycleS(Clock.GetSimTimeS());
	QueueProtection(Decide + LeakProtectionPolicy.ShelfOffDelayS, EProtectionAction::ShelvesOff, Rack, INDEX_NONE, INDEX_NONE, TEXT("rack"), Trigger);
	QueueProtection(Decide + LeakProtectionPolicy.ValveStrokeS, EProtectionAction::ValvesClose, Rack, INDEX_NONE, INDEX_NONE, TEXT("rack"), Trigger);
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), TEXT("rack"), FString::Printf(TEXT("rack isolation requested (BMS cycle %.2f s)"), Decide), Trigger, Rack, INDEX_NONE, INDEX_NONE });
}

void ADataCenterSimulationActor::ProtectRowShutdown(int32 Row, const FString& Trigger)
{
	if (Room == nullptr || Row == INDEX_NONE || ProtectionShutRows.Contains(Row)) return;
	ProtectionShutRows.Add(Row);
	const double Decide = NextBmsCycleS(Clock.GetSimTimeS());
	QueueProtection(Decide + LeakProtectionPolicy.ShelfOffDelayS, EProtectionAction::RowShelvesOff, INDEX_NONE, INDEX_NONE, Row, TEXT("row"), Trigger);
	QueueProtection(Decide + LeakProtectionPolicy.CduStopDelayS, EProtectionAction::CduStop, INDEX_NONE, INDEX_NONE, Row, TEXT("row"), Trigger);
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), TEXT("row"), FString::Printf(TEXT("row shutdown requested (BMS cycle %.2f s)"), Decide), Trigger, INDEX_NONE, INDEX_NONE, Row });
}

TArray<int32> ADataCenterSimulationActor::RowRacks(int32 Row) const
{
	TArray<int32> Out;
	if (Room == nullptr) return Out;
	const TArray<int32> RackRows = Room->GetRoutedRackRows();
	for (int32 R = 0; R < Racks.Num() && R < RackRows.Num(); ++R) if (RackRows[R] == Row && Racks[R]) Out.Add(R);
	return Out;
}

void ADataCenterSimulationActor::ApplyProtection(const FPendingProtection& P)
{
	const double Now = Clock.GetSimTimeS();
	ARackThermalActor* R = Racks.IsValidIndex(P.Rack) ? Racks[P.Rack].Get() : nullptr;
	auto ShelvesOff = [&](int32 Rack, const FString& Level)
	{
		if (!Racks.IsValidIndex(Rack) || !Racks[Rack]) return;
		for (int32 S = 0; S < Racks[Rack]->Params.Slots.Num(); ++S) if (IsShelfSlot(Racks[Rack], S)) ProtectSlotOff(Rack, S, Level, P.Trigger);
	};
	auto PowerOn = [&](int32 Rack)
	{
		// Only what protection switched off, and never a slot the W4 water path cut (tripped/damaged stays off until R).
		ARackThermalActor* A = Racks.IsValidIndex(Rack) ? Racks[Rack].Get() : nullptr;
		if (!A) return;
		const FString Id = TEXT("rack/") + VrgRackLifecycle::Identity(A);
		FRackThermalInput& In = A->GetMutableInput();
		int32 On = 0, Kept = 0;
		for (int32 S = 0; S < In.SlotPowered.Num(); ++S)
		{
			if (!ProtectionCutSlots.Contains(FIntPoint(Rack, S))) continue;
			const bool bWaterCut = GetWaterRackPowerCuts().ContainsByPredicate([&](const FWaterRackPowerCut& C) { return C.EquipmentId == Id && C.Slot == S; });
			if (bWaterCut) { ++Kept; continue; }
			In.SlotPowered[S] = true; ProtectionCutSlots.Remove(FIntPoint(Rack, S)); ++On;
		}
		// The rack breaker is closed again by the operator: the busbar re-checks its capacity at the next step and trips
		// again if the live shelves cannot carry the load (e.g. a W4-damaged shelf stays off).
		A->GetMutableModel().ResetBusbarTrip();
		LeakProtectionEvents.Add({ Now, TEXT("recover"), FString::Printf(TEXT("rack breaker closed, %d slots powered on, %d W4-cut slots kept off"), On, Kept), P.Trigger, Rack, INDEX_NONE, INDEX_NONE });
	};
	switch (P.Action)
	{
	case EProtectionAction::SlotOff:
		ProtectSlotOff(P.Rack, P.Slot, P.Level, P.Trigger);
		break;
	case EProtectionAction::ShelvesOff:
		ShelvesOff(P.Rack, P.Level);
		LeakProtectionEvents.Add({ Now, TEXT("rack"), TEXT("power shelf DC output off"), P.Trigger, P.Rack, INDEX_NONE, INDEX_NONE });
		break;
	case EProtectionAction::ValvesClose:
	{
		const bool bValves = R != nullptr && Room != nullptr && Room->SetRackValveClosed(R, true);
		LeakProtectionEvents.Add({ Now, TEXT("rack"), bValves ? TEXT("rack isolated: supply/return valves shut") : TEXT("rack valves not available"), P.Trigger, P.Rack, INDEX_NONE, INDEX_NONE });
		UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s 랙 %d 격리: 공급·회수 밸브 %s (감지 %s)"), Now, P.Rack, bValves ? TEXT("차단") : TEXT("없음"), *P.Trigger);
		break;
	}
	case EProtectionAction::RowShelvesOff:
		for (int32 Rack : RowRacks(P.Row)) ShelvesOff(Rack, TEXT("row"));
		LeakProtectionEvents.Add({ Now, TEXT("row"), TEXT("row racks power off"), P.Trigger, INDEX_NONE, INDEX_NONE, P.Row });
		UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s 줄 %d: 모든 랙 전원 차단 (감지 %s)"), Now, P.Row, *P.Trigger);
		break;
	case EProtectionAction::CduStop:
	{
		ACoolingDistributionUnitActor* Cdu = Room ? Room->GetRowCdu(P.Row) : nullptr;
		if (Cdu && Cdu->IsPumpPowered()) { Cdu->SetPumpPowered(false); ProtectionStoppedCdus.Add(P.Row); }   // the room syncs the network pump at the next step
		LeakProtectionEvents.Add({ Now, TEXT("row"), Cdu ? TEXT("row CDU shut down") : TEXT("no row CDU"), P.Trigger, INDEX_NONE, INDEX_NONE, P.Row });
		break;
	}
	case EProtectionAction::ValvesOpen:
	{
		const bool bValves = R != nullptr && Room != nullptr && Room->SetRackValveClosed(R, false);
		LeakProtectionEvents.Add({ Now, TEXT("recover"), bValves ? TEXT("supply/return valves open") : TEXT("rack valves not available"), P.Trigger, P.Rack, INDEX_NONE, INDEX_NONE });
		break;
	}
	case EProtectionAction::CduStart:
	{
		ACoolingDistributionUnitActor* Cdu = Room ? Room->GetRowCdu(P.Row) : nullptr;
		const FString Id = Cdu ? TEXT("cdu/") + VrgCduLifecycle::Identity(Cdu) : FString();
		const bool bWaterCut = GetWaterCduPowerCuts().ContainsByPredicate([&](const FWaterCduPowerCut& C) { return C.EquipmentId == Id; });
		if (Cdu && !bWaterCut && ProtectionStoppedCdus.Contains(P.Row)) { Cdu->SetPumpPowered(true); ProtectionStoppedCdus.Remove(P.Row); }
		LeakProtectionEvents.Add({ Now, TEXT("recover"), !Cdu ? TEXT("no row CDU") : bWaterCut ? TEXT("row CDU kept off (W4 water trip)") : TEXT("row CDU restarted"), P.Trigger, INDEX_NONE, INDEX_NONE, P.Row });
		break;
	}
	case EProtectionAction::PowerOn:
		PowerOn(P.Rack);
		break;
	case EProtectionAction::RecoveryDone:
		// The scope can be protected again by new alarms.
		if (P.Rack != INDEX_NONE) { ProtectionIsolatedRacks.Remove(P.Rack); ProtectionLeakingTrays.Remove(P.Rack); }
		if (P.Row != INDEX_NONE) ProtectionShutRows.Remove(P.Row);
		LeakProtectionEvents.Add({ Now, TEXT("recover"), TEXT("recovery complete"), P.Trigger, P.Rack, INDEX_NONE, P.Row });
		break;
	}
}

int32 ADataCenterSimulationActor::LiveLeakSiteRack(int32 SiteIndex) const
{
	if (!LiveLeakSites.IsValidIndex(SiteIndex) || LiveLeakSites[SiteIndex].bPlant) return INDEX_NONE;
	const FLiveLeakSite& S = LiveLeakSites[SiteIndex];
	if (S.InternalSite != INDEX_NONE) return S.RackIndex;
	if (Room == nullptr) return INDEX_NONE;
	return Room->GetRoutedRacks().IndexOfByKey(Room->GetLeakSpecRack(S.RoomSpec));
}

bool ADataCenterSimulationActor::RecoverLeakProtection(int32 Rack, int32 Row, FString& Reason)
{
	if (Mode != ESimMode::Simulation || Room == nullptr) { Reason = TEXT("시뮬레이션 중에만"); return false; }
	// Scope: the racks and rows protection isolated, narrowed to the request.
	TSet<int32> ScopeRacks, ScopeRows;
	const TArray<int32> RackRows = Room->GetRoutedRackRows();
	for (int32 R : ProtectionIsolatedRacks) if ((Rack == INDEX_NONE && Row == INDEX_NONE) || R == Rack || (Row != INDEX_NONE && RackRows.IsValidIndex(R) && RackRows[R] == Row)) ScopeRacks.Add(R);
	for (int32 W : ProtectionShutRows) if ((Rack == INDEX_NONE && Row == INDEX_NONE) || W == Row) { ScopeRows.Add(W); for (int32 R : RowRacks(W)) ScopeRacks.Add(R); }
	for (const FIntPoint& C : ProtectionCutSlots) if ((Rack == INDEX_NONE && Row == INDEX_NONE) || C.X == Rack || (Row != INDEX_NONE && RackRows.IsValidIndex(C.X) && RackRows[C.X] == Row)) ScopeRacks.Add(C.X);
	if (ScopeRacks.IsEmpty() && ScopeRows.IsEmpty()) { Reason = TEXT("복구할 보호 차단이 없음"); return false; }
	// 1. The leak must be repaired (every live room site of the scope closed).
	TArray<FString> Open;
	for (int32 I = 0; I < LiveLeakSites.Num(); ++I)
		if (GetLiveLeakHoleDiameterM(I) > 0.0 && ScopeRacks.Contains(LiveLeakSiteRack(I))) Open.Add(LiveLeakSites[I].Label);
	if (!Open.IsEmpty()) { Reason = FString::Printf(TEXT("누수가 아직 열려 있음: %s (vrg.Leak.Repair 먼저)"), *FString::Join(Open, TEXT(", "))); return false; }
	// 2. The operator must have cleared the alarms of the scope (water still there raises them again).
	TArray<FString> Alarmed;
	for (int32 I = 0; I < Instruments.Num(); ++I)
	{
		if (!Instruments.IsLeakAlarm(I)) continue;
		const FInstrumentPoint& P = Instruments.GetPoint(I);
		const bool bRackSensor = P.Site == EInstrumentSite::TrayColdPlate || P.Site == EInstrumentSite::TrayManifold || P.Site == EInstrumentSite::RackDripTray || P.Site == EInstrumentSite::RackManifoldRope;
		const int32 SensorRow = P.Site == EInstrumentSite::RoomPipe ? (P.Key >= 0 ? Room->GetPipeRow(P.Key) : Room->GetPumpRow(-1 - P.Key)) : INDEX_NONE;
		if ((bRackSensor && ScopeRacks.Contains(P.Key)) || (SensorRow != INDEX_NONE && ScopeRows.Contains(SensorRow))) Alarmed.Add(P.Id.IsEmpty() ? P.Label : P.Id);
	}
	if (!Alarmed.IsEmpty()) { Reason = FString::Printf(TEXT("경보가 남아 있음(%d개, 예: %s) - vrg.Sensors.ResetLeak 먼저"), Alarmed.Num(), *Alarmed[0]); return false; }
	// 3. Reverse order: valves open (stroke), the row CDU restarts, then breakers and power (ESTIMATED delays).
	const double Now = Clock.GetSimTimeS(), Open1 = Now + LeakProtectionPolicy.ValveStrokeS, PowerS = Open1 + LeakProtectionPolicy.RecoveryPowerOnDelayS;
	const FString Trigger = TEXT("operator recovery");
	TArray<int32> SortedRacks = ScopeRacks.Array(); SortedRacks.Sort();
	TArray<int32> SortedRows = ScopeRows.Array(); SortedRows.Sort();
	for (int32 R : SortedRacks) if (Racks.IsValidIndex(R) && Room->IsRackValveClosed(Racks[R])) QueueProtection(Open1, EProtectionAction::ValvesOpen, R, INDEX_NONE, INDEX_NONE, TEXT("recover"), Trigger);
	for (int32 W : SortedRows) QueueProtection(Open1, EProtectionAction::CduStart, INDEX_NONE, INDEX_NONE, W, TEXT("recover"), Trigger);
	for (int32 R : SortedRacks) QueueProtection(PowerS, EProtectionAction::PowerOn, R, INDEX_NONE, INDEX_NONE, TEXT("recover"), Trigger);
	for (int32 R : SortedRacks) QueueProtection(PowerS, EProtectionAction::RecoveryDone, R, INDEX_NONE, INDEX_NONE, TEXT("recover"), Trigger);
	for (int32 W : SortedRows) QueueProtection(PowerS, EProtectionAction::RecoveryDone, INDEX_NONE, INDEX_NONE, W, TEXT("recover"), Trigger);
	// Pending protection of this scope (e.g. a compute timer still running) is dropped: the operator has the scope now.
	ProtectionTimers.RemoveAll([&](const TTuple<int32, int32, double>& T) { return ScopeRacks.Contains(T.Get<0>()); });
	LeakProtectionEvents.Add({ Now, TEXT("recover"), FString::Printf(TEXT("recovery scheduled: valves %.1f s, power %.1f s"), Open1, PowerS), Trigger, Rack, INDEX_NONE, Row });
	Reason = FString::Printf(TEXT("랙 %d대·줄 %d개, 밸브 열림 %.1f s, 전원 %.1f s(추정 지연)"), SortedRacks.Num(), SortedRows.Num(), Open1, PowerS);
	return true;
}

void ADataCenterSimulationActor::LeakProtectionAfterStep()
{
	const TArray<FLeakAlarm>& Alarms = Instruments.GetAlarmLog();
	const TArray<FProcessLeakAlarm>& Process = ProcessMonitor.GetAlarmLog();
	const TArray<FSensorFaultAlarm>& Faults = Instruments.GetFaultLog();
	const double Now = Clock.GetSimTimeS();
	// Recovery steps run whatever the policy (the operator asked for them).
	auto ApplyDue = [&](bool bRecoveryOnly)
	{
		ProtectionPending.StableSort([](const FPendingProtection& A, const FPendingProtection& B) { return A.DueS < B.DueS; });
		for (int32 I = 0; I < ProtectionPending.Num();)
		{
			const bool bRecovery = ProtectionPending[I].Level == TEXT("recover");
			if (ProtectionPending[I].DueS > Now + 1e-9 || (bRecoveryOnly && !bRecovery)) { ++I; continue; }
			const FPendingProtection P = ProtectionPending[I];
			ProtectionPending.RemoveAt(I);
			ApplyProtection(P);
		}
	};
	if (!LeakProtectionPolicy.bEnabled)
	{
		// Policy off: nothing acts; the cursors follow so a later enable does not replay old alarms as new.
		ProtectionAlarmCursor = Alarms.Num(); ProtectionProcessCursor = Process.Num(); ProtectionFaultCursor = Faults.Num();
		ProtectionTimers.Reset();
		ProtectionPending.RemoveAll([](const FPendingProtection& P) { return P.Level != TEXT("recover"); });
		ApplyDue(true);
		return;
	}
	for (; ProtectionAlarmCursor < Alarms.Num(); ++ProtectionAlarmCursor)
	{
		const FLeakAlarm& A = Alarms[ProtectionAlarmCursor];
		const FInstrumentPoint& P = Instruments.GetPoint(A.Point);
		const FString Trigger = P.Id.IsEmpty() ? P.Label : P.Id;
		switch (P.Site)
		{
		case EInstrumentSite::TrayColdPlate: case EInstrumentSite::TrayManifold:
		{
			const ARackThermalActor* R = Racks.IsValidIndex(P.Key) ? Racks[P.Key].Get() : nullptr;
			if (!R || !R->Params.Slots.IsValidIndex(P.Loop)) break;
			// Tray level, by the tray BMC after its own delay: compute trays shut down after the BMC timer, switch trays at
			// once (out-of-band power off).
			const double BmcS = Now + LeakProtectionPolicy.TrayBmcDelayS;
			if (R->Params.Slots[P.Loop].DeviceType == FRackThermalModel::Gb200DevCompute && LeakProtectionPolicy.ComputeTrayShutdownDelayS > 0.0)
			{
				if (!ProtectionTimers.ContainsByPredicate([&](const TTuple<int32, int32, double>& T) { return T.Get<0>() == P.Key && T.Get<1>() == P.Loop; }))
				{
					ProtectionTimers.Add(MakeTuple(P.Key, P.Loop, BmcS + LeakProtectionPolicy.ComputeTrayShutdownDelayS));
					LeakProtectionEvents.Add({ Now, TEXT("tray"), TEXT("shutdown timer started"), Trigger, P.Key, P.Loop, INDEX_NONE });
				}
			}
			else QueueProtection(BmcS, EProtectionAction::SlotOff, P.Key, P.Loop, INDEX_NONE, TEXT("tray"), Trigger);
			TSet<int32>& Leaking = ProtectionLeakingTrays.FindOrAdd(P.Key);
			Leaking.Add(P.Loop);
			if (Leaking.Num() >= LeakProtectionPolicy.RackIsolationTrays) ProtectRackIsolate(P.Key, FString::Printf(TEXT("%d leaking trays"), Leaking.Num()));
			break;
		}
		case EInstrumentSite::RackDripTray: case EInstrumentSite::RackManifoldRope:
			ProtectRackIsolate(P.Key, Trigger);
			break;
		case EInstrumentSite::RoomPipe:
			if (Room) ProtectRowShutdown(P.Key >= 0 ? Room->GetPipeRow(P.Key) : Room->GetPumpRow(-1 - P.Key), Trigger);
			break;
		case EInstrumentSite::CduDripTray: case EInstrumentSite::CduFloorSpot:
		{
			FString CduName; P.Id.Split(TEXT(":"), &CduName, nullptr);
			for (int32 Row = 0; Room && Row < Room->GetNumRows(); ++Row)
				if (Room->GetRowCdu(Row) && Room->GetRowCdu(Row)->GetName() == CduName) ProtectRowShutdown(Row, Trigger);
			break;
		}
		default:   // yard ropes: no rack row to act on - alert only
			LeakProtectionEvents.Add({ Now, TEXT("alert"), TEXT("alarm only"), Trigger, INDEX_NONE, INDEX_NONE, INDEX_NONE });
			break;
		}
	}
	// Process-value alarms give no location: alert only (no automatic action, ESTIMATE of the operating policy).
	for (; ProtectionProcessCursor < Process.Num(); ++ProtectionProcessCursor)
	{
		const FProcessLeakAlarm& A = Process[ProtectionProcessCursor];
		LeakProtectionEvents.Add({ Now, TEXT("alert"), TEXT("alarm only (location unknown)"),
			FString::Printf(TEXT("%s %s"), *ProcessMonitor.GetLoopName(A.Loop), FProcessLeakMonitor::SignalName(A.Signal)), INDEX_NONE, INDEX_NONE, INDEX_NONE });
	}
	// Detector faults (open circuit seen by the cable supervision): NICo sends a field check, no automatic action.
	for (; ProtectionFaultCursor < Faults.Num(); ++ProtectionFaultCursor)
	{
		const FInstrumentPoint& P = Instruments.GetPoint(Faults[ProtectionFaultCursor].Point);
		LeakProtectionEvents.Add({ Now, TEXT("alert"), TEXT("sensor fault: field check (no automatic action)"), P.Id.IsEmpty() ? P.Label : P.Id, INDEX_NONE, INDEX_NONE, INDEX_NONE });
	}
	for (int32 T = ProtectionTimers.Num() - 1; T >= 0; --T)
	{
		if (Now + 1e-9 < ProtectionTimers[T].Get<2>()) continue;
		ProtectSlotOff(ProtectionTimers[T].Get<0>(), ProtectionTimers[T].Get<1>(), TEXT("tray"), TEXT("shutdown timer"));
		ProtectionTimers.RemoveAt(T);
	}
	ApplyDue(false);
}
