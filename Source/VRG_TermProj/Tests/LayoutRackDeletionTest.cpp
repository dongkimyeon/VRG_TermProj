#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackDeletionTest, "VRG.Interaction.Layout.SingleRackDeletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackDeletionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	Room->bUseGjConnections = true;
	auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
	auto* Air = World->SpawnActor<AAirFieldDebugActor>();
	auto* Existing = World->SpawnActor<ARackThermalActor>();
	Existing->ApplyGb200Nvl72Preset();
	Existing->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	Existing->bUseGjModels = true; // Match L_Test: only opted-in GJ racks participate in the display patch plan.
	Existing->bShowReadout = false;
	Sim->bShowSummary = false;
	Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Select = [&](AActor* Actor, bool bToggle = false) {
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), Down, bToggle);
	};
	auto Count = [&]() { int32 N = 0; for (TActorIterator<ARackThermalActor> It(World); It; ++It) ++N; return N; };
	auto Reject = [&](const TCHAR* Label) {
		const int32 Before = Count(); const int32 Pipes = Room->GetMutableNetwork().GetNumPipes();
		const double TimeS = Sim->GetSnapshot().SimTimeS;
		TestFalse(Label, Controller->TryDeleteLayoutRack());
		TestTrue(TEXT("Rejected deletion leaves count/pipes/time unchanged"), Count() == Before
			&& Room->GetMutableNetwork().GetNumPipes() == Pipes && Sim->GetSnapshot().SimTimeS == TimeS);
	};
	Reject(TEXT("No selection rejects deletion"));
	Select(Cdu); Reject(TEXT("Other equipment rejects deletion"));
	Select(Existing);
	Controller->BeginLayoutDragAlongRay(FVector(0, 0, 500), Down);
	Reject(TEXT("Equipment drag rejects deletion")); Controller->EndLayoutDrag(false);
	Controller->BeginLayoutMarquee(FVector2D(10, 10), false);
	Reject(TEXT("Marquee rejects deletion")); Controller->EndLayoutMarquee(false);
	Controller->BeginRackAddition();
	Reject(TEXT("Addition preview rejects deletion")); Controller->CancelRackAddition();
	Controller->SetPatchMapOpen(true);
	Reject(TEXT("Patch window rejects deletion")); Controller->SetPatchMapOpen(false);
	Controller->BeginRackAddition();
	Controller->UpdateRackAdditionAlongRay(FVector(0, 85, 500), Down);
	auto* Added = Controller->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Runtime added rack"), Added)) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false; }
	const FString AddedName = Added->GetActorNameOrLabel();
	Select(Cdu, true);
	Reject(TEXT("Mixed selection rejects deletion")); Select(Added);
	Sim->EnterSimulationMode();
	Reject(TEXT("Simulation rejects deletion"));
	Sim->Tick(.021f); // float Tick must exceed the double .02 s step, as established by the addition test.
	const auto Before = Sim->GetSnapshot();
	TestTrue(TEXT("Two-rack baseline has integrated"), Before.RackAirMassFlowKgs.Num() == 2 && Before.TotalItHeatW > 0 && Before.SimTimeS >= Sim->FixedStepS);
	Sim->EnterLayoutMode(); Select(Added);
	const int32 BeforePipes = Room->GetMutableNetwork().GetNumPipes();
	TestEqual(TEXT("Patch discovers both racks before deletion"), Room->GetPatchDevices().Num(), 2);
	TestTrue(TEXT("Delete runtime rack"), Controller->TryDeleteLayoutRack());
	TestEqual(TEXT("Exactly one rack remains"), Count(), 1);
	TestTrue(TEXT("Selection and bridge references cleared without advancing time"), Controller->GetSelectedLayoutActors().IsEmpty()
		&& Sim->GetSnapshot().RackAirMassFlowKgs.IsEmpty() && Sim->GetSnapshot().SimTimeS == Before.SimTimeS);
	TestFalse(TEXT("Repeated Delete does not delete another rack"), Controller->TryDeleteLayoutRack());
	const auto Routed = Room->GetRoutedRacks();
	TestTrue(TEXT("Deleted rack removed from routing"), Routed.Num() == 1 && Routed[0] == Existing);
	TestTrue(TEXT("One branch and existing CDU remain"), Room->GetRackBranchPipeIndices().Num() == 1
		&& Room->GetRackBranchPipeIndices()[0] != INDEX_NONE && Room->GetRowCdu(0) == Cdu
		&& Room->GetMutableNetwork().GetNumPipes() < BeforePipes);
	bool bDeletedInPatch = false;
	for (const auto& Device : Room->GetPatchDevices()) bDeletedInPatch |= Device.Label == AddedName;
	TestTrue(TEXT("Deleted device removed from patch"), !bDeletedInPatch && Room->GetPatchDevices().Num() == 1);
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double RemovedW = Before.TotalItHeatW - Sim->GetSnapshot().TotalItHeatW;
	FRackThermalModel Reference;
	Reference.Initialize(FRackThermalModel::MakeGb200Nvl72Params(), FRackThermalState());
	FRackThermalInput ReferenceInput;
	ReferenceInput.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	ReferenceInput.SledDocked.Init(true, Existing->Params.Slots.Num());
	Reference.Step(ReferenceInput, Sim->FixedStepS);
	const double ExpectedW = Reference.GetLastTotalHeatW();
	const double PowerError = FMath::Abs(RemovedW - ExpectedW) / ExpectedW;
	TestTrue(TEXT("IT power decreases by one identical preset at the same initial step"), PowerError < 1e-9);
	// Existing Gb200Nvl72Test plausibility envelope around NVIDIA ~120 kW, not a new physical estimate.
	TestTrue(TEXT("Removed full-load input satisfies existing +/-5% range"), RemovedW > 1.14e5 && RemovedW < 1.26e5);
	for (int32 Frame = 0; Frame < 20; ++Frame) Sim->Tick(.04f);
	const auto After = Sim->GetSnapshot();
	const auto AirSnapshot = Air->GetMutableSolver().MakeSnapshot(After.SimTimeS);
	TestTrue(TEXT("Only surviving rack remains in coolant/heat/air coupling"), After.RackCoolantMassFlowKgs.Num() == 1
		&& After.RackHeatToAirW.Num() == 1 && After.RackAirMassFlowKgs.Num() == 1 && After.RackDuctMassFlowKgs.Num() == 1);
	double DuctError = 1;
	if (After.RackAirMassFlowKgs.Num() == 1 && After.RackDuctMassFlowKgs.Num() == 1 && After.RackAirMassFlowKgs[0] > 0)
		DuctError = FMath::Abs(After.RackAirMassFlowKgs[0] - After.RackDuctMassFlowKgs[0]) / After.RackAirMassFlowKgs[0];
	TestTrue(TEXT("Remaining duct flow matches the rack model"), !After.bAirDuctsRejected && DuctError < 1e-9);
	TestTrue(TEXT("Global thermal ledger satisfies existing criterion"), FMath::Abs(After.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("Air pressure satisfies existing divergence criterion"), After.bAirPressureConverged && AirSnapshot.NormalizedDivergence < 1e-5);
	AddInfo(FString::Printf(TEXT("Deletion: pipes %d -> %d | removed IT %.6f W expected %.6f W relative %.3e | t %.2f s global residual %.3e | duct relative %.3e | divergence %.3e"),
		BeforePipes, Room->GetMutableNetwork().GetNumPipes(), RemovedW, ExpectedW, PowerError, After.SimTimeS,
		After.NormalizedEnergyResidual, DuctError, AirSnapshot.NormalizedDivergence));
	Sim->EnterLayoutMode(); Select(Existing);
	TestTrue(TEXT("Delete pre-existing final rack"), Controller->TryDeleteLayoutRack());
	TestTrue(TEXT("Last rack clears rows/branches/patch"), Count() == 0 && Room->GetRoutedRacks().IsEmpty()
		&& Room->GetRackBranchPipeIndices().IsEmpty() && Room->GetMutableNetwork().GetNumPipes() == 0 && Room->GetPatchDevices().IsEmpty());
	Sim->EnterSimulationMode(); Sim->Tick(.021f); Sim->Tick(.021f);
	const auto Empty = Sim->GetSnapshot();
	TestTrue(TEXT("Empty layout does not retain old heat/targets or repeatedly reset its clock"), Empty.TotalItHeatW == 0
		&& Empty.RackAirMassFlowKgs.IsEmpty() && Empty.SimTimeS >= 2 * Sim->FixedStepS);
	Sim->EnterLayoutMode();
	auto* Spine = World->SpawnActor<ASpineCabinetActor>(FVector(0, 0, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	TestTrue(TEXT("Zero racks still retains the remaining spine heat/air target"), IsValid(Spine)
		&& Sim->GetSnapshot().RackAirMassFlowKgs.Num() == 1 && Sim->GetSnapshot().TotalItHeatW > 0);
	AddInfo(FString::Printf(TEXT("Last rack: empty IT %.1f W t %.2f s | spine-only IT %.6f W"), Empty.TotalItHeatW, Empty.SimTimeS, Sim->GetSnapshot().TotalItHeatW));
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}
#endif
