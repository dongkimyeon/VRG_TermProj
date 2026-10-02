#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackDuplicationPreviewTest, "VRG.Interaction.Layout.RackDuplicationPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackDuplicationPreviewTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>();
	auto* Source = World->SpawnActor<ARackThermalActor>(FVector(0, 0, 9), FRotator(3, 27, 2));
	Source->SetActorScale3D(FVector(.8, 1.2, 1));
	Source->bShowReadout = false;
	Sim->bShowSummary = false;
	Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Select = [&](AActor* Actor, bool bToggle = false) {
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), Down, bToggle);
	};
	auto Count = [&]() { int32 N = 0; for (TActorIterator<ARackThermalActor> It(World); It; ++It) ++N; return N; };
	auto Reject = [&](const TCHAR* Label) {
		const int32 Before = Count(), Pipes = Room->GetMutableNetwork().GetNumPipes();
		const double TimeS = Sim->GetSnapshot().SimTimeS;
		TestFalse(Label, Controller->BeginRackDuplication());
		TestTrue(TEXT("Rejection leaves count/pipes/time unchanged"), Count() == Before
			&& Room->GetMutableNetwork().GetNumPipes() == Pipes && Sim->GetSnapshot().SimTimeS == TimeS);
	};
	Reject(TEXT("No selection"));
	Select(Cdu); Reject(TEXT("Not a rack"));
	Select(Source);
	Controller->BeginLayoutDragAlongRay(Source->GetActorLocation() + FVector(0, 0, 500), Down);
	Reject(TEXT("Equipment drag")); Controller->EndLayoutDrag(false);
	Controller->BeginLayoutMarquee(FVector2D(10, 10), false);
	Reject(TEXT("Marquee")); Controller->EndLayoutMarquee(false);
	Controller->BeginRackAddition(); Reject(TEXT("Addition preview")); Controller->CancelRackAddition();
	Controller->SetPatchMapOpen(true); Reject(TEXT("Patch window")); Controller->SetPatchMapOpen(false);
	Select(Cdu, true); Reject(TEXT("Multiple selection")); Select(Source);
	Sim->EnterSimulationMode(); Reject(TEXT("Simulation mode")); Sim->EnterLayoutMode(); Select(Source);
	// Ctrl release before the next PlayerTick must not erase the D-press chord.
	auto Key = [&](const FKey& Code, EInputEvent Event) {
		FInputKeyEventArgs Args; Args.Key = Code; Args.Event = Event; Args.AmountDepressed = 1;
		Controller->InputKey(Args);
	};
	Key(EKeys::D, IE_Pressed); Key(EKeys::D, IE_Released);
	TestFalse(TEXT("Plain D does not duplicate"), Controller->IsAddingRack());
	Key(EKeys::LeftControl, IE_Pressed); Key(EKeys::D, IE_Pressed);
	Key(EKeys::D, IE_Released); Key(EKeys::LeftControl, IE_Released);
	TestTrue(TEXT("Fast Ctrl+D starts preview despite same-frame release"), Controller->IsAddingRack());
	Controller->CancelRackAddition();
	Key(EKeys::RightControl, IE_Pressed); Key(EKeys::D, IE_Repeat);
	TestFalse(TEXT("Repeat cannot start a duplicate"), Controller->IsAddingRack());
	Key(EKeys::D, IE_Pressed); Key(EKeys::D, IE_Released); Key(EKeys::RightControl, IE_Released);
	TestTrue(TEXT("Right Ctrl supported"), Controller->IsAddingRack()); Controller->CancelRackAddition();

	// Fixture settings, not new equipment defaults: a modified slot layout and non-default controls.
	Source->Params.Slots.Pop(); Source->ConformInputs();
	Source->Input.ComponentHeatW[0] *= 1.2;
	Source->Input.SledDocked[0] = false;
	Source->Input.Utilization = .62;
	Source->Input.SlotUtilization.Init(.48, Source->Params.Slots.Num());
	Source->Input.FanSpeedOverride = .55;
	Source->Radiator.UAWK *= .9;
	Source->PanelThicknessM *= .8;
	Source->bShowSledLabels = false;
	// Live boundary/state values must not leak into the placement configuration.
	Source->Input.CoolantInletTempK = 330;
	Source->Input.CoolantMassFlowKgs = .7;
	Source->Input.AirInletTempK = 325;
	Source->Input.SlotAirInletTempK.Init(326, Source->Params.Slots.Num());
	Source->InitialState.ComponentTempK.Init(340, FRackThermalModel::CountNodes(Source->Params));
	Source->ResetSim(); Source->bDrivenExternally = true; Source->SetExternalSimTimeS(42);
	Source->GetMutableModel().Step(Source->Input, Sim->FixedStepS);
	const auto Captured = Source->CaptureLayoutSettings();
	const auto OriginalTransform = Source->GetActorTransform();
	const auto OriginalState = Source->GetSnapshot();
	const double OriginalEnergyJ = Source->GetMutableModel().GetEnergyInJ();
	const int32 InitialPipes = Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Begin snapshot preview"), Controller->BeginRackDuplication());
	Reject(TEXT("Duplicate already in progress"));
	Controller->UpdateRackAdditionAlongRay(FVector(0, 0, 500), Down);
	TestTrue(TEXT("Original participates in overlap check"), Controller->IsRackAdditionBlocked());
	TestNull(TEXT("Overlapping duplicate rejected"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Collision leaves one rack"), Count(), 1);
	Controller->CancelRackAddition();
	TestNull(TEXT("Cancel cannot spawn"), Controller->ConfirmRackAddition());
	TestEqual(TEXT("Cancel leaves one rack"), Count(), 1);
	TestTrue(TEXT("No preview pipes"), Room->GetMutableNetwork().GetNumPipes() == InitialPipes);
	Controller->BeginRackDuplication(); Controller->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch cancels duplication"), Controller->IsAddingRack()); Controller->SetPatchMapOpen(false);
	Controller->BeginRackDuplication(); Sim->EnterSimulationMode(); Controller->RefreshInteractionInputMode();
	TestFalse(TEXT("Mode switch cancels duplication"), Controller->IsAddingRack());
	Sim->EnterLayoutMode(); Select(Source);
	// Reinitialize the source fixture after the mode reset, then verify it is untouched by confirmation.
	Source->ResetSim(); Source->SetExternalSimTimeS(42); Source->GetMutableModel().Step(Source->Input, Sim->FixedStepS);
	TestTrue(TEXT("Restart duplication"), Controller->BeginRackDuplication());
	Controller->UpdateRackAdditionAlongRay(FVector(307, 204, 500), Down);
	FTransform Preview; FVector SizeM;
	TestTrue(TEXT("Preview available"), Controller->GetRackAdditionPreview(Preview, SizeM));
	const double SnapErrorCm = FVector::Distance(Preview.GetLocation(), FVector(305, 205, 9));
	TestTrue(TEXT("Snap preserves source height"), SnapErrorCm < 1e-9);
	TestTrue(TEXT("Source rotation/scale/size preview"), Preview.GetRotation().Equals(OriginalTransform.GetRotation(), 1e-12)
		&& Preview.GetScale3D() == OriginalTransform.GetScale3D() && SizeM == Source->RackSizeM);
	Controller->OnResetSim(); Controller->GetRackAdditionPreview(Preview, SizeM);
	TestTrue(TEXT("R rotates in world XY without flattening tilt"), Preview.GetRotation().Equals(
		FRotator(0, 90, 0).Quaternion() * OriginalTransform.GetRotation(), 1e-12));
	// Editing the source after preview start cannot change the value snapshot or retain an array alias.
	Source->Input.ComponentHeatW[0] += 7;
	const auto SourceBeforeConfirm = Source->GetSnapshot();
	const double EnergyBeforeConfirmJ = Source->GetMutableModel().GetEnergyInJ();
	auto* Clone = Controller->ConfirmRackAddition();
	if (TestNotNull(TEXT("One fresh clone"), Clone))
	{
		TestEqual(TEXT("Exactly two racks"), Count(), 2);
		TestNull(TEXT("Repeated confirm cannot spawn"), Controller->ConfirmRackAddition());
		TestTrue(TEXT("Unique identity/model"), Clone->GetFName() != Source->GetFName()
			&& &Clone->GetMutableModel() != &Source->GetMutableModel());
		TestTrue(TEXT("Value snapshot controls and slot layout"), Clone->Params.Slots.Num() == Captured.Params.Slots.Num()
			&& Clone->Params.Slots.Last().BottomZM == Captured.Params.Slots.Last().BottomZM
			&& Clone->Input.ComponentHeatW == Captured.Input.ComponentHeatW && Clone->Input.SledDocked == Captured.Input.SledDocked
			&& Clone->Input.SlotUtilization == Captured.Input.SlotUtilization && Clone->Input.Utilization == Captured.Input.Utilization
			&& Clone->Input.FanSpeedOverride == Captured.Input.FanSpeedOverride);
		TestTrue(TEXT("Display/radiator settings"), Clone->bUseGjModels == Captured.bUseGjModels
			&& Clone->Radiator.UAWK == Captured.Radiator.UAWK && Clone->PanelThicknessM == Captured.PanelThicknessM
			&& Clone->bShowSledLabels == Captured.bShowSledLabels && Clone->GetActorTransform().Equals(Preview, 1e-9));
		const FRackThermalInput DefaultInput;
		TestTrue(TEXT("No live boundaries"), Clone->Input.CoolantInletTempK == DefaultInput.CoolantInletTempK
			&& Clone->Input.CoolantMassFlowKgs == DefaultInput.CoolantMassFlowKgs && Clone->Input.AirInletTempK == DefaultInput.AirInletTempK
			&& Clone->Input.SlotAirInletTempK.IsEmpty());
		FRackThermalModel Fresh; Fresh.Initialize(Captured.Params, FRackThermalState());
		Clone->Tick(.1f);
		TestTrue(TEXT("Fresh state/clock/ledger with external clock"), Clone->bDrivenExternally && Clone->GetSnapshot().SimTimeS == 0
			&& Clone->GetMutableModel().GetEnergyInJ() == 0 && Clone->GetMutableModel().GetState().ComponentTempK == Fresh.GetState().ComponentTempK);
		TestTrue(TEXT("Original unchanged at confirmation"), Source->GetActorTransform().Equals(OriginalTransform, 0)
			&& Source->GetSnapshot().ComponentTempK == SourceBeforeConfirm.ComponentTempK
			&& Source->GetSnapshot().SimTimeS == SourceBeforeConfirm.SimTimeS
			&& Source->GetMutableModel().GetEnergyInJ() == EnergyBeforeConfirmJ);
		Clone->Input.ComponentHeatW[0] += 11; Clone->Params.Slots[0].BottomZM += .01;
		TestTrue(TEXT("Arrays have independent storage"), Source->Input.ComponentHeatW[0] == Captured.Input.ComponentHeatW[0] + 7
			&& Source->Params.Slots[0].BottomZM == Captured.Params.Slots[0].BottomZM);
		Select(Clone); TestTrue(TEXT("Runtime clone can start another duplicate"), Controller->BeginRackDuplication());
		Controller->CancelRackAddition();
	}
	AddInfo(FString::Printf(TEXT("Duplication preview: snap %.3e cm | source initial live t %.2f s energy %.3f J nodes %d | fresh clone t=0 ledger=0"),
		SnapErrorCm, OriginalState.SimTimeS, OriginalEnergyJ, Captured.Input.ComponentHeatW.Num()));
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}
#endif
