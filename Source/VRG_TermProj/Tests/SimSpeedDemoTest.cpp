// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

// 2026-11-12 (presentation): vrg.Sim.Speed advances the shared clock by speed x real time in the same fixed steps
// (rule 4: nothing skipped), the achieved speed reads back, and vrg.Demo pump schedules two CDU pump faults whose
// result is computed by the hydraulics: the standby carries the row after the first, the row's flow stops after the
// second and the chips heat up from their heat capacity (D2), with the global ledger still closed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimSpeedDemoTest, "VRG.Sim.SpeedAndDemo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSimSpeedDemoTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->SpeedStepBudgetMs = 1.0e9;   // exact scale check: no wall-time budget in the test
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->bShowSummary = false;
	TArray<ARackThermalActor*> Racks;
	for (const double Y : { 0.0, 66.0 })   // one row of two racks (synthetic placement) + CDU
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Racks.Add(Rack);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	Sim->EnterSimulationMode();
	const double OldSpeed = ADataCenterSimulationActor::GetRequestedSimSpeed();

	// Warm 1 s at x1 (the first Tick only accumulates time, and this closes the achieved-speed window).
	ADataCenterSimulationActor::SetRequestedSimSpeed(1.0);
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	// Speed: 1 s of real time at x2 = 2 s of simulated time, all in 0.02 s steps.
	ADataCenterSimulationActor::SetRequestedSimSpeed(2.0);
	const double T0 = Sim->GetSimClockTimeS();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	const double Advanced = Sim->GetSimClockTimeS() - T0;
	TestTrue(TEXT("x2: 1 s real -> 2 s simulated (+-1 step)"), FMath::Abs(Advanced - 2.0) <= Sim->FixedStepS + 1e-9);
	TestTrue(TEXT("achieved speed reads x2"), FMath::Abs(Sim->GetAchievedSimSpeed() - 2.0) < .05);
	ADataCenterSimulationActor::SetRequestedSimSpeed(100.0);
	TestEqual(TEXT("speed clamped to 8"), ADataCenterSimulationActor::GetRequestedSimSpeed(), 8.0);
	ADataCenterSimulationActor::SetRequestedSimSpeed(1.0);

	// Demo: unknown name lists, layout-only guard is not hit here, the pump scenario schedules two faults.
	bool bOk = true;
	Sim->StartDemoScenario(TEXT("nosuch"), bOk);
	TestFalse(TEXT("unknown scenario rejected"), bOk);
	const FHydraulicNetwork& Net = Room->GetNetwork();
	const int32 Pump = Room->GetRowPumpIndex(0);
	const double FlowBefore = Net.GetPumpFlowM3s(Pump);
	const FString Text = Sim->StartDemoScenario(TEXT("pump"), bOk);
	const double Start = Sim->GetSimClockTimeS() + 2.0;
	TestTrue(TEXT("pump scenario scheduled: two cdupump events at +2 s and +17 s"), bOk && Sim->EvalEvents.Num() == 2
		&& Sim->EvalEvents[0].Kind == TEXT("cdupump") && FMath::IsNearlyEqual(Sim->EvalEvents[0].TimeS, Start, 1e-9)
		&& FMath::IsNearlyEqual(Sim->EvalEvents[1].TimeS, Start + 15.0, 1e-9));
	auto MaxPartK = [&]()
	{
		double K = 0.0;
		for (const double T : Sim->MakeFaultInput().MaxComponentTempK) K = FMath::Max(K, T);
		return K;
	};
	// First fault + standby switch (2 s, estimate): run to +10 s.
	while (Sim->GetSimClockTimeS() < Start + 10.0) Sim->Tick(.1f);
	const double FlowAfterFirst = Net.GetPumpFlowM3s(Pump);
	const double HotBefore = MaxPartK();
	// Second fault at +17 s, then 10 s without any pump.
	while (Sim->GetSimClockTimeS() < Start + 27.0) Sim->Tick(.1f);
	const double FlowAfterSecond = Net.GetPumpFlowM3s(Pump);
	const double HotAfter = MaxPartK();
	const double Global = Sim->GetSnapshot().NormalizedEnergyResidual;
	AddInfo(FString::Printf(TEXT("x2 %.3f s sim / 1 s real, achieved %.3f | pump flow %.3f -> %.3f (standby) -> %.6f L/s | hottest part %.2f -> %.2f C | ledger %.3e"),
		Advanced, Sim->GetAchievedSimSpeed(), FlowBefore * 1e3, FlowAfterFirst * 1e3, FlowAfterSecond * 1e3, HotBefore - 273.15, HotAfter - 273.15, Global));
	TestTrue(TEXT("both events applied"), Sim->EvalEvents[0].bApplied && Sim->EvalEvents[1].bApplied);
	TestTrue(TEXT("standby carries the row after the first fault (>= 90 % of the flow)"), FlowBefore > 0.0 && FlowAfterFirst >= .9 * FlowBefore);
	TestTrue(TEXT("no pump left: row flow stops"), FMath::Abs(FlowAfterSecond) < 1e-6 * FlowBefore);
	TestTrue(TEXT("chips heat up without coolant flow (D2)"), HotAfter > HotBefore + 1.0);
	TestTrue(TEXT("global ledger still closes"), FMath::Abs(Global) < 1e-9);
	bOk = false;
	Sim->StartDemoScenario(TEXT("clear"), bOk);
	TestTrue(TEXT("clear removes the scheduled events"), bOk && Sim->EvalEvents.Num() == 0);

	ADataCenterSimulationActor::SetRequestedSimSpeed(OldSpeed);
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
