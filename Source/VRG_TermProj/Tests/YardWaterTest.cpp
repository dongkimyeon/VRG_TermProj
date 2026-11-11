#include "Misc/AutomationTest.h"
#include "Simulation/Water/FloorWater.h"
#include "Simulation/Water/LeakSpray.h"
#include "Simulation/Water/WaterExposure.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// W4 follow-up (2026-11-11): the plant yard ground water and the yard pump flooding response.
#if WITH_DEV_AUTOMATION_TESTS
namespace VrgYardWaterTest
{
	constexpr EAutomationTestFlags YardTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// A 4 x 3 m room at the origin, one box inside, one pump box outside at +X.
	FSprayGeometry YardGeometry()
	{
		FSprayGeometry G;
		G.RoomM = FBox(FVector(0, 0, 0), FVector(4, 3, 3));
		G.GroundZM = 0.0;
		auto Add = [&G](const FVector& Base, const FVector& Size, const TCHAR* Name)
		{
			FSprayObstacle& O = G.Obstacles.AddDefaulted_GetRef();
			O.CenterM = Base + FVector(0, 0, .5 * Size.Z); O.HalfM = .5 * Size; O.Name = Name; O.Kind = ESprayObstacleKind::Equipment;
		};
		Add(FVector(2, 1.5, 0), FVector(1, .6, 2), TEXT("inside"));
		Add(FVector(6, 1.5, 0), FVector(.8, .5, .6), TEXT("yard pump"));
		return G;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgYardWaterGrid, "VRG.Water.Yard.Grid", VrgYardWaterTest::YardTestFlags)
bool FVrgYardWaterGrid::RunTest(const FString& Parameters)
{
	using namespace VrgYardWaterTest;
	const FSprayGeometry G = YardGeometry();
	FFloorWater Room, Yard;
	FFloorWaterSettings S; S.CellM = .05;
	Room.ResetForRoom(G, S);
	TestTrue(TEXT("yard grid built"), Yard.ResetForYard(G, FBox2D(FVector2D(-2, -2), FVector2D(9, 5)), S));
	TestEqual(TEXT("room floor records only the inside box"), Room.GetFloods().Num(), 1);
	TestEqual(TEXT("room flood source"), Room.GetFloods()[0].SourceObstacle, 0);
	TestEqual(TEXT("yard records only the outside box"), Yard.GetFloods().Num(), 1);
	TestEqual(TEXT("yard flood source"), Yard.GetFloods()[0].SourceObstacle, 1);
	TestEqual(TEXT("yard pump footprint cells (0.8 x 0.5 m / 0.05 m)"), Yard.GetFloods()[0].Cells.Num(), 16 * 10);
	int32 SolidInside = 0, OpenInside = 0;
	for (int32 J = 0; J < Yard.GetNy(); ++J)
		for (int32 I = 0; I < Yard.GetNx(); ++I)
		{
			const double X = Yard.GetOriginM().X + (I + .5) * S.CellM, Y = Yard.GetOriginM().Y + (J + .5) * S.CellM;
			if (X > 0 && X < 4 && Y > 0 && Y < 3) (Yard.GetSolid()[Yard.Index(I, J)] ? SolidInside : OpenInside)++;
		}
	TestTrue(TEXT("the building footprint is solid on the yard"), SolidInside > 0 && OpenInside == 0);
	TestEqual(TEXT("yard floor at the ground"), Yard.GetFloorZM(), 0.0);
	AddInfo(FString::Printf(TEXT("마당 격자 %d x %d, 건물 칸 %d 막힘, 펌프 발판 %d칸"), Yard.GetNx(), Yard.GetNy(), SolidInside, Yard.GetFloods()[0].Cells.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgYardWaterSpray, "VRG.Water.Yard.GroundDepositsSpread", VrgYardWaterTest::YardTestFlags)
bool FVrgYardWaterSpray::RunTest(const FString& Parameters)
{
	using namespace VrgYardWaterTest;
	const FSprayGeometry G = YardGeometry();
	FLeakSpray Spray; Spray.Reset(G);
	FFloorWater Yard; FFloorWaterSettings S; S.CellM = .05;
	Yard.ResetForYard(G, FBox2D(FVector2D(-2, -2), FVector2D(9, 5)), S);
	// A yard pipe 0.4 m up (below the 0.6 m pump top), 0.8 m from the pump face, jet towards it: 0.2 L/s for 2 s.
	FSpraySource Src; Src.Id = 1; Src.OriginM = FVector(7.2, 1.5, .4); Src.DirectionM = FVector(-1, 0, 0); Src.GaugePressurePa = 1.0e5;
	double OffGrid = 0.0, Now = 0.0;
	const double Dt = .02;
	for (int32 K = 0; K < 300; ++K)
	{
		Src.VolumeM3 = K < 100 ? 2e-4 * Dt : 0.0;
		TArray<FSpraySource> Sources; if (Src.VolumeM3 > 0.0) Sources.Add(Src);
		Spray.Step(Sources, Dt, Now);
		for (const TPair<FVector, double>& D : Spray.GetLastStepGroundDepositsM3()) if (!Yard.AddWater(D.Key, D.Value)) OffGrid += D.Value;
		if (Yard.HasWater()) Yard.Step(Dt, Now);
		Now += Dt;
	}
	const FFloorWaterTotals& Y = Yard.GetTotals();
	const double Ground = Spray.GetTotals().GroundM3;
	const double Rel = FMath::Abs(Y.ReceivedM3 + OffGrid - Ground) / Ground;
	TestTrue(TEXT("ground water reached the yard"), Ground > 1e-5);
	TestTrue(TEXT("yard received = spray ground deposits (no off-grid in this yard)"), Rel < 1e-12 && OffGrid == 0.0);
	TestTrue(TEXT("yard volume ledger"), Y.Residual < 1e-12);
	TestTrue(TEXT("it spreads over many cells"), Y.WetCells > 20);
	const FFloorFlood& Pump = Yard.GetFloods()[0];
	TestTrue(TEXT("the water reached the pump footprint"), Pump.FirstWetS >= 0.0 && Pump.PeakDepthM > 0.0);
	TestTrue(TEXT("the spray also hit the pump box"), Spray.GetWetting()[1].FirstWetS >= 0.0);
	AddInfo(FString::Printf(TEXT("분사 땅 %.4f L = 마당 받음 %.4f L (상대 %.2e), 마당 잔차 %.2e, 젖은 칸 %d, 펌프 발판 최고 %.2f mm(%.2f s), 분사 접촉 %.2f mL"),
		Ground * 1e3, Y.ReceivedM3 * 1e3, Rel, Y.Residual, Y.WetCells, Pump.PeakDepthM * 1e3, Pump.FirstWetS, Spray.GetWetting()[1].WaterM3 * 1e6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgYardWaterExposureIndex, "VRG.Water.Yard.ExposureIndex", VrgYardWaterTest::YardTestFlags)
bool FVrgYardWaterExposureIndex::RunTest(const FString& Parameters)
{
	using namespace VrgYardWaterTest;
	FSprayGeometry G = YardGeometry();
	TArray<FSprayWetting> Wet; Wet.SetNum(G.Obstacles.Num());
	FFloorFlood RoomFlood; RoomFlood.Name = TEXT("inside"); RoomFlood.SourceObstacle = 0; RoomFlood.MeanDepthM = .002;
	FFloorFlood YardFlood; YardFlood.Name = TEXT("yard pump"); YardFlood.SourceObstacle = 1; YardFlood.MeanDepthM = .03;
	const TArray<FFloorFlood> RoomFloods = { RoomFlood }, YardFloods = { YardFlood };
	TArray<FWaterExposureBinding> B;
	FWaterExposureBinding& P = B.AddDefaulted_GetRef(); P.EquipmentId = TEXT("plant/pump"); P.SourceName = TEXT("yard pump"); P.SprayObstacle = 1; P.FloorFlood = 1; P.FloorContactDepthM = 0.0;
	FRackInteriorWater Interior; FWaterExposureSnapshot Out; FString Error;
	TestTrue(TEXT("yard flood read past the room's floods"), WaterExposure::Collect(B, 1.0, G, Wet, RoomFloods, Interior, {}, Out, Error, &YardFloods));
	TestEqual(TEXT("immersion from the yard record"), Out.FloodImmersionM[0], .03);
	TestFalse(TEXT("without the yard list the index is rejected"), WaterExposure::Collect(B, 1.0, G, Wet, RoomFloods, Interior, {}, Out, Error));
	B[0].SourceName = TEXT("inside");
	TestFalse(TEXT("a name that is not the yard record is rejected"), WaterExposure::Collect(B, 1.0, G, Wet, RoomFloods, Interior, {}, Out, Error, &YardFloods));
	return true;
}

// Game path: plant on, two placed FWS pumps in the yard, a plant leak aimed at pump 0, vrg.Water.Impact on (short times).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgYardPumpFlooding, "VRG.Water.Yard.PumpFloodingTrips", VrgYardWaterTest::YardTestFlags)
bool FVrgYardPumpFlooding::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = false;
	for (const double Y : { 150.0, 450.0 })
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(200, Y, 0), FRotator(0, 90, 0));
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false; Rack->LayoutId = FString::Printf(TEXT("yard-%d"), int32(Y));
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70, 60, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false; Sim->bShowSpray = false; Sim->bFacilityPlant = true; Sim->MaxStepsPerFrame = 16;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	Room->RebuildRoomNow();
	auto Cleanup = [&]() { Sim->EnterLayoutMode(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	auto Run = [&](int32 Steps) { const double End = Sim->GetSnapshot().SimTimeS + Steps * Sim->FixedStepS; for (int32 I = 0; I < Steps + 4 && Sim->GetSnapshot().SimTimeS < End - 1e-9; ++I) Sim->Tick(float(Sim->FixedStepS)); };

	// First entry only to learn where the plant leak sites are; then the pumps are placed next to the nearest one.
	Sim->EnterSimulationMode();
	const FFacilityWaterLoop* Fws = Sim->GetFacilityPlant().GetLoopFor(EPlantLoopId::Fws);
	if (!TestTrue(TEXT("plant built with an FWS loop"), Fws != nullptr && Sim->GetFacilityPlant().IsThermalActive())) { Cleanup(); return false; }
	int32 Site = INDEX_NONE;
	for (int32 I = 0; I < Sim->GetLiveLeakSites().Num() && Site == INDEX_NONE; ++I)
		if (Sim->GetLiveLeakSites()[I].bPlant && Sim->GetLiveLeakSites()[I].Loop == EPlantLoopId::Fws) Site = I;
	if (!TestTrue(TEXT("an FWS leak site exists"), Site != INDEX_NONE)) { Cleanup(); return false; }
	const FVector SiteM = Sim->GetLiveLeakSites()[Site].PositionM;
	Sim->EnterLayoutMode();
	const FVector PumpSize = APlantEquipmentActor::DefaultSizeM(EPlantUnitKind::FwsPump);
	for (int32 K = 0; K < 2; ++K)
	{
		FPlantEquipmentRecord Rec;
		Rec.Id = FString::Printf(TEXT("yard-pump-%d"), K); Rec.Name = FName(*Rec.Id); Rec.Kind = EPlantUnitKind::FwsPump; Rec.SizeM = PumpSize;
		// Pump 0 1.2 m beside the site (same Y), pump 1 farther along Y; both on the ground.
		const FVector BaseM(SiteM.X + (K == 0 ? 1.2 : 1.2), SiteM.Y + (K == 0 ? 0.0 : 4.0), 0.0);
		Rec.Transform = FTransform(FRotator::ZeroRotator, BaseM * 100.0);
		TestNotNull(TEXT("pump placed"), VrgPlantEquipment::Spawn(World, Rec));
	}
	Sim->EnterSimulationMode();
	Fws = Sim->GetFacilityPlant().GetLoopFor(EPlantLoopId::Fws);
	const int32 PumpsBefore = Fws ? Fws->GetNumRunningPumps() : 0;
	GEngine->Exec(World, TEXT("vrg.Water.Impact 1 0.3 2"));
	// Re-find the site (same index after a rebuild with the same rows) and aim it at pump 0's middle.
	const FVector PumpCentreM(SiteM.X + 1.2, SiteM.Y, .5 * PumpSize.Z);
	TestTrue(TEXT("aim"), Sim->SetLiveLeakDirection(Site, PumpCentreM - Sim->GetLiveLeakSites()[Site].PositionM));
	TestTrue(TEXT("open the plant leak"), Sim->SetLiveLeak(Site, .02, TEXT("yard pump test")));
	Run(150);
	const FFloorWater& Yard = Sim->GetYardWater();
	TestTrue(TEXT("yard ground got water"), Yard.GetTotals().ReceivedM3 > 0.0);
	TestTrue(TEXT("yard volume ledger"), Yard.GetTotals().Residual < 1e-12);
	TestTrue(TEXT("spray ground = yard received + off the yard grid"),
		FMath::Abs(Sim->GetLeakSpray().GetTotals().GroundM3 - Yard.GetTotals().ReceivedM3 - Sim->GetYardOffGridM3()) <= 1e-12 * FMath::Max(Sim->GetLeakSpray().GetTotals().GroundM3, 1e-9));
	const TArray<ADataCenterSimulationActor::FWaterPlantPumpCut>& Cuts = Sim->GetWaterPlantPumpCuts();
	const ADataCenterSimulationActor::FWaterPlantPumpCut* Cut0 = Cuts.FindByPredicate([](const ADataCenterSimulationActor::FWaterPlantPumpCut& C) { return C.Pump == 0; });
	if (TestNotNull(TEXT("the flooded yard pump tripped its plant pump"), Cut0))
	{
		TestTrue(TEXT("FWS loop"), Cut0->Loop == EPlantLoopId::Fws);
		TestTrue(TEXT("applied at the next shared step"), Cut0->AppliedTimeS >= Cut0->TripTimeS && Cut0->AppliedTimeS - Cut0->TripTimeS <= Sim->FixedStepS + 1e-12);
		Fws = Sim->GetFacilityPlant().GetLoopFor(EPlantLoopId::Fws);
		TestTrue(TEXT("plant pump 0 is tripped"), Fws && Fws->IsPumpTripped(0));
		AddInfo(FString::Printf(TEXT("펌프 0 트립 판정 %.2f s, 적용 %.2f s, 운전 펌프 %d → %d, 마당 받음 %.3f L, 젖은 칸 %d, 마당 잔차 %.2e"),
			Cut0->TripTimeS, Cut0->AppliedTimeS, PumpsBefore, Fws ? Fws->GetNumRunningPumps() : -1, Yard.GetTotals().ReceivedM3 * 1e3, Yard.GetTotals().WetCells, Yard.GetTotals().Residual));
	}
	TestNull(TEXT("the dry pump keeps running"), Cuts.FindByPredicate([](const ADataCenterSimulationActor::FWaterPlantPumpCut& C) { return C.Pump == 1; }));
	TestTrue(TEXT("heat ledger"), FMath::Abs(Sim->GetSnapshot().NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("spray ledger"), Sim->GetLeakSpray().GetTotals().Residual < 1e-12);
	// R restart: the plant pump is back, the yard dry.
	Sim->EnterSimulationMode();
	Fws = Sim->GetFacilityPlant().GetLoopFor(EPlantLoopId::Fws);
	TestTrue(TEXT("restart clears the trip"), Fws && !Fws->IsPumpTripped(0) && Sim->GetWaterPlantPumpCuts().IsEmpty() && !Sim->GetYardWater().HasWater());
	Cleanup();
	return true;
}
#endif
