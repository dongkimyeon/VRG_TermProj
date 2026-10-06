#include "Misc/AutomationTest.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FRoomFileFixture
	{
		UWorld* W; ADataCenterSimulationActor* Sim; AAirFieldDebugActor* Air;
		ADataCenterDebugController* C; ADataCenterRoomActor* Room; ARackThermalActor* Racks[2];
		FRoomFileFixture()
		{
			W = UWorld::CreateWorld(EWorldType::Game, false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
			Sim = W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
			Air = W->SpawnActor<AAirFieldDebugActor>(); Room = W->SpawnActor<ADataCenterRoomActor>();
			C = W->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart = false;
			FActorSpawnParameters P; P.Name = TEXT("RoomFileCdu");
			W->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(), FVector(-400, -200, 0), FRotator::ZeroRotator, P);
			for (int32 I = 0; I < 2; ++I)
			{
				P.Name = I == 0 ? TEXT("RoomFileRackA") : TEXT("RoomFileRackB");
				Racks[I] = W->SpawnActor<ARackThermalActor>(ARackThermalActor::StaticClass(), FVector(I == 0 ? -150 : 150, 0, 0), FRotator(0, I == 0 ? 0 : 180, 0), P);
				Racks[I]->ApplyGb200Nvl72Preset();
			}
			Sim->EnterLayoutMode();
		}
		~FRoomFileFixture() { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); }
	};
	TSharedPtr<FJsonObject> ReadRoomFile(const FString& Name)
	{
		FString Text; FFileHelper::LoadFileToString(Text, *VrgLayoutFile::PathFor(Name)); TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root); return Root;
	}
	void WriteRoomFile(const FString& Name, const TSharedPtr<FJsonObject>& Root)
	{
		FString Text; FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		FFileHelper::SaveStringToFile(Text, *VrgLayoutFile::PathFor(Name));
	}
	FRoomAirDomainSettings RoomFileDomain()
	{
		// Synthetic geometry reused from RoomAirDomainCoupling; not measured facility dimensions/physics constants.
		FRoomAirDomainSettings D; D.bManual = true; D.MinXYM = FVector2D(-5.5, -4.5); D.MaxXYM = FVector2D(5.1, 4.6); D.CeilingHeightM = 3.1; return D;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomSettingsPersistenceTest, "VRG.Interaction.Layout.RoomSettingsPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomSettingsPersistenceTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual = RoomFileDomain();
	const FString File = TEXT("AutomationRoomFile"), AutoFile = File + TEXT("Auto"), BadFile = File + TEXT("Bad"), OldFile = File + TEXT("Old");
	TestTrue(TEXT("Save automatic including inactive metre values"), VrgLayoutFile::Save(F.W, AutoFile, Message));
	TestTrue(TEXT("Set manual then save optional v2 room"), F.Sim->TrySetRoomAirDomain(Manual, Message));
	// Synthetic barrier position/size in the fixture; no measured facility geometry or material constants.
	const FLayoutBarrierRecord Wall{TEXT("room-file-wall"), TEXT("RoomFileWall"), FTransform(FRotator::ZeroRotator, FVector(-480, 200, 0), FVector(.2, 1, 2)), EBarrierKind::Wall};
	TestNotNull(TEXT("Wall fixture"), VrgBarrierLifecycle::Spawn(F.W, Wall));
	TestTrue(TEXT("Save manual and existing wall inventory"), VrgLayoutFile::Save(F.W, File, Message));
	const auto Root = ReadRoomFile(File), Domain = Root->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"));
	TestTrue(TEXT("All five metre values serialized, no integral or clock fields"), Domain->GetNumberField(TEXT("minXM")) == Manual.MinXYM.X
		&& Domain->GetNumberField(TEXT("maxYM")) == Manual.MaxXYM.Y && Domain->Values.Num() == 6 && Root->GetIntegerField(TEXT("version")) == 2);
	// Current geometry/cell size would reject this file's room; FINAL file geometry/cell size must be used.
	F.Racks[0]->SetActorLocation(FVector(900, 0, 0)); F.Air->GridParams.CellSizeM = .05;
	TestTrue(TEXT("Load validates restored layout, not current outside rack/fine cell size"), VrgLayoutFile::Load(F.W, File, Message));
	TestTrue(TEXT("Room/rack/cell size restored and history empty"), F.Sim->GetRoomAirDomain() == Manual && F.Racks[0]->GetActorLocation().X == -150
		&& F.Air->GridParams.CellSizeM == .25 && F.C->GetLayoutUndoCount() == 0 && F.C->GetLayoutRedoCount() == 0);
	for (int32 I = 0; I < 2; ++I) TestTrue(TEXT("Repeated load preserves room and single wall identity"), VrgLayoutFile::Load(F.W, File, Message)
		&& F.Sim->GetRoomAirDomain() == Manual && VrgBarrierLifecycle::Find(F.W, Wall.Id)->GetActorTransform().Equals(Wall.Transform));
	{
		FRoomFileFixture New;
		TestTrue(TEXT("New world repeated load restores manual and barriers"), VrgLayoutFile::Load(New.W, File, Message) && VrgLayoutFile::Load(New.W, File, Message)
			&& New.Sim->GetRoomAirDomain() == Manual && VrgBarrierLifecycle::Find(New.W, Wall.Id));
		TestTrue(TEXT("New world automatic file clears manual, removes omitted walls"), VrgLayoutFile::Load(New.W, AutoFile, Message)
			&& New.Sim->GetRoomAirDomain() == FRoomAirDomainSettings{} && !VrgBarrierLifecycle::Find(New.W, Wall.Id));
	}
	// Missing room field preserves current settings in both old v2 and move-only v1 files.
	auto Legacy = ReadRoomFile(File); Legacy->GetObjectField(TEXT("settings"))->RemoveField(TEXT("roomAirDomain"));
	auto Current = Manual; Current.CeilingHeightM = 3.4; F.Sim->TrySetRoomAirDomain(Current, Message);
	for (int32 Version : {2, 1})
	{
		Legacy->SetNumberField(TEXT("version"), Version); WriteRoomFile(OldFile, Legacy);
		TestTrue(TEXT("Absent field old file retains current room"), VrgLayoutFile::Load(F.W, OldFile, Message) && F.Sim->GetRoomAirDomain() == Current);
	}
	F.C->OpenRoomSettings(); auto Edited = Current; Edited.CeilingHeightM = 3.5; F.C->ApplyRoomSettings(Edited, Message); F.C->CloseRoomSettings();
	F.C->TryLayoutRotation(F.Racks[0], FRotator(0, 90, 0), true); F.C->TryUndoLayout();
	const int32 Undo = F.C->GetLayoutUndoCount(), Redo = F.C->GetLayoutRedoCount();
	const FTransform Pose = F.Racks[0]->GetActorTransform(); auto* BarrierBefore = VrgBarrierLifecycle::Find(F.W, Wall.Id);
	const auto AirBefore = F.Air->GridParams; const auto GridBefore = F.Air->GetMutableSolver().GetGrid().TemperatureK;
	auto Reject = [&](const TSharedPtr<FJsonObject>& Bad)
	{
		WriteRoomFile(BadFile, Bad); TestFalse(TEXT("Invalid final room rejects Load"), VrgLayoutFile::Load(F.W, BadFile, Message));
		TestTrue(TEXT("Reject keeps live actors, barriers, settings, history and grid"), F.Racks[0]->GetActorTransform().Equals(Pose)
			&& VrgBarrierLifecycle::Find(F.W, Wall.Id) == BarrierBefore && F.Sim->GetRoomAirDomain() == Edited
			&& F.C->GetLayoutUndoCount() == Undo && F.C->GetLayoutRedoCount() == Redo && F.Air->GridParams.CellSizeM == AirBefore.CellSizeM
			&& F.Air->GetMutableSolver().GetGrid().TemperatureK == GridBefore);
	};
	auto Bad = ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"))->SetNumberField(TEXT("ceilingHeightM"), 0); Reject(Bad);
	Bad = ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"))->RemoveField(TEXT("manual")); Reject(Bad);
	Bad = ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->SetNumberField(TEXT("cellSizeM"), .05); Reject(Bad);
	Bad = ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"))->SetNumberField(TEXT("maxYM"), .5); Reject(Bad); // Automatic CRAH/duct envelope.
	Bad = ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"))->SetNumberField(TEXT("maxXM"), 2.11); Reject(Bad); // Rack duct, body still fits.
	Bad = ReadRoomFile(File); Bad->GetArrayField(TEXT("equipment"))[0]->AsObject()->SetNumberField(TEXT("x_m"), 12); Reject(Bad);
	TestTrue(TEXT("Valid load clears BOTH history stacks, room values only until entry"), VrgLayoutFile::Load(F.W, File, Message)
		&& F.C->GetLayoutUndoCount() == 0 && F.C->GetLayoutRedoCount() == 0 && F.Sim->GetRoomAirDomain() == Manual
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK == GridBefore);
	F.Sim->EnterSimulationMode();
	TestTrue(TEXT("Next simulation entry applies outward planned grid"), F.Air->GridParams.NumCellsX == 43 && F.Air->GridParams.NumCellsY == 37 && F.Air->GridParams.NumCellsZ == 13
		&& F.Air->GetActorLocation().Equals(FVector(-550, -450, 0)));
	AddInfo(TEXT("Room v2: auto/manual exact metre restoration; old v1/v2 preserve settings; final-layout rejection before live mutation; 43x37x13=20683 cells at h0.25m; history0/0 only on success."));
	for (const auto& Name : {File, AutoFile, BadFile, OldFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingPersistenceTest, "VRG.Interaction.Layout.RoomCeilingPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingPersistenceTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual = RoomFileDomain();
	const FString File=TEXT("AutomationCeilingFile"), Empty=File+TEXT("Empty"), Old=File+TEXT("Old"), BadFile=File+TEXT("Bad");
	TestTrue(TEXT("Save explicit empty automatic list"),VrgLayoutFile::Save(F.W,Empty,Message));
	TestTrue(TEXT("Manual room"),F.Sim->TrySetRoomAirDomain(Manual,Message));
	// Synthetic corner geometry from the existing ceiling fixture, not a facility dimension/rating.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63;
	auto Second=P; Second.MinXYM.X+=1; Second.MaxXYM.X+=1;
	const TArray<FRoomCeilingPlate> Saved{P,Second};
	const FLayoutBarrierRecord Wall{TEXT("ceiling-file-wall"),TEXT("CeilingFileWall"),FTransform(FRotator::ZeroRotator,FVector(-480,200,0),FVector(.2,1,2)),EBarrierKind::Wall};
	TestNotNull(TEXT("Existing barrier inventory"),VrgBarrierLifecycle::Spawn(F.W,Wall));
	TestTrue(TEXT("Save complete authored ceiling list"),F.Sim->TrySetRoomCeilingPlates(Saved,Message) && VrgLayoutFile::Save(F.W,File,Message));
	const auto Root=ReadRoomFile(File); const auto Values=Root->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"));
	TestTrue(TEXT("v2 stores exactly five metre values per plate, no thermal state"),Root->GetIntegerField(TEXT("version"))==2 && Values.Num()==2
		&& Values[0]->AsObject()->Values.Num()==5 && Values[0]->AsObject()->GetNumberField(TEXT("minXM"))==P.MinXYM.X
		&& Values[0]->AsObject()->GetNumberField(TEXT("maxYM"))==P.MaxXYM.Y && Values[0]->AsObject()->GetNumberField(TEXT("heightM"))==P.HeightM);
	F.Sim->TrySetRoomCeilingPlates({},Message); F.Racks[0]->SetActorLocation(FVector(900,0,0)); F.Air->GridParams.CellSizeM=.05;
	TestTrue(TEXT("Final saved equipment/grid/list validated instead of current world"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==Saved && F.Racks[0]->GetActorLocation().X==-150 && F.Air->GridParams.CellSizeM==.25);
	for(int32 I=0;I<2;++I) TestTrue(TEXT("Repeated Load replaces, never appends"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==Saved);
	{
		FRoomFileFixture New;
		for(int32 I=0;I<2;++I) TestTrue(TEXT("New world restores room/wall/ceiling list without duplication"),VrgLayoutFile::Load(New.W,File,Message)
			&& New.Sim->GetRoomAirDomain()==Manual && New.Sim->GetRoomCeilingPlates()==Saved && VrgBarrierLifecycle::Find(New.W,Wall.Id));
		TestTrue(TEXT("Explicit empty list commits with automatic room despite old plates"),VrgLayoutFile::Load(New.W,Empty,Message)
			&& !New.Sim->GetRoomAirDomain().bManual && New.Sim->GetRoomCeilingPlates().IsEmpty());
	}
	auto Current=P; Current.HeightM=2.53; F.Sim->TrySetRoomCeilingPlates({Current},Message);
	for(int32 Version:{2,1})
	{
		auto Legacy=ReadRoomFile(File); Legacy->SetNumberField(TEXT("version"),Version); Legacy->GetObjectField(TEXT("settings"))->RemoveField(TEXT("roomCeilingPlates")); WriteRoomFile(Old,Legacy);
		TestTrue(TEXT("Absent optional field preserves current list in v1/v2"),VrgLayoutFile::Load(F.W,Old,Message) && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Current});
	}
	auto Edited=Manual; Edited.CeilingHeightM=3.2; F.C->OpenRoomSettings(); F.C->ApplyRoomSettings(Edited,Message); F.C->CloseRoomSettings();
	F.C->TryLayoutRotation(F.Racks[0],FRotator(0,90,0),true); F.C->TryUndoLayout();
	const int32 Undo=F.C->GetLayoutUndoCount(),Redo=F.C->GetLayoutRedoCount(); const auto Pose=F.Racks[0]->GetActorTransform();
	auto* WallBefore=VrgBarrierLifecycle::Find(F.W,Wall.Id); const auto GridBefore=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	auto Reject=[&](const TSharedPtr<FJsonObject>& Bad)
	{
		WriteRoomFile(BadFile,Bad); TestFalse(TEXT("Malformed/invalid plate list rejects whole Load"),VrgLayoutFile::Load(F.W,BadFile,Message));
		TestTrue(TEXT("Refusal retains actors/wall/room/plates/grid/BOTH history stacks"),F.Racks[0]->GetActorTransform().Equals(Pose)
			&& VrgBarrierLifecycle::Find(F.W,Wall.Id)==WallBefore && F.Sim->GetRoomAirDomain()==Edited
			&& F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Current} && F.C->GetLayoutUndoCount()==Undo && F.C->GetLayoutRedoCount()==Redo
			&& F.Air->GetMutableSolver().GetGrid().TemperatureK==GridBefore);
	};
	auto Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->SetStringField(TEXT("roomCeilingPlates"),TEXT("bad")); Reject(Bad);
	Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->SetArrayField(TEXT("roomCeilingPlates"),{MakeShared<FJsonValueNumber>(1)}); Reject(Bad);
	Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->RemoveField(TEXT("heightM")); Reject(Bad);
	Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->SetNumberField(TEXT("heightM"),0); Reject(Bad);
	Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("roomAirDomain"))->SetBoolField(TEXT("manual"),false); Reject(Bad);
	TestTrue(TEXT("Valid Load alone clears both stacks and restores saved list"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==Saved && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK==GridBefore);
	F.Sim->EnterSimulationMode(); TestEqual(TEXT("Next entry marks both nine-cell plates"),F.Sim->GetNumCeilingCells(),18);
	AddInfo(TEXT("Ceiling optional v2: exact two-plate metre list; repeated/new-world replacement, [] clears with auto, absent v1/v2 retains; malformed/geometry failures preserve state/history; next-entry18cells at h0.25m."));
	for(const auto& Name:{File,Empty,Old,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomFileCouplingTest, "VRG.Interaction.Layout.RoomPersistenceRollbackCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomFileCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual = RoomFileDomain();
	const FString File = TEXT("AutomationRoomCoupled"), FailureFile = File + TEXT("Failure");
	TestTrue(TEXT("Manual room"), F.Sim->TrySetRoomAirDomain(Manual, Message));
	// Same synthetic 0.2x1x2m geometry as the persistence fixture; not a physical curtain product rating.
	const FLayoutBarrierRecord R{TEXT("room-coupled-wall"), TEXT("RoomCoupledWall"), FTransform(FRotator::ZeroRotator, FVector(-480, 200, 0), FVector(.2, 1, 2)), EBarrierKind::Curtain};
	auto* Wall = VrgBarrierLifecycle::Spawn(F.W, R);
	if (!TestNotNull(TEXT("Barrier"), Wall)) return false;
	const FVector Down(0, 0, -1), Pivot = Wall->GetActorTransform().TransformPosition(FVector(0, 0, 50));
	F.C->SelectLayoutAlongRay(Wall->GetActorLocation() + FVector(0, 0, 1000), Down);
	F.C->SetLayoutGizmoMode(ELayoutGizmoMode::Translate);
	TestTrue(TEXT("Wall move record"), F.C->BeginLayoutGizmoDrag(1, Pivot + FVector(0, 0, 1000), Down));
	F.C->UpdateLayoutGizmoDrag(Pivot + FVector(0, 100, 1000), Down); F.C->EndLayoutDrag(true);
	const FTransform MovedWall = Wall->GetActorTransform();
	auto Edited = Manual; Edited.CeilingHeightM = 3.2;
	F.C->OpenRoomSettings(); F.C->ApplyRoomSettings(Edited, Message); F.C->CloseRoomSettings();
	F.C->TryLayoutRotation(F.Racks[0], FRotator(0, 90, 0), true); F.C->TryUndoLayout();
	TestTrue(TEXT("Mixed history fixture has BOTH stacks"), F.C->GetLayoutUndoCount() == 2 && F.C->GetLayoutRedoCount() == 1);
	F.C->SelectLayoutAlongRay(Wall->GetActorLocation() + FVector(0, 0, 1000), Down);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f);
	const double ReferenceItW = F.Sim->GetSnapshot().TotalItHeatW;
	for (int32 I = 0; I < 25; ++I) F.Sim->Tick(.04f);
	F.Sim->EnterLayoutMode(); const auto Warm = F.Sim->GetSnapshot();
	const auto Temperatures = F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Solid = F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const FVector Origin = F.Air->GetActorLocation(); const int32 Pipes = F.Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Save warm layout writes authored settings only"), VrgLayoutFile::Save(F.W, File, Message));
	// A valid file needs one new rack. Inject failure AFTER barrier replacement, then require old move refs.
	auto* Extra = VrgRackLifecycle::Spawn(F.W, TEXT("RoomFileExtra"), TEXT("room-file-extra"), ARackThermalActor::StaticClass(),
		FTransform(FRotator::ZeroRotator, FVector(-150, 150, 0)), F.Racks[0]->CaptureLayoutSettings());
	if (!TestNotNull(TEXT("Extra file rack"), Extra)) return false;
	TestTrue(TEXT("Save pending three-rack inventory"), VrgLayoutFile::Save(F.W, FailureFile, Message)); VrgRackLifecycle::Remove(Extra);
	bool bInjected = false;
	const FDelegateHandle Hook = F.W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* A)
	{
		if (Cast<ARackThermalActor>(A) && A->GetName().StartsWith(TEXT("PendingLayoutEquipment"))) { bInjected = true; A->Destroy(false, false); }
	}));
	TestFalse(TEXT("Middle equipment creation failure rolls back entire Load"), VrgLayoutFile::Load(F.W, FailureFile, Message));
	F.W->RemoveOnActorSpawnedHandler(Hook);
	TestTrue(TEXT("Failure was actually injected, not a room rejection"), bInjected);
	const auto After = F.Sim->GetSnapshot();
	auto* RestoredWall = VrgBarrierLifecycle::Find(F.W, R.Id);
	TestTrue(TEXT("Warm failure keeps setting/grid/clock/energy/routing/history and no extra rack"), RestoredWall && RestoredWall->GetActorTransform().Equals(MovedWall)
		&& !VrgRackLifecycle::Find(F.W, TEXT("room-file-extra")) && F.Sim->GetRoomAirDomain() == Edited
		&& F.C->GetLayoutUndoCount() == 2 && F.C->GetLayoutRedoCount() == 1 && After.SimTimeS == Warm.SimTimeS
		&& After.EnergyInJ == Warm.EnergyInJ && After.EnergyOutJ == Warm.EnergyOutJ && After.EnergyStoredJ == Warm.EnergyStoredJ
		&& F.C->GetSelectedLayoutActor() == RestoredWall
		&& F.Air->GetActorLocation() == Origin && F.Air->GetMutableSolver().GetGrid().TemperatureK == Temperatures
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell == Solid && F.Room->GetMutableNetwork().GetNumPipes() == Pipes);
	TestTrue(TEXT("Preserved room record still undoes"), F.C->TryUndoLayout() && F.Sim->GetRoomAirDomain() == Manual);
	TestTrue(TEXT("Preserved wall move record rebinds to rollback wall"), F.C->TryUndoLayout()
		&& VrgBarrierLifecycle::Find(F.W, R.Id)->GetActorTransform().Equals(R.Transform));
	TestTrue(TEXT("Preserved wall/room/R Redo remains usable"), F.C->TryRedoLayout() && F.C->TryRedoLayout() && F.C->TryRedoLayout());
	for (int32 I = 0; I < 2; ++I) TestTrue(TEXT("Repeated successful Load after failure"), VrgLayoutFile::Load(F.W, File, Message));
	TestTrue(TEXT("Load keeps stopped time and old live grid but clears history"), F.Sim->GetSnapshot().SimTimeS == Warm.SimTimeS
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK == Temperatures && F.C->GetLayoutUndoCount() == 0 && F.C->GetLayoutRedoCount() == 0);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const auto First = F.Sim->GetSnapshot();
	for (int32 I = 0; I < 25; ++I) F.Sim->Tick(.04f);
	const auto S = F.Sim->GetSnapshot(); const auto A = F.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
	const double ItRelative = FMath::Abs(First.TotalItHeatW - ReferenceItW) / ReferenceItW;
	TestTrue(TEXT("Fresh coupled run preserves IT/energy/pressure/duct/hydraulic criteria"), ItRelative < 1e-9 && First.SimTimeS == F.Sim->FixedStepS
		&& FMath::Abs(S.NormalizedEnergyResidual) < 1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence < 1e-5
		&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW > 0);
	TestTrue(TEXT("Barrier is solid in restored manual air grid"), F.Sim->GetNumBarrierCells() > 0 && F.Sim->GetNumSplitDucts() == 0);
	for (auto* Rack : F.Racks) for (double K : Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing inlet plausibility range15..35C"), K > 288.15 && K < 308.15);
	AddInfo(FString::Printf(TEXT("Room Load rollback/coupling: warm %.2fs unchanged; wall refs and mixed history retained; successful Load0/0 then fresh run %.2fs, IT relative %.3e, global %.3e, pressure %.3e, barrier solids%d."),
		Warm.SimTimeS, S.SimTimeS, ItRelative, S.NormalizedEnergyResidual, A.NormalizedDivergence, F.Sim->GetNumBarrierCells()));
	for (const auto& Name : {File, FailureFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
#endif
