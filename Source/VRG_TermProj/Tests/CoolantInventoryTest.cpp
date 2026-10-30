// Fill out your copyright notice in the Description page of Project Settings.

// S2b step 2 (Tools/S2b_누수_계획.md, 2026-10-20): expansion tank, coolant inventory, make-up water.
// Gas law and volume ledger of the tank alone; a leaking NVL72 row stepped in time (pressure and leak fall, mass
// conservation every step, first-order time refinement, depletion, restart, make-up); the integrated room run.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/CoolantInventory.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Visualization/GjRackLayout.h"
#include "Simulation/Evaluation/VrgFaultMonitor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgInventoryTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// NVL72 row of 8 racks (spec defaults) with the room's plain heights (synthetic placement) and a leak at mid
	// height of rack 3's supply branch. Returns the leak index; OutPump / OutReturn are the pump and CDU return pipe.
	int32 BuildLeakingRow(FHydraulicNetwork& Net, double DiameterM, int32& OutCdu, int32& OutPump, int32& OutReturn)
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = 8;
		const FInRowLoopHandles H = InRowLoopBuilder::AddLoop(Net, Spec);
		for (int32 N = 0; N < Net.GetNumNodes(); ++N) Net.SetNodeElevationM(N, 0.25);
		Net.SetNodeElevationM(H.CduNodeIndex, 0.6);
		for (const int32 Node : H.RackNodeIndices) if (Node != INDEX_NONE) Net.SetNodeElevationM(Node, 1.1);
		const int32 Hole = Net.SplitPipe(H.RackBranchPipeIndices[3], 0.5, 0.675);
		FHydraulicLeak Leak;
		Leak.NodeIndex = Hole; Leak.HoleDiameterM = DiameterM;
		OutCdu = H.CduNodeIndex; OutPump = H.PumpIndex; OutReturn = H.ReturnToCduPipeIndex;
		return Net.AddLeak(Leak);
	}

	// Steps the row for DurationS with DtS; returns the cumulative leaked volume [m^3].
	double RunLeakingRow(double DiameterM, double DtS, double DurationS, const FExpansionTankParams& Params)
	{
		FHydraulicNetwork Net;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, DiameterM, Cdu, Pump, Return);
		Net.Solve(FHydraulicSolveSettings());
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, Params);
		FHydraulicSolveResult R;
		const int32 Steps = FMath::RoundToInt32(DurationS / DtS);
		for (int32 S = 0; S < Steps; ++S) Inv.Step(Net, FHydraulicSolveSettings(), DtS, S * DtS, R);
		return Inv.GetTanks()[0].State.LeakedM3;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoolantTankLedgerTest, "VRG.Hydraulics.Inventory.TankLedgerAndGasLaw", VrgInventoryTestFlags)
bool FCoolantTankLedgerTest::RunTest(const FString& Parameters)
{
	FExpansionTankParams P;   // defaults: 24 L tank, pre-charge 0.3 bar below fill (estimates, see the struct)
	const double Fill = 2.0e5;
	FExpansionTankState S = ExpansionTank::Initial(P, Fill);
	const double PreAbs = Fill - P.PrechargeBelowFillPa + P.AtmosphericPressurePa, FillAbs = Fill + P.AtmosphericPressurePa;
	const double W0 = P.TankVolumeM3 * (1.0 - PreAbs / FillAbs);
	AddInfo(FString::Printf(TEXT("충전 2.0 bar(g), 탱크 %.1f L: 초기 물 %.6f L (해석 %.6f), 압력 %.3f Pa(g)"),
		P.TankVolumeM3 * 1e3, S.WaterVolumeM3 * 1e3, W0 * 1e3, ExpansionTank::GaugePressurePa(P, Fill, S)));
	TestTrue(TEXT("initial water = V (1 - p0/pa) (rel < 1e-12)"), FMath::Abs(S.WaterVolumeM3 - W0) / W0 < 1e-12);
	TestTrue(TEXT("initial pressure = fill (rel < 1e-12)"), FMath::Abs(ExpansionTank::GaugePressurePa(P, Fill, S) - Fill) / Fill < 1e-12);

	// A varying leak with make-up on: gas invariant and volume ledger every step; the pressure never exceeds the fill.
	P.bMakeupWater = true;
	double MaxInvariant = 0.0, MaxLedger = 0.0, MaxOverFill = 0.0;
	for (int32 K = 0; K < 2000; ++K)
	{
		const double Leak = 2.0e-4 * (1.0 + FMath::Sin(0.01 * K));   // synthetic 0 ~ 0.4 L/s, above the 0.1 L/s make-up
		ExpansionTank::Advance(P, Fill, S, Leak, 0.005, K * 0.005);
		if (S.bDepleted) break;
		const double Pabs = ExpansionTank::GaugePressurePa(P, Fill, S) + P.AtmosphericPressurePa;
		MaxInvariant = FMath::Max(MaxInvariant, FMath::Abs(Pabs * (P.TankVolumeM3 - S.WaterVolumeM3) - PreAbs * P.TankVolumeM3) / (PreAbs * P.TankVolumeM3));
		MaxLedger = FMath::Max(MaxLedger, FMath::Abs(S.InitialWaterM3 - S.WaterVolumeM3 - S.LeakedM3 + S.MakeupM3) / FMath::Max(S.LeakedM3, 1e-30));
		MaxOverFill = FMath::Max(MaxOverFill, (ExpansionTank::GaugePressurePa(P, Fill, S) - Fill) / Fill);
	}
	AddInfo(FString::Printf(TEXT("가변 누수 + 보충: 누적 누수 %.4f L, 보충 %.4f L, 기체 불변량 %.3e, 부피 장부 %.3e, 충전 초과 %.3e, 고갈 %d (t=%.2f s)"),
		S.LeakedM3 * 1e3, S.MakeupM3 * 1e3, MaxInvariant, MaxLedger, MaxOverFill, S.bDepleted ? 1 : 0, S.DepletedAtS));
	TestTrue(TEXT("P V_gas invariant (rel < 1e-12)"), MaxInvariant < 1e-12);
	TestTrue(TEXT("volume ledger W0 - W - leaked + make-up = 0 (rel to leaked < 1e-12)"), MaxLedger < 1e-12);
	TestTrue(TEXT("make-up never above the fill pressure"), MaxOverFill <= 1e-12);

	// Depletion: never more out than in, water 0, pressure 0, no further change.
	FExpansionTankParams Q;
	FExpansionTankState D = ExpansionTank::Initial(Q, Fill);
	for (int32 K = 0; K < 1000 && !D.bDepleted; ++K) ExpansionTank::Advance(Q, Fill, D, 1.0e-3, 0.02, K * 0.02);   // 1 L/s
	const double Leaked = D.LeakedM3;
	ExpansionTank::Advance(Q, Fill, D, 1.0e-3, 0.02, 99.0);
	AddInfo(FString::Printf(TEXT("고갈: t=%.2f s, 누적 누수 %.6f L = 초기 물 %.6f L"), D.DepletedAtS, D.LeakedM3 * 1e3, D.InitialWaterM3 * 1e3));
	TestTrue(TEXT("depleted: leaked = initial water exactly, water 0, pressure 0, frozen"), D.bDepleted && D.WaterVolumeM3 == 0.0
		&& FMath::Abs(D.LeakedM3 - D.InitialWaterM3) <= 1e-15 * D.InitialWaterM3 + 1e-18 && ExpansionTank::GaugePressurePa(Q, Fill, D) == 0.0 && D.LeakedM3 == Leaked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoolantRowDrainTest, "VRG.Hydraulics.Inventory.LeakDrainsTankRow", VrgInventoryTestFlags)
bool FCoolantRowDrainTest::RunTest(const FString& Parameters)
{
	const FExpansionTankParams Params;

	// (a) No open leak: the inventory never solves and the static solution stays bit-identical.
	{
		FHydraulicNetwork Net, Ref;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, 0.0, Cdu, Pump, Return);
		BuildLeakingRow(Ref, 0.0, Cdu, Pump, Return);
		Net.Solve(FHydraulicSolveSettings()); Ref.Solve(FHydraulicSolveSettings());
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, Params);
		FHydraulicSolveResult R;
		bool bAny = false;
		for (int32 S = 0; S < 50; ++S) bAny |= Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R);
		TestTrue(TEXT("(a) closed leak: no solve, bit-identical pump flow"), !bAny && Inv.GetNumSolves() == 0 && Net.GetPumpFlowM3s(Pump) == Ref.GetPumpFlowM3s(Pump));
	}

	// (b) 3 mm leak for 10 s: tank pressure and leak fall monotonically; mass conservation every step.
	{
		FHydraulicNetwork Net;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, 3.0e-3, Cdu, Pump, Return);
		const FHydraulicSolveResult R0 = Net.Solve(FHydraulicSolveSettings());
		const double Pump0 = Net.GetPumpFlowM3s(Pump);
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, Params);
		FHydraulicSolveResult R;
		double PrevP = 1e30, PrevLeak = 1e30, MaxMass = 0.0;
		bool bMonotonic = true, bConverged = R0.bConverged;
		for (int32 S = 0; S < 500; ++S)
		{
			Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R);
			bConverged &= R.bConverged;
			const double Pt = Net.GetNodePressurePa(Cdu), Ql = Net.GetLeakFlowM3s(Leak), Qp = Net.GetPumpFlowM3s(Pump);
			bMonotonic &= Pt < PrevP && Ql < PrevLeak;
			PrevP = Pt; PrevLeak = Ql;
			MaxMass = FMath::Max(MaxMass, FMath::Abs(Qp - Net.GetPipeFlowM3s(Return) - Ql) / Qp);
		}
		const FCoolantInventory::FTank& T = Inv.GetTanks()[0];
		AddInfo(FString::Printf(TEXT("(b) 3 mm 10 s: 탱크 %.2f -> %.2f kPa(g), 물 %.4f -> %.4f L, 누수 %.4f L/s (마지막), 누적 %.4f L, 펌프 %.4f -> %.4f L/s, 질량 최대 %.3e, 풀이 %d회"),
			T.FillGaugePa * 1e-3, Inv.GetTankGaugePressurePa(0) * 1e-3, T.State.InitialWaterM3 * 1e3, T.State.WaterVolumeM3 * 1e3,
			PrevLeak * 1e3, T.State.LeakedM3 * 1e3, Pump0 * 1e3, Net.GetPumpFlowM3s(Pump) * 1e3, MaxMass, Inv.GetNumSolves()));
		TestTrue(TEXT("(b) converged every step"), bConverged);
		TestTrue(TEXT("(b) tank pressure and leak fall every step (not a fixed leak)"), bMonotonic);
		TestTrue(TEXT("(b) pump out - CDU return - leak = 0 (rel to pump flow < 1e-9)"), MaxMass < 1e-9);
		TestTrue(TEXT("(b) ledger W0 - W - leaked = 0 (rel < 1e-12)"),
			FMath::Abs(T.State.InitialWaterM3 - T.State.WaterVolumeM3 - T.State.LeakedM3) / T.State.LeakedM3 < 1e-12);

		// (c) Restart: tank full, CDU at fill, the static solution again.
		Inv.Restart(Net, FHydraulicSolveSettings(), R);
		TestTrue(TEXT("(c) restart: fill state and the starting pump flow (rel < 1e-9)"),
			Inv.GetTanks()[0].State.WaterVolumeM3 == Inv.GetTanks()[0].State.InitialWaterM3 && FMath::Abs(Net.GetPumpFlowM3s(Pump) - Pump0) / Pump0 < 1e-9);
	}

	// (d) Time refinement of the explicit Euler inventory: leaked volume after 10 s vs a dt/32 reference.
	{
		const double Ref = RunLeakingRow(3.0e-3, 0.04 / 32.0, 10.0, Params);
		const double E1 = FMath::Abs(RunLeakingRow(3.0e-3, 0.04, 10.0, Params) - Ref);
		const double E2 = FMath::Abs(RunLeakingRow(3.0e-3, 0.02, 10.0, Params) - Ref);
		AddInfo(FString::Printf(TEXT("(d) 시간 정밀화: 누적 %.9f L, 오차 dt 0.04 %.3e L / 0.02 %.3e L, 비 %.4f"), Ref * 1e3, E1 * 1e3, E2 * 1e3, E1 / E2));
		TestTrue(TEXT("(d) first-order convergence: error ratio 1.9 ~ 2.1"), E2 > 0.0 && E1 / E2 > 1.9 && E1 / E2 < 2.1);
	}

	// (e) 10 mm leak until the tank is empty: depleted loop stops its pump and closes its leak, then stays idle.
	{
		FHydraulicNetwork Net;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, 10.0e-3, Cdu, Pump, Return);
		Net.Solve(FHydraulicSolveSettings());
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, Params);
		FHydraulicSolveResult R;
		int32 S = 0;
		for (; S < 1000 && !Inv.GetTanks()[0].State.bDepleted; ++S) Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R);
		Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R);   // applies the depleted boundary
		TestTrue(TEXT("(e) solve after depletion converged"), R.bConverged);
		const int32 SolvesAfter = Inv.GetNumSolves();
		const bool bIdle = !Inv.Step(Net, FHydraulicSolveSettings(), 0.02, (S + 1) * 0.02, R);
		const FCoolantInventory::FTank& T = Inv.GetTanks()[0];
		AddInfo(FString::Printf(TEXT("(e) 10 mm: 고갈 t=%.2f s, 누적 %.6f L = 초기 %.6f L, 고갈 뒤 펌프 %.3e L/s, 누수 %.3e L/s, 이후 풀이 없음 %d"),
			T.State.DepletedAtS, T.State.LeakedM3 * 1e3, T.State.InitialWaterM3 * 1e3, Net.GetPumpFlowM3s(Pump) * 1e3, Net.GetLeakFlowM3s(Leak) * 1e3, bIdle ? 1 : 0));
		TestTrue(TEXT("(e) depleted within the run"), T.State.bDepleted && T.State.DepletedAtS > 0.0);
		TestTrue(TEXT("(e) leaked exactly the tank's water"), FMath::Abs(T.State.LeakedM3 - T.State.InitialWaterM3) <= 1e-12 * T.State.InitialWaterM3);
		TestTrue(TEXT("(e) depleted loop: pump flow ~0, leak 0, then idle"), FMath::Abs(Net.GetPumpFlowM3s(Pump)) < 1e-9 && Net.GetLeakFlowM3s(Leak) == 0.0 && bIdle && SolvesAfter == Inv.GetNumSolves());
	}

	// (g) Two rows in one network, one drains: the stopped loop must solve (seeded from its own CDU, not the room mean)
	// and the other row keeps its flow. Found in the L_Test run, where the depleted-row solve stalled at zero flow.
	{
		FHydraulicNetwork Net, Ref;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, 10.0e-3, Cdu, Pump, Return);
		FInRowLoopSpec Spec;
		Spec.NumRacks = 8;
		const FInRowLoopHandles Other = InRowLoopBuilder::AddLoop(Net, Spec);
		const FInRowLoopHandles OtherRef = InRowLoopBuilder::AddLoop(Ref, Spec);
		Net.Solve(FHydraulicSolveSettings()); Ref.Solve(FHydraulicSolveSettings());
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, Params);
		Inv.AddTank(Net, Other.CduNodeIndex, Other.PumpIndex, {}, Params);
		FHydraulicSolveResult R;
		bool bConverged = true;
		for (int32 S = 0; S < 1000 && Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R); ++S) bConverged &= R.bConverged;
		const double OtherErr = FMath::Abs(Net.GetPumpFlowM3s(Other.PumpIndex) - Ref.GetPumpFlowM3s(OtherRef.PumpIndex)) / Ref.GetPumpFlowM3s(OtherRef.PumpIndex);
		AddInfo(FString::Printf(TEXT("(g) 두 줄 중 한 줄 고갈: 모든 풀이 수렴 %d, 마지막 반복 %d, 잔차 %.3e, 고갈 줄 펌프 %.3e L/s, 다른 줄 펌프 상대 차 %.3e"),
			bConverged ? 1 : 0, R.IterationsUsed, R.NormalizedResidual, Net.GetPumpFlowM3s(Pump) * 1e3, OtherErr));
		TestTrue(TEXT("(g) every solve converged, including the one after depletion"), bConverged && Inv.GetTanks()[0].State.bDepleted);
		TestTrue(TEXT("(g) drained row stopped, other row unchanged (rel < 1e-9)"), FMath::Abs(Net.GetPumpFlowM3s(Pump)) < 1e-9 && OtherErr < 1e-9);
	}

	// (f) Make-up above the leak: the tank stays at its fill pressure and the make-up equals the leak.
	{
		FExpansionTankParams M = Params;
		M.bMakeupWater = true;
		FHydraulicNetwork Net;
		int32 Cdu, Pump, Return;
		const int32 Leak = BuildLeakingRow(Net, 1.0e-3, Cdu, Pump, Return);   // ~0.013 L/s < 0.1 L/s make-up
		Net.Solve(FHydraulicSolveSettings());
		FCoolantInventory Inv;
		Inv.AddTank(Net, Cdu, Pump, { Leak }, M);
		FHydraulicSolveResult R;
		for (int32 S = 0; S < 500; ++S) Inv.Step(Net, FHydraulicSolveSettings(), 0.02, S * 0.02, R);
		const FCoolantInventory::FTank& T = Inv.GetTanks()[0];
		const double PErr = FMath::Abs(Inv.GetTankGaugePressurePa(0) - T.FillGaugePa) / T.FillGaugePa;
		AddInfo(FString::Printf(TEXT("(f) 보충: 누적 누수 %.5f L, 보충 %.5f L, 탱크 압력 오차 %.3e"), T.State.LeakedM3 * 1e3, T.State.MakeupM3 * 1e3, PErr));
		TestTrue(TEXT("(f) make-up holds the fill pressure (rel < 1e-12) and equals the leak (rel < 1e-12)"),
			PErr < 1e-12 && FMath::Abs(T.State.MakeupM3 - T.State.LeakedM3) / T.State.LeakedM3 < 1e-12);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoolantRoomCouplingTest, "VRG.Hydraulics.Inventory.RoomLeakCoupling", VrgInventoryTestFlags)
bool FCoolantRoomCouplingTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	TArray<ARackThermalActor*> Racks;
	for (const double Y : { 0.0, 66.0 })   // one row of two racks (synthetic placement)
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Racks.Add(Rack);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();

	// Without a leak the inventory never solves (the run is the static network as before).
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);
	TestEqual(TEXT("no leak: no inventory solve"), Room->GetCoolantInventory().GetNumSolves(), 0);
	Sim->EnterLayoutMode();

	// A 5 mm supply-branch leak on rack 0 (synthetic size).
	const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
	const int32 Spec = Room->AddLeak(Routed[0], false, 5.0e-3);
	Room->RebuildRoomNow();
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);   // 1 s
	const FCoolantInventory::FTank& T = Room->GetCoolantInventory().GetTanks()[0];
	const FHydraulicNetwork& Net = Room->GetNetwork();
	const int32 Leak = Room->GetLeakNetworkIndex(Spec);
	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	// The rack reads the half of its branch that enters it (downstream of the hole).
	const int32 Branch0 = Room->GetRackBranchPipeIndices()[0];
	const double Rack0Kgs = Net.GetPipeFlowM3s(Branch0) * Sim->CoolantDensityKgM3;
	const double CouplingErr = FMath::Abs(Snap.RackCoolantMassFlowKgs[0] - Rack0Kgs) / Rack0Kgs;
	const bool bDownstream = Net.GetPipeFromNodeIndex(Branch0) == Net.GetLeak(Leak).NodeIndex;
	AddInfo(FString::Printf(TEXT("5 mm 공급 분기 누수 1 s: 누수 %.4f L/s, 누적 %.4f L, 탱크 %.1f kPa(g), 랙0 %.4f / 랙1 %.4f kg/s, 결합 오차 %.3e, 전역 잔차 %.3e, 풀이 %d회"),
		Net.GetLeakFlowM3s(Leak) * 1e3, T.State.LeakedM3 * 1e3, Room->GetCoolantInventory().GetTankGaugePressurePa(0) * 1e-3,
		Snap.RackCoolantMassFlowKgs[0], Snap.RackCoolantMassFlowKgs[1], CouplingErr, Snap.NormalizedEnergyResidual, Room->GetCoolantInventory().GetNumSolves()));
	TestTrue(TEXT("leak drains the tank during the run"), T.State.LeakedM3 > 0.0 && T.State.WaterVolumeM3 < T.State.InitialWaterM3);
	TestTrue(TEXT("rack 0 reads the branch half downstream of the hole"), bDownstream);
	TestTrue(TEXT("coupled rack flow = that pipe's flow (rel < 1e-12)"), CouplingErr < 1e-12);
	TestTrue(TEXT("supply leak lowers the leaking rack's flow below its neighbour's"), Snap.RackCoolantMassFlowKgs[0] < Snap.RackCoolantMassFlowKgs[1]);
	TestTrue(TEXT("global ledger still closes (|residual| < 1e-9)"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("hydraulics converged"), Room->GetLastSolveResult().bConverged);

	// Re-entering the simulation starts a new run: tank full again.
	Sim->EnterLayoutMode();
	Sim->EnterSimulationMode();
	TestTrue(TEXT("re-entry: tank back to its fill state"), Room->GetCoolantInventory().GetTanks()[0].State.LeakedM3 == 0.0
		&& Room->GetCoolantInventory().GetTanks()[0].State.WaterVolumeM3 == Room->GetCoolantInventory().GetTanks()[0].State.InitialWaterM3);
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

namespace
{
	// One row of two GB200 racks + one CDU + air (synthetic placement), as in RoomLeakCoupling.
	struct FLeakRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;
		TArray<ARackThermalActor*> Routed;

		FLeakRoom()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false;
			Sim->MaxStepsPerFrame = 16;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
			World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
			Routed = Room->GetRoutedRacks();
		}
		~FLeakRoom()
		{
			Sim->EnterLayoutMode();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
	};

	double CoolantLedgerError(const FCouplingSnapshot& S)
	{
		const double Out = S.CoolantRejectedJ + S.CoolantLeakHeatJ;
		return Out > 0.0 ? FMath::Abs(S.RackCoolantOutJ - Out) / Out : FMath::Abs(S.RackCoolantOutJ);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoolantLeakHeatTest, "VRG.Hydraulics.Inventory.LeakHeatLedger", VrgInventoryTestFlags)
bool FCoolantLeakHeatTest::RunTest(const FString& Parameters)
{
	// (a) No leak: the leak heat term is exactly zero and the loop ledger closes as before.
	double NoLeakOutletK = 0.0;
	{
		FLeakRoom R;
		R.Sim->EnterSimulationMode();
		R.Run(1.0);
		const FCouplingSnapshot S = R.Sim->GetSnapshot();
		NoLeakOutletK = R.Routed[0]->GetMutableModel().GetState().CoolantTempK;
		AddInfo(FString::Printf(TEXT("(a) 누수 없음 1 s: 누수 열 %.3e W / %.3e J, 루프 장부 %.3e, 전역 %.3e"), S.CoolantLeakHeatW, S.CoolantLeakHeatJ, CoolantLedgerError(S), S.NormalizedEnergyResidual));
		TestTrue(TEXT("(a) no leak: leak heat exactly 0"), S.CoolantLeakHeatW == 0.0 && S.CoolantLeakHeatJ == 0.0);
		TestTrue(TEXT("(a) loop ledger rack = rejected (rel < 1e-12)"), CoolantLedgerError(S) < 1e-12);
	}

	// (b) Return leak 5 mm on rack 0 and supply leak 5 mm on rack 1 (synthetic sizes).
	{
		FLeakRoom R;
		R.Room->AddLeak(R.Routed[0], true, 5.0e-3);
		R.Room->AddLeak(R.Routed[1], false, 5.0e-3);
		R.Room->RebuildRoomNow();
		R.Sim->EnterSimulationMode();
		R.Run(0.5);
		// One more single step: the leak heat must equal m_leak*cp*(T_leak - T_tank) from the values the step used.
		FRackThermalModel& M0 = R.Routed[0]->GetMutableModel();
		const double CpJKgK = M0.GetParams().CoolantCpJKgK;
		const double TankK = R.Routed[0]->GetMutableInput().CoolantInletTempK;   // previous supply = tank water
		const double Return0K = M0.GetState().CoolantTempK;                       // rack 0 outlet at the step start
		const double TimeBefore = R.Sim->GetSnapshot().SimTimeS;
		R.Sim->Tick(.021f);
		const FCouplingSnapshot S1 = R.Sim->GetSnapshot();
		const double SupplyK = R.Routed[1]->GetMutableInput().CoolantInletTempK;
		const double ExpectedW = S1.RackReturnLeakKgs[0] * CpJKgK * (Return0K - TankK) + S1.RackSupplyLeakKgs[1] * CpJKgK * (SupplyK - TankK);
		const double TermErr = FMath::Abs(S1.CoolantLeakHeatW - ExpectedW) / FMath::Abs(ExpectedW);
		AddInfo(FString::Printf(TEXT("(b) 한 스텝(%.2f -> %.2f s): 누수 회수 %.4f / 공급 %.4f kg/s, T_tank %.3f / 회수 %.3f / 공급 %.3f K, 누수 열 %.3f W (직접 %.3f), 상대 %.3e"),
			TimeBefore, S1.SimTimeS, S1.RackReturnLeakKgs[0], S1.RackSupplyLeakKgs[1], TankK, Return0K, SupplyK, S1.CoolantLeakHeatW, ExpectedW, TermErr));
		TestTrue(TEXT("(b) exactly one step"), FMath::IsNearlyEqual(S1.SimTimeS - TimeBefore, R.Sim->FixedStepS, 1e-9));
		TestTrue(TEXT("(b) leaks flow on both branches"), S1.RackReturnLeakKgs[0] > 0.0 && S1.RackSupplyLeakKgs[1] > 0.0);
		TestTrue(TEXT("(b) leak heat = m*cp*(T_leak - T_tank) (rel < 1e-12)"), TermErr < 1e-12);

		R.Run(1.0);
		const FCouplingSnapshot S = R.Sim->GetSnapshot();
		const double LoopErr = CoolantLedgerError(S);
		AddInfo(FString::Printf(TEXT("(b) t=%.2f s: 랙 반출 %.6e J = 방출 %.6e + 누수 열 %.6e J, 루프 상대 %.3e, 전역 %.3e, 수력 %d / 공기 압력 %d"),
			S.SimTimeS, S.RackCoolantOutJ, S.CoolantRejectedJ, S.CoolantLeakHeatJ, LoopErr, S.NormalizedEnergyResidual, S.bHydraulicsConverged ? 1 : 0, S.bAirPressureConverged ? 1 : 0));
		TestTrue(TEXT("(b) loop ledger rack = rejected + leak heat (rel < 1e-12)"), LoopErr < 1e-12);
		TestTrue(TEXT("(b) leak heat is booked (> 0)"), S.CoolantLeakHeatJ > 0.0);
		TestTrue(TEXT("(b) global ledger closes with the leak outflow (|residual| < 1e-9)"), FMath::Abs(S.NormalizedEnergyResidual) < 1e-9);
		TestTrue(TEXT("(b) hydraulics and air pressure converged"), S.bHydraulicsConverged && S.bAirPressureConverged);
	}

	// (c) Supply leak on rack 0 only: the leaking rack's outlet runs hotter than without a leak (less coolant flow).
	{
		FLeakRoom R;
		R.Room->AddLeak(R.Routed[0], false, 5.0e-3);
		R.Room->RebuildRoomNow();
		R.Sim->EnterSimulationMode();
		R.Run(1.0);
		const double OutletK = R.Routed[0]->GetMutableModel().GetState().CoolantTempK;
		AddInfo(FString::Printf(TEXT("(c) 랙0 냉각수 출구 1 s: 누수 없음 %.4f K / 공급 누수 %.4f K"), NoLeakOutletK, OutletK));
		TestTrue(TEXT("(c) supply leak: leaking rack's coolant outlet rises"), OutletK > NoLeakOutletK);
	}

	// (d) 10 mm return leak until the tank is empty, then 3 s more: the loop stops, the racks keep heating through
	//     their own heat capacity (D2), and both ledgers still close.
	{
		FLeakRoom R;
		R.Room->AddLeak(R.Routed[0], true, 10.0e-3);
		R.Room->RebuildRoomNow();
		R.Sim->EnterSimulationMode();
		double T = 0.0;
		while (T < 10.0 && !R.Room->GetCoolantInventory().GetTanks()[0].State.bDepleted) { R.Run(0.1); T += 0.1; }
		R.Run(0.2);
		const FCouplingSnapshot S0 = R.Sim->GetSnapshot();
		const double Coolant0K = R.Routed[0]->GetMutableModel().GetState().CoolantTempK;
		const double WaterStored0J = R.Routed[0]->GetMutableModel().GetTrayCoolant().GetStoredJ();
		R.Run(3.0);
		const FCouplingSnapshot S = R.Sim->GetSnapshot();
		const double Coolant1K = R.Routed[0]->GetMutableModel().GetState().CoolantTempK;
		const double WaterStored1J = R.Routed[0]->GetMutableModel().GetTrayCoolant().GetStoredJ();
		const FCoolantInventory::FTank& Tank = R.Room->GetCoolantInventory().GetTanks()[0];
		const double Duty = S.CduDutyW.Num() > 0 ? S.CduDutyW[0] : -1.0;
		AddInfo(FString::Printf(TEXT("(d) 고갈 t=%.2f s; t=%.2f s: 랙 유량 %.3e / %.3e kg/s, CDU %.3e W, 누수 열 %.3e W, 랙0 냉각수 %.3f -> %.3f K, 루프 %.3e, 전역 %.3e"),
			Tank.State.DepletedAtS, S.SimTimeS, S.RackCoolantMassFlowKgs[0], S.RackCoolantMassFlowKgs[1], Duty, S.CoolantLeakHeatW,
			Coolant0K, Coolant1K, CoolantLedgerError(S), S.NormalizedEnergyResidual));
		TestTrue(TEXT("(d) tank depleted"), Tank.State.bDepleted);
		TestTrue(TEXT("(d) depleted loop: no coolant flow, no CDU duty, no leak heat"),
			FMath::Abs(S.RackCoolantMassFlowKgs[0]) < 1e-6 && FMath::Abs(S.RackCoolantMassFlowKgs[1]) < 1e-6 && FMath::Abs(Duty) < 1e-3 && S.CoolantLeakHeatW == 0.0);
		TestTrue(TEXT("(d) coolant energy accumulates without flow (D2); scalar return view is not total water storage"),
			FMath::IsFinite(Coolant1K) && (R.Routed[0]->GetMutableModel().UsesTrayCoolant() ? WaterStored1J > WaterStored0J : Coolant1K > Coolant0K));
		TestTrue(TEXT("(d) loop ledger still closes (rel < 1e-12)"), CoolantLedgerError(S) < 1e-12 && CoolantLedgerError(S0) < 1e-12);
		TestTrue(TEXT("(d) global ledger still closes (|residual| < 1e-9)"), FMath::Abs(S.NormalizedEnergyResidual) < 1e-9);
	}
	return true;
}

// ---- S2b step 4: isolation valves ----

namespace
{
	// NVL72 row with the room's plain heights; rack ClosedRack docked with 0 units when bRemoved (reference network
	// without that branch). Optional supply leak on rack 3.
	FInRowLoopHandles BuildValveRow(FHydraulicNetwork& Net, bool bRemoveRack3, double LeakDiameterM, int32& OutLeak)
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = 8;
		if (bRemoveRack3) { Spec.DockedSledsPerRack.Init(Spec.SledsPerRack, 8); Spec.DockedSledsPerRack[3] = 0; }
		const FInRowLoopHandles H = InRowLoopBuilder::AddLoop(Net, Spec);
		for (int32 N = 0; N < Net.GetNumNodes(); ++N) Net.SetNodeElevationM(N, 0.25);
		Net.SetNodeElevationM(H.CduNodeIndex, 0.6);
		for (const int32 Node : H.RackNodeIndices) if (Node != INDEX_NONE) Net.SetNodeElevationM(Node, 1.1);
		OutLeak = INDEX_NONE;
		if (LeakDiameterM > 0.0)
		{
			const int32 Hole = Net.SplitPipe(H.RackBranchPipeIndices[3], 0.5, 0.675);   // original keeps the header end
			FHydraulicLeak Leak;
			Leak.NodeIndex = Hole; Leak.HoleDiameterM = LeakDiameterM;
			OutLeak = Net.AddLeak(Leak);
		}
		return H;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicValveIsolationTest, "VRG.Hydraulics.Valve.IsolateBranchMatchesRemoved", VrgInventoryTestFlags)
bool FHydraulicValveIsolationTest::RunTest(const FString& Parameters)
{
	FHydraulicNetwork Net, Removed;
	int32 Leak, Unused;
	const FInRowLoopHandles H = BuildValveRow(Net, false, 3.0e-3, Leak);
	const FInRowLoopHandles HR = BuildValveRow(Removed, true, 0.0, Unused);
	const FHydraulicSolveResult R0 = Net.Solve(FHydraulicSolveSettings());
	const FHydraulicSolveResult RR = Removed.Solve(FHydraulicSolveSettings());
	TArray<double> Before;
	for (const int32 P : H.RackBranchPipeIndices) Before.Add(Net.GetPipeFlowM3s(P));
	const double Pump0 = Net.GetPumpFlowM3s(H.PumpIndex), Leak0 = Net.GetLeakFlowM3s(Leak);

	// Close rack 3's valves: supply pipe at the supply header (the original half of the split branch) and return pipe.
	Net.SetPipeClosed(H.RackBranchPipeIndices[3], true);
	Net.SetPipeClosed(H.RackReturnPipeIndices[3], true);
	const FHydraulicSolveResult R1 = Net.Solve(FHydraulicSolveSettings());
	double MaxDiff = 0.0, MaxChange = 0.0;
	for (int32 J = 0; J < 8; ++J)
	{
		if (J == 3) continue;
		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(Net.GetPipeFlowM3s(H.RackBranchPipeIndices[J]) - Removed.GetPipeFlowM3s(HR.RackBranchPipeIndices[J])) / FMath::Abs(Removed.GetPipeFlowM3s(HR.RackBranchPipeIndices[J])));
		MaxChange = FMath::Max(MaxChange, FMath::Abs(Net.GetPipeFlowM3s(H.RackBranchPipeIndices[J]) - Before[J]) / FMath::Abs(Before[J]));
	}
	const double PumpDiff = FMath::Abs(Net.GetPumpFlowM3s(H.PumpIndex) - Removed.GetPumpFlowM3s(HR.PumpIndex)) / Removed.GetPumpFlowM3s(HR.PumpIndex);
	int32 NumIsolated = 0;
	for (int32 N = 0; N < Net.GetNumNodes(); ++N) NumIsolated += Net.IsNodeIsolated(N) ? 1 : 0;
	AddInfo(FString::Printf(TEXT("누수 3 mm 분기 밸브 닫음: 누수 %.4f -> %.3e L/s, 펌프 %.4f -> %.4f L/s (분기 제거 망 %.4f), 다른 분기 제거 망 대비 최대 %.3e / 펌프 %.3e, 다른 랙 유량 변화 최대 %.3e, 고립 노드 %d개, 반복 %d"),
		Leak0 * 1e3, Net.GetLeakFlowM3s(Leak) * 1e3, Pump0 * 1e3, Net.GetPumpFlowM3s(H.PumpIndex) * 1e3, Removed.GetPumpFlowM3s(HR.PumpIndex) * 1e3,
		MaxDiff, PumpDiff, MaxChange, NumIsolated, R1.IterationsUsed));
	TestTrue(TEXT("solves converge"), R0.bConverged && RR.bConverged && R1.bConverged);
	TestTrue(TEXT("leak upstream valve closed: leak flow exactly 0"), Leak0 > 0.0 && Net.GetLeakFlowM3s(Leak) == 0.0);
	TestTrue(TEXT("closed pipes carry exactly 0"), Net.GetPipeFlowM3s(H.RackBranchPipeIndices[3]) == 0.0 && Net.GetPipeFlowM3s(H.RackReturnPipeIndices[3]) == 0.0);
	TestTrue(TEXT("isolation scope: rack 3 node and hole node only (2 nodes)"), Net.IsNodeIsolated(H.RackNodeIndices[3]) && NumIsolated == 2);
	TestTrue(TEXT("closed = branch removed from the start: other branches and pump (rel < 1e-9)"), MaxDiff < 1e-9 && PumpDiff < 1e-9);
	TestTrue(TEXT("hydraulic impact differs from isolation: other racks' flows change (> 1e-3)"), MaxChange > 1e-3);
	TestTrue(TEXT("mass balance at the CDU node with the valve closed (rel < 1e-9)"),
		FMath::Abs(Net.GetPumpFlowM3s(H.PumpIndex) - Net.GetPipeFlowM3s(H.ReturnToCduPipeIndex)) / Net.GetPumpFlowM3s(H.PumpIndex) < 1e-9);

	// Reopen: back to the solution before closing.
	Net.SetPipeClosed(H.RackBranchPipeIndices[3], false);
	Net.SetPipeClosed(H.RackReturnPipeIndices[3], false);
	Net.Solve(FHydraulicSolveSettings());
	double MaxBack = FMath::Abs(Net.GetLeakFlowM3s(Leak) - Leak0) / Leak0;
	for (int32 J = 0; J < 8; ++J) MaxBack = FMath::Max(MaxBack, FMath::Abs(Net.GetPipeFlowM3s(H.RackBranchPipeIndices[J]) - Before[J]) / FMath::Abs(Before[J]));
	AddInfo(FString::Printf(TEXT("다시 엶: 닫기 전 해 대비 최대 %.3e, 고립 노드 %d"), MaxBack, Net.IsNodeIsolated(H.RackNodeIndices[3]) ? 1 : 0));
	TestTrue(TEXT("reopen: back to the solution before closing (rel < 1e-9)"), MaxBack < 1e-9 && !Net.IsNodeIsolated(H.RackNodeIndices[3]));

	// A valve set that cuts a pump off from every fixed node is reported, not solved.
	FHydraulicNetwork Bad;
	int32 BadLeak;
	const FInRowLoopHandles HB = BuildValveRow(Bad, false, 0.0, BadLeak);
	for (int32 P = 0; P < Bad.GetNumPipes(); ++P) Bad.SetPipeClosed(P, true);
	Bad.SetNodeSetPressurePa(HB.CduNodeIndex, Bad.GetNodeSetPressurePa(HB.CduNodeIndex));
	// The pump still touches the CDU node, so it is not isolated: this closes everything downstream instead.
	const FHydraulicSolveResult RB = Bad.Solve(FHydraulicSolveSettings());
	AddInfo(FString::Printf(TEXT("모든 배관 닫음: 수렴 %d, ill-posed %d, 펌프 %.3e L/s"), RB.bConverged ? 1 : 0, RB.bIllPosed ? 1 : 0, Bad.GetPumpFlowM3s(HB.PumpIndex) * 1e3));
	TestTrue(TEXT("everything closed: reported (ill-posed) or a converged zero-flow pump, never a silent wrong flow"),
		RB.bIllPosed || (RB.bConverged && FMath::Abs(Bad.GetPumpFlowM3s(HB.PumpIndex)) < 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicValveRoomTest, "VRG.Hydraulics.Valve.RoomEventScopesAndLedger", VrgInventoryTestFlags)
bool FHydraulicValveRoomTest::RunTest(const FString& Parameters)
{
	FLeakRoom R;
	R.Room->AddLeak(R.Routed[0], false, 5.0e-3);   // supply leak upstream of rack 0 (synthetic size)
	R.Room->RebuildRoomNow();
	auto AddEvent = [&R](double T, bool bClose)
	{
		ADataCenterSimulationActor::FEvalEvent E;
		E.TimeS = T; E.Kind = TEXT("valve"); E.Index = 0; E.Value = bClose ? 1.0 : 0.0;
		R.Sim->EvalEvents.Add(E);
	};
	AddEvent(0.5, true);
	AddEvent(1.5, false);
	R.Sim->EnterSimulationMode();
	R.Run(0.4);
	const FCouplingSnapshot S0 = R.Sim->GetSnapshot();
	R.Run(0.3);   // the close event fired at t = 0.5 s
	const ADataCenterRoomActor::FValveReport Report = R.Room->GetLastValveReport();
	const FCoolantInventory::FTank& Tank = R.Room->GetCoolantInventory().GetTanks()[0];
	const double Leaked1 = Tank.State.LeakedM3;
	R.Run(0.6);
	const FCouplingSnapshot S1 = R.Sim->GetSnapshot();
	const double Leaked2 = Tank.State.LeakedM3;
	const double Rack1Change = FMath::Abs(Report.FlowAfterM3s[1] - Report.FlowBeforeM3s[1]) / FMath::Abs(Report.FlowBeforeM3s[1]);
	const double InvLedger = FMath::Abs(Tank.State.InitialWaterM3 - Tank.State.WaterVolumeM3 - Tank.State.LeakedM3) / Tank.State.LeakedM3;
	auto CoolantErr = [](const FCouplingSnapshot& S) { const double Out = S.CoolantRejectedJ + S.CoolantLeakHeatJ; return FMath::Abs(S.RackCoolantOutJ - Out) / Out; };
	AddInfo(FString::Printf(TEXT("t=%.2f s 밸브 닫힘(랙0): 격리 [%s] / 영향 [%s], 랙1 분기 %.4f -> %.4f L/s (%.3e); 랙0 유량 %.4f -> %.3e kg/s, 누수 %.4f -> %.3e kg/s, 누적 누수 %.5f -> %.5f L, 루프 장부 %.3e, 재고 장부 %.3e, 전역 %.3e"),
		S1.SimTimeS, Report.IsolatedRacks.Num() == 1 ? TEXT("0") : TEXT("?"), Report.ImpactedRacks.Contains(1) ? TEXT("1") : TEXT("-"),
		Report.FlowBeforeM3s[1] * 1e3, Report.FlowAfterM3s[1] * 1e3, Rack1Change,
		S0.RackCoolantMassFlowKgs[0], S1.RackCoolantMassFlowKgs[0], S0.RackSupplyLeakKgs[0], S1.RackSupplyLeakKgs[0],
		Leaked1 * 1e3, Leaked2 * 1e3, CoolantErr(S1), InvLedger, S1.NormalizedEnergyResidual));
	TestTrue(TEXT("event applied: rack 0 valves closed, converged"), R.Room->IsRackValveClosed(R.Routed[0]) && Report.bClosed && Report.bConverged);
	TestTrue(TEXT("isolation scope = rack 0 only; hydraulic impact scope = rack 1 (a different set)"),
		Report.IsolatedRacks.Num() == 1 && Report.IsolatedRacks[0] == 0 && Report.ImpactedRacks.Num() == 1 && Report.ImpactedRacks[0] == 1);
	TestTrue(TEXT("valved rack: no coolant flow, no leak, tank stops draining"),
		S1.RackCoolantMassFlowKgs[0] == 0.0 && S1.RackSupplyLeakKgs[0] == 0.0 && Leaked2 == Leaked1);
	TestTrue(TEXT("ledgers close across the valve operation (loop < 1e-12, inventory < 1e-12, global < 1e-9)"),
		CoolantErr(S1) < 1e-12 && InvLedger < 1e-12 && FMath::Abs(S1.NormalizedEnergyResidual) < 1e-9);

	R.Run(0.5);   // the open event fired at t = 1.5 s
	const FCouplingSnapshot S2 = R.Sim->GetSnapshot();
	AddInfo(FString::Printf(TEXT("t=%.2f s 다시 엶: 랙0 유량 %.4f kg/s, 누수 %.4f kg/s, 전역 %.3e"), S2.SimTimeS, S2.RackCoolantMassFlowKgs[0], S2.RackSupplyLeakKgs[0], S2.NormalizedEnergyResidual));
	TestTrue(TEXT("reopened: flow and leak resume, ledgers close"), !R.Room->IsRackValveClosed(R.Routed[0])
		&& S2.RackCoolantMassFlowKgs[0] > 0.0 && S2.RackSupplyLeakKgs[0] > 0.0 && CoolantErr(S2) < 1e-12 && FMath::Abs(S2.NormalizedEnergyResidual) < 1e-9);

	// A new run starts with every valve open and the events again.
	R.Room->SetRackValveClosed(R.Routed[1], true);
	R.Sim->EnterLayoutMode();
	R.Sim->EnterSimulationMode();
	TestTrue(TEXT("re-entry: all valves open"), !R.Room->IsRackValveClosed(R.Routed[0]) && !R.Room->IsRackValveClosed(R.Routed[1]));
	return true;
}

// ---- S2b step 5: fault judgement, events, report ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFaultMonitorJsonTest, "VRG.Hydraulics.Fault.MonitorAndReportJson", VrgInventoryTestFlags)
bool FFaultMonitorJsonTest::RunTest(const FString& Parameters)
{
	FVrgFaultMonitor M;
	M.Reset(FVrgFaultSettings());
	FVrgFaultInput In;
	In.TimeS = 1.25; In.CoolantMassFlowKgs = { 1.0, 3.0 }; In.DesignMassFlowKgs = { 3.0, 3.0 }; In.MaxComponentTempK = { 300.0, 400.0 }; In.LoopDepleted = { false, true };
	const int32 N1 = M.Update(In);
	In.TimeS = 2.5;
	const int32 N2 = M.Update(In);   // the same faults again: first occurrences only
	In.bHydraulicsConverged = false; In.HydraulicResidual = 1e-3;
	const int32 N3 = M.Update(In);
	TestTrue(TEXT("first occurrence only: low flow r0, over-temperature r1, depleted r1, then hydraulic once"), N1 == 3 && N2 == 0 && N3 == 1);
	TestTrue(TEXT("first fault time per rack"), M.GetFirstFaultTimeS(0) == 1.25 && M.GetFirstFaultTimeS(1) == 1.25 && M.GetFirstFaultTimeS(5) < 0.0);
	FVrgLeakRecord L; L.TimeS = 0.5; L.Rack = 3; L.bReturnBranch = true; L.HoleDiameterM = 3.0e-3 / 7.0; M.AddLeak(L);
	FVrgValveRecord V; V.TimeS = 5.0; V.Rack = 3; V.bClosed = true; V.IsolatedRacks = { 3 }; V.ImpactedRacks = { 0, 1, 2 }; M.AddValve(V);
	FVrgTankSample T; T.TimeS = 1.0 / 3.0; T.Row = 0; T.PressurePa = 199123.456789; T.WaterM3 = 2.3894e-3 / 3.0; T.LeakedM3 = 1e-5 / 7.0; M.AddTankSample(T);

	FString Text;
	FJsonSerializer::Serialize(M.ToJsonObject(), TJsonWriterFactory<>::Create(&Text));
	TSharedPtr<FJsonObject> Parsed;
	const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed);
	FVrgFaultMonitor Back;
	const bool bRead = bParsed && FVrgFaultMonitor::FromJsonObject(Parsed, Back);
	AddInfo(FString::Printf(TEXT("JSON %d자, 판정 %d / 누수 %d / 밸브 %d / 탱크 %d, 왕복 일치 %d"), Text.Len(), Back.GetFaults().Num(), Back.GetLeaks().Num(), Back.GetValves().Num(), Back.GetTankSamples().Num(), bRead && Back == M ? 1 : 0));
	TestTrue(TEXT("report JSON round trip: exactly equal"), bRead && Back == M);
	FVrgFaultMonitor Old;
	TestTrue(TEXT("older report without the s2b object reads as empty"), FVrgFaultMonitor::FromJsonObject(nullptr, Old) && Old.GetFaults().Num() == 0 && Old.GetLeaks().Num() == 0);
	return true;
}

namespace
{
	// One run with event-driven faults; returns the monitor and the leaked volume.
	FVrgFaultMonitor RunFaultRoom(double Seconds, double LowFlowRatio, int32 LeakRack, bool bReturn, double DiameterM, double& OutLeakedM3,
		TArray<double>* OutRatio = nullptr)
	{
		FLeakRoom R;
		R.Sim->FaultSettings.LowFlowRatio = LowFlowRatio;
		ADataCenterSimulationActor::FEvalEvent E;
		E.TimeS = 0.5; E.Kind = TEXT("leak"); E.Index = LeakRack; E.bFlag = bReturn; E.Value = DiameterM;
		R.Sim->EvalEvents.Add(E);
		R.Sim->EnterSimulationMode();
		R.Run(Seconds);
		OutLeakedM3 = R.Room->GetCoolantInventory().GetTanks()[0].State.LeakedM3;
		if (OutRatio)
		{
			const FCouplingSnapshot S = R.Sim->GetSnapshot();
			const TArray<double>& Design = R.Room->GetRackDesignFlowM3s();
			OutRatio->Reset();
			for (int32 I = 0; I < R.Routed.Num(); ++I) OutRatio->Add(S.RackCoolantMassFlowKgs[I] / (Design[I] * R.Sim->CoolantDensityKgM3));
		}
		return R.Sim->GetFaultMonitor();
	}

	TArray<int32> RacksOf(const FVrgFaultMonitor& M, EVrgFaultKind Kind)
	{
		TArray<int32> Out;
		for (const FVrgFaultRecord& F : M.GetFaults()) if (F.Kind == Kind) Out.AddUnique(F.Rack);
		Out.Sort();
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFaultRoomEventsTest, "VRG.Hydraulics.Fault.RoomEventsDeterministicNotHardcoded", VrgInventoryTestFlags)
bool FFaultRoomEventsTest::RunTest(const FString& Parameters)
{
	// (a) 10 mm return leak on rack 0 at t = 0.5 s until the row's inventory is depleted; run twice.
	double LeakedA = 0.0, LeakedB = 0.0;
	const FVrgFaultMonitor A = RunFaultRoom(3.5, 0.5, 0, true, 10.0e-3, LeakedA);
	const FVrgFaultMonitor B = RunFaultRoom(3.5, 0.5, 0, true, 10.0e-3, LeakedB);
	const double LeakT = A.GetLeaks().Num() > 0 ? A.GetLeaks()[0].TimeS : -1.0;
	FString Lines;
	for (const FVrgFaultRecord& F : A.GetFaults()) Lines += FString::Printf(TEXT(" [t=%.2f 랙%d %s %.4g]"), F.TimeS, F.Rack, FVrgFaultMonitor::KindName(F.Kind), F.Value);
	AddInfo(FString::Printf(TEXT("(a) 누수 이벤트 적용 t=%.4f s, 누적 누수 %.6f / %.6f L, 판정%s, 탱크 표본 %d, 두 실행 동일 %d"),
		LeakT, LeakedA * 1e3, LeakedB * 1e3, *Lines, A.GetTankSamples().Num(), A == B ? 1 : 0));
	TestTrue(TEXT("(a) event applied at its time on the integrated clock (within one step)"), LeakT >= 0.5 - 1e-9 && LeakT < 0.5 + 0.02);
	TestTrue(TEXT("(a) depletion judged for both racks of the row (same loop), low flow follows from the stopped pump"),
		RacksOf(A, EVrgFaultKind::InventoryDepleted) == TArray<int32>({ 0, 1 }) && RacksOf(A, EVrgFaultKind::LowCoolantFlow) == TArray<int32>({ 0, 1 }));
	TestTrue(TEXT("(a) the stopped loop still solves (no hydraulic fault)"), RacksOf(A, EVrgFaultKind::HydraulicNotConverged).Num() == 0);
	TestTrue(TEXT("(a) rerun determinism: same faults, times, leaks, tank history and leaked volume"), A == B && LeakedA == LeakedB && LeakedA > 0.0);

	// (b) Not hard-coded: the same 5 mm supply leak at rack 0 or at rack 1. With a judgement ratio of 0.985 the
	//     flagged set must match solved ratios below the threshold; the larger hydraulic impact moves with the leak.
	double Unused = 0.0;
	TArray<double> Ratio0, Ratio1;
	const FVrgFaultMonitor P0 = RunFaultRoom(1.5, 0.985, 0, false, 5.0e-3, Unused, &Ratio0);
	const FVrgFaultMonitor P1 = RunFaultRoom(1.5, 0.985, 1, false, 5.0e-3, Unused, &Ratio1);
	auto Expected = [](const TArray<double>& Ratio) { TArray<int32> Out; for (int32 I = 0; I < Ratio.Num(); ++I) if (Ratio[I] < 0.985) Out.Add(I); return Out; };
	const TArray<int32> F0 = RacksOf(P0, EVrgFaultKind::LowCoolantFlow), F1 = RacksOf(P1, EVrgFaultKind::LowCoolantFlow);
	AddInfo(FString::Printf(TEXT("(b) 공급 누수 랙0: 유량비 %.4f / %.4f -> 판정 %d개(첫 %d) | 랙1: 유량비 %.4f / %.4f -> 판정 %d개(첫 %d)"),
		Ratio0[0], Ratio0[1], F0.Num(), F0.Num() ? F0[0] : -1, Ratio1[0], Ratio1[1], F1.Num(), F1.Num() ? F1[0] : -1));
	TestTrue(TEXT("(b) flagged set = racks with recomputed ratio below the threshold"), F0 == Expected(Ratio0) && F1 == Expected(Ratio1));
	TestTrue(TEXT("(b) moving the leak moves the hydraulic impact; alarms still follow solved ratios, not a fixed rack table"),
		Ratio0[0] < Ratio0[1] && Ratio1[1] < Ratio1[0]);
	return true;
}

#endif
