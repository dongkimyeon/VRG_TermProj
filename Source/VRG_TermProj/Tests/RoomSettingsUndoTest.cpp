#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomSettingsUndoTest, "VRG.Interaction.Layout.RoomSettingsUndoRedo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomSettingsUndoTest::RunTest(const FString& Parameters)
{
	UWorld* W = UWorld::CreateWorld(EWorldType::Game, false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
	auto* Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
	auto* C = W->SpawnActor<ADataCenterDebugController>(); auto* Air = W->SpawnActor<AAirFieldDebugActor>();
	auto* Room = W->SpawnActor<ADataCenterRoomActor>();
	W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
	ARackThermalActor* Racks[2];
	for (int32 I = 0; I < 2; ++I) { Racks[I] = W->SpawnActor<ARackThermalActor>(FVector(I == 0 ? -150 : 150, 0, 0), FRotator(0, I == 0 ? 0 : 180, 0)); Racks[I]->ApplyGb200Nvl72Preset(); }
	Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double ReferenceItW = Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	const auto Warm = Sim->GetSnapshot(); const auto WarmTemps = Air->GetMutableSolver().GetGrid().TemperatureK;
	const FVector Origin = Air->GetActorLocation(); const int32 Pipes = Room->GetMutableNetwork().GetNumPipes();
	const FTransform OriginalPose = Racks[0]->GetActorTransform();
	const auto Initial = Sim->GetRoomAirDomain(); FString Message;
	// Synthetic domain geometry, same fixture family as RoomAirDomainCoupling; no new hardware constants.
	FRoomAirDomainSettings A; A.bManual = true; A.MinXYM = FVector2D(-5.5, -4.5); A.MaxXYM = FVector2D(5.1, 4.6); A.CeilingHeightM = 3.1;
	auto B = A; B.MinXYM = FVector2D(-6.5, -5.5); B.MaxXYM = FVector2D(7.1, 5.6); B.CeilingHeightM = 3.6;
	auto Apply = [&](const FRoomAirDomainSettings& Settings)
	{
		if (!C->OpenRoomSettings()) return false;
		const bool Ok = C->ApplyRoomSettings(Settings, Message); C->CloseRoomSettings(); return Ok;
	};
	auto UnchangedLive = [&]()
	{
		const auto S = Sim->GetSnapshot();
		return Air->GetActorLocation() == Origin && Air->GetMutableSolver().GetGrid().TemperatureK == WarmTemps
			&& S.SimTimeS == Warm.SimTimeS && S.EnergyInJ == Warm.EnergyInJ && S.EnergyOutJ == Warm.EnergyOutJ
			&& Room->GetMutableNetwork().GetNumPipes() == Pipes;
	};
	TestTrue(TEXT("Automatic unchanged apply does not create record"), Apply(C->GetRoomSettingsDraft()) && C->GetLayoutUndoCount() == 0);
	TestTrue(TEXT("Two room changes are two shared records"), Apply(A) && Apply(B) && C->GetLayoutUndoCount() == 2 && UnchangedLive());
	TestTrue(TEXT("Undo/Redo restores whole room settings without touching live air/clock/rows"), C->TryUndoLayout() && Sim->GetRoomAirDomain() == A
		&& C->TryRedoLayout() && Sim->GetRoomAirDomain() == B && UnchangedLive());
	C->OpenRoomSettings(); TestFalse(TEXT("Global Undo blocked while numeric modal is open"), C->TryUndoLayout()); C->CloseRoomSettings();
	// B contains this newly positioned rack and its ducts; A does not. Do not relax inclusion on Undo.
	Racks[0]->SetActorLocation(FVector(520, 0, 0));
	TestFalse(TEXT("Current equipment excludes Undo room; entire restoration rejected"), C->TryUndoLayout());
	TestTrue(TEXT("Rejected Undo preserves setting/stacks and warm physical state"), Sim->GetRoomAirDomain() == B
		&& C->GetLayoutUndoCount() == 2 && C->GetLayoutRedoCount() == 0 && UnchangedLive());
	Racks[0]->SetActorTransform(OriginalPose); TestTrue(TEXT("Undo succeeds after removing obstruction"), C->TryUndoLayout());
	auto* Outside = W->SpawnActor<ACoolingDistributionUnitActor>(FVector(900, 0, 0), FRotator::ZeroRotator);
	TestFalse(TEXT("Current equipment excludes Redo room; restoration rejected"), C->TryRedoLayout());
	TestTrue(TEXT("Rejected Redo preserves both stacks and live state"), Sim->GetRoomAirDomain() == A && C->GetLayoutUndoCount() == 1 && C->GetLayoutRedoCount() == 1 && UnchangedLive());
	Outside->Destroy(false, false);
	auto External = A; External.CeilingHeightM += .1; Sim->TrySetRoomAirDomain(External, Message);
	TestFalse(TEXT("Out-of-history setting cannot be overwritten by stale command"), C->TryRedoLayout());
	TestTrue(TEXT("Stale rejection retains setting and record"), Sim->GetRoomAirDomain() == External && C->GetLayoutRedoCount() == 1);
	Sim->TrySetRoomAirDomain(A, Message);
	TestTrue(TEXT("Valid Redo reuses same setting values"), C->TryRedoLayout() && Sim->GetRoomAirDomain() == B && UnchangedLive());
	Sim->EnterSimulationMode(); TestFalse(TEXT("Simulation blocks history"), C->TryUndoLayout());
	TestTrue(TEXT("Mode entry uses B outward grid counts"), Air->GridParams.NumCellsX == 55 && Air->GridParams.NumCellsY == 45 && Air->GridParams.NumCellsZ == 15);
	Sim->EnterLayoutMode(); TestTrue(TEXT("Mode round trip retains shared records"), C->GetLayoutUndoCount() == 2 && C->TryUndoLayout() && Sim->GetRoomAirDomain() == A);
	C->SetPatchMapOpen(true); TestFalse(TEXT("Patch blocks history"), C->TryRedoLayout()); C->SetPatchMapOpen(false);
	TestTrue(TEXT("All Undo restores original automatic mode"), C->TryUndoLayout() && Sim->GetRoomAirDomain() == Initial);
	TestTrue(TEXT("All Redo restores final manual mode"), C->TryRedoLayout() && C->TryRedoLayout() && Sim->GetRoomAirDomain() == B);
	// Interleave a room change with existing equipment movement and rotation in the SAME stack.
	C->ClearLayoutHistory(); const FVector Eye = Racks[0]->GetActorLocation() + FVector(0, 0, 500), Down(0, 0, -1);
	C->BeginLayoutDragAlongRay(Eye, Down); C->UpdateLayoutDragAlongRay(Eye + FVector(0, 100, 0), Down, true); C->EndLayoutDrag(true);
	const FTransform Moved = Racks[0]->GetActorTransform(); TestTrue(TEXT("Existing movement command recorded"), C->GetLayoutUndoCount() == 1 && !Moved.Equals(OriginalPose));
	TestTrue(TEXT("Room command interleaves with move"), Apply(A));
	TestTrue(TEXT("Existing rotation command interleaves with room"), C->TryLayoutRotation(Racks[0], FRotator(0, 90, 0), true));
	const FTransform Rotated = Racks[0]->GetActorTransform();
	TestTrue(TEXT("Reverse order rotation/room/move restores original pose/settings"), C->TryUndoLayout() && Racks[0]->GetActorTransform().Equals(Moved)
		&& C->TryUndoLayout() && Sim->GetRoomAirDomain() == B && C->TryUndoLayout() && Racks[0]->GetActorTransform().Equals(OriginalPose));
	TestTrue(TEXT("Forward order move/room/rotation restores final pose/settings"), C->TryRedoLayout() && C->TryRedoLayout() && C->TryRedoLayout()
		&& Racks[0]->GetActorTransform().Equals(Rotated) && Sim->GetRoomAirDomain() == A);
	TestTrue(TEXT("Populate both stacks before existing Load"), C->TryUndoLayout() && C->GetLayoutUndoCount() == 2 && C->GetLayoutRedoCount() == 1);
	const FString File = TEXT("AutomationRoomSettingsHistory");
	TestTrue(TEXT("Existing Save/Load works without extending room persistence"), VrgLayoutFile::Save(W, File, Message)
		&& VrgLayoutFile::Load(W, File, Message) && VrgLayoutFile::Load(W, File, Message));
	TestTrue(TEXT("Existing Load clears room and equipment records but keeps current room setting"), C->GetLayoutUndoCount() == 0 && C->GetLayoutRedoCount() == 0
		&& Sim->GetRoomAirDomain() == A && !C->TryUndoLayout() && !C->TryRedoLayout());
	FRoomAirDomainSettings EvictedState;
	for (int32 I = 0; I < 105; ++I)
	{
		auto S = A; S.CeilingHeightM = I % 2 ? 3.3 : 3.2;
		TestTrue(TEXT("Valid room edit for shared 100-command cap"), Apply(S));
		TestTrue(TEXT("Shared cap never exceeds100"), C->GetLayoutUndoCount() <= 100);
		if (I == 4) EvictedState = Sim->GetRoomAirDomain();
	}
	for (int32 I = 0; I < 100; ++I) TestTrue(TEXT("Undo retained command"), C->TryUndoLayout());
	TestTrue(TEXT("Five evicted commands cannot be undone"), Sim->GetRoomAirDomain() == EvictedState && !C->TryUndoLayout() && C->GetLayoutRedoCount() == 100);
	for (int32 I = 0; I < 100; ++I) TestTrue(TEXT("Redo retained command"), C->TryRedoLayout());
	TestTrue(TEXT("Undo once to populate Redo"), C->TryUndoLayout()); const auto Current = Sim->GetRoomAirDomain();
	TestTrue(TEXT("No-op apply preserves Redo"), Apply(Current) && C->GetLayoutRedoCount() == 1);
	auto Invalid = Current; Invalid.CeilingHeightM = 0;
	TestFalse(TEXT("Rejected apply is not recorded"), Apply(Invalid));
	C->OpenRoomSettings(); C->CloseRoomSettings();
	TestTrue(TEXT("Rejected apply and closed draft preserve stacks"), C->GetLayoutUndoCount() == 99 && C->GetLayoutRedoCount() == 1);
	auto NewEdit = Current; NewEdit.CeilingHeightM = 3.4;
	TestTrue(TEXT("New valid edit clears Redo"), Apply(NewEdit) && C->GetLayoutRedoCount() == 0 && C->GetLayoutUndoCount() == 100);
	// Confirm restored room settings still feed the original coupled solver path, not a copied live state.
	Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First = Sim->GetSnapshot();
	for (int32 I = 0; I < 25; ++I) Sim->Tick(.04f);
	const auto S = Sim->GetSnapshot(); const auto AS = Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
	TestTrue(TEXT("Fresh run preserves IT within existing criterion"), First.SimTimeS == Sim->FixedStepS && FMath::Abs(First.TotalItHeatW - ReferenceItW) / ReferenceItW < 1e-9);
	TestTrue(TEXT("Existing energy/pressure/hydraulic and duct criteria after shared room history"), FMath::Abs(S.NormalizedEnergyResidual) < 1e-9
		&& S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence < 1e-5 && S.bHydraulicsConverged && !AS.bDuctsRejected && S.CrahCoilRemovedW > 0);
	for (auto* Rack : Racks) for (double K : Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing 15..35C transient inlet safety net"), K > 288.15 && K < 308.15);
	AddInfo(FString::Printf(TEXT("Room history: shared cap100/105 edits; full settings and pose restored, rejected state unchanged, existing Load clears stacks; IT relative %.3e, global %.3e, normalized pressure %.3e at %.2fs"),
		FMath::Abs(First.TotalItHeatW - ReferenceItW) / ReferenceItW, S.NormalizedEnergyResidual, AS.NormalizedDivergence, S.SimTimeS));
	W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
