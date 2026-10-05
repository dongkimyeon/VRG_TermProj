#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCduLayoutState.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduDeletionPersistenceTest,"VRG.Interaction.Layout.CduDeletionPersistenceCoupling",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduDeletionPersistenceTest::RunTest(const FString& Parameters)
{
 auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
 UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
 auto* Base=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-200,0,0),FRotator::ZeroRotator);
 auto* Far=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-500,-150,0),FRotator::ZeroRotator);
 // Synthetic authored configuration/placement, not new hardware defaults or measured parameters.
 Far->PumpSpeedRatio=.8; Far->PumpEfficiency=.65; Far->bUseReferenceModel=false;
 auto RuntimeSettings=GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings();
 auto* Runtime=VrgCduLifecycle::Spawn(W,TEXT("RuntimeCduDeletionFixture"),FGuid::NewGuid().ToString(),FTransform(FVector(-900,700,10)),RuntimeSettings);
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 const FString Ids[]={VrgCduLifecycle::Identity(Base),VrgCduLifecycle::Identity(Far),VrgCduLifecycle::Identity(Runtime)};
 const FName Names[]={Base->GetFName(),Far->GetFName(),Runtime->GetFName()};
 const FCduLayoutSettings Configs[]={Base->CaptureLayoutSettings(),Far->CaptureLayoutSettings(),RuntimeSettings};
 const FTransform Transforms[]={Base->GetActorTransform(),Far->GetActorTransform(),Runtime->GetActorTransform()};
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
 const double BaselineIt=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode(); const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
 const FString File=TEXT("automation_cdu_delete_")+FGuid::NewGuid().ToString(), BeforeFile=File+TEXT("_before"), NoneFile=File+TEXT("_none"); FString Summary;
 const FVector Down(0,0,-1);
 auto Select=[&](AActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
 auto Count=[](UWorld* World) { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(World);It;++It) ++N; return N; };
 TestTrue(TEXT("Save original placed/runtime CDU inventory"),VrgLayoutFile::Save(W,BeforeFile,Summary));
 double MaxIt=0,MaxCoolant=0,MaxGlobal=0,MaxDiv=0,MaxHydraulic=0,MinPump=DBL_MAX,MaxPump=0;
 auto Physics=[&](ACoolingDistributionUnitActor* Expected,const TCHAR* Stage) {
  const bool HasCdu=Expected!=nullptr;
  TestTrue(TEXT("Reassigned/absent CDU and preserved rack branch"),Room->GetRowCdu(0)==Expected && Room->GetRoutedRacks().Num()==1
   && Room->GetMutableNetwork().GetNumPipes()==Pipes && Room->GetMutableNetwork().GetNumPumps()==1);
  const auto& H=Room->GetLastSolveResult();
  TestTrue(TEXT("Real/fallback hydraulics converged"),H.bConverged && !H.bIllPosed && !H.bLinearSolveFailed
   && H.NormalizedResidual<Room->SolveSettings.NormalizedResidualTarget && H.ReferenceFlowM3s>0 && H.ReferencePressurePa>0);
  MaxHydraulic=FMath::Max(MaxHydraulic,H.NormalizedResidual);
  Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
  const double ItError=FMath::Abs(First.TotalItHeatW-BaselineIt)/BaselineIt; MaxIt=FMath::Max(MaxIt,ItError);
  TestTrue(TEXT("CDU deletion never deletes rack IT or adds a fake heat source"),ItError<1e-9 && First.RackHeatToAirW.Num()==1 && First.CduDutyW.Num()==(HasCdu?1:0));
  TestEqual(TEXT("Rebuilt coupling clock fresh"),First.SimTimeS,Sim->FixedStepS);
  for(int32 I=0;I<25;++I) Sim->Tick(.04f); const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
  const double Flow=S.RackCoolantMassFlowKgs[0],Pump=Room->GetTotalPumpElectricalW();
  TestTrue(TEXT("Coolant/radiator conservation"),Flow>0 && S.TotalRadiatorRejectedW>0);
  const double Coolant=FMath::Abs(Rack->GetMutableModel().GetLastHeatRemovedByCoolantW()-S.TotalRadiatorRejectedW)/S.TotalRadiatorRejectedW;
  TestTrue(TEXT("One transfer in rack and external sink ledger"),Coolant<1e-12);
  if(HasCdu) {
   TestTrue(TEXT("One CDU group and plausible temperatures"),S.CduDutyW.Num()==1 && S.CduDutyW[0]==S.TotalRadiatorRejectedW
    && S.CduReturnTempK[0]>S.CduSupplyTempK[0] && S.CduSupplyTempK[0]>=Expected->FacilitySupplyTempK);
   const double Hydro=Room->GetMutableNetwork().GetPumpHydraulicPowerW(0);
   TestTrue(TEXT("Replacement CDU settings feed hydraulic power"),FMath::Abs(Pump-Hydro/Expected->PumpEfficiency)/Pump<1e-12);
  } else TestTrue(TEXT("Last CDU removal activates radiator, not a stale HX group"),S.CduDutyW.IsEmpty() && S.CduSupplyTempK.IsEmpty());
  const auto Power=Sim->GetEvaluation().GetWindowPower();
  TestTrue(TEXT("Pump power in electrical ledger only once"),Pump>0 && FMath::Abs(Power.PumpW-Pump)/Pump<1e-12);
  // Existing GB200 pump/IT safety net and SI mass closure. No new fitted hardware values.
  TestTrue(TEXT("Plausibility and hydraulic mass closure"),Pump/S.TotalItHeatW<.05
   && FMath::Abs(Room->GetMutableNetwork().GetPumpFlowM3s(0)*Sim->CoolantDensityKgM3-Flow)/Flow<1e-9);
  TestTrue(TEXT("Global conservation and air pressure"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9
   && S.bHydraulicsConverged && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
  MaxCoolant=FMath::Max(MaxCoolant,Coolant); MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
  MinPump=FMath::Min(MinPump,Pump); MaxPump=FMath::Max(MaxPump,Pump);
  AddInfo(FString::Printf(TEXT("CDU delete %s: cdu%d ITerr%.3e coolant%.3e global%.3e hydraulic%.3e refs %.6e m3/s %.6e Pa div%.3e pump%.6fW flow%.6fkg/s t%.2fs"),
   Stage,Count(W),ItError,Coolant,S.NormalizedEnergyResidual,H.NormalizedResidual,H.ReferenceFlowM3s,H.ReferencePressurePa,AS.NormalizedDivergence,Pump,Flow,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Select(Base); TestTrue(TEXT("Delete placed CDU"),C->TryDeleteLayoutCdu()); Physics(Far,TEXT("delete placed/reassign"));
 TestTrue(TEXT("Explicit placed deletion saved"),VrgLayoutFile::Save(W,File,Summary) && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==1);
 TestTrue(TEXT("Undo placed deletion"),C->TryUndoLayout()); Base=VrgCduLifecycle::Find(W,Ids[0]); Physics(Base,TEXT("undo"));
 TestTrue(TEXT("Redo placed deletion"),C->TryRedoLayout()); Physics(Far,TEXT("redo"));
 C->ClearLayoutHistory(); TestTrue(TEXT("Deletion intent outlives history clear"),VrgLayoutFile::Save(W,File,Summary)
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(Ids[0]));
 TestTrue(TEXT("Load original inventory restores placed CDU"),VrgLayoutFile::Load(W,BeforeFile,Summary)); Base=VrgCduLifecycle::Find(W,Ids[0]); Physics(Base,TEXT("before load"));
 TestTrue(TEXT("Load deleted inventory/repeat"),VrgLayoutFile::Load(W,File,Summary) && VrgLayoutFile::Load(W,File,Summary)
  && Count(W)==2 && Summary.Contains(TEXT("CDU0 생성/0 제거")) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Physics(Far,TEXT("deleted load"));
 Select(Runtime); TestTrue(TEXT("Delete runtime CDU"),C->TryDeleteLayoutCdu()); TestEqual(TEXT("Only placed deletion intent recorded"),W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num(),1);
 TestTrue(TEXT("Runtime deletion undo/redo"),C->TryUndoLayout() && C->TryRedoLayout());
 Select(Far); TestTrue(TEXT("Delete last CDU"),C->TryDeleteLayoutCdu()); Physics(nullptr,TEXT("last delete/radiator"));
 TestTrue(TEXT("Undo last deletion"),C->TryUndoLayout()); Far=VrgCduLifecycle::Find(W,Ids[1]); Physics(Far,TEXT("last undo"));
 TestTrue(TEXT("Redo last deletion"),C->TryRedoLayout()); Physics(nullptr,TEXT("last redo"));
 TestTrue(TEXT("Save absent CDU inventory"),VrgLayoutFile::Save(W,NoneFile,Summary));
 TestTrue(TEXT("Restore all then reload absent inventory"),VrgLayoutFile::Load(W,BeforeFile,Summary) && VrgLayoutFile::Load(W,NoneFile,Summary)
  && Count(W)==0 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Physics(nullptr,TEXT("none load"));
 UWorld* Fresh=NewWorld(); auto* FS=Fresh->SpawnActor<ADataCenterSimulationActor>(); auto* FC=Fresh->SpawnActor<ADataCenterDebugController>();
 FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
 P.Name=Names[0]; Fresh->SpawnActor<ACoolingDistributionUnitActor>(P); P.Name=Names[1]; Fresh->SpawnActor<ACoolingDistributionUnitActor>(P);
 TestTrue(TEXT("Fresh world applies both explicit placed deletions/repeat"),VrgLayoutFile::Load(Fresh,NoneFile,Summary)
  && VrgLayoutFile::Load(Fresh,NoneFile,Summary) && Count(Fresh)==0 && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==2);
 TestTrue(TEXT("Fresh original inventory restores all and repeats without duplicates"),VrgLayoutFile::Load(Fresh,BeforeFile,Summary)
  && VrgLayoutFile::Load(Fresh,BeforeFile,Summary) && Count(Fresh)==3 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
  && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0);
 for(int32 I=0;I<3;++I) { auto* Restored=VrgCduLifecycle::Find(Fresh,Ids[I]);
  TestTrue(TEXT("Identity/name/config/transform/provenance restored"),Restored && Restored->GetFName()==Names[I] && Restored->CaptureLayoutSettings()==Configs[I]
   && Restored->GetActorTransform().Equals(Transforms[I],1e-9) && Restored->bRuntimeLayoutCdu==(I==2)); }
 TestEqual(TEXT("Load never imports simulation time"),FS->GetSnapshot().SimTimeS,0.0);
 // Malformed live/deleted duplicate and runtime deletion intent must fail before mutation.
 FString Json; FFileHelper::LoadFileToString(Json,*VrgLayoutFile::PathFor(BeforeFile)); TSharedPtr<FJsonObject> Root;
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Root);
 auto Write=[&]() { FString Text; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Text)); FFileHelper::SaveStringToFile(Text,*VrgLayoutFile::PathFor(File)); };
 TSharedPtr<FJsonObject> RuntimeEntry;
 for(const auto& V:Root->GetArrayField(TEXT("equipment"))) if(V->AsObject()->GetStringField(TEXT("name"))==Names[2].ToString()) RuntimeEntry=V->AsObject();
 Root->SetArrayField(TEXT("deletedCdus"),{MakeShared<FJsonValueObject>(RuntimeEntry)}); Write();
 TestFalse(TEXT("Live/deleted duplicate rejects transaction"),VrgLayoutFile::Load(Fresh,File,Summary));
 TestTrue(TEXT("Rejected load leaves inventory/history/deletion intent unchanged"),Count(Fresh)==3 && FC->GetLayoutUndoCount()==0 && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.IsEmpty());
 RuntimeEntry->SetStringField(TEXT("name"),TEXT("InvalidRuntimeDeletionRecord")); RuntimeEntry->SetStringField(TEXT("cduId"),TEXT("invalid-runtime-delete")); Write();
 TestFalse(TEXT("Runtime CDU in placed-deletion list rejected"),VrgLayoutFile::Load(Fresh,File,Summary));
 // Legacy omissions have no placed deletion intent; old v2 and v1 preserve all CDUs.
 TArray<TSharedPtr<FJsonValue>> Legacy;
 for(const auto& V:Root->GetArrayField(TEXT("equipment"))) if(!V->AsObject()->HasField(TEXT("cduId"))) Legacy.Add(V);
 Root->SetArrayField(TEXT("equipment"),Legacy); Root->RemoveField(TEXT("deletedCdus")); Root->RemoveField(TEXT("cduInventory")); Write();
 TestTrue(TEXT("Old v2 omission preserves placed and runtime CDUs"),VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==3);
 Root->SetNumberField(TEXT("version"),1); Write(); TestTrue(TEXT("Old v1 omission remains transform-only"),VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==3);
 AddInfo(FString::Printf(TEXT("CDU deletion maxima: baseline IT %.6fW ITerr%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e pump%.6f..%.6fW; nearest replacement, fallback radiator/virtual hydraulic pump, placed/runtime fresh/repeat/legacy files; pump heat split remains existing debt."),
  BaselineIt,MaxIt,MaxCoolant,MaxGlobal,MaxHydraulic,MaxDiv,MinPump,MaxPump));
 for(const auto& F:{File,BeforeFile,NoneFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(F));
 Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
