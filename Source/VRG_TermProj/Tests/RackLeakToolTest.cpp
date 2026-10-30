#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackLeakTool, "VRG.Hydraulics.LiveLeak.InternalTool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackLeakTool::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = false;
	auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(200, 150, 0), FRotator(0, 37, 0));
	Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false; Rack->LayoutId = TEXT("tool_rack"); Rack->SetActorScale3D(FVector(1.1, .9, 1.2));
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100, 150, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false; Sim->bFacilityPlant = false; Sim->MaxStepsPerFrame = 16;
	Room->RebuildRoomNow(); Sim->EnterSimulationMode();
	const auto* H = Room->GetInRackHandles(Rack);
	if (!TestNotNull(TEXT("native internal network"), H)) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false; }
	const auto Leaks = H->Leaks; const auto Pipes = H->Pipes;
	const int32 NodeCount = Room->GetNetwork().GetNumNodes(), PipeCount = Room->GetNetwork().GetNumPipes(), LeakCount = Room->GetNetwork().GetNumLeaks();
	int32 InternalCount = 0; for (const auto& S : Sim->GetLiveLeakSites()) InternalCount += S.InternalSite != INDEX_NONE;
	TestEqual(TEXT("27 trays x five addressable sites"), InternalCount, 135);
	auto EyeAt = [&](double X, double Z) { return SimUnits::UnrealToMeters(Rack->GetActorTransform().TransformPosition(FVector(X * 100, 0, Z * 100))); };
	for (int32 T = 0; T < H->Slots.Num(); ++T)
	{
		const auto& P = Rack->Params.Slots[H->Slots[T]]; const double Z = P.BottomZM + .5 * P.HeightM;
		for (int32 C = 0; C < 6; ++C)
			for (bool Rear : {false, true})
			{
				const int32 I = Sim->FindInternalLeakOnRay(EyeAt(Rear ? -2.5 : 2.5, Z), Rack->GetActorForwardVector() * (Rear ? 1 : -1), C);
				if (!TestTrue(TEXT("front/rear ray hits liquid tray under transformed rack"), Sim->GetLiveLeakSites().IsValidIndex(I))) continue;
				const auto& S = Sim->GetLiveLeakSites()[I]; const int32 Front[] = {2, 1, 3};
				TestTrue(TEXT("height and kind cycle preserve tray/slot"), S.Tray == T && S.Slot == H->Slots[T] && S.InternalSite == (Rear ? (C % 2 == 0 ? 0 : 4) : Front[C % 3]));
				TestTrue(TEXT("key and quoted display label resolve one exact site"), Sim->ResolveLiveLeakSite(S.Key) == I && Sim->ResolveLiveLeakSite(S.Label) == I);
			}
	}
	for (const auto& P : Rack->Params.Slots)
		if (!Rack->Params.DeviceTypes[P.DeviceType].bLiquidCooled)
			TestEqual(TEXT("air-cooled slots never select adjacent liquid tray"), Sim->FindInternalLeakOnRay(EyeAt(2.5, P.BottomZM + .5 * P.HeightM), -Rack->GetActorForwardVector(), 0), INDEX_NONE);
	TestEqual(TEXT("parallel/side ray has no internal target"), Sim->FindInternalLeakOnRay(EyeAt(2.5, 1), Rack->GetActorRightVector(), 0), INDEX_NONE);
	TestEqual(TEXT("invalid token cannot repair site zero"), Sim->ResolveLiveLeakSite(TEXT("bad")), INDEX_NONE);
	const int32 Target = Sim->ResolveLiveLeakSite(TEXT("rack:0:tray:0:plate"));
	TestTrue(TEXT("internal size presets"), Sim->GetLiveLeakToolHoleM(Target, 1) == .001 && Sim->GetLiveLeakToolHoleM(Target, 2) == .003
		&& Sim->GetLiveLeakToolHoleM(Target, 3) == InRackNetwork::HoseDiameterM && Sim->GetLiveLeakToolHoleM(Target, 4) == 0.0);
	auto Run = [&](int32 N) { for (int32 I = 0; I < N / 5; ++I) Sim->Tick(.1f); };
	// Actual console parser and named event, not an alternate test-only setter.
	TestTrue(TEXT("named At console recognized"), IConsoleManager::Get().ProcessUserConsoleInput(TEXT("vrg.Leak.At rack:0:tray:0:plate 3"), *GLog, World));
	Run(25);
	TestTrue(TEXT("named console opens solved internal leak"), Sim->GetLiveLeakHoleDiameterM(Target) == .003 && Sim->GetLiveLeakFlowM3s(Target) > 0.0);
	IConsoleManager::Get().ProcessUserConsoleInput(TEXT("vrg.Leak.Repair rack:0:tray:0:plate"), *GLog, World);
	Run(5); TestEqual(TEXT("repair re-solves to zero flow"), Sim->GetLiveLeakFlowM3s(Target), 0.0);
	TestTrue(TEXT("no topology/handle renumbering"), NodeCount == Room->GetNetwork().GetNumNodes() && PipeCount == Room->GetNetwork().GetNumPipes()
		&& LeakCount == Room->GetNetwork().GetNumLeaks() && H->Leaks == Leaks && H->Pipes == Pipes);
	TestEqual(TEXT("no interior W2 spray"), Sim->GetLeakSpray().GetTotals().EmittedM3, 0.0);
	const double WaterBefore = Sim->GetRackInteriorCoupling().GetWater().ReceivedM3;
	Sim->RebuildLiveLeakSites();
	TestTrue(TEXT("listing refresh preserves water and exact target identity"), WaterBefore == Sim->GetRackInteriorCoupling().GetWater().ReceivedM3
		&& Sim->ResolveLiveLeakSite(TEXT("rack:0:tray:0:plate")) == Target);
	Sim->EnterLayoutMode();
	ADataCenterSimulationActor::FEvalEvent E; E.Kind = TEXT("leakat"); E.TimeS = .1; E.Value = .001; E.Args = {TEXT("rack:0:tray:0:plate")}; Sim->EvalEvents.Add(E);
	Sim->EnterSimulationMode(); Run(15);
	TestTrue(TEXT("named event uses shared clock and same mutation route"), Sim->GetFaultMonitor().GetLeaks().Num() == 1
		&& FMath::Abs(Sim->GetFaultMonitor().GetLeaks()[0].TimeS - .1) < Sim->FixedStepS && Sim->GetLiveLeakHoleDiameterM(Target) == .001);
	AddInfo(FString::Printf(TEXT("tool sites %d nodes %d pipes %d leaks %d; named event at %.12g s; global normalized %.12g"),
		InternalCount, NodeCount, PipeCount, LeakCount, Sim->GetFaultMonitor().GetLeaks().IsEmpty() ? -1.0 : Sim->GetFaultMonitor().GetLeaks()[0].TimeS, Sim->GetSnapshot().NormalizedEnergyResidual));
	TestTrue(TEXT("existing hydraulic/air convergence and energy bound"), Sim->GetSnapshot().bHydraulicsConverged && Sim->GetSnapshot().bAirPressureConverged && FMath::Abs(Sim->GetSnapshot().NormalizedEnergyResidual) < 1e-9);
	Sim->EnterLayoutMode(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return true;
}
#endif
