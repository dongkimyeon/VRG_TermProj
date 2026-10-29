#include "Misc/AutomationTest.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
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
#endif
