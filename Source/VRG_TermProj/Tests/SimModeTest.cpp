// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// Layout / simulation modes (2026-09-27): nothing integrates in layout mode, entering simulation builds the coupling
// from the layout as it is at that moment and starts at t = 0, and a rack moved in layout mode is where the next
// simulation puts its solid cells. Placement helpers: pick any equipment box, floor point snapping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimModeTest, "VRG.Interaction.SimMode.LayoutThenSimulate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSimModeTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	ARackThermalActor* A = World->SpawnActor<ARackThermalActor>(FVector(-150, 0, 0), FRotator(0, 0, 0));
	ARackThermalActor* B = World->SpawnActor<ARackThermalActor>(FVector(150, 0, 0), FRotator(0, 180, 0));
	for (ARackThermalActor* Rack : {A, B})
	{
		Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
		Rack->Input = FRackThermalInput();
		Rack->RackSizeM = FVector(1.068, .6, 2.236);
		Rack->bShowReadout = false;
		Rack->ResetSim(); Rack->StartSim();  // as after BeginPlay: running on its own
	}
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->bShowSummary = false;
	Air->FitToRacks(); Air->ResetSim(); Air->StartSim();
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;

	// 1) Layout: ticking integrates nothing (integrated clock, rack models, air all hold still).
	Sim->EnterLayoutMode();
	const double RackT0 = A->GetSnapshot().SimTimeS;
	for (int32 F = 0; F < 10; ++F) { Sim->Tick(.32f); A->Tick(.32f); Air->Tick(.32f); }
	const bool bLayoutStill = Sim->GetSnapshot().SimTimeS == 0.0 && A->GetSnapshot().SimTimeS == RackT0
		&& Air->GetMutableSolver().GetEnergyInJ() == 0.0 && Sim->GetSimMode() == ESimMode::Layout;
	TestTrue(TEXT("Layout mode integrates nothing"), bLayoutStill);

	// 2) Move rack B 1 m along +Y while in layout, then simulate: the coupling and the solids follow the new layout.
	B->SetActorLocation(FVector(150, 100, 0));
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.32f);
	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	const FUniformAirGrid& G = Air->GetMutableSolver().GetGrid();
	auto SolidAt = [&](const FVector& PM)
	{
		const FVector L = PM - Air->GetActorLocation() / 100.0;
		const int32 X = FMath::FloorToInt32(L.X / G.GetCellSizeM()), Y = FMath::FloorToInt32(L.Y / G.GetCellSizeM()), Z = FMath::FloorToInt32(L.Z / G.GetCellSizeM());
		return G.IsValidCell(X, Y, Z) && G.IsSolidCell(X, Y, Z);
	};
	const bool bNewSolid = SolidAt(FVector(1.5, 1.0, 1.0)), bOldFree = !SolidAt(FVector(1.5, -.2, 1.0));
	AddInfo(FString::Printf(TEXT("시뮬레이션 진입 뒤 t=%.2f s, 랙 %d대, 전역 잔차 %.3e | 옮긴 랙 자리 고체 %d, 옛 자리 비움 %d | 격자 원점 %s"),
		Snap.SimTimeS, Snap.RackAirMassFlowKgs.Num(), Snap.NormalizedEnergyResidual, bNewSolid, bOldFree, *(Air->GetActorLocation() / 100.0).ToString()));
	TestTrue(TEXT("Simulation runs from t = 0"), Snap.SimTimeS > 0.0 && Snap.SimTimeS < 16 * 10 * .02 + 1e-9);
	TestEqual(TEXT("Both racks coupled"), Snap.RackAirMassFlowKgs.Num(), 2);
	TestTrue(TEXT("Moved rack is solid where it now stands"), bNewSolid);
	TestTrue(TEXT("Its old place is air"), bOldFree);
	TestTrue(TEXT("Global ledger closes"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);

	// 3) Back to layout stops the clock.
	Sim->EnterLayoutMode();
	const double TStop = Sim->GetSnapshot().SimTimeS;
	for (int32 F = 0; F < 5; ++F) Sim->Tick(.32f);
	TestTrue(TEXT("Layout stops the clock"), Sim->GetSnapshot().SimTimeS == TStop);

	// 4) Placement helpers: a CRAH unit is pickable like a rack; floor points snap to the grid step.
	ACrahUnitActor* Crah = World->SpawnActor<ACrahUnitActor>(FVector(0, 300, 0), FRotator::ZeroRotator);
	AActor* Picked = ADataCenterDebugController::PickPlaceableAlongRay(World, FVector(0, 0, 100), FVector(0, 1, 0), 1500.0);
	FVector P;
	const bool bFloor = ADataCenterDebugController::FloorPointSnapped(FVector(3, 7, 160), FVector(.6, 0, -.8).GetSafeNormal(), 0.0, .05, P);
	AddInfo(FString::Printf(TEXT("집기: %s | 바닥점 %s"), Picked ? *Picked->GetClass()->GetName() : TEXT("없음"), *P.ToString()));
	TestTrue(TEXT("CRAH unit is picked"), Picked == Crah);
	TestTrue(TEXT("Floor point on the plane and on the 5 cm grid"), bFloor && P.Z == 0.0
		&& FMath::IsNearlyEqual(FMath::Fmod(P.X, 5.0), 0.0, 1e-9) && FMath::IsNearlyEqual(FMath::Fmod(P.Y, 5.0), 0.0, 1e-9));
	TestFalse(TEXT("Looking up never hits the floor"), ADataCenterDebugController::FloorPointSnapped(FVector(0, 0, 160), FVector(1, 0, .2), 0.0, .05, P));

	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
