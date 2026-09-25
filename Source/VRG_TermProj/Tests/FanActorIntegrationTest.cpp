// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// The integrated actor builds fan faces from real rack actors (rotated 0 and 180 degrees, as in the two
// L_Test rows), every slot's fan flow passes through the air grid in the rack's front-to-rear direction,
// and the global ledger closes. Headless (no rendering).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFanActorIntegrationTest, "VRG.Coupling.FanDuctsFromRackActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FFanActorIntegrationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;

	// Row A looks +X, row B looks -X: cold aisle between them (x = 0), rears outward.
	ARackThermalActor* A = World->SpawnActor<ARackThermalActor>(FVector(-150, 0, 0), FRotator(0, 0, 0));
	ARackThermalActor* B = World->SpawnActor<ARackThermalActor>(FVector(150, 0, 0), FRotator(0, 180, 0));
	for (ARackThermalActor* Rack : {A, B})
	{
		Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
		Rack->Input = FRackThermalInput();
		Rack->RackSizeM = FVector(1.068, .6, 2.236);
		Rack->bShowReadout = false;
		Rack->ResetSim(); Rack->PauseSim();
	}
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->FitToRacks();
	Air->ResetSim();
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->RebuildCoupling();
	Sim->StartSim();
	for (int32 Frame = 0; Frame < 50; ++Frame) Sim->Tick(.32f);  // 50 x 16 x 0.02 s = 16 s

	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	const FAirSolverSnapshot AirSnap = Air->GetMutableSolver().MakeSnapshot(Snap.SimTimeS);
	TestEqual(TEXT("Both racks coupled"), Snap.RackAirMassFlowKgs.Num(), 2);
	if (Snap.RackAirMassFlowKgs.Num() != 2) { World->DestroyWorld(false); return false; }

	double TotalKgs = 0.0;
	for (int32 R = 0; R < 2; ++R)
	{
		TotalKgs += Snap.RackAirMassFlowKgs[R];
		AddInfo(FString::Printf(TEXT("랙 %d: 팬 풍량 %.4f kg/s, 격자 관통 %.4f kg/s"), R, Snap.RackAirMassFlowKgs[R], Snap.RackDuctMassFlowKgs[R]));
		TestTrue(TEXT("Rack fans move air"), Snap.RackAirMassFlowKgs[R] > .5);
		TestTrue(TEXT("Every slot's flow passes through the grid"), FMath::Abs(Snap.RackDuctMassFlowKgs[R] - Snap.RackAirMassFlowKgs[R]) < 1e-9 * Snap.RackAirMassFlowKgs[R]);
	}
	const double Rho = Air->GetMutableSolver().GetGrid().GetParams().DensityKgM3;
	AddInfo(FString::Printf(TEXT("t=%.2f s | 덕트 면 %d개, 유량 %.4f m3/s (랙 합 %.4f m3/s) | 팬 %.3f kW | 전역 정규화 잔차 %.3e | 정규화 발산 %.3e | CFL %.3f"),
		Snap.SimTimeS, AirSnap.NumDuctFaces, AirSnap.DuctFlowM3s, TotalKgs / Rho, Snap.TotalFanPowerW / 1000.0,
		Snap.NormalizedEnergyResidual, AirSnap.NormalizedDivergence, AirSnap.CourantNumber));
	TestFalse(TEXT("Ducts accepted"), Snap.bAirDuctsRejected || AirSnap.bDuctsRejected);
	TestTrue(TEXT("Grid carries the racks' volume flow"), FMath::Abs(AirSnap.DuctFlowM3s - TotalKgs / Rho) < 1e-9 * AirSnap.DuctFlowM3s);
	TestTrue(TEXT("Global ledger closes"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("Divergence free"), AirSnap.NormalizedDivergence < 1e-5);

	// Direction: rack A (+X front) pushes air toward -X at its rear, rack B toward +X. Look at the grid
	// velocity of the fluid cells right behind each rack at mid height.
	const FUniformAirGrid& G = Air->GetMutableSolver().GetGrid();
	const FVector OriginM = Air->GetActorLocation() / 100.0;
	auto VelocityXBehind = [&](const ARackThermalActor* Rack, double RearOffsetM)
	{
		const FVector P = Rack->GetActorLocation() / 100.0 - Rack->GetActorForwardVector() * RearOffsetM + FVector(0, 0, 1.0) - OriginM;
		const int32 X = FMath::FloorToInt32(P.X / G.GetCellSizeM()), Y = FMath::FloorToInt32(P.Y / G.GetCellSizeM()), Z = FMath::FloorToInt32(P.Z / G.GetCellSizeM());
		if (!G.IsValidCell(X, Y, Z)) return 0.0;
		return .5 * (G.VelocityXMs[G.FaceXIndex(X, Y, Z)] + G.VelocityXMs[G.FaceXIndex(X + 1, Y, Z)]);
	};
	const double VA = VelocityXBehind(A, .7), VB = VelocityXBehind(B, .7);
	AddInfo(FString::Printf(TEXT("랙 뒤 1 m 높이 X 속도: A %.3f m/s (−X 기대) / B %.3f m/s (+X 기대)"), VA, VB));
	TestTrue(TEXT("Rack A exhausts toward -X"), VA < -.05);
	TestTrue(TEXT("Rack B exhausts toward +X"), VB > .05);

	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
