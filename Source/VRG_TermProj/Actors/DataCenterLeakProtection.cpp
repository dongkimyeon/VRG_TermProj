// Fill out your copyright notice in the Description page of Project Settings.

// W5-3 (Tools/누수_실시간_계획.md L5 (다), 2026-11-03): protective shutdown and isolation from the leak detectors.
// The policy, its sources and its estimates are on ADataCenterSimulationActor::FLeakProtectionPolicy. Decisions are made
// after the instruments of a step and act on the inputs of the next step (slot power, rack valves, CDU power), so both
// step loops (Tick and RunSteps, which share EvalAfterStep) behave the same.

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
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
				UE_LOG(LogTemp, Display, TEXT("[Protect] 누수 보호 정책 %s (컴퓨트 트레이 셧다운 지연 %.1f s 추정, 랙 격리 누수 트레이 %d개)"),
					It->LeakProtectionPolicy.bEnabled ? TEXT("켬") : TEXT("끔"), It->LeakProtectionPolicy.ComputeTrayShutdownDelayS, It->LeakProtectionPolicy.RackIsolationTrays);
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
	ProtectionAlarmCursor = ProtectionProcessCursor = 0;
	ProtectionLeakingTrays.Reset();
	ProtectionTimers.Reset();
	ProtectionIsolatedRacks.Reset();
	ProtectionShutRows.Reset();
	LeakProtectionEvents.Reset();
}

void ADataCenterSimulationActor::ProtectSlotOff(int32 Rack, int32 Slot, const FString& Level, const FString& Trigger)
{
	ARackThermalActor* R = Racks.IsValidIndex(Rack) ? Racks[Rack].Get() : nullptr;
	if (!R) return;
	FRackThermalInput& In = R->GetMutableInput();
	if (In.SlotPowered.IsEmpty()) In.SlotPowered.Init(true, FRackThermalModel::NumSlotsOf(R->Params));
	if (!In.SlotPowered.IsValidIndex(Slot) || !In.SlotPowered[Slot]) return;
	In.SlotPowered[Slot] = false;
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), Level, TEXT("slot power off"), Trigger, Rack, Slot, INDEX_NONE });
	UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s %s: 랙 %d 슬롯 %d 전원 차단 (원인: 보호 동작, 감지 %s)"), Clock.GetSimTimeS(), *Level, Rack, Slot, *Trigger);
}

void ADataCenterSimulationActor::ProtectRackIsolate(int32 Rack, const FString& Trigger)
{
	ARackThermalActor* R = Racks.IsValidIndex(Rack) ? Racks[Rack].Get() : nullptr;
	if (!R || ProtectionIsolatedRacks.Contains(Rack)) return;
	ProtectionIsolatedRacks.Add(Rack);
	// 1. Power shelf DC output off (every shelf off -> the shared busbar has no source -> the whole rack is off).
	for (int32 S = 0; S < R->Params.Slots.Num(); ++S)
		if (IsShelfSlot(R, S)) ProtectSlotOff(Rack, S, TEXT("rack"), Trigger);
	// 2. Rack supply and return valves shut (the network solve gives what that changes, D4).
	const bool bValves = Room != nullptr && Room->SetRackValveClosed(R, true);
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), TEXT("rack"), bValves ? TEXT("rack isolated: power shelves off, supply/return valves shut") : TEXT("rack power shelves off (no valve)"),
		Trigger, Rack, INDEX_NONE, INDEX_NONE });
	UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s 랙 %d 격리: 전원 셸프 출력 차단%s (감지 %s)"), Clock.GetSimTimeS(), Rack, bValves ? TEXT(", 공급·회수 밸브 차단") : TEXT(""), *Trigger);
}

void ADataCenterSimulationActor::ProtectRowShutdown(int32 Row, const FString& Trigger)
{
	if (Room == nullptr || Row == INDEX_NONE || ProtectionShutRows.Contains(Row)) return;
	ProtectionShutRows.Add(Row);
	const TArray<int32> RackRows = Room->GetRoutedRackRows();
	for (int32 R = 0; R < Racks.Num() && R < RackRows.Num(); ++R)
		if (RackRows[R] == Row && Racks[R])
			for (int32 S = 0; S < Racks[R]->Params.Slots.Num(); ++S)
				if (IsShelfSlot(Racks[R], S)) ProtectSlotOff(R, S, TEXT("row"), Trigger);
	ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(Row);
	if (Cdu) Cdu->SetPumpPowered(false);   // the room syncs the network pump at the next step
	LeakProtectionEvents.Add({ Clock.GetSimTimeS(), TEXT("row"), Cdu ? TEXT("row racks power off, row CDU shut down") : TEXT("row racks power off"), Trigger, INDEX_NONE, INDEX_NONE, Row });
	UE_LOG(LogTemp, Warning, TEXT("[Protect] t=%.2f s 줄 %d: 모든 랙 전원 차단%s (감지 %s)"), Clock.GetSimTimeS(), Row, Cdu ? TEXT(", 줄 CDU 정지") : TEXT(""), *Trigger);
}

void ADataCenterSimulationActor::LeakProtectionAfterStep()
{
	const TArray<FLeakAlarm>& Alarms = Instruments.GetAlarmLog();
	const TArray<FProcessLeakAlarm>& Process = ProcessMonitor.GetAlarmLog();
	if (!LeakProtectionPolicy.bEnabled)
	{
		// Policy off: nothing acts; the cursors follow so a later enable does not replay old alarms as new.
		ProtectionAlarmCursor = Alarms.Num(); ProtectionProcessCursor = Process.Num();
		ProtectionTimers.Reset();
		return;
	}
	const double Now = Clock.GetSimTimeS();
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
			// Tray level: compute trays shut down after the BMC timer, switch trays at once (out-of-band power off).
			if (R->Params.Slots[P.Loop].DeviceType == FRackThermalModel::Gb200DevCompute && LeakProtectionPolicy.ComputeTrayShutdownDelayS > 0.0)
			{
				if (!ProtectionTimers.ContainsByPredicate([&](const TTuple<int32, int32, double>& T) { return T.Get<0>() == P.Key && T.Get<1>() == P.Loop; }))
				{
					ProtectionTimers.Add(MakeTuple(P.Key, P.Loop, Now + LeakProtectionPolicy.ComputeTrayShutdownDelayS));
					LeakProtectionEvents.Add({ Now, TEXT("tray"), TEXT("shutdown timer started"), Trigger, P.Key, P.Loop, INDEX_NONE });
				}
			}
			else ProtectSlotOff(P.Key, P.Loop, TEXT("tray"), Trigger);
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
	for (int32 T = ProtectionTimers.Num() - 1; T >= 0; --T)
	{
		if (Now + 1e-9 < ProtectionTimers[T].Get<2>()) continue;
		ProtectSlotOff(ProtectionTimers[T].Get<0>(), ProtectionTimers[T].Get<1>(), TEXT("tray"), TEXT("shutdown timer"));
		ProtectionTimers.RemoveAt(T);
	}
}
