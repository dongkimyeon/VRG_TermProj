// Fill out your copyright notice in the Description page of Project Settings.

// 2026-11-03 (Tools/실사양정합_계획.md P3): CDU control and redundancy against how real CDUs run.
//  - Pump VFD holds the differential pressure at the hydraulically remote rack at its design value (2 R Q_design^2):
//    settled at build (exact r^2 scaling), held during a leak within the VFD ramp, the remote rack at its design flow.
//  - Primary valve holds the coolant supply temperature at its design setpoint at part load (fixed boundary and with
//    the plant's FWS valves), the global heat ledger closed.
//  - N+1 pumps: a pump fault stops the loop for the changeover time, the standby restores the same operating point; a
//    second fault leaves no pump; repair restores it. The response is the network solve (D4).
// Setpoints, ramp, stroke and changeover time are ESTIMATES (CoolingDistributionUnitActor.h); the tests check the
// control does what it states, not that the estimates are right.

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
	constexpr EAutomationTestFlags VrgCduControlFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// One row of two GB200 racks on one CDU (the CDU's pump was sized for a row of eight).
	struct FCduControlRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		ACoolingDistributionUnitActor* Cdu = nullptr;
		TArray<ARackThermalActor*> Racks;

		FCduControlRoom(bool bDpControl, bool bSupplyControl, bool bPlant = false, double Utilization = 1.0)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false;
			Sim->MaxStepsPerFrame = 16;
			Sim->bFacilityPlant = bPlant;
			Sim->PlantThermalSpec.AmbientK = 298.15;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			Room->bUseGjConnections = true;
			Room->bShowSummary = false;
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
				Rack->Input.Utilization = Utilization;
				Racks.Add(Rack);
			}
			Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Cdu->bPumpDpControl = bDpControl;
			Cdu->bSupplyTempControl = bSupplyControl;
			Room->RebuildRoomNow();
		}
		~FCduControlRoom()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		const FHydraulicNetwork& Net() const { return Room->GetNetwork(); }
		double PumpFlowM3s() const { return Net().GetPumpFlowM3s(Room->GetRowPumpIndex(0)); }
		double PumpSpeed() const { return Net().GetPumpSpeedRatio(Room->GetRowPumpIndex(0)); }
		double FarRackFlowM3s() const
		{
			const TArray<int32>& Branches = Room->GetRackBranchPipeIndices();
			return Branches.Num() > 0 && Branches.Last() != INDEX_NONE ? FMath::Abs(Net().GetPipeFlowM3s(Branches.Last())) : 0.0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduPumpDpControlTest, "VRG.Hydraulics.CduControl.PumpDpHoldsRemoteRack", VrgCduControlFlags)
bool FCduPumpDpControlTest::RunTest(const FString& Parameters)
{
	double FixedFlow = 0.0, FixedFar = 0.0, FixedPumpW = 0.0;
	{
		FCduControlRoom R(false, false);
		FixedFlow = R.PumpFlowM3s(); FixedFar = R.FarRackFlowM3s();
		FixedPumpW = R.Net().GetPumpElectricalInputPowerW(R.Room->GetRowPumpIndex(0));
		TestTrue(TEXT("control off: fixed speed 1, no setpoint"), R.PumpSpeed() == 1.0 && R.Room->GetRowDpSetpointPa(0) == 0.0);
	}
	FCduControlRoom R(true, false);
	const double Setpoint = R.Room->GetRowDpSetpointPa(0);
	const double Expected = 2.0 * R.Room->RackBranchResistancePaS2M6 * FMath::Square(R.Cdu->RackDesignFlowM3s);
	const double DpRel = FMath::Abs(R.Room->GetRowRemoteDpPa(0) - Setpoint) / Setpoint;
	const double FarRel = FMath::Abs(R.FarRackFlowM3s() - R.Cdu->RackDesignFlowM3s) / R.Cdu->RackDesignFlowM3s;
	const double Speed0 = R.PumpSpeed();
	const double PumpW = R.Net().GetPumpElectricalInputPowerW(R.Room->GetRowPumpIndex(0));
	AddInfo(FString::Printf(TEXT("고정 회전: 펌프 %.3f L/s, 먼 랙 %.4f L/s, 전기 %.0f W | 차압 제어: 설정 %.1f kPa(기대 %.1f), 먼 랙 차압 상대 차 %.2e, 먼 랙 %.4f L/s(설계 상대 %.2e), 회전 %.4f, 펌프 %.3f L/s, 전기 %.0f W, 포화 %d"),
		FixedFlow * 1e3, FixedFar * 1e3, FixedPumpW, Setpoint * 1e-3, Expected * 1e-3, DpRel, R.FarRackFlowM3s() * 1e3, FarRel, Speed0, R.PumpFlowM3s() * 1e3, PumpW,
		R.Room->IsRowPumpSpeedSaturated(0) ? 1 : 0));
	TestTrue(TEXT("setpoint = 2 R Q_design^2"), FMath::Abs(Setpoint - Expected) <= 1e-12 * Expected);
	TestTrue(TEXT("settled at build: remote differential at the setpoint (relative < 1e-6)"), DpRel < 1e-6 && !R.Room->IsRowPumpSpeedSaturated(0));
	TestTrue(TEXT("remote rack at its design flow (relative < 1e-3)"), FarRel < 1e-3);
	TestTrue(TEXT("two racks on an eight-rack pump: slower, less flow and power than the fixed speed"), Speed0 < 1.0 && R.PumpFlowM3s() < FixedFlow && PumpW < FixedPumpW);

	// A 5 mm leak on rack 0's supply branch from the start of the run (topology fixed before entry, as vrg.Leak.Add).
	R.Room->AddLeak(R.Racks[0], false, 5.0e-3);
	R.Room->RebuildRoomNow();
	R.Sim->EnterSimulationMode();
	double WorstStep = 0.0, Last = R.PumpSpeed();
	const double Ramp = R.Cdu->PumpRampPerS * R.Sim->FixedStepS;
	for (int32 F = 0; F < 60; ++F)
	{
		R.Sim->Tick(.1f);
		WorstStep = FMath::Max(WorstStep, FMath::Abs(R.PumpSpeed() - Last) / 5.0);   // 5 steps per 0.1 s frame
		Last = R.PumpSpeed();
	}
	const double LeakDpRel = FMath::Abs(R.Room->GetRowRemoteDpPa(0) - Setpoint) / Setpoint;
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("5 mm 누수 6 s: 회전 %.4f -> %.4f(스텝당 최대 평균 변화 %.2e, 램프 %.2e), 먼 랙 차압 상대 차 %.2e, 전역 열수지 %.2e"),
		Speed0, R.PumpSpeed(), WorstStep, Ramp, LeakDpRel, Global));
	TestTrue(TEXT("the leak lowers the pressure: the VFD speeds up within its ramp"), R.PumpSpeed() > Speed0 && WorstStep <= Ramp * (1.0 + 1e-9));
	TestTrue(TEXT("the remote differential is back at the setpoint during the leak (relative < 1e-3)"), LeakDpRel < 1e-3);
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduSupplyTempControlTest, "VRG.Coupling.CduControl.SupplyTempHoldsSetpoint", VrgCduControlFlags)
bool FCduSupplyTempControlTest::RunTest(const FString& Parameters)
{
	for (const bool bPlant : { false, true })
	{
		double OffSupplyK = 0.0;
		{
			FCduControlRoom Off(true, false, bPlant, 0.5);
			Off.Sim->EnterSimulationMode();
			Off.Run(120.0);
			OffSupplyK = Off.Sim->GetSnapshot().CduSupplyTempK[0];
		}
		FCduControlRoom R(true, true, bPlant, 0.5);
		R.Sim->EnterSimulationMode();
		R.Run(120.0);
		const auto* C = R.Sim->GetCduSupplyControl(0);
		const double SupplyK = R.Sim->GetSnapshot().CduSupplyTempK[0];
		const double Opening = bPlant ? R.Sim->GetFacilityPlant().GetCduValveOpening(0) : C->Opening;
		const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
		AddInfo(FString::Printf(TEXT("%s, 부하 50 %%, 120 s: 설정 %.3f C | 제어 끔 공급 %.3f C | 제어 켬 공급 %.3f C(차 %.3e K), 밸브 개도 %.4f, 포화 %d, 전역 열수지 %.2e"),
			bPlant ? TEXT("설비 FWS 밸브") : TEXT("고정 경계"), C->SetpointK - 273.15, OffSupplyK - 273.15, SupplyK - 273.15, SupplyK - C->SetpointK, Opening,
			C->bSaturated ? 1 : 0, Global));
		TestTrue(TEXT("without control the part-load supply runs below the setpoint"), OffSupplyK < C->SetpointK - 0.5);
		TestTrue(TEXT("with control the supply is at the setpoint (< 0.05 K) with the valve throttled"), FMath::Abs(SupplyK - C->SetpointK) < 0.05 && Opening < 1.0 && !C->bSaturated);
		TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduPumpChangeoverTest, "VRG.Hydraulics.CduControl.PumpChangeover", VrgCduControlFlags)
bool FCduPumpChangeoverTest::RunTest(const FString& Parameters)
{
	FCduControlRoom R(true, false);
	R.Sim->EnterSimulationMode();
	R.Run(1.0);
	const double Q0 = R.PumpFlowM3s();
	TestTrue(TEXT("fault of the running pump accepted"), R.Room->FailRowCduPump(0));
	R.Run(1.0);   // inside the 2 s changeover
	const double QGap = R.PumpFlowM3s();
	const bool bGapOff = !R.Net().IsPumpPowered(R.Room->GetRowPumpIndex(0));
	R.Run(2.0);   // standby running
	const double Q1 = R.PumpFlowM3s();
	TestTrue(TEXT("second fault accepted"), R.Room->FailRowCduPump(0));
	TestFalse(TEXT("no healthy pump left to fail"), R.Room->FailRowCduPump(0));
	R.Run(3.0);
	const double QNone = R.PumpFlowM3s();
	const bool bNoneOff = !R.Net().IsPumpPowered(R.Room->GetRowPumpIndex(0)) && !R.Cdu->HasHealthyPump();
	TestTrue(TEXT("repair"), R.Room->RepairRowCduPumps(0));
	R.Run(1.0);
	const double Q2 = R.PumpFlowM3s();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	AddInfo(FString::Printf(TEXT("펌프 유량: 정상 %.4f, 전환 중 %.4f, 대기 펌프 %.4f, 두 대 고장 %.4f, 수리 뒤 %.4f L/s (복귀 상대 %.2e / %.2e), 전역 열수지 %.2e"),
		Q0 * 1e3, QGap * 1e3, Q1 * 1e3, QNone * 1e3, Q2 * 1e3, FMath::Abs(Q1 - Q0) / Q0, FMath::Abs(Q2 - Q0) / Q0, Global));
	TestTrue(TEXT("during the changeover no pump runs and the flow drops"), bGapOff && QGap < 0.5 * Q0);
	TestTrue(TEXT("the standby restores the operating point (relative < 1e-6)"), FMath::Abs(Q1 - Q0) / Q0 < 1e-6);
	TestTrue(TEXT("no healthy pump: no active pump, flow drops"), bNoneOff && QNone < 0.5 * Q0);
	TestTrue(TEXT("repair restores the operating point (relative < 1e-6)"), FMath::Abs(Q2 - Q0) / Q0 < 1e-6);
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), Global < 1e-9);
	return true;
}

#endif
