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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingFileCouplingTest, "VRG.Interaction.Layout.RoomCeilingPersistenceRollbackCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingFileCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingCoupled"), BadFile=File+TEXT("Bad"), FailureFile=File+TEXT("Failure");
	TestTrue(TEXT("Manual room"),F.Sim->TrySetRoomAirDomain(Manual,Message));
	// Existing synthetic corner plate/wall and extra-rack fixture, not new product/physics defaults.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63;
	auto Second=P; Second.MinXYM.X+=1; Second.MaxXYM.X+=1;
	const TArray<FRoomCeilingPlate> Saved{P,Second};
	const FLayoutBarrierRecord Wall{TEXT("ceiling-coupled-wall"),TEXT("CeilingCoupledWall"),FTransform(FRotator::ZeroRotator,FVector(-480,200,0),FVector(.2,1,2)),EBarrierKind::Curtain};
	TestNotNull(TEXT("Barrier"),VrgBarrierLifecycle::Spawn(F.W,Wall));
	TestTrue(TEXT("Save room/barrier/two-plate reference"),F.Sim->TrySetRoomCeilingPlates(Saved,Message) && VrgLayoutFile::Save(F.W,File,Message));
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Origin=F.Air->GetActorLocation();
	const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes(); const auto Pose=F.Racks[0]->GetActorTransform();
	auto Third=P; Third.MinXYM.X+=2; Third.MaxXYM.X+=2; Third.HeightM=2.53;
	F.C->OpenRoomCeilingSettings(); TestTrue(TEXT("Pending ceiling history differs from saved list"),F.C->AddRoomCeiling(Third,Message)); F.C->CloseRoomSettings();
	auto Edited=Manual; Edited.CeilingHeightM=3.2; F.C->OpenRoomSettings(); F.C->ApplyRoomSettings(Edited,Message); F.C->CloseRoomSettings(); F.C->TryUndoLayout();
	const TArray<FRoomCeilingPlate> Current{P,Second,Third};
	TestTrue(TEXT("Both stacks before refusal"),F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==1);
	auto Unchanged=[&]()
	{
		const auto S=F.Sim->GetSnapshot(); auto* B=VrgBarrierLifecycle::Find(F.W,Wall.Id);
		return F.Sim->GetSimMode()==ESimMode::Layout && F.Sim->GetRoomAirDomain()==Manual && F.Sim->GetRoomCeilingPlates()==Current
			&& F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==1 && F.Racks[0]->GetActorTransform().Equals(Pose,0)
			&& B && B->GetActorTransform().Equals(Wall.Transform) && F.Air->GetActorLocation()==Origin
			&& F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
			&& F.Room->GetMutableNetwork().GetNumPipes()==Pipes && S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ
			&& S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ;
	};
	// The CURRENT rack at Z0 clears a Z2.5 plate; the loaded rack at Z1 does not. Validate FINAL geometry.
	auto Bad=ReadRoomFile(File); auto O=Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject();
	O->SetNumberField(TEXT("minXM"),-1.7); O->SetNumberField(TEXT("maxXM"),-1.3); O->SetNumberField(TEXT("minYM"),-.1); O->SetNumberField(TEXT("maxYM"),.1);
	for(const auto& V:Bad->GetArrayField(TEXT("equipment"))) if(V->AsObject()->GetStringField(TEXT("name"))==F.Racks[0]->GetName()) V->AsObject()->SetNumberField(TEXT("z_m"),1);
	auto* BarrierBefore=VrgBarrierLifecycle::Find(F.W,Wall.Id); WriteRoomFile(BadFile,Bad);
	TestTrue(TEXT("Loaded elevated rack rejects saved plate before live changes"),!VrgLayoutFile::Load(F.W,BadFile,Message)
		&& Message.Contains(TEXT("천장 판")) && Unchanged() && VrgBarrierLifecycle::Find(F.W,Wall.Id)==BarrierBefore);
	auto* Extra=VrgRackLifecycle::Spawn(F.W,TEXT("CeilingFileExtra"),TEXT("ceiling-file-extra"),ARackThermalActor::StaticClass(),
		FTransform(FRotator::ZeroRotator,FVector(-150,150,0)),F.Racks[0]->CaptureLayoutSettings());
	if(!TestNotNull(TEXT("Extra file rack"),Extra)) return false;
	TestTrue(TEXT("Save pending three-rack inventory"),VrgLayoutFile::Save(F.W,FailureFile,Message)); VrgRackLifecycle::Remove(Extra);
	auto Failure=ReadRoomFile(FailureFile); Failure->GetObjectField(TEXT("settings"))->SetArrayField(TEXT("roomCeilingPlates"),ReadRoomFile(File)->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))); WriteRoomFile(FailureFile,Failure);
	bool bInjected=false; const auto Hook=F.W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* A)
	{ if(Cast<ARackThermalActor>(A) && A->GetName().StartsWith(TEXT("PendingLayoutEquipment"))) { bInjected=true; A->Destroy(false,false); } }));
	TestFalse(TEXT("Middle creation failure rejects whole Load"),VrgLayoutFile::Load(F.W,FailureFile,Message)); F.W->RemoveOnActorSpawnedHandler(Hook);
	TestTrue(TEXT("Failure actually injected after preflight and preserves warm state/plates/history"),bInjected && Unchanged() && !VrgRackLifecycle::Find(F.W,TEXT("ceiling-file-extra")));
	TestTrue(TEXT("Ceiling and room history remain usable after rollback"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Saved
		&& F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==Current && F.C->TryRedoLayout() && F.Sim->GetRoomAirDomain()==Edited && F.C->TryUndoLayout());
	for(int32 I=0;I<2;++I) TestTrue(TEXT("Successful repeated Load replaces three with saved two plates"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==Saved && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Load delays solid/temperature application and retains stopped clock"),F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	double MaxEnergy=0,MaxDiv=0,MaxIt=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f);
		const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Reloaded plates/wall and existing IT/heat/pressure/hydraulic/duct/coil criteria"),Case.Sim->GetNumCeilingCells()==18 && Case.Sim->GetNumBarrierCells()>0
			&& Case.Air->GridParams.NumCellsX==43 && Case.Air->GridParams.NumCellsY==37 && Case.Air->GridParams.NumCellsZ==13
			&& It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0 && First.SimTimeS==Case.Sim->FixedStepS);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing inlet safety15..35C"),K>288.15 && K<308.15);
	};
	Run(F);
	{ FRoomFileFixture New; TestTrue(TEXT("Fresh world repeated Load before coupled run"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)); Run(New); }
	AddInfo(FString::Printf(TEXT("Ceiling v2 warm rollback %.2fs exact; final loaded rack/plate collision rejected; shared records survive; success0/0 and delayed grid; same/new-world18plate cells43x37x13, IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,BadFile,FailureFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingDeleteCouplingTest,"VRG.Interaction.Layout.RoomCeilingDeletePersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingDeleteCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingDeleted"), Empty=File+TEXT("Empty"), BadFile=File+TEXT("Bad");
	TestTrue(TEXT("Manual room"),F.Sim->TrySetRoomAirDomain(Manual,Message));
	// Synthetic existing corner geometry, separated plate and curtain; no new physical defaults.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63;
	auto Second=P; Second.MinXYM.X+=3; Second.MaxXYM.X+=3; const TArray<FRoomCeilingPlate> Before{P,Second},After{Second};
	const FLayoutBarrierRecord Wall{TEXT("ceiling-delete-wall"),TEXT("CeilingDeleteWall"),FTransform(FRotator::ZeroRotator,FVector(-480,200,0),FVector(.2,1,2)),EBarrierKind::Curtain};
	TestNotNull(TEXT("Barrier"),VrgBarrierLifecycle::Spawn(F.W,Wall)); TestTrue(TEXT("Initial plates"),F.Sim->TrySetRoomCeilingPlates(Before,Message));
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto Origin=F.Air->GetActorLocation(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes(); const auto Pose=F.Racks[0]->GetActorTransform();
	auto Frozen=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ
		&& S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetActorLocation()==Origin && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	const FVector Eye(-520,340,400),SecondEye(-185,340,400),Down(0,0,-1);
	F.C->SetRoomCeilingEditing(true); F.C->SelectRoomCeilingAlongRay(Eye,Down);
	F.Racks[0]->SetActorLocation(FVector(-185,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Invalid remaining plate rejects deletion without changing selection/history/grid"),!F.C->TryDeleteRoomCeiling()
		&& F.Sim->GetRoomCeilingPlates()==Before && F.C->GetSelectedRoomCeilingIndex()==0 && F.C->GetLayoutUndoCount()==0 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Delete one changes metadata only at warm1.02s"),F.C->TryDeleteRoomCeiling() && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	F.C->SetRoomCeilingEditing(false); TestTrue(TEXT("Cancel mode records nothing and keeps warm state"),!F.C->TryDeleteRoomCeiling() && F.C->GetLayoutUndoCount()==1 && Frozen());
	F.C->SetRoomCeilingEditing(true); F.C->SelectRoomCeilingAlongRay(SecondEye,Down);
	F.Racks[0]->SetActorLocation(FVector(-485,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Undo revalidates deleted plate and preserves selection/BOTH stacks on obstruction"),!F.C->TryUndoLayout()
		&& F.Sim->GetRoomCeilingPlates()==After && F.C->GetSelectedRoomCeilingIndex()==0 && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Remove obstruction then exact Undo/Redo clears old selection"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before
		&& F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen());
	F.C->SelectRoomCeilingAlongRay(SecondEye,Down); TestTrue(TEXT("Save deleted inventory"),VrgLayoutFile::Save(F.W,File,Message));
	auto Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->SetNumberField(TEXT("heightM"),0); WriteRoomFile(BadFile,Bad);
	auto* BarrierBefore=VrgBarrierLifecycle::Find(F.W,Wall.Id);
	TestTrue(TEXT("Failed Load keeps ceiling mode/selection/state/history and live barrier"),!VrgLayoutFile::Load(F.W,BadFile,Message)
		&& F.C->IsRoomCeilingEditing() && F.C->GetSelectedRoomCeilingIndex()==0 && F.C->GetLayoutUndoCount()==1 && Frozen()
		&& F.Sim->GetRoomCeilingPlates()==After && VrgBarrierLifecycle::Find(F.W,Wall.Id)==BarrierBefore);
	for(int32 I=0;I<2;++I) TestTrue(TEXT("Success Load restores deletion, clears editing/selection/history without stale delete"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==After && !F.C->IsRoomCeilingEditing() && F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE
		&& !F.C->TryDeleteRoomCeiling() && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0);
	TestTrue(TEXT("Successful Load keeps old18-cell mask/temperatures until next entry"),F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	double MaxIt=0,MaxEnergy=0,MaxDiv=0;
	auto Run=[&](FRoomFileFixture& Case,int32 Cells)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f);
		const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS); const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Deletion reflected in cells with room/wall/cooling and existing conservation/plausibility"),Case.Sim->GetNumCeilingCells()==Cells
			&& Case.Sim->GetNumBarrierCells()>0 && Case.Sim->GetRoomAirDomain()==Manual && Case.Air->GridParams.NumCellsX==43 && Case.Air->GridParams.NumCellsY==37 && Case.Air->GridParams.NumCellsZ==13
			&& It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C transient inlet safety"),K>288.15 && K<308.15);
	};
	Run(F,9);
	{ FRoomFileFixture New; TestTrue(TEXT("Fresh world repeated Load preserves one-plate deletion"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message) && New.Sim->GetRoomCeilingPlates()==After); Run(New,9); }
	F.Sim->EnterLayoutMode(); F.C->SetRoomCeilingEditing(true); F.C->SelectRoomCeilingAlongRay(SecondEye,Down);
	TestTrue(TEXT("Last plate delete/Undo/Redo and empty list save"),F.C->TryDeleteRoomCeiling() && F.Sim->GetRoomCeilingPlates().IsEmpty()
		&& F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==After && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates().IsEmpty() && VrgLayoutFile::Save(F.W,Empty,Message));
	Run(F,0);
	{ FRoomFileFixture New; TestTrue(TEXT("Fresh world loads explicit empty deletion"),VrgLayoutFile::Load(New.W,Empty,Message) && New.Sim->GetRoomCeilingPlates().IsEmpty()); Run(New,0); }
	AddInfo(FString::Printf(TEXT("Ceiling delete warm%.2fs frozen; rejected delete/Undo/Load keep state/selection/history; successful Load clears stale selection; same/new-world plates18->9->0 at43x37x13, IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,Empty,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingCornerCouplingTest,"VRG.Interaction.Layout.RoomCeilingCornerHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingCornerCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingCorner"),BadFile=File+TEXT("Bad");
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture metres.
	const TArray<FRoomCeilingPlate> Before{P,P}; F.Sim->TrySetRoomAirDomain(Manual,Message); F.Sim->TrySetRoomCeilingPlates(Before,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto Origin=F.Air->GetActorLocation(); const auto Pose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Frozen=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ
		&& S.EnergyStoredJ==Warm.EnergyStoredJ && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetActorLocation()==Origin && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	const FVector Eye(-490,340,400),Down(0,0,-1);
	auto Begin=[&]() { return F.C->SetRoomCeilingEditing(true) && F.C->SelectRoomCeilingAlongRay(Eye,Down) && F.C->BeginRoomCeilingCornerDrag(3,Eye,Down); };
	TestTrue(TEXT("Selected first duplicate begins edit"),Begin()); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(37,23,0),Down);
	FRoomCeilingPlate Edited; bool Valid=false; TestTrue(TEXT("Expanded valid candidate preview"),F.C->GetRoomCeilingPreview(Edited,Valid) && Valid);
	const TArray<FRoomCeilingPlate> After{Edited,P};
	TestTrue(TEXT("Warm preview exact live/record/height/opposite preservation"),Edited.HeightM==P.HeightM && Edited.MinXYM==P.MinXYM
		&& FMath::IsNearlyEqual(Edited.MaxXYM.X,-4.25,1e-12) && FMath::IsNearlyEqual(Edited.MaxXYM.Y,3.85,1e-12)
		&& F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && Frozen());
	TestTrue(TEXT("Release one index and one shared command; duplicate remains"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After
		&& F.C->GetLayoutUndoCount()==1 && F.C->GetSelectedRoomCeilingIndex()==0 && Frozen());
	TestTrue(TEXT("Whole list exact Undo/Redo"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen());
	F.C->TryUndoLayout();
	TestTrue(TEXT("No-op click begins"),Begin()); TestFalse(TEXT("No-op off-grid authored corner does not snap or record"),F.C->EndRoomCeilingCornerDrag(true));
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(37,23,0),Down); F.C->EndRoomCeilingCornerDrag(false);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,200,0),Down);
	TestFalse(TEXT("Invalid release refused"),F.C->EndRoomCeilingCornerDrag(true));
	TestTrue(TEXT("Cancel/no-op/reject preserve Redo and warm state"),F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen());
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(37,23,0),Down);
	F.Racks[0]->SetActorLocation(FVector(-410,380,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Release revalidates newly obstructed expanded cells, no state/history change"),!F.C->EndRoomCeilingCornerDrag(true)
		&& F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	Begin(); auto Taller=Manual; Taller.CeilingHeightM=3.2; F.Sim->TrySetRoomAirDomain(Taller,Message);
	TestTrue(TEXT("External room change refuses captured gesture"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomAirDomain()==Taller && F.Sim->GetRoomCeilingPlates()==Before
		&& F.C->GetLayoutRedoCount()==1 && Frozen()); F.Sim->TrySetRoomAirDomain(Manual,Message);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(37,23,0),Down);
	TestTrue(TEXT("New valid commit clears Redo"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen());
	F.Racks[0]->SetActorLocation(FVector(-485,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Undo current-equipment validation failure preserves state/history"),!F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==After
		&& F.C->GetSelectedRoomCeilingIndex()==0 && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Obstruction removed Undo/Redo works"),F.C->TryUndoLayout() && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen());
	TestTrue(TEXT("Save edited complete list"),VrgLayoutFile::Save(F.W,File,Message));
	auto Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->SetNumberField(TEXT("heightM"),0); WriteRoomFile(BadFile,Bad);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(10,10,0),Down);
	TestTrue(TEXT("Failed Load preserves pending gesture/list/history/warm state"),!VrgLayoutFile::Load(F.W,BadFile,Message) && F.C->IsLayoutDragging()
		&& F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Successful Load cancels pending gesture and resets history, physical mask delayed"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==After && !F.C->IsRoomCeilingEditing() && !F.C->IsLayoutDragging() && F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE
		&& F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0 && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	TestTrue(TEXT("Repeated Load replaces without duplicates"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After);
	double MaxIt=0,MaxEnergy=0,MaxDiv=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f);
		const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Next entry applies expanded union16cells, room/cooling/conservation/plausibility"),Case.Sim->GetNumCeilingCells()==16
			&& Case.Sim->GetRoomCeilingPlates()==After && Case.Air->GridParams.NumCellsX==43 && Case.Air->GridParams.NumCellsY==37 && Case.Air->GridParams.NumCellsZ==13
			&& It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C inlet safety"),K>288.15 && K<308.15);
	};
	Run(F);
	{ FRoomFileFixture New; TestTrue(TEXT("Fresh world repeated Load restores ordered duplicate/edited list"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)
		&& New.Sim->GetRoomCeilingPlates()==After && New.C->GetLayoutUndoCount()==0 && New.C->GetLayoutRedoCount()==0); Run(New); }
	AddInfo(FString::Printf(TEXT("Ceiling corner warm%.2fs exact; one duplicate edited max(-4.25,3.85)m height2.63m opposite fixed, cancel/refusal/no-op Redo retained; final equipment/room revalidated; Load pending cancel/0records and next-entry9->16plate cells43x37x13, IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHeightCouplingTest,"VRG.Interaction.Layout.RoomCeilingHeightHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHeightCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingHeight"),BadFile=File+TEXT("Bad");
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture metres.
	const TArray<FRoomCeilingPlate> Before{P,P}; F.Sim->TrySetRoomAirDomain(Manual,Message); F.Sim->TrySetRoomCeilingPlates(Before,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto Origin=F.Air->GetActorLocation(); const auto Pose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Frozen=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ
		&& S.EnergyStoredJ==Warm.EnergyStoredJ && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetActorLocation()==Origin && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	const FVector PickEye(-490,340,400),Down(0,0,-1),Eye(-390,337.5,270),Side(-1,0,0);
	auto Begin=[&]() { return F.C->SetRoomCeilingEditing(true) && F.C->SelectRoomCeilingAlongRay(PickEye,Down) && F.C->BeginRoomCeilingHeightDrag(Eye,Side); };
	TestTrue(TEXT("Selected duplicate height begins"),Begin()); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side);
	FRoomCeilingPlate Edited; bool Valid=false; TestTrue(TEXT("Valid height candidate"),F.C->GetRoomCeilingPreview(Edited,Valid) && Valid);
	const TArray<FRoomCeilingPlate> After{Edited,P};
	TestTrue(TEXT("Height snap/XY exact and live state frozen before release"),FMath::IsNearlyEqual(Edited.HeightM,2.85,1e-12)
		&& Edited.MinXYM==P.MinXYM && Edited.MaxXYM==P.MaxXYM && F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && Frozen());
	TestTrue(TEXT("Only selected duplicate changed, one record"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Whole list Undo/Redo exact"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen()); F.C->TryUndoLayout();
	Begin(); TestFalse(TEXT("Unmoved authored2.63m click not recorded/snapped"),F.C->EndRoomCeilingCornerDrag(true));
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side); F.C->EndRoomCeilingCornerDrag(false);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,60),Side); TestFalse(TEXT("Out-of-room release refused"),F.C->EndRoomCeilingCornerDrag(true));
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side); F.C->UpdateRoomCeilingCornerDrag(PickEye,Down);
	TestFalse(TEXT("Parallel update cannot commit old valid height"),F.C->EndRoomCeilingCornerDrag(true));
	TestTrue(TEXT("Cancel/no-op/refusal preserve list and BOTH stacks/warm state"),F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen());
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side);
	F.Racks[0]->SetActorLocation(FVector(-485,340,100*(2.8-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Final equipment collision revalidated, records/state retained"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==Before
		&& F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	Begin(); auto Taller=Manual; Taller.CeilingHeightM=3.2; F.Sim->TrySetRoomAirDomain(Taller,Message);
	TestTrue(TEXT("External room change prevents commit"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomAirDomain()==Taller && F.Sim->GetRoomCeilingPlates()==Before
		&& F.C->GetLayoutRedoCount()==1 && Frozen()); F.Sim->TrySetRoomAirDomain(Manual,Message);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(PickEye,Down); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side);
	TestTrue(TEXT("Recovered valid height commit clears Redo"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen());
	F.Racks[0]->SetActorLocation(FVector(-485,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Undo obstruction keeps entire record and live state"),!F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==After
		&& F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Undo/Redo works after obstruction removed"),F.C->TryUndoLayout() && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen());
	TestTrue(TEXT("Save height list"),VrgLayoutFile::Save(F.W,File,Message));
	auto Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->SetNumberField(TEXT("heightM"),0); WriteRoomFile(BadFile,Bad);
	Begin(); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,5),Side);
	TestTrue(TEXT("Failed Load retains pending height/list/state/history"),!VrgLayoutFile::Load(F.W,BadFile,Message) && F.C->IsLayoutDragging()
		&& F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Success Load cancels pending height and clears selection/history, old mask until entry"),VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==After && !F.C->IsRoomCeilingEditing() && !F.C->IsLayoutDragging() && F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE
		&& F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0 && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	TestTrue(TEXT("Repeated Load not appended"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After);
	double MaxIt=0,MaxEnergy=0,MaxDiv=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f);
		const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Next entry two distinct Z layers18cells and existing cooling/conservation/plausibility"),Case.Sim->GetNumCeilingCells()==18
			&& Case.Sim->GetRoomCeilingPlates()==After && Case.Air->GridParams.NumCellsX==43 && Case.Air->GridParams.NumCellsY==37 && Case.Air->GridParams.NumCellsZ==13
			&& It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0);
		const auto& Grid=Case.Air->GetMutableSolver().GetGrid();
		for(int32 Y=30;Y<33;++Y) for(int32 X=1;X<4;++X) for(int32 Z:{10,11}) TestTrue(TEXT("Requested2.63/2.85m occupy Z10/11 exactly"),Grid.bSolidCell[Grid.CellIndex(X,Y,Z)]);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C transient inlet safety"),K>288.15 && K<308.15);
	};
	Run(F);
	{ FRoomFileFixture New; TestTrue(TEXT("Fresh world repeated Load retains height/XY/order and0records"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)
		&& New.Sim->GetRoomCeilingPlates()==After && New.C->GetLayoutUndoCount()==0 && New.C->GetLayoutRedoCount()==0); Run(New); }
	AddInfo(FString::Printf(TEXT("Ceiling height warm%.2fs exact; selected duplicate2.63->2.85m XY unchanged, Undo/Redo/cancel/refusal/ray/no-op/final-validation; Load pending cancel/0records; next-entry9->18cells Z10/11 h0.25m43x37x13, IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingMoveCouplingTest,"VRG.Interaction.Layout.RoomCeilingMoveHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingMoveCouplingTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingMove"),BadFile=File+TEXT("Bad");
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture metres.
	const TArray<FRoomCeilingPlate> Before{P,P}; F.Sim->TrySetRoomAirDomain(Manual,Message); F.Sim->TrySetRoomCeilingPlates(Before,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto Origin=F.Air->GetActorLocation(); const auto Pose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Frozen=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ
		&& S.EnergyStoredJ==Warm.EnergyStoredJ && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetActorLocation()==Origin && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	const FVector Eye(-490,340,400),Down(0,0,-1),Delta(87,-23,0),MovedEye(-402,317,400);
	auto Begin=[&](const FVector& Start) { return F.C->SetRoomCeilingEditing(true) && F.C->BeginRoomCeilingPointerDrag(INDEX_NONE,Start,Down); };
	TestTrue(TEXT("Body begins first duplicate"),Begin(Eye)); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down);
	FRoomCeilingPlate Edited; bool Valid=false; TestTrue(TEXT("Valid translated preview"),F.C->GetRoomCeilingPreview(Edited,Valid) && Valid);
	const TArray<FRoomCeilingPlate> After{Edited,P};
	TestTrue(TEXT("Snap/offset/size/height preserved, no live mutation before release"),FMath::IsNearlyEqual(Edited.MinXYM.X,-4.25,1e-12)
		&& FMath::IsNearlyEqual(Edited.MinXYM.Y,2.9,1e-12) && (Edited.MaxXYM-Edited.MinXYM).Equals(P.MaxXYM-P.MinXYM,1e-12)
		&& Edited.HeightM==P.HeightM && F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && Frozen());
	TestTrue(TEXT("Commit one duplicate in one shared record"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Whole list Undo/Redo exact"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen()); F.C->TryUndoLayout();
	Begin(Eye); TestFalse(TEXT("No-op click does not snap or record"),F.C->EndRoomCeilingCornerDrag(true));
	Begin(Eye); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down); F.C->EndRoomCeilingCornerDrag(false);
	Begin(Eye); F.C->UpdateRoomCeilingCornerDrag(Eye+FVector(-300,0,0),Down); TestFalse(TEXT("Outside destination rejected"),F.C->EndRoomCeilingCornerDrag(true));
	Begin(Eye); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down); F.C->UpdateRoomCeilingCornerDrag(Eye,FVector(1,0,0)); TestFalse(TEXT("Missing plane cannot commit last valid destination"),F.C->EndRoomCeilingCornerDrag(true));
	TestTrue(TEXT("Cancel/no-op/reject preserve list/BOTH stacks and warm state"),F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen());
	Begin(Eye); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down); F.Racks[0]->SetActorLocation(FVector(-385,320,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Release revalidates equipment only at destination without partial change"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==Before
		&& F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	Begin(Eye); auto Taller=Manual; Taller.CeilingHeightM=3.2; F.Sim->TrySetRoomAirDomain(Taller,Message);
	TestTrue(TEXT("External room refuses captured move"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomAirDomain()==Taller && F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutRedoCount()==1 && Frozen()); F.Sim->TrySetRoomAirDomain(Manual,Message);
	Begin(Eye); F.C->UpdateRoomCeilingCornerDrag(Eye,FVector(1,0,0)); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down);
	TestTrue(TEXT("Recovered ray permits valid move and clears Redo"),F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen());
	F.Racks[0]->SetActorLocation(FVector(-485,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestTrue(TEXT("Undo obstacle keeps state/history"),!F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen()); F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Undo/Redo after removing obstruction"),F.C->TryUndoLayout() && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==After && Frozen());
	TestTrue(TEXT("Save translated list"),VrgLayoutFile::Save(F.W,File,Message));
	auto Bad=ReadRoomFile(File); Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->SetNumberField(TEXT("heightM"),0); WriteRoomFile(BadFile,Bad);
	Begin(MovedEye); F.C->UpdateRoomCeilingCornerDrag(MovedEye+FVector(10,10,0),Down);
	TestTrue(TEXT("Failed Load keeps pending move/list/state/history"),!VrgLayoutFile::Load(F.W,BadFile,Message) && F.C->IsLayoutDragging() && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Successful Load cancels pending move/selection/history, physical grid delayed"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After
		&& !F.C->IsRoomCeilingEditing() && !F.C->IsLayoutDragging() && F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	TestTrue(TEXT("Repeated Load no duplicate appending"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After);
	double MaxIt=0,MaxEnergy=0,MaxDiv=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot(); for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f);
		const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS); const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Next entry moved+original18cell union and existing cooling/conservation/plausibility"),Case.Sim->GetNumCeilingCells()==18 && Case.Sim->GetRoomCeilingPlates()==After
			&& Case.Air->GridParams.NumCellsX==43 && Case.Air->GridParams.NumCellsY==37 && Case.Air->GridParams.NumCellsZ==13 && It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9
			&& S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5 && S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0);
		const auto& Grid=Case.Air->GetMutableSolver().GetGrid();
		for(int32 Y=29;Y<32;++Y) for(int32 X=5;X<8;++X) TestTrue(TEXT("New location solid at original Z10"),Grid.bSolidCell[Grid.CellIndex(X,Y,10)]);
		for(int32 Y=30;Y<33;++Y) for(int32 X=1;X<4;++X) TestTrue(TEXT("Other duplicate retained at old cells"),Grid.bSolidCell[Grid.CellIndex(X,Y,10)]);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C inlet safety"),K>288.15 && K<308.15);
	};
	Run(F); { FRoomFileFixture New; TestTrue(TEXT("Fresh world repeated Load retains order/geometry/0records"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)
		&& New.Sim->GetRoomCeilingPlates()==After && New.C->GetLayoutUndoCount()==0 && New.C->GetLayoutRedoCount()==0); Run(New); }
	AddInfo(FString::Printf(TEXT("Ceiling move warm%.2fs exact; selected duplicate min(-4.25,2.9)m size/height retained; Undo/Redo/cancel/refusal/no-op/final validation; Load pending cancel/0records; next-entry9->18cells movedX5..7Y29..31Z10 old duplicate preserved h0.25m43x37x13, IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHolePersistenceTest,"VRG.Interaction.Layout.RoomCeilingHoleHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHolePersistenceTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	F.Sim->Containment=EAisleContainment::BlankingAndColdAisleRoof;
	const FString File=TEXT("AutomationCeilingHoles"),BadFile=File+TEXT("Bad"),OldFile=File+TEXT("Old");
	// Existing synthetic remote plate plus a wall crossing its hole; no facility/physics constants added.
	const FLayoutBarrierRecord Wall{TEXT("ceiling-hole-wall"),TEXT("CeilingHoleWall"),FTransform(FRotator::ZeroRotator,FVector(-480,320,0),FVector(.2,1,3.5)),EBarrierKind::Wall};
	TestNotNull(TEXT("Wall owner fixture"),VrgBarrierLifecycle::Spawn(F.W,Wall));
	Manual.CeilingHeightM=4; TestTrue(TEXT("Manual fixture"),F.Sim->TrySetRoomAirDomain(Manual,Message));
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	const auto Baseline=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const int32 RoofCells=F.Sim->GetNumContainmentCells(),WallCells=F.Sim->GetNumBarrierCells();
	TestTrue(TEXT("Existing wall and cold aisle roof are actual producers"),RoofCells>0 && WallCells>0); F.Sim->EnterLayoutMode();
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.25,3); P.MaxXYM=FVector2D(-4.25,4); P.HeightM=2.625;
	FRoomCeilingHole Hole; Hole.LocalMinXYM=FVector2D(.125); Hole.LocalMaxXYM=FVector2D(.875); P.Holes.Add(Hole);
	const TArray<FRoomCeilingPlate> Before{P}; TestTrue(TEXT("Set initial holed plate"),F.Sim->TrySetRoomCeilingPlates(Before,Message));
	for(auto* Rack:F.Racks) Rack->Input=InitialInput;
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode();
	const auto Warm=F.Sim->GetSnapshot();
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto RackPose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Frozen=[&]() { return F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS && F.Sim->GetSnapshot().EnergyInJ==Warm.EnergyInJ
		&& F.Sim->GetSnapshot().EnergyOutJ==Warm.EnergyOutJ && F.Sim->GetSnapshot().EnergyStoredJ==Warm.EnergyStoredJ
		&& F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	TestTrue(TEXT("Existing list record includes holes"),F.C->OpenRoomCeilingSettings() && F.C->AddRoomCeiling(P,Message)); F.C->CloseRoomSettings();
	const FVector Down(0,0,-1),Eye(-475,350,400),Delta(50,-50,0);
	TestTrue(TEXT("Move selected duplicate with holes"),F.C->SetRoomCeilingEditing(true) && F.C->SelectRoomCeilingAlongRay(Eye,Down)
		&& F.C->BeginRoomCeilingMoveDrag(Eye,Down)); F.C->UpdateRoomCeilingCornerDrag(Eye+Delta,Down);
	TestTrue(TEXT("Commit value-only move"),F.C->EndRoomCeilingCornerDrag(true) && Frozen());
	const FVector HeightEye(-425,0,262.5),AlongY(0,1,0);
	TestTrue(TEXT("Height gesture keeps local holes"),F.C->BeginRoomCeilingHeightDrag(HeightEye,AlongY)); F.C->UpdateRoomCeilingCornerDrag(HeightEye+FVector(0,0,25),AlongY);
	TestTrue(TEXT("Height commit"),F.C->EndRoomCeilingCornerDrag(true) && Frozen());
	auto Moved=P; Moved.MinXYM+=FVector2D(.5,-.5); Moved.MaxXYM+=FVector2D(.5,-.5); Moved.HeightM=2.9; // Existing5cm absolute height snap.
	const TArray<FRoomCeilingPlate> After{Moved,P};
	TestTrue(TEXT("Only selected plate transformed, holes unchanged"),F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==3);
	for(int32 I=0;I<3;++I) TestTrue(TEXT("Whole-list Undo retains holes/state"),F.C->TryUndoLayout() && Frozen());
	TestTrue(TEXT("Initial exact list"),F.Sim->GetRoomCeilingPlates()==Before);
	for(int32 I=0;I<3;++I) TestTrue(TEXT("Whole-list Redo retains holes/state"),F.C->TryRedoLayout() && Frozen());
	TestTrue(TEXT("Final exact list"),F.Sim->GetRoomCeilingPlates()==After);
	// Opposite corner fixed; shrinking to0.5m must reject the local0.875m hole, no truncation.
	const FVector MovedEye(-425,300,400),CornerEye(-375,350,400);
	TestTrue(TEXT("Select moved plate before resize"),F.C->SelectRoomCeilingAlongRay(MovedEye,Down) && F.C->BeginRoomCeilingCornerDrag(3,CornerEye,Down));
	F.C->UpdateRoomCeilingCornerDrag(CornerEye-FVector(50,0,0),Down);
	TestTrue(TEXT("Invalid resize rejected without metadata/history/grid change"),!F.C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==3 && Frozen());
	TestTrue(TEXT("Save local holes without thermal state"),VrgLayoutFile::Save(F.W,File,Message));
	const auto SavedJson=ReadRoomFile(File); const auto PlateJson=SavedJson->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject();
	TestTrue(TEXT("Optional holes contain only four local metre values"),PlateJson->GetArrayField(TEXT("holes"))[0]->AsObject()->Values.Num()==4
		&& PlateJson->GetArrayField(TEXT("holes"))[0]->AsObject()->GetNumberField(TEXT("maxXM"))==.875);
	auto RejectLoad=[&](TSharedPtr<FJsonObject> Bad)
	{
		WriteRoomFile(BadFile,Bad);
		TestFalse(TEXT("Invalid hole Load rejected before mutation"),VrgLayoutFile::Load(F.W,BadFile,Message));
		TestTrue(TEXT("Equipment/wall/room/list/clock/grid/history unchanged"),F.Sim->GetRoomCeilingPlates()==After && F.Sim->GetRoomAirDomain()==Manual
			&& F.Racks[0]->GetActorTransform().Equals(RackPose,0) && VrgBarrierLifecycle::Find(F.W,Wall.Id)->GetActorTransform().Equals(Wall.Transform,0)
			&& F.C->GetLayoutUndoCount()==3 && F.C->GetLayoutRedoCount()==0 && Frozen() && !Message.IsEmpty());
	};
	for(int32 Case=0;Case<4;++Case)
	{
		auto Bad=ReadRoomFile(File); auto BP=Bad->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject();
		if(Case==0) BP->SetNumberField(TEXT("holes"),1);
		else if(Case==1) BP->GetArrayField(TEXT("holes"))[0]->AsObject()->RemoveField(TEXT("maxYM"));
		else BP->GetArrayField(TEXT("holes"))[0]->AsObject()->SetNumberField(TEXT("maxXM"),Case==2?1.01:.2);
		RejectLoad(Bad);
	}
	// Changed hole values are part of stale-selection protection, even when outer plate geometry is equal.
	auto External=After; External[0].Holes[0].LocalMaxXYM.X=.75;
	TestTrue(TEXT("External hole edit accepted"),F.Sim->TrySetRoomCeilingPlates(External,Message));
	TestEqual(TEXT("External hole edit invalidates selected index"),F.C->GetSelectedRoomCeilingIndex(),INDEX_NONE); F.Sim->TrySetRoomCeilingPlates(After,Message);
	TestTrue(TEXT("Successful repeated Load restores full list, clears records, delays mask"),VrgLayoutFile::Load(F.W,File,Message) && VrgLayoutFile::Load(F.W,File,Message)
		&& F.Sim->GetRoomCeilingPlates()==After && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0 && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	// Previous files: missing ceiling inventory preserves current holes; a stored old plate without holes is closed.
	auto Legacy=ReadRoomFile(File); Legacy->GetObjectField(TEXT("settings"))->RemoveField(TEXT("roomCeilingPlates"));
	for(int32 Version:{1,2}) { Legacy->SetNumberField(TEXT("version"),Version); WriteRoomFile(OldFile,Legacy);
		TestTrue(TEXT("Absent old inventory retains local holes"),VrgLayoutFile::Load(F.W,OldFile,Message) && F.Sim->GetRoomCeilingPlates()==After); }
	Legacy=ReadRoomFile(File); for(const auto& V:Legacy->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))) V->AsObject()->RemoveField(TEXT("holes"));
	WriteRoomFile(OldFile,Legacy); auto Closed=After; for(auto& C:Closed) C.Holes.Reset();
	TestTrue(TEXT("Stored old plates missing holes restore closed plates"),VrgLayoutFile::Load(F.W,OldFile,Message) && F.Sim->GetRoomCeilingPlates()==Closed);
	TestTrue(TEXT("Restore holes after compatibility case"),VrgLayoutFile::Load(F.W,File,Message));
	double MaxIt=0,MaxEnergy=0,MaxDiv=0,MaxWater=0,MaxDuct=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		FAirGridParams GP; FBox Actual(ForceInit); TArray<FRoomCeilingPlan> Plans;
		TestTrue(TEXT("Plan loaded holes"),Case.Sim->ValidateRoomAirDomain(Manual,GP,Actual,Message) && VrgRoomAirDomain::BuildCeilingPlans(Manual,After,GP,Actual,Plans,Message));
		const auto& G=Case.Air->GetMutableSolver().GetGrid(); auto Expected=Baseline; int32 Added=0,PreservedOpen=0,FluidOpen=0;
		for(const auto& Plan:Plans) for(int32 Y=Plan.MinCell.Y;Y<Plan.MaxCellExclusive.Y;++Y) for(int32 X=Plan.MinCell.X;X<Plan.MaxCellExclusive.X;++X)
		{
			const int32 I=G.CellIndex(X,Y,Plan.MinCell.Z);
			if(VrgRoomAirDomain::IsCeilingCellOpen(Plan,X,Y)) { if(Baseline[I]) ++PreservedOpen; else ++FluidOpen; continue; }
			if(!Expected[I]) { Expected[I]=true; ++Added; }
		}
		TestTrue(TEXT("Exact union: actual wall/roof/equipment preserved and clear holes fluid"),G.bSolidCell==Expected && Case.Sim->GetNumCeilingCells()==Added
			&& PreservedOpen>0 && FluidOpen>0 && Case.Sim->GetNumContainmentCells()==RoofCells && Case.Sim->GetNumBarrierCells()==WallCells);
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f); const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
		double Removed=0,Duct=0; for(auto* Rack:Case.Racks) { Removed+=Rack->GetMutableModel().GetLastHeatRemovedByCoolantW(); TestTrue(TEXT("Positive coolant flow"),Rack->Input.CoolantMassFlowKgs>0);
			for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C inlet plausibility"),K>288.15 && K<308.15); }
		for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I) { TestTrue(TEXT("Positive rack air coupling"),S.RackAirMassFlowKgs[I]>0);
			Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]); }
		const double Water=FMath::Abs(Removed-S.TotalRadiatorRejectedW)/S.TotalRadiatorRejectedW;
		MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		MaxWater=FMath::Max(MaxWater,Water); MaxDuct=FMath::Max(MaxDuct,Duct);
		TestTrue(TEXT("Existing IT/cooling/energy/pressure/hydraulic criteria"),It<1e-9 && Water<1e-12 && Duct<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9
			&& S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5 && S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0
			&& Case.Room->GetTotalPumpElectricalW()>0 && Case.Room->GetTotalPumpElectricalW()/S.TotalItHeatW<.05);
		AddInfo(FString::Printf(TEXT("Ceiling holes loaded t%.2fs h0.25m grid%dx%dx%d added%d preservedOtherSolidOpen%d fluidOpen%d; energy%.3e pressure%.3e"),S.SimTimeS,GP.NumCellsX,GP.NumCellsY,GP.NumCellsZ,Added,PreservedOpen,FluidOpen,S.NormalizedEnergyResidual,A.NormalizedDivergence));
	};
	Run(F); { FRoomFileFixture New; TestTrue(TEXT("Fresh repeated load restores holes/wall/room/no duplicates/empty history"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)
		&& New.Sim->GetRoomCeilingPlates()==After && New.C->GetLayoutUndoCount()==0 && New.C->GetLayoutRedoCount()==0 && VrgBarrierLifecycle::Find(New.W,Wall.Id)); Run(New); }
	AddInfo(FString::Printf(TEXT("Ceiling holes history3 exact Undo/Redo; movement localXY preserved height2.625->2.9m; resize rejected; warm%.2fs unchanged; v2 holes/old fields/atomic failure; IT%.3e globalAbs%.3e pressure%.3e water%.3e duct%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv,MaxWater,MaxDuct));
	for(const auto& Name:{File,BadFile,OldFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHoleUiHistoryTest,"VRG.Interaction.Layout.RoomCeilingHoleUiHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHoleUiHistoryTest::RunTest(const FString& Parameters)
{
	FRoomFileFixture F; FString Message; const auto Manual=RoomFileDomain(); const auto InitialInput=F.Racks[0]->Input;
	const FString File=TEXT("AutomationCeilingHoleUi"),BadFile=File+TEXT("Bad");
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.25,3); P.MaxXYM=FVector2D(-4.25,4); P.HeightM=2.625; // Existing synthetic metre fixture.
	const TArray<FRoomCeilingPlate> Before{P}; F.Sim->TrySetRoomAirDomain(Manual,Message); F.Sim->TrySetRoomCeilingPlates(Before,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW;
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode(); const auto Warm=F.Sim->GetSnapshot();
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Pose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Frozen=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Room->GetMutableNetwork().GetNumPipes()==Pipes && F.Racks[0]->GetActorTransform().Equals(Pose,0); };
	const FVector Eye(-475,350,400),Down(0,0,-1);
	auto Open=[&]() { return F.C->SetRoomCeilingEditing(true) && F.C->SelectRoomCeilingAlongRay(Eye,Down) && F.C->OpenRoomCeilingHoleSettings(); };
	FRoomCeilingHole H; H.LocalMinXYM=FVector2D(.125); H.LocalMaxXYM=FVector2D(.875);
	TestTrue(TEXT("Cancel blank hole draft"),Open()); F.C->CloseRoomSettings();
	TestTrue(TEXT("Cancel leaves list/state/history"),F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && Frozen());
	TestTrue(TEXT("Add through selected-plate modal"),Open() && F.C->AddRoomCeilingHole(H,Message)); F.C->CloseRoomSettings();
	auto WithHole=P; WithHole.Holes.Add(H);
	TestTrue(TEXT("One Add one complete-list record, grid delayed"),F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{WithHole} && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Whole-list Undo/Redo without physical rebuild"),F.C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before && F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{WithHole} && Frozen());
	F.C->TryUndoLayout(); TestTrue(TEXT("Redo retained before rejected/cancelled draft"),Open());
	auto Bad=H; Bad.LocalMaxXYM=FVector2D(.2);
	TestFalse(TEXT("Zero-cell Add rejected"),F.C->AddRoomCeilingHole(Bad,Message)); F.C->CloseRoomSettings();
	TestTrue(TEXT("Refusal/cancel preserves both stacks and warm state"),F.Sim->GetRoomCeilingPlates()==Before && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==1 && Frozen());
	TestTrue(TEXT("Redo still applies exact hole"),F.C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()[0]==WithHole && Frozen()); F.C->TryUndoLayout();
	// Same UI path with a different valid hole checks new-edit Redo clearing; no extra physical constants.
	FRoomCeilingHole NewHole; NewHole.LocalMinXYM=FVector2D(0); NewHole.LocalMaxXYM=FVector2D(.75);
	TestTrue(TEXT("New confirmed hole clears Redo"),Open() && F.C->AddRoomCeilingHole(NewHole,Message) && F.C->GetLayoutUndoCount()==1 && F.C->GetLayoutRedoCount()==0 && Frozen());
	auto Final=P; Final.Holes.Add(NewHole); const TArray<FRoomCeilingPlate> After{Final};
	TestTrue(TEXT("Save during modal stores confirmed data only"),VrgLayoutFile::Save(F.W,File,Message));
	auto BadJson=ReadRoomFile(File); BadJson->GetObjectField(TEXT("settings"))->GetArrayField(TEXT("roomCeilingPlates"))[0]->AsObject()->GetArrayField(TEXT("holes"))[0]->AsObject()->SetNumberField(TEXT("maxXM"),1.1); WriteRoomFile(BadFile,BadJson);
	TestTrue(TEXT("Failed Load leaves selected hole modal/list/room/history and physical state"),!VrgLayoutFile::Load(F.W,BadFile,Message) && F.C->IsRoomSettingsOpen()
		&& F.Sim->GetRoomCeilingPlates()==After && F.Sim->GetRoomAirDomain()==Manual && F.C->GetSelectedRoomCeilingIndex()==0 && F.C->GetLayoutUndoCount()==1 && Frozen());
	TestTrue(TEXT("Successful Load closes hole draft and clears records, mask/temps/clock delayed"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After
		&& !F.C->IsRoomSettingsOpen() && !F.C->IsRoomCeilingEditing() && F.C->GetSelectedRoomCeilingIndex()==INDEX_NONE && F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS);
	TestTrue(TEXT("Repeated Load no duplicate holes"),VrgLayoutFile::Load(F.W,File,Message) && F.Sim->GetRoomCeilingPlates()==After);
	double MaxIt=0,MaxEnergy=0,MaxDiv=0;
	auto Run=[&](FRoomFileFixture& Case)
	{
		for(auto* Rack:Case.Racks) Rack->Input=InitialInput;
		Case.Sim->EnterSimulationMode(); Case.Sim->Tick(.021f); const auto First=Case.Sim->GetSnapshot();
		TestEqual(TEXT("Next entry opens9 of16 plate cells"),Case.Sim->GetNumCeilingCells(),7);
		const auto& G=Case.Air->GetMutableSolver().GetGrid();
		for(int32 Y=30;Y<34;++Y) for(int32 X=1;X<5;++X) TestEqual(TEXT("Exact next-entry hole footprint"),G.IsSolidCell(X,Y,10),!(X<4 && Y<33));
		for(int32 I=0;I<25;++I) Case.Sim->Tick(.04f); const auto S=Case.Sim->GetSnapshot(); const auto A=Case.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt; MaxIt=FMath::Max(MaxIt,It); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
		TestTrue(TEXT("Existing cooling/energy/pressure/plausibility criteria"),It<1e-9 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && A.bPressureConverged && A.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !A.bDuctsRejected && S.CrahCoilRemovedW>0 && G.GetNumCells()==20683);
		for(auto* Rack:Case.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing15..35C inlet safety"),K>288.15 && K<308.15);
	};
	Run(F); { FRoomFileFixture New; TestTrue(TEXT("Fresh repeated Load restores selected-UI-created hole, no appending/records"),VrgLayoutFile::Load(New.W,File,Message) && VrgLayoutFile::Load(New.W,File,Message)
		&& New.Sim->GetRoomCeilingPlates()==After && New.C->GetLayoutUndoCount()==0 && New.C->GetLayoutRedoCount()==0); Run(New); }
	AddInfo(FString::Printf(TEXT("Hole UI history warm%.2fs state exact; one hole per record; Undo/Redo/cancel/refusal/Redo clearing; modal failed Load retained/success closes; repeated/fresh v2 exact; next-entry16->7solid cells with9open h0.25m43x37x13; IT%.3e globalAbs%.3e pressure%.3e at1.02s."),Warm.SimTimeS,MaxIt,MaxEnergy,MaxDiv));
	for(const auto& Name:{File,BadFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name)); return true;
}
#endif
