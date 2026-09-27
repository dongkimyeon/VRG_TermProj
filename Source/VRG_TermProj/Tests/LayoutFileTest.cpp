// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS

// S6 (2026-09-27): layout save / load. Placement and cooling settings come back exactly; a missing actor is reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutFileTest, "VRG.Interaction.Layout.SaveLoadRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutFileTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	ARackThermalActor* A = World->SpawnActor<ARackThermalActor>(FVector(-150, 20, 0), FRotator(0, 0, 0));
	ARackThermalActor* B = World->SpawnActor<ARackThermalActor>(FVector(150, -40, 0), FRotator(0, 180, 0));
	ACrahUnitActor* Crah = World->SpawnActor<ACrahUnitActor>(FVector(0, 300, 0), FRotator(0, 90, 0));
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Sim->Containment = EAisleContainment::Blanking;
	Sim->CrahLayout = ECrahLayout::Perimeter;
	Air->GridParams.CellSizeM = .125;
	const FTransform TA = A->GetActorTransform(), TB = B->GetActorTransform(), TC = Crah->GetActorTransform();

	const FString Name = TEXT("AutomationRoundTrip");
	FString Summary;
	TestTrue(TEXT("Saved"), VrgLayoutFile::Save(World, Name, Summary));
	AddInfo(Summary);

	// Move and rotate everything, change the settings, then load.
	A->SetActorLocationAndRotation(FVector(500, 500, 0), FRotator(0, 90, 0));
	B->SetActorLocationAndRotation(FVector(-500, 0, 0), FRotator(0, 270, 0));
	Crah->SetActorLocationAndRotation(FVector(0, -300, 0), FRotator(0, 0, 0));
	Sim->Containment = EAisleContainment::None;
	Sim->CrahLayout = ECrahLayout::InRow;
	Air->GridParams.CellSizeM = .25;
	TestTrue(TEXT("Loaded"), VrgLayoutFile::Load(World, Name, Summary));
	AddInfo(Summary);
	auto Same = [](const FTransform& X, const AActor* Actor)
	{
		return X.GetLocation().Equals(Actor->GetActorLocation(), 1e-6) && FMath::IsNearlyEqual(X.Rotator().Yaw, Actor->GetActorRotation().Yaw, 1e-6);
	};
	TestTrue(TEXT("Rack A back in place"), Same(TA, A));
	TestTrue(TEXT("Rack B back in place (180 deg)"), Same(TB, B));
	TestTrue(TEXT("CRAH back in place (90 deg)"), Same(TC, Crah));
	TestTrue(TEXT("Settings restored"), Sim->Containment == EAisleContainment::Blanking && Sim->CrahLayout == ECrahLayout::Perimeter
		&& Air->GridParams.CellSizeM == .125);

	// A saved actor that is no longer in the level is reported, not silently skipped.
	B->Rename(TEXT("RackRenamedAfterSave"));  // the saved name no longer exists (Destroy warns in a context-less test world)
	AddExpectedMessage(TEXT("레벨에 없는 장비"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Missing equipment reported"), VrgLayoutFile::Load(World, Name, Summary));
	AddInfo(Summary);

	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name));
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
