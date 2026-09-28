#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerInput.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutSelectionTest, "VRG.Interaction.Layout.SingleSelectionAndFocus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutSelectionTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Pawn = World->SpawnActor<ADefaultPawn>();
	Controller->Possess(Pawn);
	Controller->PlayerInput = NewObject<UPlayerInput>(Controller);
	Controller->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>();
	Controller->PlayerCameraManager->InitializeFor(Controller);
	Controller->bFrameEquipmentOnStart = false;
	TArray<AActor*> Equipment;
	Equipment.Add(World->SpawnActor<ARackThermalActor>(FVector(0, 0, 0), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ASpineCabinetActor>(FVector(300, 0, 0), FRotator(0, 90, 0)));
	Equipment.Add(World->SpawnActor<ACoolingDistributionUnitActor>(FVector(600, 0, 0), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACrahUnitActor>(FVector(900, 0, 0), FRotator::ZeroRotator));
	Sim->EnterLayoutMode();
	Controller->RefreshInteractionInputMode();
	TArray<FTransform> OriginalTransforms;
	for (AActor* Actor : Equipment) OriginalTransforms.Add(Actor->GetActorTransform());
	for (AActor* Actor : Equipment)
	{
		// Ray supplied at the cursor-deprojection boundary; actual OS/Slate mouse capture is separate.
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, -500, 100), FVector(0, 2, 0));
		TestTrue(TEXT("Each supported equipment kind selectable"), Controller->GetSelectedLayoutActor() == Actor);
	}
	AActor* Selected = Equipment.Last();
	auto* Behind = World->SpawnActor<ACrahUnitActor>(FVector(900, 400, 0), FRotator::ZeroRotator);
	Controller->SelectLayoutAlongRay(FVector(900, -2500, 100), FVector(0, 1, 0));
	TestTrue(TEXT("Nearest box selected, beyond old 15 m reach"), Controller->GetSelectedLayoutActor() == Selected);
	Controller->SelectLayoutAlongRay(FVector::ZeroVector, FVector::ZeroVector);
	TestTrue(TEXT("Invalid ray preserves selection"), Controller->GetSelectedLayoutActor() == Selected);

	const FVector BeforeMap = Pawn->GetActorLocation();
	Controller->SetPatchMapOpen(true);
	Controller->SelectLayoutAlongRay(FVector(-5000, -5000, 100), FVector(0, 1, 0));
	Controller->OnFocusEquipment();
	TestTrue(TEXT("Patch map prevents selection changes"), Controller->GetSelectedLayoutActor() == Selected);
	TestTrue(TEXT("Patch map prevents F movement"), Pawn->GetActorLocation() == BeforeMap);
	Controller->SetPatchMapOpen(false);
	TestTrue(TEXT("Layout cursor restored after map"), Controller->bShowMouseCursor);
	Controller->PlayerInput->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::RightMouseButton, IE_Pressed, FPlatformTime::Cycles64()));
	Controller->PlayerInput->ProcessInputStack(TArray<UInputComponent*>(), 0.1f, false);
	Controller->SelectLayoutAlongRay(FVector(-5000, -5000, 100), FVector(0, 1, 0));
	TestTrue(TEXT("RMB navigation cannot clear selection"), Controller->GetSelectedLayoutActor() == Selected);
	Controller->PlayerInput->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::RightMouseButton, IE_Released, FPlatformTime::Cycles64()));
	Controller->PlayerInput->ProcessInputStack(TArray<UInputComponent*>(), 0.1f, false);

	FVector SelectedEye, AllEye; FRotator SelectedLook, AllLook;
	const double Fov = Controller->PlayerCameraManager->GetFOVAngle();
	ADataCenterDebugController::EquipmentViewpoint(World, Fov, 16.0 / 9.0, SelectedEye, SelectedLook, Selected);
	ADataCenterDebugController::EquipmentViewpoint(World, Fov, 16.0 / 9.0, AllEye, AllLook);
	Controller->OnFocusEquipment();
	const double SelectedViewErrorCm = FVector::Distance(Pawn->GetActorLocation(), SelectedEye);
	TestTrue(TEXT("F frames the clicked object"), SelectedViewErrorCm < 1e-9);
	TestTrue(TEXT("F does not clear selection"), Controller->GetSelectedLayoutActor() == Selected);
	TestTrue(TEXT("Single view differs from all-equipment view"), FVector::Distance(SelectedEye, AllEye) > 100);
	Controller->SelectLayoutAlongRay(FVector(-5000, -5000, 100), FVector(0, 1, 0));
	TestNull(TEXT("Empty click clears selection"), Controller->GetSelectedLayoutActor());
	Controller->OnFocusEquipment();
	const double AllViewErrorCm = FVector::Distance(Pawn->GetActorLocation(), AllEye);
	TestTrue(TEXT("F with no selection frames everything"), AllViewErrorCm < 1e-9);
	for (int32 I = 0; I < Equipment.Num(); ++I)
		TestTrue(TEXT("Selection and F leave equipment unchanged"), Equipment[I]->GetActorTransform().Equals(OriginalTransforms[I], 0));
	TestEqual(TEXT("Selection and F leave SimTime unchanged"), Sim->GetSnapshot().SimTimeS, 0.0);

	Controller->SelectLayoutAlongRay(FVector(900, 800, 100), FVector(0, -1, 0));
	TestTrue(TEXT("Back device selected from opposite direction"), Controller->GetSelectedLayoutActor() == Behind);
	Behind->Destroy();
	TestNull(TEXT("Destroyed selection becomes null"), Controller->GetSelectedLayoutActor());
	ADataCenterDebugController::EquipmentViewpoint(World, Fov, 16.0 / 9.0, AllEye, AllLook);
	Controller->OnFocusEquipment();
	TestTrue(TEXT("Destroyed selection safely falls back to all"), FVector::Distance(Pawn->GetActorLocation(), AllEye) < 1e-9);

	Controller->SelectLayoutAlongRay(FVector(0, -500, 100), FVector(0, 1, 0));
	Sim->EnterSimulationMode();
	Controller->RefreshInteractionInputMode();
	TestNull(TEXT("Entering simulation clears layout selection"), Controller->GetSelectedLayoutActor());
	Controller->SelectLayoutAlongRay(FVector(0, -500, 100), FVector(0, 1, 0));
	TestNull(TEXT("Simulation ignores selection rays"), Controller->GetSelectedLayoutActor());
	const FVector SimEye = Pawn->GetActorLocation();
	Controller->OnFocusEquipment();
	TestTrue(TEXT("Simulation ignores layout F"), Pawn->GetActorLocation() == SimEye);
	AddInfo(FString::Printf(TEXT("Selection: 4 equipment kinds, nearest/empty/UI/RMB/mode/destroy cases; selected/all F errors %.3e / %.3e cm; equipment and SimTime unchanged."), SelectedViewErrorCm, AllViewErrorCm));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
