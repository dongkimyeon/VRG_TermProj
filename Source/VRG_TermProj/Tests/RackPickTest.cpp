// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/RackThermalActor.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// E acts on the one unit the camera looks at, not on every rack. Two racks face each other across a
// cold aisle as in L_Test (row A +X at x = -1.8 m, row B -X at x = +1.8 m); the view ray picks the right one,
// and undocking a slot in it leaves the other rack untouched. Headless: the ray is given, not a real camera.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackPickTest, "VRG.Interaction.RackPick.LookedAtUnitOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackPickTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;

	ARackThermalActor* A = World->SpawnActor<ARackThermalActor>(FVector(-180, 0, 0), FRotator(0, 0, 0));
	ARackThermalActor* B = World->SpawnActor<ARackThermalActor>(FVector(180, 0, 0), FRotator(0, 180, 0));
	TArray<TObjectPtr<ARackThermalActor>> Racks;
	for (ARackThermalActor* Rack : {A, B})
	{
		Rack->RackSizeM = FVector(1.068, .6, 2.236);
		Rack->bShowReadout = false;
		Rack->ApplyGb200Nvl72Preset();  // 37 devices, many 1U (4.4 cm) tall: the fine case for aiming
		Rack->PauseSim();
		Racks.Add(Rack);
	}
	constexpr double MaxCm = 1500.0;

	// From the aisle centre at eye height (1.6 m): looking -X sees A's front, +X sees B's front.
	const FVector Eye(0, 0, 160);
	TestTrue(TEXT("Looking -X picks rack A"), ADataCenterDebugController::PickRackAlongRay(Racks, Eye, FVector(-1, 0, 0), MaxCm) == A);
	TestTrue(TEXT("Looking +X picks rack B"), ADataCenterDebugController::PickRackAlongRay(Racks, Eye, FVector(1, 0, 0), MaxCm) == B);
	// Along the aisle, over the racks, or beyond the reach: nothing.
	TestNull(TEXT("Looking along the aisle picks nothing"), ADataCenterDebugController::PickRackAlongRay(Racks, Eye, FVector(0, 1, 0), MaxCm));
	TestNull(TEXT("Looking over the racks picks nothing"), ADataCenterDebugController::PickRackAlongRay(Racks, FVector(0, 0, 300), FVector(-1, 0, 0), MaxCm));
	TestNull(TEXT("Beyond the reach picks nothing"), ADataCenterDebugController::PickRackAlongRay(Racks, Eye, FVector(-1, 0, 0), 50.0));
	// From outside behind A looking +X through A toward B: the nearer rack (A) wins.
	TestTrue(TEXT("Nearest rack wins"), ADataCenterDebugController::PickRackAlongRay(Racks, FVector(-400, 0, 100), FVector(1, 0, 0), MaxCm) == A);
	// Just beside rack A's edge (width 0.6 m -> |y| <= 30 cm): 35 cm off misses, 25 cm hits.
	TestNull(TEXT("Ray past the side misses"), ADataCenterDebugController::PickRackAlongRay(Racks, FVector(0, 35, 100), FVector(-1, 0, 0), MaxCm));
	TestTrue(TEXT("Ray at the side hits"), ADataCenterDebugController::PickRackAlongRay(Racks, FVector(0, 25, 100), FVector(-1, 0, 0), MaxCm) == A);

	// The unit is the slot at the height where the ray enters the box. Aim at the middle of every slot of B
	// (slightly downward, as from standing eye height) and get that slot back.
	int32 Mismatches = 0;
	for (int32 Slot = 0; Slot < B->Params.NumSleds; ++Slot)
	{
		double BottomM = 0.0, HeightM = 0.0;
		B->GetSlotHeightRangeM(Slot, BottomM, HeightM);
		const FVector Target(180 - .5 * 106.8, 0, (BottomM + .5 * HeightM) * 100.0);  // point on B's front face
		const FVector From(-50, 10, 170);
		double HitZM = -1.0;
		const ARackThermalActor* Hit = ADataCenterDebugController::PickRackAlongRay(Racks, From, (Target - From).GetSafeNormal(), MaxCm, &HitZM);
		if (Hit != B || ADataCenterDebugController::SlotAtHeight(*B, HitZM) != Slot) ++Mismatches;
	}
	AddInfo(FString::Printf(TEXT("슬롯 %d개 조준, 불일치 %d"), B->Params.NumSleds, Mismatches));
	TestEqual(TEXT("Aiming at a slot picks that slot"), Mismatches, 0);
	TestEqual(TEXT("Above the top slot is no unit"), ADataCenterDebugController::SlotAtHeight(*B, B->RackSizeM.Z + .1), int32(INDEX_NONE));

	// E: undocking the looked-at unit leaves every other unit and the other rack as they were.
	double HitZM = 0.0;
	double BottomM = 0.0, HeightM = 0.0;
	B->GetSlotHeightRangeM(3, BottomM, HeightM);
	ARackThermalActor* Picked = ADataCenterDebugController::PickRackAlongRay(Racks, FVector(0, 0, (BottomM + .5 * HeightM) * 100.0), FVector(1, 0, 0), MaxCm, &HitZM);
	const int32 DockedA = A->GetNumDockedSleds(), DockedB = B->GetNumDockedSleds();
	if (TestNotNull(TEXT("Picked"), Picked)) Picked->ToggleSledDocked(ADataCenterDebugController::SlotAtHeight(*Picked, HitZM));
	AddInfo(FString::Printf(TEXT("꽂힌 유닛: A %d -> %d, B %d -> %d"), DockedA, A->GetNumDockedSleds(), DockedB, B->GetNumDockedSleds()));
	TestEqual(TEXT("Other rack untouched"), A->GetNumDockedSleds(), DockedA);
	TestEqual(TEXT("Only one unit left the picked rack"), B->GetNumDockedSleds(), DockedB - 1);
	TestFalse(TEXT("The looked-at unit is out"), B->IsSledDocked(3));
	TestTrue(TEXT("Neighbour units still in"), B->IsSledDocked(2) && B->IsSledDocked(4));

	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
