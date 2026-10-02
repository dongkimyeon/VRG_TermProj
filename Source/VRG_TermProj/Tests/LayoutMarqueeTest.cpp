#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "Math/PerspectiveMatrix.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutMarqueeTest, "VRG.Interaction.Layout.ScreenRectangleSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutMarqueeTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	auto* A = World->SpawnActor<ARackThermalActor>(FVector(-25, 0, 100), FRotator::ZeroRotator);
	auto* B = World->SpawnActor<ARackThermalActor>(FVector(25, 0, 100), FRotator::ZeroRotator);
	auto* Offscreen = World->SpawnActor<ACrahUnitActor>(FVector(1000, 0, 100), FRotator::ZeroRotator);
	auto* Behind = World->SpawnActor<ACrahUnitActor>(FVector(0, 0, -100), FRotator::ZeroRotator);
	A->RackSizeM = B->RackSizeM = Offscreen->SizeM = Behind->SizeM = FVector(.2, .2, .2);
	Sim->EnterLayoutMode();
	// Synthetic camera along +Z, 90 degree FOV, near=10 cm, nonzero viewport origin.
	const FMatrix Projection = FReversedZPerspectiveMatrix(UE_DOUBLE_PI / 4.0, 1.0, 1.0, 10.0);
	const FIntRect View(20, 30, 220, 230);
	FBox2D Bounds;
	TestTrue(TEXT("Visible box projects"), Controller->ProjectLayoutBoxToScreen(A, Projection, View, Bounds));
	const double ProjectionErrorPx = FMath::Max(FVector2D::Distance(Bounds.Min, FVector2D(85, 120)), FVector2D::Distance(Bounds.Max, FVector2D(107.5, 140)));
	TestTrue(TEXT("Analytic perspective bounds and viewport offset"), ProjectionErrorPx < 1e-9);
	TestFalse(TEXT("Behind-camera box excluded"), Controller->ProjectLayoutBoxToScreen(Behind, Projection, View, Bounds));
	TestFalse(TEXT("Offscreen box excluded"), Controller->ProjectLayoutBoxToScreen(Offscreen, Projection, View, Bounds));
	Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(84, 119), FVector2D(108, 141)), Projection, View, false);
	TestTrue(TEXT("Rectangle replaces selection with A"), Controller->GetSelectedLayoutActors().Num() == 1 && Controller->GetSelectedLayoutActor() == A);
	Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(132, 119), FVector2D(156, 141)), Projection, View, true);
	TestEqual(TEXT("Shift rectangle adds B"), Controller->GetSelectedLayoutActors().Num(), 2);
	Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(132, 119), FVector2D(156, 141)), Projection, View, true);
	TestEqual(TEXT("Add does not duplicate selection"), Controller->GetSelectedLayoutActors().Num(), 2);
	Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(106, 125), FVector2D(109, 128)), Projection, View, false);
	TestTrue(TEXT("Partial projected overlap selects A"), Controller->GetSelectedLayoutActors().Num() == 1 && Controller->GetSelectedLayoutActor() == A);
	A->SetActorLocation(FVector(0, 0, -5));
	TestTrue(TEXT("Box crossing eye and near plane projects after clipping"), Controller->ProjectLayoutBoxToScreen(A, Projection, View, Bounds));
	TestTrue(TEXT("Near-plane clipped bounds remain in viewport"), Bounds.Min.X >= 20 && Bounds.Max.X <= 220 && Bounds.Min.Y >= 30 && Bounds.Max.Y <= 230);
	A->SetActorLocation(FVector(-25, 0, 100));
	Controller->BeginLayoutMarquee(FVector2D(120, 150), true);
	Controller->UpdateLayoutMarquee(FVector2D(60, 80));
	TestTrue(TEXT("Reverse drag draws normalized rectangle"), Controller->GetLayoutMarqueeRect(Bounds) && Bounds.Min == FVector2D(60, 80) && Bounds.Max == FVector2D(120, 150));
	TestFalse(TEXT("Marquee is separate from equipment drag"), Controller->IsLayoutDragging());
	TestFalse(TEXT("Marquee blocks legacy move"), Controller->TryLayoutMove(A, FVector(300, 0, 0)));
	Controller->EndLayoutMarquee(false);
	TestTrue(TEXT("Esc-equivalent cancellation preserves previous selection"), Controller->GetSelectedLayoutActor() == A);
	Controller->BeginLayoutMarquee(FVector2D(100, 100), true);
	Controller->UpdateLayoutMarquee(FVector2D(102, 102));
	TestFalse(TEXT("Click jitter does not draw rectangle"), Controller->GetLayoutMarqueeRect(Bounds));
	Controller->EndLayoutMarquee(true);
	TestTrue(TEXT("Empty Shift click clears all"), Controller->GetSelectedLayoutActors().IsEmpty());
	Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(84, 119), FVector2D(108, 141)), Projection, View, false);
	Controller->BeginLayoutMarquee(FVector2D(100, 100), false);
	Controller->UpdateLayoutMarquee(FVector2D(150, 150));
	Controller->SetPatchMapOpen(true); Controller->SetPatchMapOpen(false);
	TestFalse(TEXT("Patch opening cancels rectangle"), Controller->IsLayoutMarqueeActive());
	TestTrue(TEXT("Patch cancellation retains prior selection"), Controller->GetSelectedLayoutActor() == A);
	Controller->BeginLayoutMarquee(FVector2D(100, 100), false);
	Sim->EnterSimulationMode(); Controller->RefreshInteractionInputMode();
	TestFalse(TEXT("Mode transition cancels rectangle"), Controller->IsLayoutMarqueeActive());
	TestFalse(TEXT("Simulation ignores rectangle selection"), Controller->SelectLayoutInScreenRect(FBox2D(FVector2D(20, 30), FVector2D(220, 230)), Projection, View, false));
	TestEqual(TEXT("Rectangle gesture does not advance SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);
	AddInfo(FString::Printf(TEXT("Marquee: analytic projection max error %.3e px; replace/add/partial/behind/offscreen/near-plane/reverse/cancel/click-jitter/patch/mode cases; SimTime change 0 s. Synthetic projection, not OS/Slate validation."), ProjectionErrorPx));
	World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	return true;
}
#endif
