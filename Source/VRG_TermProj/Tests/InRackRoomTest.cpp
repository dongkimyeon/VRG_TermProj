#include "Misc/AutomationTest.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Coupling/InRackTransport.h"
#include "Simulation/Core/ElapsedTimer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgInRackRoom, "VRG.Hydraulics.InRack.Room",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FVrgInRackRoom::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	Room->bUseGjConnections = false;
	ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>();
	Rack->ApplyGb200Nvl72Preset();
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 0, 0), FRotator::ZeroRotator);
	Room->bUseInRackNetworks = false; Room->RebuildRoomNow();
	const double OldFlow = Room->GetNetwork().GetPipeFlowM3s(Room->GetRackBranchPipeIndices()[0]);
	// Completion condition 9: compare direction of the existing dock-count approximation
	// with the explicit tray's two endpoint valves, without forcing equal magnitudes.
	const int32 DetachedSlot = InRackNetwork::FromThermal(Rack->Params, Rack->GetMutableInput()).Slots[0];
	Rack->SetSledDocked(DetachedSlot, false); Room->RebuildRoom(); Room->RebuildRoomNow();
	const double OldDetachedFlow = Room->GetNetwork().GetPipeFlowM3s(Room->GetRackBranchPipeIndices()[0]);
	Rack->SetSledDocked(DetachedSlot, true);
	Room->bUseInRackNetworks = true; Room->RebuildRoom(); Room->RebuildRoomNow();
	TestTrue(TEXT("room solve converged"), Room->GetLastSolveResult().bConverged);
	const FInRackHandles* H = Room->GetInRackHandles(Rack);
	if (TestNotNull(TEXT("actual CDU rack internal network"), H))
	{
		FHydraulicNetwork& Net = Room->GetMutableNetwork();
		const int32 Nodes = Net.GetNumNodes(), Pipes = Net.GetNumPipes(), Leaks = Net.GetNumLeaks();
		const int32 Branch = Room->GetRackBranchPipeIndices()[0];
		const double Flow = Net.GetPipeFlowM3s(Branch);
		AddInfo(FString::Printf(TEXT("room old/new flow %.12g / %.12g m3/s difference %.12g; balance %.12g m3/s; nodes=%d pipes=%d leaks=%d"),
			OldFlow, Flow, Flow - OldFlow, H->MaxBalanceErrorM3s(Net), Nodes, Pipes, Leaks));
		TestTrue(TEXT("original pressure budget"), FMath::Abs(Flow / OldFlow - 1.0) < 1e-8);
		TestEqual(TEXT("27x5 closed sites"), H->Leaks.Num(), 135);
		for (const FRoomPipeRoute& Route : Room->GetPipeRoutesM()) TestFalse(TEXT("internal edges absent from routes/instruments/web"), Net.IsRackInternalPipe(Route.Edge));
		Room->ConsumeRebuiltFlag();
		Rack->SetSledDocked(H->Slots[0], false);
		TestTrue(TEXT("dock update observed"), Room->SyncInRackDocking());
		Room->RebuildRoomNow();
		TestFalse(TEXT("dock does not rebuild room"), Room->ConsumeRebuiltFlag());
		TestEqual(TEXT("nodes stable"), Net.GetNumNodes(), Nodes);
		TestEqual(TEXT("pipes stable"), Net.GetNumPipes(), Pipes);
		TestEqual(TEXT("leaks stable"), Net.GetNumLeaks(), Leaks);
		TestTrue(TEXT("both endpoint valves closed"), Net.IsPipeClosed(H->InletPipes[0]) && Net.IsPipeClosed(H->OutletPipes[0]));
		TestEqual(TEXT("detached flow zero"), Net.GetPipeFlowM3s(H->InletPipes[0]), 0.0);
		const double NewDetachedFlow = Net.GetPipeFlowM3s(Branch);
		TestTrue(TEXT("legacy dock-count and explicit tray closing have same flow trend"), OldDetachedFlow > 0.0 && OldDetachedFlow < OldFlow
			&& NewDetachedFlow > 0.0 && NewDetachedFlow < Flow);
		AddInfo(FString::Printf(TEXT("dock trend slot%d: legacy %.12g -> %.12g, explicit %.12g -> %.12g m3/s; detached branch %.12g m3/s"),
			DetachedSlot, OldFlow, OldDetachedFlow, Flow, NewDetachedFlow, Net.GetPipeFlowM3s(H->InletPipes[0])));
		TestTrue(TEXT("remaining network converges"), Room->GetLastSolveResult().bConverged);
		Net.SetLeakHoleDiameterM(H->Leaks[5 * 13 + 2], .003);
		TestTrue(TEXT("internal leak solved"), Net.Solve(Room->SolveSettings).bConverged);
		TestTrue(TEXT("internal leak inventory membership"), Room->GetCoolantInventory().GetTanks()[0].LeakIndices.Contains(H->Leaks[5 * 13 + 2]));
		TestTrue(TEXT("internal leak separate from W1 sites"), Room->IsInternalLeak(H->Leaks[5 * 13 + 2]));
		TestTrue(TEXT("reverse outlet retained"), Net.GetPipeFlowM3s(H->OutletPipes[13]) < 0.0);
	}
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgInRackRoomSteps, "VRG.Coupling.InRack.TickRunStepsInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FVrgInRackRoomSteps::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = false;
	auto* Rack = World->SpawnActor<ARackThermalActor>(); Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 0, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false; Sim->bFacilityPlant = false; Sim->MaxStepsPerFrame = 16;
	Room->RebuildRoomNow();
	TArray<double> TickK;
	double TickLeakM3 = 0.0, WorstGlobal = 0.0, WorstInventory = 0.0, WorstWaterSplit = 0.0, WorstInventoryRel = 0.0, InitialHxWK = 0.0;
	for (int32 Path = 0; Path < 2; ++Path)
	{
		Sim->EnterSimulationMode();
		const double HxWK = Sim->GetCouplingBridge().GetCduGroups()[0].Hx.UAWK;
		if (Path == 0) InitialHxWK = HxWK;
		else TestEqual(TEXT("restart design UA independent of previous live flow"), HxWK, InitialHxWK);
		// Open both holes at t=0; the first step's inventory volume must not be dropped.
		const FInRackHandles H = *Room->GetInRackHandles(Rack);
		const int32 Nodes = Room->GetNetwork().GetNumNodes(), Pipes = Room->GetNetwork().GetNumPipes();
		TestTrue(TEXT("actual bridge enables tray storage"), Rack->GetMutableModel().UsesTrayCoolant());
		TestTrue(TEXT("open internal hole without topology change"), Room->SetInternalLeakHoleDiameterM(Rack, 13, 2, .003));
		TestTrue(TEXT("open external W2 hole simultaneously"), Room->SetLeakHoleDiameterM(0, .002));
		if (Path == 0) for (int32 Frame = 0; Frame < 6; ++Frame) Sim->Tick(.1f);
		else
		{
			Sim->QueueWarmupSteps(30);
			for (int32 Frame = 0; Frame < 100 && Sim->GetSnapshot().SimTimeS < .6 - 1e-9; ++Frame) Sim->Tick(0.0f);
			TestTrue(TEXT("RunSteps advances identical physical interval"), FMath::Abs(Sim->GetSnapshot().SimTimeS - .6) < 1e-9);
		}
		const auto& Tank = Room->GetCoolantInventory().GetTanks()[0];
		double InternalM3 = 0.0; for (const auto& Entry : Sim->GetInternalLeakedM3()) InternalM3 += Entry.Value;
		const double WaterSplit = InternalM3 + Sim->GetLeakSpray().GetTotals().EmittedM3 - Tank.State.LeakedM3;
		const double InventoryError = Tank.State.InitialWaterM3 + Tank.State.MakeupM3 - Tank.State.WaterVolumeM3 - Tank.State.LeakedM3;
		WorstInventory = FMath::Max(WorstInventory, FMath::Abs(InventoryError)); WorstWaterSplit = FMath::Max(WorstWaterSplit, FMath::Abs(WaterSplit));
		WorstGlobal = FMath::Max(WorstGlobal, FMath::Abs(Sim->GetSnapshot().NormalizedEnergyResidual));
		WorstInventoryRel = FMath::Max(WorstInventoryRel, FMath::Abs(InventoryError) / Tank.State.LeakedM3);
		TestTrue(TEXT("actual hydraulic and air pressure convergence"), Sim->GetSnapshot().bHydraulicsConverged && Sim->GetSnapshot().bAirPressureConverged);
		TestTrue(TEXT("internal water reserved separately"), InternalM3 > 0.0 && Sim->GetLeakSpray().GetTotals().EmittedM3 > 0.0);
		TestEqual(TEXT("runtime node numbers stable"), Room->GetNetwork().GetNumNodes(), Nodes);
		TestEqual(TEXT("runtime pipe numbers stable"), Room->GetNetwork().GetNumPipes(), Pipes);
		if (Path == 0) { TickK = Rack->GetMutableModel().GetState().ComponentTempK; TickLeakM3 = Tank.State.LeakedM3; }
		else
		{
			double MaxK = 0.0; const auto& WarmK = Rack->GetMutableModel().GetState().ComponentTempK;
			for (int32 P = 0; P < TickK.Num(); ++P) MaxK = FMath::Max(MaxK, FMath::Abs(TickK[P] - WarmK[P]));
			AddInfo(FString::Printf(TEXT("Tick/RunSteps max component difference %.12g K; leaked volume difference %.12g m3; global %.12g; inventory %.12g m3; internal+W2 split %.12g m3; internal %.12g m3 W2 %.12g m3"),
				MaxK, Tank.State.LeakedM3 - TickLeakM3, WorstGlobal, WorstInventory, WorstWaterSplit, InternalM3, Sim->GetLeakSpray().GetTotals().EmittedM3));
			TestTrue(TEXT("same heat coupling path"), MaxK < 1e-10 && FMath::Abs(Tank.State.LeakedM3 - TickLeakM3) < 1e-12);
		}
		// Repair the LAST two holes with make-up disabled: the previous leaking pressures
		// must be invalidated even though the inventory would otherwise become idle.
		const double BeforeRepairM3 = Tank.State.LeakedM3;
		Room->SetInternalLeakHoleDiameterM(Rack, 13, 2, 0.0); Room->SetLeakHoleDiameterM(0, 0.0);
		Sim->Tick(.1f);
		TestEqual(TEXT("repair does not leak more volume"), Tank.State.LeakedM3, BeforeRepairM3);
		TestTrue(TEXT("last-hole repair solves the closed topology"), H.MaxBalanceErrorM3s(Room->GetNetwork()) < 1e-12);
		Sim->EnterLayoutMode();
	}
	TestTrue(TEXT("existing global criterion"), WorstGlobal < 1e-9);
	TestTrue(TEXT("inventory and W2 split conservation"), WorstInventory < 1e-12 && WorstWaterSplit < 1e-12);
	AddInfo(FString::Printf(TEXT("inventory absolute %.12g m3 / leaked-volume normalized %.12g; restart UA %.12g W/K"), WorstInventory, WorstInventoryRel, InitialHxWK));
	TestTrue(TEXT("existing relative inventory criterion"), WorstInventoryRel < 1e-12);
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgInRackRoomCost, "VRG.Hydraulics.InRack.Room32Cost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FVrgInRackRoomCost::RunTest(const FString& Parameters)
{
	const auto Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalInput Input; Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	FHydraulicNetwork Old, New;
	TArray<FInRowLoopHandles> Handles;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		FInRowLoopSpec Spec; Spec.NumRacks = 8;
		InRowLoopBuilder::AddLoop(Old, Spec);
		TArray<FInRackSpec> Internal; Internal.Init(InRackNetwork::FromThermal(Params, Input), 8);
		Handles.Add(InRowLoopBuilder::AddLoop(New, Spec, &Internal));
	}
	const double StartOld = FPlatformTime::Seconds(); const auto OldResult = Old.Solve(FHydraulicSolveSettings());
	const double OldMs = (FPlatformTime::Seconds() - StartOld) * 1000.0;
	const double StartNew = FPlatformTime::Seconds(); const auto NewResult = New.Solve(FHydraulicSolveSettings());
	const double NewMs = (FPlatformTime::Seconds() - StartNew) * 1000.0;
	TestTrue(TEXT("32-rack normal old/new converge"), OldResult.bConverged && NewResult.bConverged && NewResult.bUsedSparse);
	auto CheckTransport = [&](const TCHAR* Case)
	{
		TArray<double> Scalar, Bulk;
		for (int32 I = 0; I < New.GetNumNodes(); ++I) Scalar.Add(New.GetNodeImbalanceM3s(I));
		New.GetNodeImbalancesM3s(Bulk);
		auto EqualBits = [](const TArray<double>& A, const TArray<double>& B) {
			return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(double)) == 0;
		};
		TestTrue(TEXT("bulk residuals bit-identical to independent scalar queries, every node"), EqualBits(Scalar, Bulk));
		TArray<FTrayCoolantInput> Legacy;
		const double StartLegacy = FPlatformTime::Seconds();
		for (const auto& Row : Handles) for (int32 I = 0; I < Row.InRacks.Num(); ++I) {
			FTrayCoolantInput T;
			TestTrue(TEXT("scalar transport builds"), InRackTransport::Build(New, Row.InRacks[I], Row.RackReturnPipeIndices[I],
				New.GetFluidDensityKgM3(), Params.CoolantCpJKgK, T));
			Legacy.Add(MoveTemp(T));
		}
		const double LegacyMs = (FPlatformTime::Seconds() - StartLegacy) * 1000;
		const double StartBulk = FPlatformTime::Seconds();
		New.GetNodeImbalancesM3s(Bulk);
		int32 R = 0;
		for (const auto& Row : Handles) for (int32 I = 0; I < Row.InRacks.Num(); ++I) {
			FTrayCoolantInput T;
			TestTrue(TEXT("shared residual transport builds"), InRackTransport::Build(New, Row.InRacks[I], Row.RackReturnPipeIndices[I],
				New.GetFluidDensityKgM3(), Params.CoolantCpJKgK, T, &Bulk));
			const auto& Before = Legacy[R++];
			TestTrue(TEXT("all transport coefficients, signed flows and mass diagnostics unchanged"),
				EqualBits(T.Network.NetWK, Before.Network.NetWK) && EqualBits(T.Network.BoundaryWK, Before.Network.BoundaryWK)
				&& EqualBits(T.Network.LeakWK, Before.Network.LeakWK) && EqualBits(T.Network.ClassLeakWK, Before.Network.ClassLeakWK)
				&& EqualBits(T.Network.ClassLeakKgs, Before.Network.ClassLeakKgs)
				&& EqualBits(T.InletMassFlowKgs, Before.InletMassFlowKgs) && EqualBits(T.OutletMassFlowKgs, Before.OutletMassFlowKgs)
				&& EqualBits(T.LeakMassFlowKgs, Before.LeakMassFlowKgs) && T.ReturnMassFlowKgs == Before.ReturnMassFlowKgs
				&& T.Network.LeakKgs == Before.Network.LeakKgs && T.Network.MassResidualKgs == Before.Network.MassResidualKgs);
		}
		AddInfo(FString::Printf(TEXT("32rack transport %s scalar %.6f ms/shared %.6f ms; every-node residual and all rack operators bit-identical; same solved state, no physics approximation"),
			Case, LegacyMs, (FPlatformTime::Seconds() - StartBulk) * 1000));
	};
	CheckTransport(TEXT("normal"));
	New.SetLeakHoleDiameterM(Handles[0].InRacks[0].Leaks[13 * 5 + 2], .003);
	const double StartLeak = FPlatformTime::Seconds(); const auto LeakResult = New.Solve(FHydraulicSolveSettings());
	const double LeakMs = (FPlatformTime::Seconds() - StartLeak) * 1000.0;
	TestTrue(TEXT("32-rack changed operating point converges"), LeakResult.bConverged);
	CheckTransport(TEXT("leak/reverse"));
 const auto& P=New.GetLastCosts();
 AddInfo(FString::Printf(TEXT("hydraulic profile total %.6f setup %.6f residual %.6f jacobian %.6f CSC %.6f analyze %.6f factor %.6f backsolve %.6f ms; Newton %d residual calls %d"),
 P.TotalMs,P.SetupMs,P.ResidualMs,P.JacobianMs,P.Sparse.AssemblyMs,P.Sparse.AnalyzeMs,P.Sparse.FactorMs,P.Sparse.BacksolveMs,P.NewtonIterations,P.ResidualCalls));
 double TimerMs=0; const double TimerStart=FPlatformTime::Seconds();
 for(int32 I=0;I<100000;++I) { FElapsedTimer Time(TimerMs); }
 AddInfo(FString::Printf(TEXT("observer empty scope average %.3f ns (100000 scopes, includes loop/calls); measured inner sum %.6f ms"),
 (FPlatformTime::Seconds()-TimerStart)*1e9/100000,TimerMs));

	AddInfo(FString::Printf(TEXT("32rack 4x8 hydraulic only: nodes old/new %d/%d pipes %d/%d leaks %d; cold solve old %.6fms/%dit new %.6fms/%dit; changed leak solve %.6fms/%dit; normalized residual %.12g; sparse %d"),
		Old.GetNumNodes(), New.GetNumNodes(), Old.GetNumPipes(), New.GetNumPipes(), New.GetNumLeaks(), OldMs, OldResult.IterationsUsed, NewMs, NewResult.IterationsUsed,
		LeakMs, LeakResult.IterationsUsed, LeakResult.NormalizedResidual, LeakResult.bUsedSparse));
	return true;
}
#endif
