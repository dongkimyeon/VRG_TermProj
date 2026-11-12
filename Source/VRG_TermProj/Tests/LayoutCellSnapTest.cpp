// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/FacilityPlantDisplayActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interaction/DataCenterDebugController.h"
#include "Visualization/GjRackLayout.h"
#include "Visualization/PlantDisplay.h"

#if WITH_DEV_AUTOMATION_TESTS

// 2026-11-12 (user request "셀에 맞춰서 딱딱"): layout footprints snap their min X / Y corner to the air grid's cell
// lines (any 90° yaw), the auto-fitted grid origin sits on the world cell lattice so those lines are the air cells,
// and the old 5 cm centre snap is used when cell snap is off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCellSnapTest, "VRG.Interaction.Layout.CellSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCellSnapTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterDebugController* C = World->SpawnActor<ADataCenterDebugController>();
	const FVector RackSizeM(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	const FVector Raw(123.4, -56.7, 0.0);
	TestTrue(TEXT("no air grid: cell snap unavailable, location unchanged"), !C->UseCellSnap()
		&& C->SnapFootprintToCellsCm(Raw, FQuat::Identity, RackSizeM) == Raw);

	// An air grid whose origin is off the world lattice: the lines run through its origin with the cell size as step.
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>(FVector(37.0, -12.0, 0.0), FRotator::ZeroRotator);
	Air->bShowSummary = false;
	Air->GridParams.CellSizeM = 0.25;
	double StepCm = 0.0; FVector2D OriginCm;
	TestTrue(TEXT("lattice = air grid origin and cell size"), C->GetAirCellLatticeCm(StepCm, OriginCm) && StepCm == 25.0
		&& OriginCm.Equals(FVector2D(37.0, -12.0), 1e-9) && C->UseCellSnap());
	auto OnLine = [&](double Cm, double LineCm) { const double K = (Cm - LineCm) / StepCm; return FMath::Abs(K - FMath::RoundToDouble(K)) < 1e-9; };
	double WorstMoveCm = 0.0;
	bool bCorners = true;
	for (const double Yaw : { 0.0, 90.0, 180.0, 270.0 })
		for (const FVector& P : { Raw, FVector(-310.2, 77.7, 5.0), FVector(37.0 + 12.4, -12.0 - 12.6, 0.0) })
		{
			const FQuat Q = FRotator(0.0, Yaw, 0.0).Quaternion();
			const FVector S = C->SnapFootprintToCellsCm(P, Q, RackSizeM);
			const bool bTurned = FMath::Abs(FMath::Sin(FMath::DegreesToRadians(Yaw))) > .5;
			const double HalfX = 50.0 * (bTurned ? RackSizeM.Y : RackSizeM.X), HalfY = 50.0 * (bTurned ? RackSizeM.X : RackSizeM.Y);
			bCorners &= OnLine(S.X - HalfX, OriginCm.X) && OnLine(S.Y - HalfY, OriginCm.Y) && S.Z == P.Z;
			WorstMoveCm = FMath::Max(WorstMoveCm, FMath::Max(FMath::Abs(S.X - P.X), FMath::Abs(S.Y - P.Y)));
		}
	TestTrue(TEXT("footprint min corner on cell lines at 0/90/180/270 deg, height kept"), bCorners);
	TestTrue(TEXT("nearest line: moves at most half a cell"), WorstMoveCm <= .5 * StepCm + 1e-9);
	const FVector Once = C->SnapFootprintToCellsCm(Raw, FQuat::Identity, RackSizeM);
	TestTrue(TEXT("idempotent"), C->SnapFootprintToCellsCm(Once, FQuat::Identity, RackSizeM).Equals(Once, 1e-9));
	C->bSnapToAirCells = false;
	TestTrue(TEXT("off: location unchanged (legacy PlaceSnapM path)"), !C->UseCellSnap() && C->SnapFootprintToCellsCm(Raw, FQuat::Identity, RackSizeM) == Raw);
	C->bSnapToAirCells = true;

	// Auto fit: the grid origin lands on the world lattice of the cell size and still covers the racks.
	ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(213.0, -87.0, 0.0), FRotator::ZeroRotator);
	Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false; Rack->RackSizeM = RackSizeM;
	Air->FitToRacks();
	const FBox D = Air->GetDomainBoundsM();
	const auto OnWorldLattice = [](double M) { const double K = M / .25; return FMath::Abs(K - FMath::RoundToDouble(K)) < 1e-9; };
	TestTrue(TEXT("fitted origin on the world 0.25 m lattice"), OnWorldLattice(D.Min.X) && OnWorldLattice(D.Min.Y));
	const FVector RackM = Rack->GetActorLocation() / 100.0;
	TestTrue(TEXT("fitted domain still covers the rack"), D.Min.X < RackM.X - .5 * RackSizeM.X && D.Max.X > RackM.X + .5 * RackSizeM.X
		&& D.Min.Y < RackM.Y - .5 * RackSizeM.Y && D.Max.Y > RackM.Y + .5 * RackSizeM.Y);
	// A snapped rack's edge now coincides with an air cell face.
	C->GetAirCellLatticeCm(StepCm, OriginCm);
	const FVector Snapped = C->SnapFootprintToCellsCm(Rack->GetActorLocation(), FQuat::Identity, RackSizeM);
	const double CellIndexX = ((Snapped.X - 50.0 * RackSizeM.X) / 100.0 - D.Min.X) / .25;
	TestTrue(TEXT("snapped rack edge = an air cell face"), FMath::Abs(CellIndexX - FMath::RoundToDouble(CellIndexX)) < 1e-9);
	AddInfo(FString::Printf(TEXT("lattice step %.1f cm, worst move %.2f cm, fitted origin (%.3f, %.3f) m"), StepCm, WorstMoveCm, D.Min.X, D.Min.Y));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// 2026-11-12 (user request "웹 페이지에서 주변 설비까지"): the plan view carries the plant yard display's units and pipes
// (copied, not recomputed) and the JSON round-trips; reports keep reading without the field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebPlantYardLayoutTest, "VRG.Web.PlantYardLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebPlantYardLayoutTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	TestFalse(TEXT("no yard display: no plant field"), Sim->BuildDashboardLayout().bHasPlant);

	AFacilityPlantDisplayActor* Yard = World->SpawnActor<AFacilityPlantDisplayActor>();
	FPlantDisplaySnapshot S;
	S.bLive = true;
	FPlantDisplayEquipment& Dc = S.Equipment.AddDefaulted_GetRef();
	Dc.Loop = EPlantLoopId::Fws; Dc.Kind = EPlantEquipmentKind::DryCooler; Dc.Index = 2;
	Dc.CenterM = FVector(20.0, 3.0, 1.2); Dc.SizeM = FVector(6.0, 2.3, 2.4); Dc.Rotation = FRotator(0, 90, 0).Quaternion();
	Dc.bPlaced = true; Dc.bRunning = true;
	FPlantDisplayEquipment& Ch = S.Equipment.AddDefaulted_GetRef();
	Ch.Loop = EPlantLoopId::Chw; Ch.Kind = EPlantEquipmentKind::Chiller; Ch.CenterM = FVector(24.0, -2.0, 1.0); Ch.bFailed = true;
	FPlantDisplayPipe& P = S.Pipes.AddDefaulted_GetRef();
	P.Loop = EPlantLoopId::Cw; P.FlowM3s = .012; P.TempK = 305.0; P.bClosed = false; P.PointsM = { FVector(18, 0, 3), FVector(18, 4, 3), FVector(22, 4, 3) };
	FPlantDisplayHeader& H = S.Headers.AddDefaulted_GetRef();
	H.Loop = EPlantLoopId::Fws; H.TempK = 300.0; H.PointsM = { FVector(16, -1, 3.5), FVector(16, 6, 3.5) };
	Yard->ShowSnapshot(S);

	const FVrgDashboardLayout L = Sim->BuildDashboardLayout();
	TestTrue(TEXT("plant copied: 2 units, pipe + header"), L.bHasPlant && L.bPlantLive && L.PlantUnits.Num() == 2 && L.PlantPipes.Num() == 2);
	if (L.PlantUnits.Num() == 2 && L.PlantPipes.Num() == 2)
	{
		const FVrgDashboardPlantUnit& U = L.PlantUnits[0];
		TestTrue(TEXT("unit fields = display snapshot"), U.Kind == TEXT("drycooler") && U.Loop == TEXT("fws") && U.Index == 2 && U.CenterM == Dc.CenterM
			&& U.SizeM == Dc.SizeM && FMath::IsNearlyEqual(U.YawDeg, 90.0, 1e-9) && U.bPlaced && U.bRunning && !U.bFailed);
		TestTrue(TEXT("failed chiller kept"), L.PlantUnits[1].Kind == TEXT("chiller") && L.PlantUnits[1].Loop == TEXT("chw") && L.PlantUnits[1].bFailed);
		TestTrue(TEXT("pipe = snapshot (flow, temperature, points), header thick"), L.PlantPipes[0].Loop == TEXT("cw") && L.PlantPipes[0].FlowM3s == .012
			&& L.PlantPipes[0].TempK == 305.0 && L.PlantPipes[0].PointsM == P.PointsM && !L.PlantPipes[0].bHeader
			&& L.PlantPipes[1].bHeader && L.PlantPipes[1].PointsM == H.PointsM);
	}
	FVrgDashboardLayout Back;
	TestTrue(TEXT("JSON round-trip parses"), FVrgDashboardLayout::FromJson(L.ToJson(), Back));
	TestTrue(TEXT("JSON round-trip exact"), Back.bHasPlant && Back.bPlantLive && Back.PlantUnits.Num() == 2 && Back.PlantPipes.Num() == 2
		&& Back.PlantUnits[0].CenterM == Dc.CenterM && Back.PlantUnits[1].bFailed && Back.PlantPipes[0].PointsM == P.PointsM && Back.PlantPipes[0].FlowM3s == .012);
	// Structure only (vrg.Plant.Enable 0): no flow / temperature passed on.
	S.bLive = false;
	Yard->ShowSnapshot(S);
	const FVrgDashboardLayout Off = Sim->BuildDashboardLayout();
	TestTrue(TEXT("structure only: no flow or temperature"), Off.bHasPlant && !Off.bPlantLive && Off.PlantPipes.Num() == 2
		&& Off.PlantPipes[0].FlowM3s == 0.0 && Off.PlantPipes[0].TempK == 0.0);
	FVrgDashboardLayout Old;
	TestTrue(TEXT("older JSON without the plant field still parses"), FVrgDashboardLayout::FromJson(
		TEXT("{\"simTimeS\":1,\"equipment\":[],\"inletWindowMeanK\":[],\"recommendedInletK\":300}"), Old) && !Old.bHasPlant);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
