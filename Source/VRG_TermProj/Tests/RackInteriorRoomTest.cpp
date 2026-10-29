#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackInteriorRoom, "VRG.Water.RackInterior.RoomFloorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackInteriorRoom::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = false;
	auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(200, 150, 0), FRotator(0, 90, 0));
	Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false; Rack->SetActorScale3D(FVector(1.1, .9, 1.2));
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100, 150, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false; Sim->bFacilityPlant = false; Sim->MaxStepsPerFrame = 16;
	Room->RebuildRoomNow();
	TArray<double> TickHeld, TickTransit, TickRearTransit, TickPassed, TickExposure, TickDepth, TickComponent;
	double TickReceived = 0.0, TickFloor = 0.0, MaxInteriorResidual = 0.0, MaxFloorResidual = 0.0, MaxEnergy = 0.0;
	for (int32 Path = 0; Path < 2; ++Path)
	{
		Sim->bLeakSpray = Path == 0; // W2 off must not disable interior water or W3.
		Sim->EnterSimulationMode();
		if (!TestTrue(TEXT("valid transformed room enters simulation"), Sim->GetSimMode() == ESimMode::Simulation)) break;
		TestEqual(TEXT("new run clears received water"), Sim->GetRackInteriorCoupling().GetWater().ReceivedM3, 0.0);
		TestEqual(TEXT("new run clears floor stock"), Sim->GetFloorWater().GetTotals().ReceivedM3, 0.0);
		const auto H = *Room->GetInRackHandles(Rack);
		const FVector Front = Sim->GetRackWaterFloorPointsM(false)[0], Rear = Sim->GetRackWaterFloorPointsM(true)[0];
		TestTrue(TEXT("yaw and X scale put front/rear on expected world Y"), FMath::Abs(Front.X - 2.0) < 1e-10
			&& FMath::Abs(Front.Y - (1.5 + .5 * Rack->RackSizeM.X * 1.1)) < 1e-10
			&& FMath::Abs(Rear.Y - (1.5 - .5 * Rack->RackSizeM.X * 1.1)) < 1e-10);
		const int32 HighTray = 20, LowTray = 0;
		TestTrue(TEXT("open manifold and plate at first step"), Room->SetInternalLeakHoleDiameterM(Rack, HighTray, 0, .003)
			&& Room->SetInternalLeakHoleDiameterM(Rack, LowTray, 2, InRackNetwork::HoseDiameterM));
		auto Run = [&](int32 Steps)
		{
			if (Path == 0) for (int32 Frame = 0; Frame < Steps / 5; ++Frame) Sim->Tick(.1f);
			else
			{
				const double EndS = Sim->GetSnapshot().SimTimeS + Steps * Sim->FixedStepS;
				Sim->QueueWarmupSteps(Steps);
				for (int32 Frame = 0; Frame < 100 && Sim->GetSnapshot().SimTimeS < EndS - 1e-9; ++Frame) Sim->Tick(0.0f);
				TestTrue(TEXT("RunSteps covers the requested interval"), FMath::Abs(Sim->GetSnapshot().SimTimeS - EndS) < 1e-9);
			}
		};
		Run(100);
		const double ReceivedAtRepair = Sim->GetRackInteriorCoupling().GetWater().ReceivedM3;
		const double FloorAtRepair = Sim->GetFloorWater().GetTotals().ReceivedM3;
		const int32 SubstepsAtRepair = Sim->GetFloorWater().GetTotals().Substeps;
		Room->SetInternalLeakHoleDiameterM(Rack, HighTray, 0, 0.0); Room->SetInternalLeakHoleDiameterM(Rack, LowTray, 2, 0.0);
		Run(50);
		const auto& C = Sim->GetRackInteriorCoupling(); const auto& W = C.GetWater();
		const auto& Floor = Sim->GetFloorWater().GetTotals();
		TestEqual(TEXT("repair adds no new water"), W.ReceivedM3, ReceivedAtRepair);
		TestTrue(TEXT("retained water still reaches floor after repair"), Floor.ReceivedM3 > FloorAtRepair && Floor.Substeps > SubstepsAtRepair);
		TestEqual(TEXT("internal leaks never enter W2"), Sim->GetLeakSpray().GetTotals().EmittedM3, 0.0);
		TestTrue(TEXT("valid floor has no pending input beyond existing mass bound"), C.PendingInputM3(Sim->GetInternalLeakedM3()) < 1e-15);
		TestEqual(TEXT("valid floor accepts all bottom arrivals"), C.PendingFloorM3(), 0.0);
		double Delivered = 0.0, Stored = 0.0, Input = 0.0;
		for (double V : C.GetFloorDeliveredM3(false)) Delivered += V;
		for (double V : C.GetFloorDeliveredM3(true)) Delivered += V;
		for (double V : W.HeldM3) { Stored += V; TestTrue(TEXT("slot capacity plausibility"), V >= 0.0 && V <= W.CapacityM3); }
		for (double V : W.TransitM3) Stored += V;
		for (double V : W.GetRearTransitM3()) Stored += V;
		for (const auto& E : Sim->GetInternalLeakedM3()) Input += E.Value;
		const double Residual = Input - Stored - Delivered - C.PendingFloorM3() - C.PendingInputM3(Sim->GetInternalLeakedM3());
		MaxInteriorResidual = FMath::Max(MaxInteriorResidual, FMath::Abs(Residual));
		MaxFloorResidual = FMath::Max(MaxFloorResidual, Floor.Residual);
		MaxEnergy = FMath::Max(MaxEnergy, FMath::Abs(Sim->GetSnapshot().NormalizedEnergyResidual));
		TestTrue(TEXT("inventory -> interior -> W3 one transfer"), FMath::Abs(Floor.ReceivedM3 - Delivered) < 1e-15
			&& FMath::Abs(Room->GetCoolantInventory().GetTanks()[0].State.LeakedM3 - Input) < 1e-15);
		const auto& Params = Rack->GetMutableModel().GetParams();
		const auto& HighSlot = Params.Slots[H.Slots[HighTray]];
		const double HighM = HighSlot.BottomZM + .5 * HighSlot.HeightM;
		int32 Above = 0, AirWet = 0;
		for (int32 S = 0; S < Params.Slots.Num(); ++S)
		{
			const auto& Slot = Params.Slots[S];
			if (Slot.BottomZM + .5 * Slot.HeightM > HighM) { ++Above; TestEqual(TEXT("slots above sources remain dry"), W.FirstWetS[S], -1.0); }
			if (!Params.DeviceTypes[Slot.DeviceType].bLiquidCooled && W.FirstWetS[S] >= 0.0) ++AirWet;
		}
		TestTrue(TEXT("air-cooled slots included, upper dry slots exercised"), AirWet > 0 && Above > 0);
		TestTrue(TEXT("pressure and hydraulic convergence retained"), Sim->GetSnapshot().bAirPressureConverged && Sim->GetSnapshot().bHydraulicsConverged);
		for (double K : Sim->GetSnapshot().RackAirInletTempK) TestTrue(TEXT("existing inlet plausibility"), K >= 288.15 && K <= 308.15);
		if (Path == 0)
		{
			TickHeld = W.HeldM3; TickTransit = W.TransitM3; TickRearTransit = W.GetRearTransitM3(); TickPassed = W.PassedM3;
			TickExposure = W.RearExposureM3; TickDepth = Sim->GetFloorWater().GetDepthM(); TickReceived = W.ReceivedM3;
			TickFloor = Floor.ReceivedM3; TickComponent = Rack->GetMutableModel().GetState().ComponentTempK;
		}
		else
		{
			TestTrue(TEXT("Tick/RunSteps same slot water and exposure"), TickHeld == W.HeldM3 && TickTransit == W.TransitM3
				&& TickRearTransit == W.GetRearTransitM3() && TickPassed == W.PassedM3 && TickExposure == W.RearExposureM3);
			TestTrue(TEXT("Tick/RunSteps same W3 and heat states"), TickDepth == Sim->GetFloorWater().GetDepthM()
				&& TickComponent == Rack->GetMutableModel().GetState().ComponentTempK && TickReceived == W.ReceivedM3 && TickFloor == Floor.ReceivedM3);
		}
		AddInfo(FString::Printf(TEXT("actual path%d t=%.2fs input %.12g held+transit %.12g floor %.12g pending %.12g m3; interior residual %.12g m3 / input %.12g; floor residual %.12g; energy %.12g; air slots wet%d above dry%d"),
			Path, Sim->GetSnapshot().SimTimeS, Input, Stored, Delivered, C.PendingFloorM3(), Residual, FMath::Abs(Residual) / Input,
			Floor.Residual, Sim->GetSnapshot().NormalizedEnergyResidual, AirWet, Above));
		Sim->EnterLayoutMode();
	}
	TestTrue(TEXT("existing mass and heat criteria"), MaxInteriorResidual < 1e-15 && MaxFloorResidual < 1e-12 && MaxEnergy < 1e-9);
	Sim->EnterLayoutMode(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}
#endif
