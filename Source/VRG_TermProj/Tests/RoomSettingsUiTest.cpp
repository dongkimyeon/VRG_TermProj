#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/RoomSettingsWidget.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerInput.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomSettingsUiTest, "VRG.Interaction.Layout.RoomSettingsUiInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomSettingsUiTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart = false;
	auto* Air = World->SpawnActor<AAirFieldDebugActor>();
	auto* Rack = World->SpawnActor<ARackThermalActor>(); Rack->ApplyGb200Nvl72Preset();
	auto* Pawn = World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn);
	C->PlayerInput = NewObject<UPlayerInput>(C);
	C->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>(); C->PlayerCameraManager->InitializeFor(C);
	// Use in-row planning so a single synthetic rack is not an unpaired perimeter row.
	Sim->EnterLayoutMode(); C->RefreshInteractionInputMode();
	const auto Initial = Sim->GetRoomAirDomain(); const auto GridParams = Air->GridParams;
	const auto Origin = Air->GetActorLocation(); const auto Pose = Rack->GetActorTransform();
	const FVector Eye = Rack->GetActorLocation() + FVector(0, 0, 500), Down(0, 0, -1);
	C->SelectLayoutAlongRay(Eye, Down); TestTrue(TEXT("Open layout-only room editor"), C->OpenRoomSettings());
	TestFalse(TEXT("Duplicate window rejected"), C->OpenRoomSettings());
	const auto Seed = C->GetRoomSettingsDraft();
	TestTrue(TEXT("Automatic empty input seeded from live grid without mutation"), !Seed.bManual && Seed.CeilingHeightM == GridParams.NumCellsZ * GridParams.CellSizeM);
	FRoomAirDomainSettings Draft = Initial; FString Message;
	const TArray<FString> Fields = {TEXT(" -5.5 "), TEXT("5.1"), TEXT("-4.5"), TEXT("4.6"), TEXT("3.1")};
	TestTrue(TEXT("Strict draft parsing accepts signed decimals in metres"), URoomSettingsWidget::ParseDraft(true, Fields, Draft, Message));
	TestTrue(TEXT("Draft does not apply settings or move grid"), Sim->GetRoomAirDomain() == Initial && Air->GetActorLocation() == Origin);
	const auto ValidDraft = Draft;
	for (const FString& Bad : {FString(), FString(TEXT("12garbage")), FString(TEXT("nan")), FString(TEXT("inf")), FString(TEXT("1e999")), FString(TEXT("3,1"))})
	{
		auto InvalidFields = Fields; InvalidFields[4] = Bad;
		TestFalse(TEXT("Malformed or non-finite input rejected"), URoomSettingsWidget::ParseDraft(true, InvalidFields, Draft, Message));
		TestTrue(TEXT("Parse rejection leaves output/settings untouched with reason"), Draft == ValidDraft && Sim->GetRoomAirDomain() == Initial && !Message.IsEmpty());
	}
	const FVector CameraBefore = Pawn->GetActorLocation(); const auto HeatKnob = C->CpuHeatKnob.Value;
	C->ApplyLayoutCameraInput(1, FVector(1, 1, 1), 1, true); C->OnFocusEquipment(); C->OnResetSim();
	C->BeginRackAddition(); C->BeginLayoutDragAlongRay(Eye, Down);
	TestFalse(TEXT("Room editing gates dragging and addition"), C->IsLayoutDragging() || C->IsAddingRack());
	TestFalse(TEXT("Room editing gates rotation"), C->TryLayoutRotation(Rack, FRotator(0, 90, 0), true));
	for (const auto Key : {EKeys::One, EKeys::R, EKeys::Enter, EKeys::G, EKeys::T, EKeys::C, EKeys::Delete, EKeys::LeftControl, EKeys::D, EKeys::Z, EKeys::Y})
	{ FInputKeyEventArgs Args; Args.Key = Key; Args.Event = IE_Pressed; C->InputKey(Args); }
	TestTrue(TEXT("Numeric modal owns input; camera/equipment/heat/mode/clock unchanged"), Pawn->GetActorLocation() == CameraBefore
		&& Rack->GetActorTransform().Equals(Pose) && C->CpuHeatKnob.Value == HeatKnob && Sim->GetSimMode() == ESimMode::Layout
		&& Sim->GetSnapshot().SimTimeS == 0 && Sim->GetRoomAirDomain() == Initial);
	auto BadRoom = ValidDraft; BadRoom.CeilingHeightM = 1;
	TestFalse(TEXT("Apply uses geometry validator"), C->ApplyRoomSettings(BadRoom, Message));
	TestTrue(TEXT("Rejected apply preserves live grid/settings and reports reason"), Sim->GetRoomAirDomain() == Initial && Air->GetActorLocation() == Origin && !Message.IsEmpty());
	TestTrue(TEXT("Manual apply accepted"), C->ApplyRoomSettings(ValidDraft, Message));
	TestTrue(TEXT("Actual planned range/count displayed while live domain remains unchanged"), Message.Contains(TEXT("20683"))
		&& Message.Contains(TEXT("43x37x13")) && Sim->GetRoomAirDomain() == ValidDraft && Air->GridParams.NumCellsX == GridParams.NumCellsX && Air->GetActorLocation() == Origin);
	FInputKeyEventArgs Esc; Esc.Key = EKeys::Escape; Esc.Event = IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc closes draft while preserving applied setting and layout cursor"), !C->IsRoomSettingsOpen() && C->bShowMouseCursor && Sim->GetRoomAirDomain() == ValidDraft);
	C->ApplyLayoutCameraInput(1, FVector(1, 0, 0), 0, true);
	TestTrue(TEXT("Closing restores camera input"), Pawn->GetActorLocation() != CameraBefore);
	TestFalse(TEXT("Closed window cannot apply"), C->ApplyRoomSettings({}, Message));
	C->OpenRoomSettings(); C->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch entry closes room editor"), C->IsRoomSettingsOpen());
	TestFalse(TEXT("Patch owns input and cannot open room"), C->OpenRoomSettings()); C->SetPatchMapOpen(false);
	TestTrue(TEXT("Room can reopen after patch close"), C->OpenRoomSettings());
	C->CloseRoomSettings(); C->BeginRackAddition(); TestFalse(TEXT("Addition preview blocks room entry"), C->OpenRoomSettings()); C->CancelRackAddition();
	C->BeginLayoutDragAlongRay(Eye, Down); TestFalse(TEXT("Drag blocks room entry"), C->OpenRoomSettings()); C->EndLayoutDrag(false);
	C->OpenRoomSettings(); Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("External mode change closes editor and hides cursor"), !C->IsRoomSettingsOpen() && !C->bShowMouseCursor);
	TestFalse(TEXT("Simulation cannot open room editor"), C->OpenRoomSettings());
	TestTrue(TEXT("Manual air plan used on simulation entry"), Air->GridParams.NumCellsX == 43 && Air->GetActorLocation() == FVector(-550, -450, 0));
	Sim->EnterLayoutMode(); C->RefreshInteractionInputMode(); C->OpenRoomSettings();
	TestTrue(TEXT("Auto toggle accepted without inactive draft-number mutation"), C->ApplyRoomSettings({}, Message)
		&& !Sim->GetRoomAirDomain().bManual && Sim->GetRoomAirDomain().MinXYM == ValidDraft.MinXYM && Message.Contains(TEXT("자동 맞춤")));
	C->CloseRoomSettings();
	AddInfo(TEXT("Room UI: draft/apply separation, strict finite parsing, rejection/mode/input gates, planned43x37x13=20683cells at h0.25m, deferred grid, close restores synthetic camera input. No OS/Slate focus claim."));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
