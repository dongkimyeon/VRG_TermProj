#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FRoomCornerWorld
	{
		UWorld* W; ADataCenterSimulationActor* Sim; AAirFieldDebugActor* Air;
		ADataCenterDebugController* C; ADataCenterRoomActor* Room; ARackThermalActor* Racks[2];
		FRoomCornerWorld()
		{
			W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
			Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
			Air=W->SpawnActor<AAirFieldDebugActor>(); Room=W->SpawnActor<ADataCenterRoomActor>();
			C=W->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
			FActorSpawnParameters P; P.Name=TEXT("RoomCornerCdu");
			W->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(),FVector(-400,-200,0),FRotator::ZeroRotator,P);
			for(int32 I=0;I<2;++I)
			{
				P.Name=I==0?TEXT("RoomCornerRackA"):TEXT("RoomCornerRackB");
				Racks[I]=W->SpawnActor<ARackThermalActor>(ARackThermalActor::StaticClass(),FVector(I==0?-150:150,0,0),FRotator(0,I==0?0:180,0),P);
				Racks[I]->ApplyGb200Nvl72Preset();
			}
			Sim->EnterLayoutMode(); C->RefreshInteractionInputMode();
		}
		~FRoomCornerWorld() { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); }
	};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCornerHistoryTest, "VRG.Interaction.Layout.RoomCornerHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCornerHistoryTest::RunTest(const FString& Parameters)
{
	FRoomCornerWorld F; auto* C=F.C; auto* Sim=F.Sim; FString Message; const FVector Down(0,0,-1), Eye(510,460,500);
	F.Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double ReferenceItW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	const auto Warm=Sim->GetSnapshot(); const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Origin=F.Air->GetActorLocation(); const auto Grid=F.Air->GridParams; const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	const FTransform Pose=F.Racks[0]->GetActorTransform();
	// Same synthetic domain as existing RoomSettingsUndoRedo/RoomSettingsPersistence; no hardware constants.
	FRoomAirDomainSettings A; A.bManual=true; A.MinXYM=FVector2D(-5.5,-4.5); A.MaxXYM=FVector2D(5.1,4.6); A.CeilingHeightM=3.1;
	TestTrue(TEXT("Fixture manual domain accepted"),Sim->TrySetRoomAirDomain(A,Message) && C->SetRoomBoundsEditing(true));
	auto UnchangedLive=[&]() { const auto S=Sim->GetSnapshot(); return Temps==F.Air->GetMutableSolver().GetGrid().TemperatureK
		&& F.Air->GetActorLocation()==Origin && F.Air->GridParams.NumCellsX==Grid.NumCellsX
		&& S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ
		&& F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(613,573,500),Down);
	FRoomAirDomainSettings B; bool bValid=false; C->GetRoomBoundsPreview(B,bValid);
	TestFalse(TEXT("Undo/Redo blocked during preview"),C->TryUndoLayout() || C->TryRedoLayout());
	TestTrue(TEXT("Commit changes only settings, creates one command"),bValid && C->EndRoomCornerDrag(true) && Sim->GetRoomAirDomain()==B
		&& C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0 && !C->IsLayoutDragging() && UnchangedLive());
	TestTrue(TEXT("Idle handle editor shares room Undo/Redo"),C->TryUndoLayout() && Sim->GetRoomAirDomain()==A && C->TryRedoLayout() && Sim->GetRoomAirDomain()==B && UnchangedLive());
	C->TryUndoLayout();
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(700,600,500),Down); C->EndRoomCornerDrag(false);
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(10,10,500),Down); C->EndRoomCornerDrag(true);
	C->BeginRoomCornerDrag(3,Eye,Down); C->EndRoomCornerDrag(true); // Click without movement is not an edit.
	TestTrue(TEXT("Cancel/rejection/no-op preserve existing Redo and warm state"),Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1 && UnchangedLive());
	C->TryRedoLayout(); F.Racks[0]->SetActorLocation(FVector(900,0,0));
	TestFalse(TEXT("Undo revalidates current equipment, never bypasses inclusion"),C->TryUndoLayout());
	TestTrue(TEXT("Rejected Undo keeps setting and command"),Sim->GetRoomAirDomain()==B && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0);
	F.Racks[0]->SetActorTransform(Pose); C->TryUndoLayout();
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(613,573,500),Down);
	auto External=A; External.CeilingHeightM=3.2; Sim->TrySetRoomAirDomain(External,Message);
	TestFalse(TEXT("Stale drag cannot overwrite externally changed room"),C->EndRoomCornerDrag(true));
	TestTrue(TEXT("External room and stacks retained"),Sim->GetRoomAirDomain()==External && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
	Sim->TrySetRoomAirDomain(A,Message);
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(613,573,500),Down);
	auto* Outside=F.W->SpawnActor<ACoolingDistributionUnitActor>(FVector(1200,0,0),FRotator::ZeroRotator);
	TestFalse(TEXT("Commit revalidates new obstruction after valid preview"),C->EndRoomCornerDrag(true));
	TestTrue(TEXT("Failed commit changes neither setting/stacks/live fields"),Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1 && UnchangedLive());
	Outside->Destroy(false,false); C->TryRedoLayout(); C->TryUndoLayout();
	C->BeginRoomCornerDrag(3,Eye,Down); C->UpdateRoomCornerDrag(FVector(613,573,500),Down);
	TestTrue(TEXT("New confirmed room edit clears Redo"),C->EndRoomCornerDrag(true) && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0);
	const FString File=TEXT("AutomationRoomCornerHistory");
	TestTrue(TEXT("Existing v2 saves confirmed corner domain"),VrgLayoutFile::Save(F.W,File,Message));
	Sim->TrySetRoomAirDomain(External,Message);
	TestTrue(TEXT("Repeated Load restores exact room and cancels handles"),VrgLayoutFile::Load(F.W,File,Message) && VrgLayoutFile::Load(F.W,File,Message)
		&& Sim->GetRoomAirDomain()==B && !C->IsRoomBoundsEditing() && !C->IsLayoutDragging() && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	// Successful inventory Load rebuilds/clears the bridge's old ledger; rejection/cancel above must not.
	// The stopped clock and old air field survive until the existing fresh-run entry path.
	const auto LoadedSnapshot=Sim->GetSnapshot();
	TestTrue(TEXT("Successful Load retains stopped time/live air allocation and temperature"),LoadedSnapshot.SimTimeS==Warm.SimTimeS
		&& Temps==F.Air->GetMutableSolver().GetGrid().TemperatureK && F.Air->GetActorLocation()==Origin
		&& F.Air->GridParams.NumCellsX==Grid.NumCellsX && F.Room->GetMutableNetwork().GetNumPipes()==Pipes);
	TestTrue(TEXT("Successful Load follows existing cleared coupling ledger policy"),LoadedSnapshot.EnergyInJ==0 && LoadedSnapshot.EnergyOutJ==0 && LoadedSnapshot.EnergyStoredJ==0);
	{
		FRoomCornerWorld Fresh;
		TestTrue(TEXT("Fresh world load and repeated load restore ID/inventory/room without duplicate"),VrgLayoutFile::Load(Fresh.W,File,Message) && VrgLayoutFile::Load(Fresh.W,File,Message)
			&& Fresh.Sim->GetRoomAirDomain()==B && Fresh.C->GetLayoutUndoCount()==0 && Fresh.C->GetLayoutRedoCount()==0 && Fresh.Racks[0]->GetActorTransform().Equals(Pose));
		int32 RackCount=0,CduCount=0;
		for(TActorIterator<ARackThermalActor> It(Fresh.W);It;++It) ++RackCount;
		for(TActorIterator<ACoolingDistributionUnitActor> It(Fresh.W);It;++It) ++CduCount;
		TestTrue(TEXT("Fresh repeated load keeps two racks and one CDU"),RackCount==2 && CduCount==1);
		Fresh.Sim->EnterSimulationMode(); Fresh.Sim->Tick(.021f); const auto First=Fresh.Sim->GetSnapshot();
		TestTrue(TEXT("Only next simulation entry applies snapped room allocation"),Fresh.Air->GetActorLocation()==FVector(-550,-450,0)
			&& Fresh.Air->GridParams.NumCellsX==47 && Fresh.Air->GridParams.NumCellsY==41 && Fresh.Air->GridParams.NumCellsZ==13);
		for(int32 I=0;I<25;++I) Fresh.Sim->Tick(.04f);
		const auto S=Fresh.Sim->GetSnapshot(); const auto AS=Fresh.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const double ItRelative=FMath::Abs(First.TotalItHeatW-ReferenceItW)/ReferenceItW;
		TestTrue(TEXT("Existing IT/heat balance/pressure/hydraulic/duct/coil criteria after corner save/load"),First.SimTimeS==Fresh.Sim->FixedStepS && ItRelative<1e-9
			&& FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5
			&& S.bHydraulicsConverged && !AS.bDuctsRejected && S.CrahCoilRemovedW>0);
		for(auto* R:Fresh.Racks) for(double K:R->Input.SlotAirInletTempK) TestTrue(TEXT("Existing transient inlet safety range15..35C"),K>288.15 && K<308.15);
		AddInfo(FString::Printf(TEXT("Room corner: saved XY delta %.3e m, grid47x41x13=25051cells, IT relative %.3e, global %.3e, normalized pressure %.3e at %.2fs; warm layout settings/history edits did not alter K/cm/s/J/pipe count."),
			(Fresh.Sim->GetRoomAirDomain().MaxXYM-B.MaxXYM).Size(),ItRelative,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
		Fresh.Sim->EnterLayoutMode(); Fresh.C->SetRoomBoundsEditing(true);
		Fresh.C->BeginRoomCornerDrag(3,FVector(615,575,500),Down); Fresh.C->UpdateRoomCornerDrag(FVector(700,650,500),Down);
		Fresh.Sim->EnterSimulationMode(); Fresh.C->RefreshInteractionInputMode();
		TestTrue(TEXT("External mode entry cancels preview, retains confirmed room/history"),!Fresh.C->IsRoomBoundsEditing() && !Fresh.C->IsLayoutDragging()
			&& Fresh.Sim->GetRoomAirDomain()==B && Fresh.C->GetLayoutUndoCount()==0);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomHeightHistoryTest, "VRG.Interaction.Layout.RoomHeightHistoryPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomHeightHistoryTest::RunTest(const FString& Parameters)
{
	// Reuse the existing two NVL72 racks/CDU fixture and synthetic room; no new hardware/physics parameters.
	FRoomCornerWorld F; auto* C=F.C; auto* Sim=F.Sim; FString Message; const FVector Eye(980,5,300), Side(-1,0,0);
	F.Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
	const double ReferenceItW=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
	const auto Warm=Sim->GetSnapshot(); const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Origin=F.Air->GetActorLocation(); const auto Grid=F.Air->GridParams; const auto Pose=F.Racks[0]->GetActorTransform();
	const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto UnchangedLive=[&]() { const auto S=Sim->GetSnapshot(); return Temps==F.Air->GetMutableSolver().GetGrid().TemperatureK
		&& F.Air->GetActorLocation()==Origin && F.Air->GridParams.NumCellsZ==Grid.NumCellsZ && F.Racks[0]->GetActorTransform().Equals(Pose,0)
		&& S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ
		&& F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	FRoomAirDomainSettings A; A.bManual=true; A.MinXYM=FVector2D(-5.5,-4.5); A.MaxXYM=FVector2D(5.1,4.6); A.CeilingHeightM=3.1;
	TestTrue(TEXT("Existing manual API activates fixture height editor"),Sim->TrySetRoomAirDomain(A,Message) && C->SetRoomBoundsEditing(true));
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,88),Side);
	FRoomAirDomainSettings B; bool bValid=false; C->GetRoomBoundsPreview(B,bValid);
	TestTrue(TEXT("Confirmed height4m is one room record, XY/live simulation unchanged"),bValid && C->EndRoomCornerDrag(true)
		&& B.CeilingHeightM==4 && B.MinXYM==A.MinXYM && B.MaxXYM==A.MaxXYM && Sim->GetRoomAirDomain()==B && C->GetLayoutUndoCount()==1 && UnchangedLive());
	TestTrue(TEXT("Height Undo/Redo restores exact room via shared record"),C->TryUndoLayout() && Sim->GetRoomAirDomain()==A
		&& C->TryRedoLayout() && Sim->GetRoomAirDomain()==B && UnchangedLive());
	F.Racks[0]->SetActorLocation(Pose.GetLocation()+FVector(0,0,140));
	TestFalse(TEXT("Undo cannot restore ceiling below current elevated equipment"),C->TryUndoLayout());
	TestTrue(TEXT("Height restore refusal retains setting and record"),Sim->GetRoomAirDomain()==B && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0);
	F.Racks[0]->SetActorTransform(Pose); C->TryUndoLayout();
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,88),Side); C->EndRoomCornerDrag(false);
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,-200),Side); C->EndRoomCornerDrag(true);
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(FVector(-20,5,1000),FVector(0,0,-1)); C->EndRoomCornerDrag(true);
	C->BeginRoomHeightDrag(Eye,Side); C->EndRoomCornerDrag(true);
	auto OffGrid=A; OffGrid.CeilingHeightM=3.12; Sim->TrySetRoomAirDomain(OffGrid,Message);
	C->BeginRoomHeightDrag(Eye,Side); C->EndRoomCornerDrag(true);
	TestTrue(TEXT("No-movement click preserves unsnapped numeric height and existing Redo"),Sim->GetRoomAirDomain()==OffGrid && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
	Sim->TrySetRoomAirDomain(A,Message);
	TestTrue(TEXT("Canceled/rejected/no-op heights retain original room, Redo and warm states"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1 && UnchangedLive());
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,88),Side);
	auto External=A; External.CeilingHeightM=3.2; Sim->TrySetRoomAirDomain(External,Message);
	TestFalse(TEXT("External room change refuses stale height confirmation"),C->EndRoomCornerDrag(true));
	TestTrue(TEXT("External height and both stacks retained"),Sim->GetRoomAirDomain()==External && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
	Sim->TrySetRoomAirDomain(A,Message);
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,88),Side);
	F.Racks[0]->SetActorLocation(Pose.GetLocation()+FVector(0,0,400));
	TestFalse(TEXT("Commit revalidates equipment raised after valid preview"),C->EndRoomCornerDrag(true));
	TestTrue(TEXT("Late obstruction cannot partially change room/history"),Sim->GetRoomAirDomain()==A && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1);
	F.Racks[0]->SetActorTransform(Pose);
	C->BeginRoomHeightDrag(Eye,Side); C->UpdateRoomCornerDrag(Eye+FVector(0,0,88),Side);
	TestTrue(TEXT("New confirmed height alone clears Redo"),C->EndRoomCornerDrag(true) && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0 && UnchangedLive());
	const FString File=TEXT("AutomationRoomHeightHistory"); TestTrue(TEXT("Existing v2 saves confirmed height"),VrgLayoutFile::Save(F.W,File,Message));
	Sim->TrySetRoomAirDomain(External,Message);
	TestTrue(TEXT("Repeated Load restores height/XY, clears records and cancels handles"),VrgLayoutFile::Load(F.W,File,Message) && VrgLayoutFile::Load(F.W,File,Message)
		&& Sim->GetRoomAirDomain()==B && !C->IsRoomBoundsEditing() && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
	const auto Loaded=Sim->GetSnapshot();
	TestTrue(TEXT("Successful Load retains stopped clock/old air; existing rebuild clears coupling ledger"),Loaded.SimTimeS==Warm.SimTimeS && Loaded.EnergyInJ==0 && Loaded.EnergyOutJ==0
		&& Temps==F.Air->GetMutableSolver().GetGrid().TemperatureK && F.Air->GetActorLocation()==Origin && F.Air->GridParams.NumCellsZ==Grid.NumCellsZ);
	FRoomCornerWorld Fresh; const auto FreshOrigin=Fresh.Air->GetActorLocation(); const auto FreshGrid=Fresh.Air->GridParams;
	TestTrue(TEXT("Fresh repeated Load restores exact height/XY without applying grid"),VrgLayoutFile::Load(Fresh.W,File,Message) && VrgLayoutFile::Load(Fresh.W,File,Message)
		&& Fresh.Sim->GetRoomAirDomain()==B && Fresh.C->GetLayoutUndoCount()==0 && Fresh.C->GetLayoutRedoCount()==0
		&& Fresh.Air->GetActorLocation()==FreshOrigin && Fresh.Air->GridParams.NumCellsZ==FreshGrid.NumCellsZ);
	int32 RackCount=0,CduCount=0; for(TActorIterator<ARackThermalActor> It(Fresh.W);It;++It) ++RackCount;
	for(TActorIterator<ACoolingDistributionUnitActor> It(Fresh.W);It;++It) ++CduCount;
	TestTrue(TEXT("Fresh repeated height Load keeps two racks/one CDU"),RackCount==2 && CduCount==1);
	Fresh.Sim->EnterSimulationMode(); Fresh.Sim->Tick(.021f); const auto First=Fresh.Sim->GetSnapshot();
	TestTrue(TEXT("Next entry applies sameXY and16Z cells for4m height"),Fresh.Air->GetActorLocation()==FVector(-550,-450,0)
		&& Fresh.Air->GridParams.NumCellsX==43 && Fresh.Air->GridParams.NumCellsY==37 && Fresh.Air->GridParams.NumCellsZ==16);
	for(int32 I=0;I<25;++I) Fresh.Sim->Tick(.04f);
	const auto S=Fresh.Sim->GetSnapshot(); const auto AS=Fresh.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
	const double ItRelative=FMath::Abs(First.TotalItHeatW-ReferenceItW)/ReferenceItW;
	TestTrue(TEXT("Existing height IT/energy/pressure/hydraulic/duct/coil criteria"),First.SimTimeS==Fresh.Sim->FixedStepS && ItRelative<1e-9
		&& FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5
		&& S.bHydraulicsConverged && !AS.bDuctsRejected && S.CrahCoilRemovedW>0);
	for(auto* R:Fresh.Racks) for(double K:R->Input.SlotAirInletTempK) TestTrue(TEXT("Existing transient15..35C inlet safety"),K>288.15 && K<308.15);
	AddInfo(FString::Printf(TEXT("Room height: saved height delta %.3e m, XY unchanged, grid43x37x16=25456cells; IT relative %.3e, global %.3e, normalized pressure %.3e at %.2fs; warm0.02s edit/rejection/Undo state unchanged, successful Load clears old bridge ledger."),
		FMath::Abs(Fresh.Sim->GetRoomAirDomain().CeilingHeightM-B.CeilingHeightM),ItRelative,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
	Fresh.Sim->EnterLayoutMode(); Fresh.C->SetRoomBoundsEditing(true); Fresh.C->BeginRoomHeightDrag(Eye,Side);
	Fresh.C->UpdateRoomCornerDrag(Eye+FVector(0,0,50),Side); Fresh.Sim->EnterSimulationMode(); Fresh.C->RefreshInteractionInputMode();
	TestTrue(TEXT("External simulation entry cancels unconfirmed height without record"),!Fresh.C->IsRoomBoundsEditing() && !Fresh.C->IsLayoutDragging()
		&& Fresh.Sim->GetRoomAirDomain()==B && Fresh.C->GetLayoutUndoCount()==0);
	return true;
}
#endif
