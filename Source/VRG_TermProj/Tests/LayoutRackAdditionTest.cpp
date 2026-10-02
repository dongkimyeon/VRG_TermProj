#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackAdditionTest, "VRG.Interaction.Layout.Gb200RackAddition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackAdditionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
	auto* Air = World->SpawnActor<AAirFieldDebugActor>();
	Sim->bShowSummary = false;
	auto* Existing = World->SpawnActor<ARackThermalActor>();
	Existing->ApplyGb200Nvl72Preset();
	Existing->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	Existing->bShowReadout = false;
	Room->RebuildRoomNow();
	auto Count = [&]() { int32 N = 0; for (TActorIterator<ARackThermalActor> It(World); It; ++It) ++N; return N; };
	const FVector Down(0, 0, -1);
	auto Preview = [&](const FVector& P) { Controller->UpdateRackAdditionAlongRay(P + FVector(0, 0, 500), Down); };
	Sim->EnterLayoutMode();
	const int32 InitialPipes = Room->GetMutableNetwork().GetNumPipes();
	Controller->BeginRackAddition(); Preview(FVector(2, 83, 0));
	FTransform Proposed; FVector SizeM;
	TestTrue(TEXT("Value-only preview"), Controller->GetRackAdditionPreview(Proposed, SizeM));
	const double SnapErrorCm = FVector::Distance(Proposed.GetLocation(), FVector(0, 85, 0));
	TestTrue(TEXT("Five cm snap"), SnapErrorCm < 1e-9);
	TestEqual(TEXT("No preview rack"), Count(), 1);
	TestEqual(TEXT("No preview pipes"), Room->GetMutableNetwork().GetNumPipes(), InitialPipes);
	Controller->OnResetSim(); Controller->GetRackAdditionPreview(Proposed, SizeM);
	TestTrue(TEXT("R rotates preview only"), FMath::Abs(Proposed.Rotator().Yaw - 90.0) < 1e-10 && Existing->GetActorRotation().IsZero());
	Controller->CancelRackAddition();
	TestNull(TEXT("Cancel cannot confirm"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Cancel rack count unchanged"), Count(), 1);
	Controller->BeginRackAddition(); Preview(FVector::ZeroVector);
	TestTrue(TEXT("Collision red/rejection condition"), Controller->IsRackAdditionBlocked());
	TestNull(TEXT("Collision cannot spawn"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Collision rack count unchanged"), Count(), 1);
	TestEqual(TEXT("Collision pipes unchanged"), Room->GetMutableNetwork().GetNumPipes(), InitialPipes);
	// Total clearance is 1 cm, not 1 cm per box. Use unsnapped helper at its exact boundary.
	TestTrue(TEXT("Less than total one cm clearance blocks"), Controller->IsLayoutBoxBlocked(World, SizeM, FTransform(FVector(0, 60.99, 0))));
	TestFalse(TEXT("Total one cm clearance accepted"), Controller->IsLayoutBoxBlocked(World, SizeM, FTransform(FVector(0, 61, 0))));
	Controller->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels addition"), Controller->IsAddingRack());
	Controller->SetPatchMapOpen(false);
	Controller->BeginRackAddition(); Preview(FVector(0, 85, 0));
	Sim->EnterSimulationMode(); Controller->RefreshInteractionInputMode();
	TestFalse(TEXT("Simulation cancels addition"), Controller->IsAddingRack());
	TestNull(TEXT("Simulation cannot add"), Controller->ConfirmRackAddition());
	// Tick takes float: .02f is slightly below the double fixed step .02 s and may perform zero steps.
	Sim->Tick(.021f);
	const double BeforeItW = Sim->GetSnapshot().TotalItHeatW;
	TestTrue(TEXT("Baseline contains a completed fixed step"), Sim->GetSnapshot().SimTimeS >= Sim->FixedStepS && BeforeItW > 0);
	Sim->EnterLayoutMode();
	Controller->BeginRackAddition(); Preview(FVector(0, 85, 0));
	auto* Added = Controller->ConfirmRackAddition();
	if (!TestNotNull(TEXT("Confirm creates rack"), Added)) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false; }
	TestEqual(TEXT("Exactly one additional rack"), Count(), 2);
	TestNull(TEXT("Repeated confirm cannot add"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Repeated confirm leaves two"), Count(), 2);
	TestTrue(TEXT("37-slot preset / GJ / external clock"), Added->Params.Slots.Num() == FRackThermalModel::MakeGb200Nvl72Params().Slots.Num()
		&& Added->Input.ComponentHeatW == FRackThermalModel::MakeGb200Nvl72HeatW() && Added->bUseGjModels && Added->bDrivenExternally);
	Added->Tick(.1f);
	TestEqual(TEXT("Layout does not advance rack clock"), Added->GetSnapshot().SimTimeS, 0.0);
	const auto Routed = Room->GetRoutedRacks();
	const auto Rows = Room->GetRoutedRackRows();
	const int32 OldIndex = Routed.IndexOfByKey(Existing), NewIndex = Routed.IndexOfByKey(Added);
	TestTrue(TEXT("Added rack is discovered in same row"), OldIndex != INDEX_NONE && NewIndex != INDEX_NONE && Rows[OldIndex] == Rows[NewIndex]);
	const auto& Branches = Room->GetRackBranchPipeIndices();
	TestTrue(TEXT("New hydraulic branch and CDU"), Branches.IsValidIndex(NewIndex) && Branches[NewIndex] != INDEX_NONE
		&& Branches[NewIndex] != Branches[OldIndex] && Room->GetRowCdu(Rows[NewIndex]) == Cdu && Room->GetMutableNetwork().GetNumPipes() > InitialPipes);
	Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double DeltaItW = Sim->GetSnapshot().TotalItHeatW - BeforeItW;
	FRackThermalModel Reference;
	Reference.Initialize(FRackThermalModel::MakeGb200Nvl72Params(), FRackThermalState());
	FRackThermalInput ReferenceInput;
	ReferenceInput.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	ReferenceInput.SledDocked.Init(true, Added->Params.Slots.Num());
	Reference.Step(ReferenceInput, Sim->FixedStepS);
	const double ExpectedW = Reference.GetLastTotalHeatW();
	const double PowerRelativeError = FMath::Abs(DeltaItW - ExpectedW) / ExpectedW;
	TestTrue(TEXT("Preset IT input increases by one rack"), PowerRelativeError < 1e-9);
	// Reuse Gb200Nvl72Test: NVIDIA ~120 kW, existing +/-5% plausibility envelope,
	// not a newly measured error threshold or a new physical default.
	TestTrue(TEXT("Full-load NVL72 input in existing NVIDIA +/-5% envelope"), DeltaItW > 1.14e5 && DeltaItW < 1.26e5);
	for (int32 Frame = 0; Frame < 20; ++Frame) Sim->Tick(.04f);
	const auto Snapshot = Sim->GetSnapshot();
	const auto AirSnapshot = Air->GetMutableSolver().MakeSnapshot(Snapshot.SimTimeS);
	TestEqual(TEXT("Both racks have air coupling"), Snapshot.RackAirMassFlowKgs.Num(), 2);
	TestTrue(TEXT("Air domain has not rejected ducts"), !Snapshot.bAirDuctsRejected && !AirSnapshot.bDuctsRejected);
	double MaxDuctError = 0;
	for (int32 I = 0; I < Snapshot.RackAirMassFlowKgs.Num(); ++I)
	{
		TestTrue(TEXT("Rack carries fan flow"), Snapshot.RackAirMassFlowKgs[I] > 0);
		MaxDuctError = FMath::Max(MaxDuctError, FMath::Abs(Snapshot.RackAirMassFlowKgs[I] - Snapshot.RackDuctMassFlowKgs[I]) / Snapshot.RackAirMassFlowKgs[I]);
	}
	TestTrue(TEXT("Every new rack duct carries its model flow"), MaxDuctError < 1e-9);
	TestTrue(TEXT("Global heat ledger existing criterion"), FMath::Abs(Snapshot.NormalizedEnergyResidual) < 1e-9);
	AddInfo(FString::Printf(TEXT("Addition: snap %.3e cm | pipes %d -> %d | IT delta %.6f W expected %.6f W rel %.3e | t %.2f s global residual %.3e | duct rel %.3e | divergence %.3e"),
		SnapErrorCm, InitialPipes, Room->GetMutableNetwork().GetNumPipes(), DeltaItW, ExpectedW, PowerRelativeError,
		Snapshot.SimTimeS, Snapshot.NormalizedEnergyResidual, MaxDuctError, AirSnapshot.NormalizedDivergence));
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}
#endif
