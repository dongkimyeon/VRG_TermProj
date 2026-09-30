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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRotationTest, "VRG.Interaction.Layout.SelectedQuarterTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRotationTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	auto* Controller = World->SpawnActor<ADataCenterDebugController>();
	Sim->EnterLayoutMode();
	TArray<AActor*> Equipment;
	Equipment.Add(World->SpawnActor<ARackThermalActor>(FVector(0, 0, 7), FRotator(0, 17, 0)));
	Equipment.Add(World->SpawnActor<ASpineCabinetActor>(FVector(300, 0, 7), FRotator(0, 170, 0)));
	Equipment.Add(World->SpawnActor<ACoolingDistributionUnitActor>(FVector(600, 0, 7), FRotator::ZeroRotator));
	Equipment.Add(World->SpawnActor<ACrahUnitActor>(FVector(900, 0, 7), FRotator(0, -90, 0)));
	double MaxYawErrorDeg = 0.0;
	for (AActor* Actor : Equipment)
	{
		const FTransform Original = Actor->GetActorTransform();
		Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), FVector(0, 0, -1));
		TestTrue(TEXT("Clicked equipment selected"), Controller->GetSelectedLayoutActor() == Actor);
		for (int32 Turn = 1; Turn <= 4; ++Turn)
		{
			Controller->OnResetSim(); // The actual R handler, including mode dispatch.
			const double Error = FMath::Abs(FMath::FindDeltaAngleDegrees(Original.Rotator().Yaw + 90.0 * Turn, Actor->GetActorRotation().Yaw));
			MaxYawErrorDeg = FMath::Max(MaxYawErrorDeg, Error);
			TestTrue(TEXT("R adds exactly a quarter turn, including non-cardinal/wrapped headings"), Error < 1e-10);
			TestTrue(TEXT("Rotation leaves origin and scale unchanged"), Actor->GetActorLocation() == Original.GetLocation() && Actor->GetActorScale3D() == Original.GetScale3D());
		}
		TestTrue(TEXT("Four R presses restore starting orientation"), Actor->GetActorQuat().Equals(Original.GetRotation(), 1e-12));
		Controller->BeginLayoutDragAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), FVector(0, 0, -1));
		const FQuat BeforeBlocked = Actor->GetActorQuat();
		Controller->OnResetSim();
		TestTrue(TEXT("Even an armed drag blocks R"), Actor->GetActorQuat() == BeforeBlocked);
		Controller->EndLayoutDrag(false);
		Controller->SetPatchMapOpen(true);
		Controller->OnResetSim();
		TestTrue(TEXT("Patch focus blocks layout R"), Actor->GetActorQuat() == BeforeBlocked);
		Controller->SetPatchMapOpen(false);
	}
	Controller->SelectLayoutAlongRay(FVector(-10000, -10000, 500), FVector(0, 0, -1));
	TArray<FTransform> BeforeEmpty;
	for (AActor* Actor : Equipment) BeforeEmpty.Add(Actor->GetActorTransform());
	Controller->OnResetSim();
	for (int32 I = 0; I < Equipment.Num(); ++I)
		TestTrue(TEXT("No selection: no equipment changes"), Equipment[I]->GetActorTransform().Equals(BeforeEmpty[I], 0));
	TestEqual(TEXT("Layout R does not advance SimTime"), Sim->GetSnapshot().SimTimeS, 0.0);
	Sim->EnterSimulationMode();
	Sim->Tick(0.1f);
	const double BeforeResetS = Sim->GetSnapshot().SimTimeS;
	TestTrue(TEXT("Simulation clock advanced before reset"), BeforeResetS > 0);
	Controller->OnResetSim();
	TestEqual(TEXT("Simulation R still resets clock"), Sim->GetSnapshot().SimTimeS, 0.0);
	for (int32 I = 0; I < Equipment.Num(); ++I)
		TestTrue(TEXT("Simulation R does not rotate equipment"), Equipment[I]->GetActorTransform().Equals(BeforeEmpty[I], 0));
	AddInfo(FString::Printf(TEXT("Selected R: four equipment kinds and four turns; max yaw error %.3e deg; layout SimTime change 0 s; simulation reset %.3f -> %.3f s."), MaxYawErrorDeg, BeforeResetS, Sim->GetSnapshot().SimTimeS));
	World->DestroyWorld(false);
	GEngine->DestroyWorldContext(World);
	return true;
}
#endif
