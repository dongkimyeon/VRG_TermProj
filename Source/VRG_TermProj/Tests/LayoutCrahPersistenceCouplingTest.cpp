#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahPersistenceCouplingTest,"VRG.Interaction.Layout.CrahPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCrahPersistenceCouplingTest::RunTest(const FString& Parameters)
{
	struct FFixture {
		UWorld* W; ADataCenterSimulationActor* Sim; ADataCenterDebugController* C; ADataCenterRoomActor* Room; AAirFieldDebugActor* Air;
		FFixture(int32 Mode) {
			W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
			Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->CrahLayout=Mode==0?ECrahLayout::InRow:ECrahLayout::Perimeter;
			C=W->SpawnActor<ADataCenterDebugController>(); Room=W->SpawnActor<ADataCenterRoomActor>(); Air=W->SpawnActor<AAirFieldDebugActor>();
			FActorSpawnParameters CP; CP.Name=TEXT("CrahTestCdu");
			W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,-200,0),FRotator::ZeroRotator,CP);
			for(int32 I=0;I<2;++I) { FActorSpawnParameters P; P.Name=FName(*FString::Printf(TEXT("CrahTestRack%d"),I));
				auto* Rack=W->SpawnActor<ARackThermalActor>(FVector(I==0?-150:150,0,0),FRotator(0,I==0?0:180,0),P);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false; }
			Room->RebuildRoomNow(); Sim->EnterLayoutMode();
		}
		~FFixture() { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); }
		int32 Count() const { int32 N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N; }
	};
	for(int32 Mode=0;Mode<2;++Mode)
	{
		FFixture F(Mode);
		const int32 AutoCount=Mode==0?4:2;
		const FString Base=FString::Printf(TEXT("AutomationCrahBase%d"),Mode), Edited=Base+TEXT("Edited"), Bad=Base+TEXT("Bad"), Legacy=Base+TEXT("Legacy"); FString Summary;
		TestTrue(TEXT("Save automatic regime"),VrgLayoutFile::Save(F.W,Base,Summary));
		F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f);
		const double BaselineIt=F.Sim->GetSnapshot().TotalItHeatW;
		const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
		TestEqual(TEXT("Automatic physical units"),F.Sim->GetNumCrahUnits(),AutoCount);
		for(int32 I=0;I<25;++I) F.Sim->Tick(.04f); F.Sim->EnterLayoutMode();
		const auto Warm=F.Sim->GetSnapshot();
		F.C->BeginCrahAddition(); F.C->UpdateRackAdditionAlongRay(FVector(0,250,500),FVector(0,0,-1));
		int32 SpawnCount=0;
		const auto Handler=F.W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor) {
			if(Cast<ACrahUnitActor>(Actor) && ++SpawnCount==2) Actor->Destroy(false,false);
		}));
		TestNull(TEXT("Warm conversion failure"),F.C->ConfirmCrahAddition()); F.W->RemoveOnActorSpawnedHandler(Handler);
		const auto Failed=F.Sim->GetSnapshot();
		TestTrue(TEXT("Failed conversion keeps live time, energy and airflow exactly"),F.Count()==0 && F.C->GetLayoutUndoCount()==0
			&& Failed.SimTimeS==Warm.SimTimeS && Failed.EnergyInJ==Warm.EnergyInJ && Failed.EnergyOutJ==Warm.EnergyOutJ
			&& Failed.EnergyStoredJ==Warm.EnergyStoredJ && Failed.RackAirMassFlowKgs==Warm.RackAirMassFlowKgs);
		auto* Added=F.C->ConfirmCrahAddition();
		if(!TestNotNull(TEXT("Confirm conversion and addition"),Added)) continue;
		const FString AddedId=VrgCrahLifecycle::Identity(Added);
		double MaxEnergy=0,MaxFlow=0,MaxFan=0,MaxDiv=0;
		auto Physics=[&](FFixture& X,int32 Expected,bool Placed,const TCHAR* Stage) {
			X.Sim->EnterSimulationMode(); X.Sim->Tick(.021f);
			TestTrue(TEXT("CRAH never counted as rack IT"),FMath::Abs(X.Sim->GetSnapshot().TotalItHeatW-BaselineIt)/BaselineIt<1e-9);
			for(int32 I=0;I<20;++I) X.Sim->Tick(.04f);
			const auto S=X.Sim->GetSnapshot(); const auto A=X.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
			const auto& GP=X.Air->GetMutableSolver().GetGrid().GetParams();
			const double Design=X.Sim->GetDesignAirLoadW()/(GP.DensityKgM3*GP.SpecificHeatJKgK*X.Sim->CrahDesignAirDeltaTK);
			const double FlowError=FMath::Abs(S.CrahFlowM3s-Design)/Design;
			const double ExpectedFan=X.Sim->CrahFanPressurePa*S.CrahFlowM3s/X.Sim->CrahFanEfficiency;
			const double FanError=FMath::Abs(S.CrahFanPowerW-ExpectedFan)/ExpectedFan;
			TestTrue(TEXT("Every converted/new unit participates"),X.Sim->GetNumCrahUnits()==Expected && X.Sim->GetNumActiveCrahUnits()==Expected && X.Sim->IsUsingPlacedCrahs()==Placed && S.CrahCoilRemovedW>0);
			TestTrue(TEXT("Existing design flow and D8 fan operating power"),Design>0 && ExpectedFan>0 && FlowError<1e-9 && FanError<1e-9);
			TestTrue(TEXT("Unchanged hydraulic inventory and rack targets"),X.Room->GetMutableNetwork().GetNumPipes()==Pipes && S.RackAirMassFlowKgs.Num()==2);
			const double DuctError=FMath::Abs(A.DuctFlowM3s-(S.RackAirMassFlowKgs[0]+S.RackAirMassFlowKgs[1])/GP.DensityKgM3-S.CrahFlowM3s)/A.DuctFlowM3s;
			TestTrue(TEXT("No lost rack or cooler duct flow"),!S.bAirDuctsRejected && !A.bDuctsRejected && DuctError<1e-9);
			TestTrue(TEXT("Energy and pressure criteria"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && A.bPressureConverged && A.NormalizedDivergence<1e-5);
			MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxFlow=FMath::Max(MaxFlow,FMath::Max(FlowError,DuctError)); MaxFan=FMath::Max(MaxFan,FanError); MaxDiv=FMath::Max(MaxDiv,A.NormalizedDivergence);
			AddInfo(FString::Printf(TEXT("CRAH %d %s: count %d flow %.6f m3/s fan %.6f W cooling %.6f W energy %.3e duct %.3e divergence %.3e"),Mode,Stage,Expected,S.CrahFlowM3s,S.CrahFanPowerW,S.CrahCoilRemovedW,S.NormalizedEnergyResidual,DuctError,A.NormalizedDivergence));
			X.Sim->EnterLayoutMode();
		};
		Physics(F,AutoCount+1,true,TEXT("add"));
		TestTrue(TEXT("Undo restores auto"),F.C->TryUndoLayout()); Physics(F,AutoCount,false,TEXT("undo"));
		TestTrue(TEXT("Redo restores placed"),F.C->TryRedoLayout()); Physics(F,AutoCount+1,true,TEXT("redo"));
		Added=VrgCrahLifecycle::Find(F.W,AddedId); Added->VolumeFlowM3s=F.Sim->GetSnapshot().CrahFlowM3s*.1; Added->RefreshVisuals();
		// Synthetic move: loaded conversion metadata must retain the original bounds, not rebase onto this pose.
  for(TActorIterator<ACrahUnitActor> It(F.W);It;++It) if(It->bConvertedAutomaticCrah) {
   It->AddActorWorldOffset(FVector(0,It->GetActorLocation().Y<0?-50:50,0));
   TestFalse(TEXT("Moved converted unit expands air bounds"),It->PreservesAutomaticAirBounds()); break;
  }
  TArray<FCrahLayoutRecord> Records;
		for(TActorIterator<ACrahUnitActor> It(F.W);It;++It) Records.Add({VrgCrahLifecycle::Identity(*It),It->GetFName(),It->GetActorTransform(),It->CaptureLayoutSettings(),It->bRuntimeLayoutCrah,It->bConvertedAutomaticCrah,It->AutomaticLayoutTransform,It->AutomaticLayoutSizeM});
		TestTrue(TEXT("Save placed inventory"),VrgLayoutFile::Save(F.W,Edited,Summary));
		FString Text; FFileHelper::LoadFileToString(Text,*VrgLayoutFile::PathFor(Base)); TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root); Root->RemoveField(TEXT("crahInventory")); Root->RemoveField(TEXT("deletedCrahs"));
		Text.Reset(); FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Text)); FFileHelper::SaveStringToFile(Text,*VrgLayoutFile::PathFor(Legacy));
		TestTrue(TEXT("Old v2 omission preserves placed units"),VrgLayoutFile::Load(F.W,Legacy,Summary)); TestEqual(TEXT("Legacy count"),F.Count(),AutoCount+1);
		TestTrue(TEXT("Load base restores automatic regime"),VrgLayoutFile::Load(F.W,Base,Summary)); TestEqual(TEXT("All runtime units removed"),F.Count(),0);
		TestTrue(TEXT("Load edited restores units"),VrgLayoutFile::Load(F.W,Edited,Summary));
		TestTrue(TEXT("Repeated load idempotent"),VrgLayoutFile::Load(F.W,Edited,Summary)); TestEqual(TEXT("No duplicate units"),F.Count(),AutoCount+1);
		TestTrue(TEXT("Load clears both histories"),F.C->GetLayoutUndoCount()==0 && F.C->GetLayoutRedoCount()==0);
		FFileHelper::LoadFileToString(Text,*VrgLayoutFile::PathFor(Edited)); FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root);
		for(const auto& Item:Root->GetArrayField(TEXT("equipment"))) if(Item->AsObject()->HasField(TEXT("crahSettings"))) { Item->AsObject()->GetObjectField(TEXT("crahSettings"))->SetNumberField(TEXT("volumeFlowM3s"),-1); break; }
		Text.Reset(); FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Text)); FFileHelper::SaveStringToFile(Text,*VrgLayoutFile::PathFor(Bad));
		TestFalse(TEXT("Invalid flow rejects entire load"),VrgLayoutFile::Load(F.W,Bad,Summary)); TestEqual(TEXT("Malformed load count unchanged"),F.Count(),AutoCount+1);
		Physics(F,AutoCount+1,true,TEXT("load"));
		{ FFixture Other(Mode); TestTrue(TEXT("Separate world loads runtime coolers"),VrgLayoutFile::Load(Other.W,Edited,Summary));
			for(const auto& R:Records) { auto* U=VrgCrahLifecycle::Find(Other.W,R.Id); TestTrue(TEXT("New world identity/settings/transform"),U && U->GetFName()==R.Name && U->CaptureLayoutSettings()==R.Settings && U->GetActorTransform().Equals(R.Transform,1e-9)
     && U->bConvertedAutomaticCrah==R.bConvertedAutomatic && (!R.bConvertedAutomatic || (U->AutomaticLayoutTransform.Equals(R.AutomaticTransform,1e-9)
     && U->AutomaticLayoutSizeM.Equals(R.AutomaticSizeM,1e-9)))); }
			TestTrue(TEXT("New world repeated load"),VrgLayoutFile::Load(Other.W,Edited,Summary)); Physics(Other,AutoCount+1,true,TEXT("newworld")); }
		AddInfo(FString::Printf(TEXT("CRAH summary %d: IT delta 0 W, max energy %.3e flow %.3e fan %.3e divergence %.3e; warm rollback exact; v2/legacy/newworld passed"),Mode,MaxEnergy,MaxFlow,MaxFan,MaxDiv));
		for(const auto& Name:{Base,Edited,Bad,Legacy}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Name));
	}
	return true;
}
#endif
