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

#endif
