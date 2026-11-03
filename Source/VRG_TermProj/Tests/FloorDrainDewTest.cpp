// Fill out your copyright notice in the Description page of Project Settings.

// 2026-11-03 (Tools/실사양정합_계획.md P5, P6).
//  - P5 floor drains and drip trays: a tray holds what lands in it up to its capacity and passes the rest to the floor;
//    a drain takes water out of its cell as an orifice; the ledger received = floor + drained + trays (+ clipped) closes;
//    a grid without drains keeps the GPU choice and behaves as before (no drain, no tray: bit-identical depths).
//  - P6 dew point: the CDU coolant supply against the room dew point input; a dew point above the supply flags the risk.

#include "Misc/AutomationTest.h"
#include "Simulation/Water/FloorWater.h"
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
	constexpr EAutomationTestFlags VrgSpecFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorDrainTrayTest, "VRG.Water.Floor.DrainAndDripTray", VrgSpecFlags)
bool FFloorDrainTrayTest::RunTest(const FString& Parameters)
{
	FFloorWaterSettings S;
	S.CellM = 0.025;
	auto Make = [&](FFloorWater& F, bool bDrainTray)
	{
		F.Reset(FVector2D::ZeroVector, 80, 80, 0.0, S);   // 2 m x 2 m
		if (!bDrainTray) return;
		F.AddTray(TEXT("tray"), FVector2D(0.2, 0.2), FVector2D(0.6, 0.6), 1.0e-3);   // 1 L
		TestTrue(TEXT("drain on the grid"), F.AddDrain(TEXT("drain"), FVector(1.5, 1.0, 0.0), 0.1));
	};
	FFloorWater With, Without;
	Make(With, true);
	Make(Without, false);
	// 1.5 L into the tray footprint (1 L stays, 0.5 L overflows), 2 L near the drain, the same 2 L on the plain floor.
	TestTrue(TEXT("tray deposit"), With.AddWater(FVector(0.4, 0.4, 0.0), 1.5e-3));
	TestTrue(TEXT("floor deposit"), With.AddWater(FVector(1.45, 1.0, 0.0), 2.0e-3));
	TestTrue(TEXT("plain floor deposit"), Without.AddWater(FVector(1.45, 1.0, 0.0), 2.0e-3));
	for (int32 K = 0; K < 500; ++K) { With.Step(0.02, K * 0.02); Without.Step(0.02, K * 0.02); }
	const FFloorWaterTotals& T = With.GetTotals();
	const FFloorTray& Tray = With.GetTrays()[0];
	AddInfo(FString::Printf(TEXT("10 s: 받음 %.6f L = 바닥 %.6f + 배수 %.6f + 트레이 %.6f L (넘침 %.6f L), 잔차 %.2e | 배수 없는 바닥 %.6f L, 잔차 %.2e"),
		T.ReceivedM3 * 1e3, T.StoredM3 * 1e3, T.DrainedM3 * 1e3, T.TrayM3 * 1e3, Tray.OverflowM3 * 1e3, T.Residual, Without.GetTotals().StoredM3 * 1e3, Without.GetTotals().Residual));
	TestTrue(TEXT("the tray holds its capacity and passes the rest to the floor"), FMath::IsNearlyEqual(Tray.StoredM3, 1.0e-3, 1e-15) && FMath::IsNearlyEqual(Tray.OverflowM3, 0.5e-3, 1e-15) && Tray.FirstWetS >= 0.0);
	TestTrue(TEXT("the drain takes water out"), T.DrainedM3 > 0.0 && With.GetDrains()[0].DrainedM3 == T.DrainedM3);
	TestTrue(TEXT("ledger: received = floor + drained + trays (relative < 1e-12)"), T.Residual < 1e-12 && FMath::IsNearlyEqual(T.ReceivedM3, 3.5e-3, 1e-15));
	TestTrue(TEXT("less water stays on a drained floor than on a plain one"), T.StoredM3 < Without.GetTotals().StoredM3 + 0.5e-3);
	TestFalse(TEXT("a grid with a drain stays on the CPU"), With.WantsGpu());
	// No drain, no tray: the same deposits step to the same depths as before the feature (bit-identical path).
	FFloorWater A, B;
	Make(A, false); Make(B, false);
	A.AddWater(FVector(1.0, 1.0, 0.0), 1e-3); B.AddWater(FVector(1.0, 1.0, 0.0), 1e-3);
	for (int32 K = 0; K < 50; ++K) { A.Step(0.02, K * 0.02); B.Step(0.02, K * 0.02); }
	TestTrue(TEXT("without drains or trays the floor is unchanged (bit-identical repeat)"), A.GetDepthM() == B.GetDepthM() && A.GetTotals().DrainedM3 == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDewPointCheckTest, "VRG.Coupling.DewPoint.CduSupplyMargin", VrgSpecFlags)
bool FDewPointCheckTest::RunTest(const FString& Parameters)
{
	auto RunWith = [&](double DewK, double& OutRiskS, double& OutMinMarginK, double& OutSupplyK)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16; Sim->RoomDewPointK = DewK;
		ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
		Room->bUseGjConnections = true; Room->bShowSummary = false;
		World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
		Room->RebuildRoomNow();
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 20; ++F) Sim->Tick(.1f);
		OutRiskS = Sim->GetCondensationRiskS(); OutMinMarginK = Sim->GetMinDewMarginK(); OutSupplyK = Sim->GetSnapshot().CduSupplyTempK[0];
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	double RiskDefault = 0, MarginDefault = 0, SupplyDefault = 0, RiskHumid = 0, MarginHumid = 0, SupplyHumid = 0;
	RunWith(288.15, RiskDefault, MarginDefault, SupplyDefault);   // 15 C dew point (default)
	RunWith(303.15, RiskHumid, MarginHumid, SupplyHumid);         // 30 C dew point (above the supply)
	AddInfo(FString::Printf(TEXT("이슬점 15 C: 공급 %.2f C, 최소 여유 %.2f K, 위험 %.2f s | 이슬점 30 C: 공급 %.2f C, 최소 여유 %.2f K, 위험 %.2f s"),
		SupplyDefault - 273.15, MarginDefault, RiskDefault, SupplyHumid - 273.15, MarginHumid, RiskHumid));
	TestTrue(TEXT("default room: the supply stays above dew point + 2 K, no risk"), RiskDefault == 0.0 && MarginDefault >= 2.0);
	TestTrue(TEXT("humid room: the supply is below dew point + 2 K the whole run, risk time > 0"), RiskHumid > 1.9 && MarginHumid < 2.0);
	TestTrue(TEXT("the check does not change the physics (same supply)"), SupplyDefault == SupplyHumid);
	return true;
}

#endif
