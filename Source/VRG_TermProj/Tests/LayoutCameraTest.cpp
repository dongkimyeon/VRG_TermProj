#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CrahUnitActor.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "InputKeyEventArgs.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCameraTest, "VRG.Interaction.Layout.CameraInputAndMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCameraTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Pawn = World->SpawnActor<ADefaultPawn>();
	Controller->Possess(Pawn);
	Controller->PlayerInput = NewObject<UPlayerInput>(Controller);
	Controller->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>();
	Controller->PlayerCameraManager->InitializeFor(Controller);
	Controller->bFrameEquipmentOnStart = false;
	Sim->EnterLayoutMode();
	Controller->RefreshInteractionInputMode();
	TestTrue(TEXT("Layout cursor visible"), Controller->bShowMouseCursor);
	TestTrue(TEXT("Default Pawn movement suppressed"), Controller->IsMoveInputIgnored());
	Pawn->AddMovementInput(FVector::ForwardVector, 1.0f);
	TestTrue(TEXT("Default movement does not accumulate"), Pawn->GetPendingMovementInputVector().IsZero());
	Pawn->SetActorLocation(FVector::ZeroVector);
	Controller->SetControlRotation(FRotator::ZeroRotator);
	for (int32 I = 0; I < 30; ++I)
		Controller->ApplyLayoutCameraInput(1.0f / 30.0f, FVector::ZeroVector, 0, false);
	TestTrue(TEXT("No input: exactly stationary"), Pawn->GetActorLocation().IsZero());
	Controller->ApplyLayoutCameraInput(1, FVector(1, 0, 0), 0, false);
	TestTrue(TEXT("W without RMB: stationary"), Pawn->GetActorLocation().IsZero());
	Controller->ApplyLayoutCameraInput(1, FVector(1, 0, 0), 0, true);
	TestEqual(TEXT("Forward 3 m in 1 s"), Pawn->GetActorLocation().X, 300.0);
	Controller->ApplyLayoutCameraInput(1, FVector(0, 0, 1), 0, true);
	TestEqual(TEXT("E ascends in world Z"), Pawn->GetActorLocation().Z, 300.0);
	const FVector BeforeSpeed = Pawn->GetActorLocation();
	Controller->ApplyLayoutCameraInput(0, FVector::ZeroVector, 1, true);
	TestEqual(TEXT("RMB wheel changes speed"), Controller->GetLayoutCameraSpeedMs(), 3.75);
	TestTrue(TEXT("Speed wheel does not dolly"), Pawn->GetActorLocation() == BeforeSpeed);
	Controller->ApplyLayoutCameraInput(0, FVector::ZeroVector, 1, false);
	TestEqual(TEXT("Wheel dolly 1.875 m"), Pawn->GetActorLocation().X - BeforeSpeed.X, 187.5);
	Controller->ApplyLayoutCameraInput(0, FVector::ZeroVector, 100, true);
	TestEqual(TEXT("Maximum speed"), Controller->GetLayoutCameraSpeedMs(), 24.0);
	Controller->ApplyLayoutCameraInput(0, FVector::ZeroVector, -100, true);
	TestEqual(TEXT("Minimum speed"), Controller->GetLayoutCameraSpeedMs(), 0.25);
	const FRotator BeforeLook = Controller->GetControlRotation();
	Controller->AddYawInput(10);
	Controller->UpdateRotation(0.1f);
	TestTrue(TEXT("Mouse without RMB does not rotate layout"), Controller->GetControlRotation().Equals(BeforeLook, 0));
	Controller->PlayerInput->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::RightMouseButton, IE_Pressed, FPlatformTime::Cycles64()));
	Controller->PlayerInput->ProcessInputStack(TArray<UInputComponent*>(), 0.1f, false);
	Controller->AddYawInput(10);
	Controller->UpdateRotation(0.1f);
	TestFalse(TEXT("RMB allows layout rotation"), Controller->GetControlRotation().Equals(BeforeLook, 0));
	Controller->PlayerInput->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::RightMouseButton, IE_Released, FPlatformTime::Cycles64()));
	Controller->PlayerInput->ProcessInputStack(TArray<UInputComponent*>(), 0.1f, false);
	Controller->SetControlRotation(BeforeLook);
	Pawn->GetMovementComponent()->Velocity = FVector(120, 0, 0);
	Controller->PlayerTick(0.1f);
	TestTrue(TEXT("Layout clears leftover Pawn inertia"), Pawn->GetMovementComponent()->Velocity.IsZero());

	Controller->SetPatchMapOpen(true);
	const FVector BeforePatch = Pawn->GetActorLocation();
	Controller->ApplyLayoutCameraInput(1, FVector(1, 1, 1), 1, true);
	Controller->OnFocusEquipment();
	TestTrue(TEXT("Patch map blocks navigation"), Pawn->GetActorLocation() == BeforePatch);
	Controller->bShowMouseCursor = true;
	Controller->SetPatchMapOpen(false);
	TestTrue(TEXT("Patch close restores layout cursor"), Controller->bShowMouseCursor);
	TestEqual(TEXT("Camera did not advance SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);

	Sim->EnterSimulationMode();
	Controller->RefreshInteractionInputMode();
	TestFalse(TEXT("Simulation cursor hidden"), Controller->bShowMouseCursor);
	TestFalse(TEXT("Simulation default movement restored"), Controller->IsMoveInputIgnored());
	const FVector BeforeSim = Pawn->GetActorLocation();
	Controller->ApplyLayoutCameraInput(1, FVector(1, 1, 1), 1, true);
	TestTrue(TEXT("Layout movement disabled in simulation"), Pawn->GetActorLocation() == BeforeSim);
	Controller->AddYawInput(10);
	Controller->UpdateRotation(0.1f);
	TestFalse(TEXT("Simulation free look preserved"), Controller->GetControlRotation().Equals(BeforeLook, 0));
	Controller->SetPatchMapOpen(true);
	Controller->bShowMouseCursor = true;
	Controller->SetPatchMapOpen(false);
	TestFalse(TEXT("Patch close restores simulation cursor"), Controller->bShowMouseCursor);
	AddInfo(TEXT("Camera: idle drift 0 cm; forward 300 cm/s; wheel 187.5 cm; residual velocity 0 cm/s; SimTime change 0 s. Synthetic input, not OS/Slate capture validation."));
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutFocusTest, "VRG.Interaction.Layout.FocusEquipment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutFocusTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* Selected = World->SpawnActor<ACrahUnitActor>(FVector(150, 220, 0), FRotator(0, 90, 0));
	World->SpawnActor<ACrahUnitActor>(FVector(10000, 0, 0), FRotator::ZeroRotator);
	const FVector Original = Selected->GetActorLocation();
	double WorstRatio = 0;
	for (double Aspect : {16.0 / 9.0, 9.0 / 16.0})
	{
		FVector Eye; FRotator Look;
		TestTrue(TEXT("Single rotated equipment focus"), ADataCenterDebugController::EquipmentViewpoint(World, 90, Aspect, Eye, Look, Selected));
		const FRotationMatrix View(Look);
		const FVector Half = Selected->SizeM * 50;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Local((Corner & 1 ? 1 : -1) * Half.X, (Corner & 2 ? 1 : -1) * Half.Y, Corner & 4 ? 2 * Half.Z : 0);
			const FVector Relative = Selected->GetActorTransform().TransformPositionNoScale(Local) - Eye;
			const double Depth = FVector::DotProduct(Relative, Look.Vector());
			TestTrue(TEXT("Corner in front"), Depth > 0);
			const double Ratio = FMath::Max(FMath::Abs(FVector::DotProduct(Relative, View.GetScaledAxis(EAxis::Y))) / Depth,
				FMath::Abs(FVector::DotProduct(Relative, View.GetScaledAxis(EAxis::Z))) * Aspect / Depth);
			WorstRatio = FMath::Max(WorstRatio, Ratio);
		}
		TestTrue(TEXT("Far unrelated equipment excluded"), FVector::Distance(Eye, Original) < 2000);
	}
	TestTrue(TEXT("Corners fit horizontal/vertical FOV"), WorstRatio < 1);
	TestTrue(TEXT("Focus does not move equipment"), Selected->GetActorLocation() == Original);
	AddInfo(FString::Printf(TEXT("Selected focus maximum projected edge ratio %.9f (1 = viewport edge), equipment displacement 0 cm."), WorstRatio));
	World->DestroyWorld(false);
	return true;
}
#endif
