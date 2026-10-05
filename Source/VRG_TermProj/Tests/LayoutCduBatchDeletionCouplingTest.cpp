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

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduBatchDeletionCouplingTest,"VRG.Interaction.Layout.CduBatchDeletionPersistenceCoupling",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduBatchDeletionCouplingTest::RunTest(const FString& Parameters)
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
 const FString File=TEXT("automation_cdu_batch_delete_")+FGuid::NewGuid().ToString(), BeforeFile=File+TEXT("_before"), NoneFile=File+TEXT("_none"); FString Summary;
 const FVector Down(0,0,-1);
 auto Select=[&](AActor* A,bool Add=false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down,Add); };
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
  AddInfo(FString::Printf(TEXT("CDU batch delete %s: cdu%d ITerr%.3e coolant%.3e global%.3e hydraulic%.3e refs %.6e m3/s %.6e Pa div%.3e pump%.6fW flow%.6fkg/s t%.2fs"),
   Stage,Count(W),ItError,Coolant,S.NormalizedEnergyResidual,H.NormalizedResidual,H.ReferenceFlowM3s,H.ReferencePressurePa,AS.NormalizedDivergence,Pump,Flow,S.SimTimeS));
  Sim->EnterLayoutMode();
 };

 // Paused warm simulation: a failed destruction must preserve both shared and rack integrals, not just actor counts.
 Physics(Base,TEXT("warm before failed batch"));
 const auto Warm=Sim->GetSnapshot();
 const double RackIn=Rack->GetMutableModel().GetEnergyInJ(), RackOut=Rack->GetMutableModel().GetEnergyOutJ();
 const double RackStored=Rack->GetMutableModel().GetEnergyStoredJ();
 Select(Base); Select(Runtime,true);
 const int32 HistoryBefore=C->GetLayoutUndoCount();
 const auto Handler=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* A) {
  if(A==Base) Runtime->SetRole(ROLE_SimulatedProxy);
 }));
 TestFalse(TEXT("Warm middle deletion failure rejects whole group"),C->TryDeleteLayoutCdu()); W->RemoveOnActorDestroyedHandler(Handler);
 Base=VrgCduLifecycle::Find(W,Ids[0]); Runtime=VrgCduLifecycle::Find(W,Ids[2]);
 if(!TestNotNull(TEXT("Warm rollback restored placed CDU"),Base) || !TestNotNull(TEXT("Warm rollback preserved runtime CDU"),Runtime)) {
  W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false;
 }
 const auto After=Sim->GetSnapshot();
 TestTrue(TEXT("Warm rollback preserves complete ledger/clock and coolant/air coupling"),Warm.SimTimeS>0
  && After.SimTimeS==Warm.SimTimeS && After.EnergyInJ==Warm.EnergyInJ && After.EnergyOutJ==Warm.EnergyOutJ
  && After.EnergyStoredJ==Warm.EnergyStoredJ && After.NormalizedEnergyResidual==Warm.NormalizedEnergyResidual
  && After.CduSupplyTempK==Warm.CduSupplyTempK && After.CduReturnTempK==Warm.CduReturnTempK && After.CduDutyW==Warm.CduDutyW
  && After.RackCoolantMassFlowKgs==Warm.RackCoolantMassFlowKgs && After.RackHeatToAirW==Warm.RackHeatToAirW
  && Rack->GetMutableModel().GetEnergyInJ()==RackIn && Rack->GetMutableModel().GetEnergyOutJ()==RackOut
  && Rack->GetMutableModel().GetEnergyStoredJ()==RackStored);
 TestTrue(TEXT("Warm rollback rebinds CDU assignment/selection and preserves transaction state"),Count(W)==3 && Room->GetRowCdu(0)==Base
  && C->GetSelectedLayoutActors().Num()==2 && C->GetLayoutUndoCount()==HistoryBefore && C->GetLayoutRedoCount()==0
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.IsEmpty());
 AddInfo(FString::Printf(TEXT("Warm batch rollback: t%.2fs stored%.6fJ rackIn%.6fJ rackOut%.6fJ; exact equality of integrals, CDU temperatures/duty and coupling arrays."),
  Warm.SimTimeS,Warm.EnergyStoredJ,RackIn,RackOut));
 TestTrue(TEXT("Delete placed and runtime CDU as one group"),C->TryDeleteLayoutCdu()); Physics(Far,TEXT("mixed provenance delete/reassign"));
 TestTrue(TEXT("One record for both and only placed intent"),Count(W)==1 && C->GetLayoutUndoCount()==HistoryBefore+1
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==1);
 TestTrue(TEXT("Save deleted group inventory"),VrgLayoutFile::Save(W,File,Summary));
 TestTrue(TEXT("Undo whole group"),C->TryUndoLayout()); Base=VrgCduLifecycle::Find(W,Ids[0]); Runtime=VrgCduLifecycle::Find(W,Ids[2]);
 TestEqual(TEXT("Undo restores both"),Count(W),3); Physics(Base,TEXT("mixed group undo"));
 TestTrue(TEXT("Redo whole group"),C->TryRedoLayout()); TestEqual(TEXT("Redo removes both"),Count(W),1); Physics(Far,TEXT("mixed group redo"));
 TestTrue(TEXT("Load original inventory restores both"),VrgLayoutFile::Load(W,BeforeFile,Summary));
 Base=VrgCduLifecycle::Find(W,Ids[0]); Runtime=VrgCduLifecycle::Find(W,Ids[2]); Physics(Base,TEXT("before load"));
 TestTrue(TEXT("Repeated group deletion Load is idempotent and clears history"),VrgLayoutFile::Load(W,File,Summary)
  && VrgLayoutFile::Load(W,File,Summary) && Count(W)==1 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
  && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Physics(Far,TEXT("deleted load"));
 TestTrue(TEXT("Original Load before last group deletion"),VrgLayoutFile::Load(W,BeforeFile,Summary));
 Base=VrgCduLifecycle::Find(W,Ids[0]); Far=VrgCduLifecycle::Find(W,Ids[1]); Runtime=VrgCduLifecycle::Find(W,Ids[2]);
 Select(Base); Select(Far,true); Select(Runtime,true);
 TestTrue(TEXT("One batch deletes every CDU including last"),C->TryDeleteLayoutCdu());
 TestTrue(TEXT("Empty inventory has two placed intents and one history"),Count(W)==0
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==2 && C->GetLayoutUndoCount()==1);
 Physics(nullptr,TEXT("last whole group/radiator"));
 TestTrue(TEXT("Undo last whole group"),C->TryUndoLayout()); Base=VrgCduLifecycle::Find(W,Ids[0]); Physics(Base,TEXT("last group undo"));
 TestTrue(TEXT("Redo last whole group"),C->TryRedoLayout()); Physics(nullptr,TEXT("last group redo"));
 C->ClearLayoutHistory();
 TestTrue(TEXT("Deletion intent persists after history clear and saves"),VrgLayoutFile::Save(W,NoneFile,Summary)
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==2);
 TestTrue(TEXT("Restore original then repeatedly Load empty inventory"),VrgLayoutFile::Load(W,BeforeFile,Summary)
  && VrgLayoutFile::Load(W,NoneFile,Summary) && VrgLayoutFile::Load(W,NoneFile,Summary) && Count(W)==0
  && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Physics(nullptr,TEXT("empty repeated load"));
 UWorld* Fresh=NewWorld(); auto* FS=Fresh->SpawnActor<ADataCenterSimulationActor>(); FS->bShowSummary=false;
 auto* FC=Fresh->SpawnActor<ADataCenterDebugController>();
 FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
 P.Name=Names[0]; Fresh->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(),Transforms[0],P);
 P.Name=Names[1]; auto* FreshFar=Fresh->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(),Transforms[1],P);
 FS->EnterLayoutMode(); FC->SelectLayoutAlongRay(FreshFar->GetActorLocation()+FVector(0,0,500),Down); FC->OnResetSim();
 TestEqual(TEXT("Fresh fixture has prior edit"),FC->GetLayoutUndoCount(),1);
 TestTrue(TEXT("Fresh world deleted group Load/repeat is duplicate-free and clears history"),VrgLayoutFile::Load(Fresh,File,Summary)
  && VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==1 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
  && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==1 && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0);
 TestTrue(TEXT("Fresh world empty group Load/repeat applies both placed intents"),VrgLayoutFile::Load(Fresh,NoneFile,Summary)
  && VrgLayoutFile::Load(Fresh,NoneFile,Summary) && Count(Fresh)==0 && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==2);
 TestTrue(TEXT("Fresh original inventory restores all exactly once"),VrgLayoutFile::Load(Fresh,BeforeFile,Summary)
  && VrgLayoutFile::Load(Fresh,BeforeFile,Summary) && Count(Fresh)==3 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
  && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0 && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.IsEmpty());
 for(int32 I=0;I<3;++I) { auto* Restored=VrgCduLifecycle::Find(Fresh,Ids[I]);
  TestTrue(TEXT("Group identity/name/config/whole transform/provenance restored"),Restored && Restored->GetFName()==Names[I]
   && Restored->CaptureLayoutSettings()==Configs[I] && Restored->GetActorTransform().Equals(Transforms[I],1e-9) && Restored->bRuntimeLayoutCdu==(I==2)); }
 TestEqual(TEXT("Fresh Load never imports warm clock"),FS->GetSnapshot().SimTimeS,0.0);
 TestEqual(TEXT("Fresh Load never imports warm integral"),FS->GetSnapshot().EnergyInJ,0.0);
 AddInfo(FString::Printf(TEXT("CDU batch deletion maxima: baseline IT %.6fW ITerr%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e pump%.6f..%.6fW; 3/1/3/1 and 3/0/3/0, fresh/repeat inventory, paused warm failure preserves state. Existing pump heat split debt unchanged."),
  BaselineIt,MaxIt,MaxCoolant,MaxGlobal,MaxHydraulic,MaxDiv,MinPump,MaxPump));
 for(const auto& F:{File,BeforeFile,NoneFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(F));
 Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
