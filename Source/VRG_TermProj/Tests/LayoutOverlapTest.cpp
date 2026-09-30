#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutOverlapTest, "VRG.Interaction.Layout.EquipmentOverlapGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutOverlapTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* Rack = World->SpawnActor<ARackThermalActor>();
	auto* Blocker = World->SpawnActor<ACrahUnitActor>(FVector(300, 0, 0), FRotator::ZeroRotator);
	Rack->RackSizeM = FVector(1, .5, 1);
	Blocker->SizeM = FVector(.2, .2, 1);
	Sim->EnterLayoutMode();
	const FTransform Original = Rack->GetActorTransform();
	auto IsBlocked = [&](const FTransform& Proposed) { return ADataCenterDebugController::IsLayoutPlacementBlocked(Rack, Proposed); };
	TestFalse(TEXT("Self excluded; initial layout allowed"), IsBlocked(Original));
	// Depth half widths: 50 + 10 cm. A 0.99 cm gap is too small; 1 cm is allowed.
	Blocker->SetActorLocation(FVector(60.99, 0, 0));
	TestTrue(TEXT("0.99 cm gap rejected"), IsBlocked(Original));
	Blocker->SetActorLocation(FVector(61, 0, 0));
	TestFalse(TEXT("Exactly 1 cm gap allowed"), IsBlocked(Original));
	Blocker->SetActorLocation(FVector(61.01, 0, 0));
	TestFalse(TEXT("1.01 cm gap allowed"), IsBlocked(Original));
	Blocker->SetActorLocation(FVector(300, 0, 0));
	Controller->BeginLayoutDragAlongRay(FVector(0, 0, 300), FVector(0, 0, -1));
	Controller->UpdateLayoutDragAlongRay(FVector(300, 0, 300), FVector(0, 0, -1));
	TestTrue(TEXT("Conflicting preview requests red indication"), Controller->IsLayoutDragPreviewBlocked());
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Rejected drop leaves entire original transform"), Rack->GetActorTransform().Equals(Original, 0));
	TestFalse(TEXT("Rejected drop releases drag"), Controller->IsLayoutDragging());
	Blocker->SetActorLocation(FVector(300, 500, 0));
	Controller->BeginLayoutDragAlongRay(FVector(0, 0, 300), FVector(0, 0, -1));
	Controller->UpdateLayoutDragAlongRay(FVector(300, 0, 300), FVector(0, 0, -1));
	TestFalse(TEXT("Unoccupied preview allowed"), Controller->IsLayoutDragPreviewBlocked());
	Blocker->SetActorLocation(FVector(300, 0, 0));
	Controller->EndLayoutDrag(true);
	TestTrue(TEXT("Drop rechecks obstacles moved after preview"), Rack->GetActorTransform().Equals(Original, 0));
	TestFalse(TEXT("Legacy G shared move rejects collision"), Controller->TryLayoutMove(Rack, FVector(300, 0, 0)));
	TestTrue(TEXT("Free move accepted"), Controller->TryLayoutMove(Rack, FVector(150, 0, 0)));
	TestEqual(TEXT("Free move applied exactly"), Rack->GetActorLocation().X, 150.0);
	Rack->SetActorTransform(Original);

	Blocker->SetActorLocation(FVector(0, 40, 0));
	TestFalse(TEXT("Unrotated rectangle clear"), IsBlocked(Original));
	Controller->SelectLayoutAlongRay(FVector(0, 0, 300), FVector(0, 0, -1));
	Controller->OnResetSim();
	TestTrue(TEXT("R rejects conflicting quarter turn"), Rack->GetActorTransform().Equals(Original, 0));
	TestFalse(TEXT("Legacy T shared rotation rejects collision"), Controller->TryLayoutRotation(Rack, FRotator(0, 90, 0)));
	Blocker->SetActorLocation(FVector(300, 0, 0));
	Controller->OnResetSim();
	const double RotationErrorDeg = FMath::Abs(FMath::FindDeltaAngleDegrees(90.0, Rack->GetActorRotation().Yaw));
	TestTrue(TEXT("Free quarter turn accepted"), RotationErrorDeg < 1e-10);
	Rack->SetActorTransform(Original);
	Blocker->SetActorLocation(FVector(90, 0, 0));
	FTransform Scaled = Original;
	Scaled.SetScale3D(FVector(2, 1, 1));
	TestTrue(TEXT("Scaled box collision included"), IsBlocked(Scaled));
	TestFalse(TEXT("Unscaled box at same origin clear"), IsBlocked(Original));
	FTransform Tilted = Original;
	Tilted.SetRotation(FRotator(-45, 0, 0).Quaternion());
	TestTrue(TEXT("Tilted height contributes to floor footprint"), IsBlocked(Tilted));

	// Parallel diagonal thin rectangles: their AABBs overlap, actual footprints have a gap.
	Rack->RackSizeM = FVector(1, .2, 1);
	Rack->SetActorRotation(FRotator(0, 45, 0));
	Blocker->SizeM = FVector(1, .2, 1);
	Blocker->SetActorRotation(FRotator(0, 45, 0));
	Blocker->SetActorLocation(FVector(-25, 25, 0));
	TestFalse(TEXT("Separated diagonal boxes allowed despite AABB overlap"), IsBlocked(Rack->GetActorTransform()));
	Blocker->SetActorLocation(FVector(0, 0, 1000));
	TestTrue(TEXT("Floor projection ignores vertical separation by design"), IsBlocked(Rack->GetActorTransform()));
	Blocker->SetActorLocation(FVector(10000, 0, 0));
	Rack->SetActorTransform(Original);
	auto* Spine = World->SpawnActor<ASpineCabinetActor>(FVector(2000, 0, 0), FRotator::ZeroRotator);
	auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(4000, 0, 0), FRotator::ZeroRotator);
	for (AActor* Other : TArray<AActor*>{ Blocker, Spine, Cdu })
	{
		FTransform Overlap = Rack->GetActorTransform();
		Overlap.SetLocation(Other->GetActorLocation());
		TestTrue(TEXT("CRAH/spine/CDU all block rack placement"), IsBlocked(Overlap));
		TestFalse(TEXT("Each equipment kind cannot move onto rack"), Controller->TryLayoutMove(Other, FVector::ZeroVector));
	}
	TestEqual(TEXT("Overlap checks and rejected actions leave SimTime unchanged"), Sim->GetSnapshot().SimTimeS, 0.0);
	Sim->EnterSimulationMode();
	TestFalse(TEXT("Shared move cannot modify simulation"), Controller->TryLayoutMove(Rack, FVector(300, 0, 0)));
	TestFalse(TEXT("Shared rotation cannot modify simulation"), Controller->TryLayoutRotation(Rack, FRotator(0, 90, 0)));
	AddInfo(FString::Printf(TEXT("Overlap guards: 0.99/1.00/1.01 cm boundaries; projected/scaled/tilted/diagonal boxes; all equipment kinds; rejected drop/rotation transform unchanged, SimTime change 0 s; allowed yaw error %.3e deg."), RotationErrorDeg));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
