#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Air/AirConnectivity.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	int32 CountBarriers(UWorld* W) { int32 N = 0; for (TActorIterator<ALayoutBarrierActor> It(W); It; ++It) ++N; return N; }
	ALayoutBarrierActor* OnlyBarrier(UWorld* W, int32 Index = 0)
	{
		int32 I = 0;
		for (TActorIterator<ALayoutBarrierActor> It(W); It; ++It) if (I++ == Index) return *It;
		return nullptr;
	}
}

// Layout editing of walls / curtains: value preview addition, collision with equipment, barrier-barrier overlap,
// gizmo move / rotate / scale with snaps, blocked and cancelled gizmo edits, one-command Undo/Redo, deletion and
// duplication, and layout file v2 round trip. UI geometry only: UE cm at the actor boundary, no solver state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutBarrierEditTest, "VRG.Interaction.Layout.BarrierGizmoUndoSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutBarrierEditTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>();
	auto* Rack = W->SpawnActor<ARackThermalActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	Sim->EnterLayoutMode();
	const FVector Down(0, 0, -1);
	auto Above = [](const FVector& P) { return P + FVector(0, 0, 1000); };

	// Addition: default wall size, 5 cm snap, one command.
	C->BeginWallAddition();
	C->UpdateRackAdditionAlongRay(Above(FVector(402, 3, 0)), Down);
	ALayoutBarrierActor* Wall = C->ConfirmBarrierAddition();
	if (!TestNotNull(TEXT("Wall added"), Wall)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
	TestTrue(TEXT("Snapped location and default size"), Wall->GetActorLocation().Equals(FVector(400, 5, 0), 1e-9)
		&& Wall->GetSizeM().Equals(ALayoutBarrierActor::DefaultSizeM(EBarrierKind::Wall), 1e-12));
	TestTrue(TEXT("One command, selected"), C->GetLayoutUndoCount() == 1 && C->GetSelectedLayoutActor() == Wall);

	// Blocked on equipment: the preview over the rack is red and confirming does nothing.
	C->BeginCurtainAddition();
	C->UpdateRackAdditionAlongRay(Above(FVector::ZeroVector), Down);
	TestTrue(TEXT("Over the rack is blocked"), C->IsRackAdditionBlocked());
	TestNull(TEXT("Blocked confirm"), C->ConfirmBarrierAddition());
	TestTrue(TEXT("Blocked leaves count/history"), CountBarriers(W) == 1 && C->GetLayoutUndoCount() == 1);
	// Barriers may overlap each other (wall corners).
	C->UpdateRackAdditionAlongRay(Above(FVector(400, 100, 0)), Down);
	ALayoutBarrierActor* Curtain = C->ConfirmBarrierAddition();
	TestTrue(TEXT("Overlapping curtain allowed"), Curtain != nullptr && CountBarriers(W) == 2 && Curtain->Kind == EBarrierKind::Curtain);

	// Gizmo on the wall: move along Y by a 123 cm cursor motion -> 125 cm (5 cm snap).
	C->SelectLayoutAlongRay(Above(Wall->GetActorLocation()) + FVector(0, -100, 0), Down);
	TestTrue(TEXT("Gizmo target"), C->GetLayoutGizmoTarget() == Wall);
	const FTransform T0 = Wall->GetActorTransform();
	const FVector Pivot0 = T0.TransformPosition(FVector(0, 0, 50));
	TestTrue(TEXT("Translate mode"), C->SetLayoutGizmoMode(ELayoutGizmoMode::Translate));
	TestTrue(TEXT("Begin move Y"), C->BeginLayoutGizmoDrag(1, Above(Pivot0), Down));
	TestTrue(TEXT("Gizmo drag blocks other edits"), C->IsLayoutDragging() && !C->TryDeleteLayoutBarrier() && !C->TryUndoLayout());
	C->UpdateLayoutGizmoDrag(Above(Pivot0) + FVector(0, 123, 0), Down);
	TestTrue(TEXT("Live move snapped"), Wall->GetActorLocation().Equals(T0.GetLocation() + FVector(0, 125, 0), 1e-9));
	C->EndLayoutDrag(true);
	const FTransform T1 = Wall->GetActorTransform();
	TestTrue(TEXT("Move recorded"), C->GetLayoutUndoCount() == 3 && !C->IsLayoutGizmoDragging());

	// Rotate about world Z: cursor sweeps a quarter circle -> +90 deg (15 deg snap).
	TestTrue(TEXT("Rotate mode"), C->SetLayoutGizmoMode(ELayoutGizmoMode::Rotate));
	const FVector Pivot1 = T1.TransformPosition(FVector(0, 0, 50));
	TestTrue(TEXT("Begin rotate Z"), C->BeginLayoutGizmoDrag(2, Above(Pivot1) + FVector(100, 0, 0), Down));
	C->UpdateLayoutGizmoDrag(Above(Pivot1) + FVector(2, 100, 0), Down);
	C->EndLayoutDrag(true);
	const FTransform T2 = Wall->GetActorTransform();
	TestTrue(TEXT("Yaw +90 about the box centre"), FMath::IsNearlyEqual(FRotator::NormalizeAxis(T2.Rotator().Yaw - T1.Rotator().Yaw), 90.0, 1e-9)
		&& T2.TransformPosition(FVector(0, 0, 50)).Equals(Pivot1, 1e-6));

	// Scale along the wall's own X (now world Y): a 10 cm handle motion -> thickness +20 cm (both sides).
	TestTrue(TEXT("Scale mode"), C->SetLayoutGizmoMode(ELayoutGizmoMode::Scale));
	const FVector Pivot2 = T2.TransformPosition(FVector(0, 0, 50));
	const FVector LocalX = T2.GetRotation().GetAxisX();
	TestTrue(TEXT("Begin scale X"), C->BeginLayoutGizmoDrag(0, Above(Pivot2), Down));
	C->UpdateLayoutGizmoDrag(Above(Pivot2) + LocalX * 10.0, Down);
	C->EndLayoutDrag(true);
	TestTrue(TEXT("Thickness 0.2 -> 0.4 m, origin fixed"), FMath::IsNearlyEqual(Wall->GetSizeM().X, 0.4, 1e-9)
		&& Wall->GetActorLocation().Equals(T2.GetLocation(), 1e-9));
	const FTransform T3 = Wall->GetActorTransform();

	// A gizmo move into the rack is shown blocked and rejected on release; Esc-style cancel restores.
	// Fixture: the rack moves into the wall's Y band (x clear of it), so a pure X move then reaches it.
	Rack->SetActorLocation(FVector(0, T3.GetLocation().Y, 0));
	TestTrue(TEXT("Translate again"), C->SetLayoutGizmoMode(ELayoutGizmoMode::Translate));
	const FVector Pivot3 = T3.TransformPosition(FVector(0, 0, 50));
	TestTrue(TEXT("Begin move X"), C->BeginLayoutGizmoDrag(0, Above(Pivot3), Down));
	C->UpdateLayoutGizmoDrag(Above(Pivot3) - FVector(T3.GetLocation().X, 0, 0), Down);
	TestTrue(TEXT("Blocked live edit"), C->IsLayoutGizmoBlocked());
	C->EndLayoutDrag(true);
	TestTrue(TEXT("Blocked release restores, no history"), Wall->GetActorTransform().Equals(T3, 0.0) && C->GetLayoutUndoCount() == 5);
	TestTrue(TEXT("Begin cancel"), C->BeginLayoutGizmoDrag(1, Above(Pivot3), Down));
	C->UpdateLayoutGizmoDrag(Above(Pivot3) + FVector(0, 200, 0), Down);
	C->EndLayoutDrag(false);
	TestTrue(TEXT("Cancel restores, no history"), Wall->GetActorTransform().Equals(T3, 0.0) && C->GetLayoutUndoCount() == 5);

	// Undo / Redo of the three gizmo edits.
	TestTrue(TEXT("Undo scale/rotate/move"), C->TryUndoLayout() && C->TryUndoLayout() && C->TryUndoLayout()
		&& Wall->GetActorTransform().Equals(T0, 1e-9));
	TestTrue(TEXT("Redo all three"), C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout() && Wall->GetActorTransform().Equals(T3, 1e-9));

	// Delete -> Undo recreates the same name/transform and rebinds the earlier gizmo commands.
	const FString WallId = VrgBarrierLifecycle::Identity(Wall);
	const FName WallName = Wall->GetFName();
	C->SelectLayoutAlongRay(Above(Wall->GetActorLocation()), Down);
	TestTrue(TEXT("Delete one command"), C->TryDeleteLayoutBarrier() && CountBarriers(W) == 1 && C->GetLayoutUndoCount() == 6);
	TestTrue(TEXT("Undo delete"), C->TryUndoLayout());
	ALayoutBarrierActor* Restored = VrgBarrierLifecycle::Find(W, WallId);
	if (TestNotNull(TEXT("Restored wall"), Restored))
	{
		TestTrue(TEXT("Same name/transform/kind"), Restored->GetFName() == WallName && Restored->GetActorTransform().Equals(T3, 1e-9)
			&& Restored->Kind == EBarrierKind::Wall);
		TestTrue(TEXT("Earlier gizmo edit rebound"), C->TryUndoLayout() && Restored->GetActorTransform().Equals(T2, 1e-9) && C->TryRedoLayout());
	}
	TestTrue(TEXT("Redo delete"), C->TryRedoLayout() && CountBarriers(W) == 1 && C->TryUndoLayout() && CountBarriers(W) == 2);

	// Duplicate (Ctrl+D path): kind, rotation and size copied, fresh identity.
	Restored = VrgBarrierLifecycle::Find(W, WallId);
	C->SelectLayoutAlongRay(Above(Restored->GetActorLocation()), Down);
	TestTrue(TEXT("Begin duplicate"), C->BeginBarrierDuplication());
	C->UpdateRackAdditionAlongRay(Above(FVector(-500, -400, 0)), Down);
	ALayoutBarrierActor* Copy = C->ConfirmBarrierAddition();
	TestTrue(TEXT("Duplicate keeps size/rotation, new id"), Copy && Copy->GetSizeM().Equals(Restored->GetSizeM(), 1e-12)
		&& Copy->GetActorQuat().Equals(Restored->GetActorQuat(), 1e-12) && VrgBarrierLifecycle::Identity(Copy) != WallId && CountBarriers(W) == 3);

	// Layout file v2: the barrier list is the complete inventory; repeated loads do not duplicate.
	TArray<FTransform> Saved;
	for (TActorIterator<ALayoutBarrierActor> It(W); It; ++It) Saved.Add(It->GetActorTransform());
	FString Summary;
	TestTrue(TEXT("Save"), VrgLayoutFile::Save(W, TEXT("BarrierTest"), Summary));
	C->SelectLayoutAlongRay(Above(Copy->GetActorLocation()), Down);
	TestTrue(TEXT("Delete copy before load"), C->TryDeleteLayoutBarrier() && CountBarriers(W) == 2);
	TestTrue(TEXT("Load"), VrgLayoutFile::Load(W, TEXT("BarrierTest"), Summary));
	TestTrue(TEXT("Reload"), VrgLayoutFile::Load(W, TEXT("BarrierTest"), Summary));
	int32 Matched = 0;
	for (TActorIterator<ALayoutBarrierActor> It(W); It; ++It)
		for (const FTransform& T : Saved) if (It->GetActorTransform().Equals(T, 1e-6)) { ++Matched; break; }
	TestTrue(TEXT("Restored inventory, history cleared"), CountBarriers(W) == 3 && Matched == 3 && C->GetLayoutUndoCount() == 0);
	AddInfo(Summary);
	IFileManager::Get().Delete(*VrgLayoutFile::PathFor(TEXT("BarrierTest")));

	TestEqual(TEXT("Layout clock stays zero"), Sim->GetSnapshot().SimTimeS, 0.0);
	(void)Rack;
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W);
	return true;
}

// Barriers in the air grid: a 2 cm curtain across the room is watertight (two air regions), barrier cells next to a
// rack's air path stay open (ducts accepted), a closed enclosure behind a rack splits its duct and is reported, and
// with a valid wall the coupled run keeps the global ledger, divergence and pressure criteria.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBarrierAirTest, "VRG.Coupling.BarrierSolidsAndAirPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FBarrierAirTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), W)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);  // actor destruction needs a world context
	auto* Rack = W->SpawnActor<ARackThermalActor>(FVector::ZeroVector, FRotator::ZeroRotator);  // front (intake) faces +X
	Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
	Rack->Input = FRackThermalInput();
	Rack->RackSizeM = FVector(1.068, .6, 2.236);
	Rack->bShowReadout = false;
	Rack->ResetSim(); Rack->PauseSim();
	auto* Air = W->SpawnActor<AAirFieldDebugActor>();
	Air->FitToRacks();
	Air->ResetSim();
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	const FUniformAirGrid& G = Air->GetMutableSolver().GetGrid();
	const double H = G.GetCellSizeM();
	const FVector OriginM = Air->GetActorLocation() / 100.0;
	const FVector MaxM = OriginM + G.GetDomainSizeM();
	auto Spawn = [&](const TCHAR* Name, EBarrierKind Kind, const FVector& BaseM, double YawDeg, const FVector& SizeM)
	{
		// Synthetic geometry for the test, not a facility layout.
		return VrgBarrierLifecycle::Spawn(W, {Name, FName(Name), FTransform(FRotator(0, YawDeg, 0), BaseM * 100.0, SizeM), Kind});
	};
	auto Rebuild = [&]() { Sim->RebuildCoupling(); Sim->StartSim(); Sim->Tick(.02f); };
	auto Regions = [&]() { TArray<int32> L; return VrgAirConnectivity::LabelAirRegions(G, L); };

	// 1) Thin curtain across the whole room behind the exhaust region, square to X and then turned 10 degrees.
	const double RackBackM = -.5 * 1.068;
	const double CurtainX = OriginM.X + .5;
	// The curtain (one cell thick in the grid, +-0.06 m for the 10 degree turn near the rack) stays clear of the
	// one-cell air path kept open behind the rack.
	TestTrue(TEXT("Room leaves space behind the rack for the test curtain"), CurtainX + .51 * H + .06 < RackBackM - H);
	Rebuild();
	const int32 RegionsBefore = Regions();
	auto* Curtain = Spawn(TEXT("TestCurtain"), EBarrierKind::Curtain, FVector(CurtainX, .5 * (OriginM.Y + MaxM.Y), 0.0), 0.0,
		FVector(.02, 2.0 * (MaxM.Y - OriginM.Y), 2.0 * (MaxM.Z - OriginM.Z)));
	Rebuild();
	const int32 CurtainCells = Sim->GetNumBarrierCells(), RegionsSquare = Regions();
	Curtain->SetActorRotation(FRotator(0, 10, 0));
	Rebuild();
	const int32 RegionsTurned = Regions();
	AddInfo(FString::Printf(TEXT("셀 %.3f m | 커튼 셀 %d | 공기 영역 %d -> 직각 %d / 10도 %d | 갈린 덕트 %d"),
		H, CurtainCells, RegionsBefore, RegionsSquare, RegionsTurned, Sim->GetNumSplitDucts()));
	TestEqual(TEXT("One air region without barriers"), RegionsBefore, 1);
	TestTrue(TEXT("2 cm curtain marks cells"), CurtainCells > 0);
	TestEqual(TEXT("Square curtain is watertight"), RegionsSquare, 2);
	TestEqual(TEXT("Turned curtain is watertight"), RegionsTurned, 2);
	TestEqual(TEXT("No duct split by the curtain"), Sim->GetNumSplitDucts(), 0);
	Curtain->Destroy();

	// 2) A wall flush against the rack front: cells on the rack's air path stay open, ducts are accepted.
	Spawn(TEXT("FrontWall"), EBarrierKind::Wall, FVector(.5 * 1.068 + .1, 0, 0), 0.0, FVector(.2, 1.4, 2.4));
	Rebuild();
	const int32 KeptOpen = Sim->GetNumBarrierCellsKeptOpen();
	const bool bDuctsRejected = Air->GetMutableSolver().WereDuctsRejected();
	TestTrue(TEXT("Cells in front of the rack kept open"), KeptOpen > 0);
	TestFalse(TEXT("Ducts accepted"), bDuctsRejected);
	TestEqual(TEXT("Front wall does not split the duct"), Sim->GetNumSplitDucts(), 0);

	// Coupled run with this wall: ledger, divergence and pressure criteria of the existing tests.
	for (int32 Frame = 0; Frame < 25; ++Frame) Sim->Tick(.32f);  // 8 s
	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	const FAirSolverSnapshot AirSnap = Air->GetMutableSolver().MakeSnapshot(Snap.SimTimeS);
	AddInfo(FString::Printf(TEXT("앞벽 셀 %d / 비운 셀 %d | t=%.2f s | 전역 잔차 %.3e | 발산 %.3e | 압력 %s"),
		Sim->GetNumBarrierCells(), KeptOpen, Snap.SimTimeS, Snap.NormalizedEnergyResidual, AirSnap.NormalizedDivergence,
		AirSnap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
	TestTrue(TEXT("Global ledger closes"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("Divergence free"), AirSnap.NormalizedDivergence < 1e-5);
	TestTrue(TEXT("Pressure converged"), AirSnap.bPressureConverged);
	for (TActorIterator<ALayoutBarrierActor> It(W); It; ++It) It->Destroy();

	// 3) A sealed pocket behind the rack (three full-height walls touching the rack sides): the exhaust region is
	//    cut off from the intake, which has no pressure solution. Reported at setup. Ceiling-coil mode: automatic
	//    in-row coolers at the row ends would draw from the pocket (their kept-open intake path), which is a
	//    legitimate return path, not a sealed pocket.
	Sim->CrahLayout = ECrahLayout::CeilingCoil;
	const double Top = 2.0 * (MaxM.Z - OriginM.Z);
	Spawn(TEXT("BackWall"), EBarrierKind::Wall, FVector(RackBackM - .8, 0, 0), 0.0, FVector(.2, 1.2, Top));
	// Side walls stand clear of the rack's width (its one-cell air path stays open) and reach the cap over the rack.
	Spawn(TEXT("SideWallA"), EBarrierKind::Wall, FVector(RackBackM - .256, .55, 0), 0.0, FVector(1.4, .2, Top));
	Spawn(TEXT("SideWallB"), EBarrierKind::Wall, FVector(RackBackM - .256, -.55, 0), 0.0, FVector(1.4, .2, Top));
	// Cap over the rack top: otherwise air returns over the rack from the pocket to the front.
	Spawn(TEXT("CapWall"), EBarrierKind::Wall, FVector(0, 0, 2.236), 0.0, FVector(.2, 1.2, Top));
	Rebuild();
	AddInfo(FString::Printf(TEXT("뒤쪽 밀폐: 공기 영역 %d | 갈린 덕트 %d"), Regions(), Sim->GetNumSplitDucts()));
	TestTrue(TEXT("Sealed exhaust pocket splits ducts"), Sim->GetNumSplitDucts() > 0);

	W->DestroyWorld(false); GEngine->DestroyWorldContext(W);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
