#include "Misc/AutomationTest.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/DataCenterDebugController.h"
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
#endif
