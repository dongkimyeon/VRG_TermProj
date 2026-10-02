#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutGroupRotationTest, "VRG.Interaction.Layout.GroupQuarterTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutGroupRotationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* A = World->SpawnActor<ARackThermalActor>(FVector(0, 0, 7), FRotator::ZeroRotator);
	auto* B = World->SpawnActor<ARackThermalActor>(FVector(120, 0, 12), FRotator::ZeroRotator);
	auto* Obstacle = World->SpawnActor<ACrahUnitActor>(FVector(1000, 0, 0), FRotator::ZeroRotator);
	A->RackSizeM = FVector(1, .2, 1); A->SetActorScale3D(FVector(2, 1, 1));
	B->RackSizeM = Obstacle->SizeM = FVector(.2, .2, 1);
	Sim->EnterLayoutMode();
	const FTransform StartA = A->GetActorTransform(), StartB = B->GetActorTransform();
	Controller->SelectLayoutAlongRay(FVector(0, 0, 500), FVector(0, 0, -1));
	Controller->SelectLayoutAlongRay(FVector(120, 0, 500), FVector(0, 0, -1), true);
	// Scaled bounds [-100,130] x [-10,10] give pivot (15,0), not origin centroid (60,0).
	Controller->OnResetSim();
	const double PivotErrorCm = FMath::Max(FVector::Distance(A->GetActorLocation(), FVector(15, -15, 7)),
		FVector::Distance(B->GetActorLocation(), FVector(15, 105, 12)));
	const double DistanceErrorCm = FMath::Abs(FVector::Distance(A->GetActorLocation(), B->GetActorLocation())
		- FVector::Distance(StartA.GetLocation(), StartB.GetLocation()));
	TestTrue(TEXT("R rotates about scaled floor AABB centre"), PivotErrorCm < 1e-9);
	TestTrue(TEXT("Inter-device distance preserved"), DistanceErrorCm < 1e-9);
	TestTrue(TEXT("Height and scale preserved"), A->GetActorLocation().Z == 7 && B->GetActorLocation().Z == 12
		&& A->GetActorScale3D() == StartA.GetScale3D() && B->GetActorScale3D() == StartB.GetScale3D());
	TestTrue(TEXT("Orientation quarter turns with position"), FMath::Abs(FMath::FindDeltaAngleDegrees(A->GetActorRotation().Yaw, 90.0)) < 1e-10);
	for (int32 I = 0; I < 3; ++I) Controller->OnResetSim();
	const double ReturnErrorCm = FMath::Max(FVector::Distance(A->GetActorLocation(), StartA.GetLocation()), FVector::Distance(B->GetActorLocation(), StartB.GetLocation()));
	TestTrue(TEXT("Four turns restore positions and orientations"), ReturnErrorCm < 1e-9
		&& A->GetActorQuat().Equals(StartA.GetRotation(), 1e-12) && B->GetActorQuat().Equals(StartB.GetRotation(), 1e-12));
	A->SetActorTransform(StartA); B->SetActorTransform(StartB);
	// UE may normalize a quaternion during restoration. Rejection must preserve the actual pre-command transforms exactly.
	const FTransform BeforeRejectA = A->GetActorTransform(), BeforeRejectB = B->GetActorTransform();
	Obstacle->SetActorLocation(FVector(15, 105, 12));
	Controller->OnResetSim();
	TestTrue(TEXT("One proposed member collision rejects entire R rotation"), A->GetActorTransform().Equals(BeforeRejectA, 0) && B->GetActorTransform().Equals(BeforeRejectB, 0));
	Obstacle->SetActorLocation(FVector(1000, 0, 0));
	B->GetRootComponent()->SetMobility(EComponentMobility::Static);
	TestFalse(TEXT("Immovable member blocks transaction"), Controller->TryRotateLayoutSelection());
	TestTrue(TEXT("No partial change on immovable member"), A->GetActorTransform().Equals(BeforeRejectA, 0) && B->GetActorTransform().Equals(BeforeRejectB, 0));
	B->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	Controller->BeginLayoutDragAlongRay(FVector(0, 0, 500), FVector(0, 0, -1));
	TestFalse(TEXT("Equipment drag blocks group rotation"), Controller->TryRotateLayoutSelection());
	Controller->EndLayoutDrag(false);
	Controller->BeginLayoutMarquee(FVector2D(10, 10), false);
	TestFalse(TEXT("Marquee blocks group rotation"), Controller->TryRotateLayoutSelection());
	Controller->EndLayoutMarquee(false);
	Controller->SetPatchMapOpen(true);
	TestFalse(TEXT("Patch focus blocks group rotation"), Controller->TryRotateLayoutSelection());
	Controller->SetPatchMapOpen(false);
	TestEqual(TEXT("Rotation does not advance SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);
	Sim->EnterSimulationMode();
	TestFalse(TEXT("Simulation cannot rotate group"), Controller->TryRotateLayoutSelection());
	AddInfo(FString::Printf(TEXT("Group rotation: scaled pivot (15,0) cm; position/distance/four-turn errors %.3e / %.3e / %.3e cm; collision and gesture rejection unchanged; SimTime change 0 s."), PivotErrorCm, DistanceErrorCm, ReturnErrorCm));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
