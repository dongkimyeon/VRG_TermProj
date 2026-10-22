// Fill out your copyright notice in the Description page of Project Settings.

// 2026-10-22 instruments (user request): pipe points with flow / water temperature / a leak-detection cable, unit and
// CDU readings. The bank alone: leak alarm timing against the wetting volume and polling, the cover radius, the latch,
// the detection flow, and a channel's first-order response. Integrated: points on every room and plant pipe, readings
// against the solved values, and a room TCS leak and a facility leak each detected near where the solve put them.

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
	constexpr EAutomationTestFlags VrgInstrumentFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstrumentBankUnitTest, "VRG.Sensors.Instruments.LeakCableAndChannels", VrgInstrumentFlags)
bool FInstrumentBankUnitTest::RunTest(const FString& Parameters)
{
	FInstrumentBank Bank;
	Bank.Reset();
	FInstrumentPoint Near, Far;
	Near.Site = Far.Site = EInstrumentSite::PlantPipe;
	Near.bLeakCable = Far.bLeakCable = true;
	Near.CoverRadiusM = Far.CoverRadiusM = 1.8;
	Near.PositionM = FVector(0, 0, 3); Far.PositionM = FVector(10, 0, 3);
	Near.Channels = { InstrumentSpecs::Make(TEXT("temp"), EInstrumentQuantity::TemperatureK, 300.0) };
	Bank.AddPoint(Near, { 300.0 });
	Bank.AddPoint(Far, { 0.0 });
	const double Dt = 0.02;
	const FLeakCableSpec Spec;
	TArray<FInstrumentLeak> Leaks;
	double NearAt = -1.0, FarAt = -1.0;
	bool bDripAlarm = false;
	for (int32 I = 1; I <= 2000; ++I)   // 40 s
	{
		const double T = I * Dt;
		Leaks.Reset();
		if (T > 5.0 && T <= 10.0) Leaks.Add({ FVector(1.0, 0.5, 3.0), 0.5 * Spec.DetectFlowM3s });   // a drip under the detection flow
		if (T > 12.0 && T <= 30.0) Leaks.Add({ FVector(1.0, 0.5, 3.0), 2.0e-4 });                     // 0.2 L/s from 12 s to 30 s
		TArray<TArray<double>> Truth = { { T > 20.0 ? 310.0 : 300.0 }, {} };   // the near point's water steps +10 K at 20 s
		for (const FLeakAlarm& A : Bank.Advance(T, Dt, Truth, Leaks)) (A.Point == 0 ? NearAt : FarAt) = A.TimeS;
		bDripAlarm |= T <= 10.0 + Dt && Bank.IsLeakAlarm(0);
		if (FMath::IsNearlyEqual(T, 25.0, 1e-9))
		{
			// tau 5 s after the step at 20 s: 1 - exp(-1) of the way, then noise (0.05 K) and the 0.1 K rounding.
			const double Expected = 300.0 + 10.0 * (1.0 - FMath::Exp(-5.0 / 5.0));
			AddInfo(FString::Printf(TEXT("수온 계단 +10 K 뒤 5 s 판독 %.2f K (1차 응답 %.2f K)"), Bank.GetReading(0, 0), Expected));
			TestTrue(TEXT("temperature channel follows its 5 s first-order response (within noise + rounding)"), FMath::Abs(Bank.GetReading(0, 0) - Expected) < 0.25);
		}
	}
	AddInfo(FString::Printf(TEXT("누수 12 s 시작: 가까운 지점 경보 %.2f s, 먼 지점 %s, 감지 유량 미만 물방울 경보 %d, 끝 뒤에도 유지 %d"),
		NearAt, FarAt < 0.0 ? TEXT("없음") : *FString::Printf(TEXT("%.2f s"), FarAt), bDripAlarm ? 1 : 0, Bank.IsLeakAlarm(0) ? 1 : 0));
	TestFalse(TEXT("a drip below the detection flow raises nothing"), bDripAlarm);
	// 0.5 L at 0.2 L/s = 2.5 s after 12 s: the first poll after 14.5 s.
	const double WetAt = 12.0 + Spec.WettingVolumeM3 / 2.0e-4;
	TestTrue(TEXT("alarm at the first poll after the wetting volume has come out (14.5 s .. 15.5 s)"), NearAt >= WetAt - Dt && NearAt <= WetAt + Spec.SamplePeriodS + Dt);
	TestTrue(TEXT("a point beyond its cover radius stays quiet"), FarAt < 0.0);
	TestTrue(TEXT("the alarm latches after the leak stops"), Bank.IsLeakAlarm(0) && Bank.GetAlarmLog().Num() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstrumentIntegrationTest, "VRG.Sensors.Instruments.PlantAndRoomLeaks", VrgInstrumentFlags)
bool FInstrumentIntegrationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	Room->bUseGjConnections = true;
	Room->bShowSummary = false;
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
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
	// A 5 mm room leak from the start (its topology is fixed before the run, as vrg.Leak.Add requires).
	const int32 RoomLeak = Room->AddLeak(Racks[0], false, 5.0e-3);
	Room->RebuildRoomNow();
	Sim->EnterSimulationMode();
	auto Run = [&](double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); };
	Run(3.0);
	const FInstrumentBank& Bank = Sim->GetInstruments();
	int32 RoomPipes = 0, PlantPipes = 0, Units = 0, Cdus = 0;
	double WorstRoomFlowRel = 0.0, WorstPumpFlowRel = 0.0;
	for (int32 I = 0; I < Bank.Num(); ++I)
	{
		const FInstrumentPoint& P = Bank.GetPoint(I);
		RoomPipes += P.Site == EInstrumentSite::RoomPipe ? 1 : 0;
		PlantPipes += P.Site == EInstrumentSite::PlantPipe ? 1 : 0;
		Units += P.Site == EInstrumentSite::PlantUnit ? 1 : 0;
		Cdus += P.Site == EInstrumentSite::Cdu ? 1 : 0;
		// Readings against the solved values (steady flows; 0.5 % noise, 1 s lag): within 3 %.
		if (P.Site == EInstrumentSite::RoomPipe && P.Key >= 0 && Bank.HasReading(I, 0))
		{
			const double True = Room->GetNetwork().GetPipeFlowM3s(P.Key);
			if (FMath::Abs(True) > 1e-4) WorstRoomFlowRel = FMath::Max(WorstRoomFlowRel, FMath::Abs(Bank.GetReading(I, 0) - True) / FMath::Abs(True));
		}
		if (P.Site == EInstrumentSite::PlantUnit && P.Label.StartsWith(TEXT("FWS 펌프")) && Bank.HasReading(I, 1))
		{
			const FPlantUnitTruth T = Sim->GetFacilityPlant().GetPumpTruth(EPlantLoopId::Fws, P.Key);
			if (T.bRunning) WorstPumpFlowRel = FMath::Max(WorstPumpFlowRel, FMath::Abs(Bank.GetReading(I, 1) - T.FlowM3s) / T.FlowM3s);
		}
	}
	TestTrue(TEXT("points on room pipes, plant pipes, plant units and the CDU"), RoomPipes > 0 && PlantPipes > 0 && Units > 0 && Cdus == 1);
	TestTrue(TEXT("no facility alarm before the facility leak"), !Bank.GetAlarmLog().ContainsByPredicate([&](const FLeakAlarm& A) { return Bank.GetPoint(A.Point).Site == EInstrumentSite::PlantPipe; }));
	TestTrue(TEXT("room pipe flow readings within 3 % of the solve"), WorstRoomFlowRel < 0.03);
	TestTrue(TEXT("FWS pump flow readings within 3 % of the solve"), WorstPumpFlowRel < 0.03);

	// A facility leak (10 mm at the first CDU branch supply) at 3 s.
	const double LeakStartS = Sim->GetEvaluation().GetSimTimeS();
	TestTrue(TEXT("facility leak added"), Sim->ApplyPlantFault({ TEXT("leak"), TEXT("fws"), TEXT("branchsupply"), TEXT("0"), TEXT("10") }));
	Run(14.0);
	const FVector RoomLeakM = Room->GetNodePositionM(Room->GetNetwork().GetLeak(Room->GetLeakNetworkIndex(RoomLeak)).NodeIndex);
	int32 RoomAlarms = 0, PlantAlarms = 0;
	double RoomFirst = TNumericLimits<double>::Max(), RoomLast = 0.0, First = TNumericLimits<double>::Max(), Last = 0.0, WorstRoomDistM = 0.0;
	for (const FLeakAlarm& A : Bank.GetAlarmLog())
	{
		const FInstrumentPoint& P = Bank.GetPoint(A.Point);
		if (P.Site == EInstrumentSite::RoomPipe)
		{
			++RoomAlarms; WorstRoomDistM = FMath::Max(WorstRoomDistM, FVector::Dist(P.PositionM, RoomLeakM));
			RoomFirst = FMath::Min(RoomFirst, A.TimeS); RoomLast = FMath::Max(RoomLast, A.TimeS);
			continue;
		}
		++PlantAlarms;
		First = FMath::Min(First, A.TimeS); Last = FMath::Max(Last, A.TimeS);
	}
	AddInfo(FString::Printf(TEXT("계측 지점: 방 배관 %d, 설비 배관 %d, 설비 장비 %d, CDU %d (누수 케이블 %d) | 유량 판독 최대 상대 차 방 %.3e, FWS 펌프 %.3e | 방 누수(0 s): 경보 %d곳 %.2f~%.2f s, 지점-누수 최대 %.2f m | 설비 누수(%.2f s): 경보 %d곳 %.2f~%.2f s"),
		RoomPipes, PlantPipes, Units, Cdus, Bank.GetNumLeakCables(), WorstRoomFlowRel, WorstPumpFlowRel, RoomAlarms, RoomFirst, RoomLast, WorstRoomDistM,
		LeakStartS, PlantAlarms, First, Last));
	double NearestRoomM = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Bank.Num(); ++I)
		if (Bank.GetPoint(I).Site == EInstrumentSite::RoomPipe) NearestRoomM = FMath::Min(NearestRoomM, FVector::Dist(Bank.GetPoint(I).PositionM, RoomLeakM));
	AddInfo(FString::Printf(TEXT("방 누수 위치 %s, 유량 %.4f L/s, 구멍 %.4f m, 누수 번호 %d / %d, 재고 풀이 %d회, 탱크 %d, 가장 가까운 방 배관 지점 %.2f m"), *RoomLeakM.ToCompactString(),
		Room->GetNetwork().GetLeakFlowM3s(Room->GetLeakNetworkIndex(RoomLeak)) * 1e3, Room->GetNetwork().GetLeak(Room->GetLeakNetworkIndex(RoomLeak)).HoleDiameterM,
		Room->GetLeakNetworkIndex(RoomLeak), Room->GetNetwork().GetNumLeaks(), Room->GetCoolantInventory().GetNumSolves(), Room->GetCoolantInventory().GetTanks().Num(), NearestRoomM));
	TestTrue(TEXT("the room leak is detected by points near it"), RoomAlarms > 0 && WorstRoomDistM < 3.0);
	TestTrue(TEXT("the facility leak is detected"), PlantAlarms > 0);
	// The leak flows are around 0.36 L/s (room, 5 mm) and 1.25 L/s (facility, 10 mm): 0.5 L comes out within 2 s.
	TestTrue(TEXT("room alarms within 3 s of the start"), RoomFirst <= 3.0);
	TestTrue(TEXT("facility alarms after the leak, within 2 s"), First > LeakStartS && Last <= LeakStartS + 2.0 + 0.1);
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
