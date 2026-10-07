#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/RackThermalActor.h"
#include "Components/StaticMeshComponent.h"
#include "Simulation/Core/SimUnits.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FShellFixture
{
	UWorld* World; ADataCenterSimulationActor* Sim; ADataCenterRoomActor* Room; AAirFieldDebugActor* Air;
	ARackThermalActor* Racks[2];
	FShellFixture()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
		Room = World->SpawnActor<ADataCenterRoomActor>(); Air = World->SpawnActor<AAirFieldDebugActor>();
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
		for (int32 I = 0; I < 2; ++I)
		{
			Racks[I] = World->SpawnActor<ARackThermalActor>(FVector(I == 0 ? -150 : 150, 0, 0), FRotator(0, I == 0 ? 0 : 180, 0));
			Racks[I]->ApplyGb200Nvl72Preset(); Racks[I]->bShowReadout = false;
		}
		Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	}
	~FShellFixture() { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
};

FBox PlannedBox(const AAirFieldDebugActor* Air)
{
	FAirGridParams G; FVector O;
	if (!Air->PlanFitToRacks(G, O)) return FBox(ForceInit);
	return FBox(O, O + FVector(G.NumCellsX, G.NumCellsY, G.NumCellsZ) * G.CellSizeM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomShellAutoTest, "VRG.Interaction.Layout.RoomShellAuto",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomShellAutoTest::RunTest(const FString& Parameters)
{
	FShellFixture F;
	const FAirGridParams GridBefore = F.Air->GridParams; const FVector OriginBefore = F.Air->GetActorLocation();
	F.Sim->Tick(.01f);
	FBox Shown = F.Sim->GetDisplayedRoomShellBoundsM();
	TestTrue(TEXT("Layout auto room: shell follows the planned auto-fit"), Shown.IsValid && Shown.Equals(PlannedBox(F.Air), 1e-12));
	TestEqual(TEXT("Four walls and a ceiling, all visible without a camera"), F.Sim->GetNumVisibleRoomShellPieces(), 5);
	TestTrue(TEXT("Planning the shell does not fit or move the live grid"), F.Air->GetActorLocation() == OriginBefore
		&& F.Air->GridParams.NumCellsX == GridBefore.NumCellsX && F.Air->GridParams.NumCellsY == GridBefore.NumCellsY
		&& F.Air->GridParams.NumCellsZ == GridBefore.NumCellsZ);

	// Every piece lies outside the domain (no air cell is covered) and touches its face.
	{
		TArray<UStaticMeshComponent*> Meshes; F.Sim->GetComponents(Meshes);
		int32 Outside = 0, Shell = 0;
		const FBox InnerCm(SimUnits::MetersToUnreal(Shown.Min) + FVector(.01), SimUnits::MetersToUnreal(Shown.Max) - FVector(.01));
		for (const UStaticMeshComponent* M : Meshes)
		{
			if (!M->IsRegistered() || M->GetStaticMesh() == nullptr) continue;
			const FBox B = M->Bounds.GetBox();
			if (!B.ExpandBy(FVector(.02)).Intersect(FBox(SimUnits::MetersToUnreal(Shown.Min), SimUnits::MetersToUnreal(Shown.Max)))) continue;
			++Shell;
			if (!B.Intersect(InnerCm)) ++Outside;
		}
		TestEqual(TEXT("All five pieces sit on the domain faces"), Shell, 5);
		TestEqual(TEXT("No piece overlaps the air domain interior"), Outside, 5);
	}

	// Moving a rack in layout mode moves the walls with the next auto-fit plan.
	F.Racks[1]->SetActorLocation(FVector(350, 0, 0));
	F.Sim->Tick(.01f);
	Shown = F.Sim->GetDisplayedRoomShellBoundsM();
	TestTrue(TEXT("Shell follows a moved rack"), Shown.Equals(PlannedBox(F.Air), 1e-12));
	TestTrue(TEXT("The +X wall moved outward by the rack shift (cell rounding allowed)"), Shown.Max.X - Shown.Min.X >= 2.0 - F.Air->GridParams.CellSizeM);

	// Manual room: the shell is the manual room's actual (cell-rounded) bounds.
	FRoomAirDomainSettings S; S.bManual = true;  // synthetic test geometry, not a facility dimension
	S.MinXYM = FVector2D(-5.5, -4.5); S.MaxXYM = FVector2D(5.1, 4.6); S.CeilingHeightM = 3.1;
	FString Reason;
	TestTrue(TEXT("Manual room accepted"), F.Sim->TrySetRoomAirDomain(S, Reason));
	F.Sim->Tick(.01f);
	FAirGridParams G; FBox Actual(ForceInit);
	TestTrue(TEXT("Manual plan"), F.Sim->ValidateRoomAirDomain(S, G, Actual, Reason));
	TestTrue(TEXT("Layout manual room: shell = actual manual bounds"), F.Sim->GetDisplayedRoomShellBoundsM().Equals(Actual, 1e-12)
		&& Actual.Min == FVector(-5.5, -4.5, 0) && Actual.Max == FVector(5.25, 4.75, 3.25));

	// Simulation: the live grid, and the shell adds no solid cells.
	F.Sim->EnterSimulationMode();
	const auto SolidOn = F.Air->GetMutableSolver().GetGrid().bSolidCell;
	F.Sim->Tick(.021f);
	TestTrue(TEXT("Simulation: shell = live domain"), F.Sim->GetDisplayedRoomShellBoundsM().Equals(F.Air->GetDomainBoundsM(), 1e-12));
	F.Sim->bAutoRoomShell = false; F.Sim->UpdateRoomShell();
	TestEqual(TEXT("Shell off removes every piece"), F.Sim->GetNumVisibleRoomShellPieces(), 0);
	TestTrue(TEXT("Shell on/off leaves the solid mask untouched"), F.Air->GetMutableSolver().GetGrid().bSolidCell == SolidOn);
	F.Sim->bAutoRoomShell = true; F.Sim->UpdateRoomShell();
	TestEqual(TEXT("Shell back on"), F.Sim->GetNumVisibleRoomShellPieces(), 5);

	// Cutaway rule.
	const FBox B(FVector(0, 0, 0), FVector(10, 8, 3));
	bool V[ADataCenterSimulationActor::NumRoomShellPieces];
	ADataCenterSimulationActor::ComputeRoomShellVisibility(B, FVector(5, 4, 1.5), true, V);
	TestTrue(TEXT("Camera inside: everything visible"), V[0] && V[1] && V[2] && V[3] && V[4]);
	ADataCenterSimulationActor::ComputeRoomShellVisibility(B, FVector(-6, 4, 8), true, V);
	TestTrue(TEXT("Camera outside -X and above: -X wall and ceiling cut away"), !V[0] && V[1] && V[2] && V[3] && !V[4]);
	ADataCenterSimulationActor::ComputeRoomShellVisibility(B, FVector(12, 11, 1), true, V);
	TestTrue(TEXT("Camera outside +X/+Y corner: those two walls cut away"), V[0] && !V[1] && V[2] && !V[3] && V[4]);
	ADataCenterSimulationActor::ComputeRoomShellVisibility(B, FVector(-6, -6, 8), false, V);
	TestTrue(TEXT("Cutaway off: everything visible"), V[0] && V[1] && V[2] && V[3] && V[4]);
	return true;
}
#endif
