#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/VrgCduLayoutState.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduDuplicationCouplingTest,"VRG.Interaction.Layout.CduDuplicationPersistenceCoupling",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduDuplicationCouplingTest::RunTest(const FString& Parameters)
{
 auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
 auto Count=[](UWorld* World) { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(World);It;++It) ++N; return N; };
 double MaxIt=0,MaxGlobal=0,MaxCoolant=0,MaxDiv=0,MaxHydraulic=0,MinPump=DBL_MAX,MaxPump=0,MinFlow=DBL_MAX,MaxFlow=0;
 for(int32 Case=0;Case<2;++Case)
 {
  UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->EnterLayoutMode();
  auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
  auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
  Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
  Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
  const FVector Down(0,0,-1);
  auto Select=[&](AActor* A) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down); };
  ACoolingDistributionUnitActor* Base=nullptr;
  if(Case==0) Base=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-500,-150,25),FRotator::ZeroRotator);
  else {
   auto Config=GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings();
   // Synthetic authored copy/serialization conditions, not measured or new hardware defaults.
   Config.PumpSpeedRatio=.8; Config.PumpEfficiency=.65; Config.SizeM*=1.1; Config.bUseReferenceModel=false;
   Config.FacilitySupplyTempK+=2; Config.FacilityMassFlowKgs=3; Config.HxUAWK=5000;
   Config.ConnectionHeightM=.3; Config.ConnectionSpacingM=.4;
   Base=VrgCduLifecycle::Spawn(W,TEXT("RuntimeCduPhysicsSource"),FGuid::NewGuid().ToString(),
    FTransform(FRotator(0,90,0),FVector(-500,-150,25),FVector(.9,1.1,1.2)),Config);
  }
  // Existing deletion intent must survive clone Undo/Redo and both save inventories.
  auto* Deleted=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(1000,1000,0),FRotator::ZeroRotator);
  const FString DeletedId=VrgCduLifecycle::Identity(Deleted); const FName DeletedName=Deleted->GetFName();
  Select(Deleted); TestTrue(TEXT("Unrelated placed deletion"),C->TryDeleteLayoutCdu()); C->ClearLayoutHistory();
  const auto Config=Base->CaptureLayoutSettings(); const auto Original=Base->GetActorTransform();
  const FString SourceId=VrgCduLifecycle::Identity(Base); const FName SourceName=Base->GetFName();
  Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineIt=Sim->GetSnapshot().TotalItHeatW;
  for(int32 I=0;I<25;++I) Sim->Tick(.04f); TestTrue(TEXT("Source coupling has advanced before clone"),Sim->GetSnapshot().SimTimeS>1);
  Sim->EnterLayoutMode(); const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
  const FString File=TEXT("automation_cdu_copy_")+FGuid::NewGuid().ToString(), BeforeFile=File+TEXT("_before"); FString Summary;
  TestTrue(TEXT("Save source and unrelated deletion"),VrgLayoutFile::Save(W,BeforeFile,Summary));
  Select(Base); TestTrue(TEXT("Begin duplicate"),C->BeginCduDuplication());
  C->UpdateRackAdditionAlongRay(FVector(-200,0,500),Down); auto* Clone=C->ConfirmCduAddition();
  if(!TestNotNull(TEXT("One confirmed CDU clone"),Clone)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
  const FString Id=VrgCduLifecycle::Identity(Clone); const FName Name=Clone->GetFName(); const auto T=Clone->GetActorTransform();
  TestTrue(TEXT("Source untouched and clone config independent"),Id!=SourceId && Name!=SourceName && Clone->bRuntimeLayoutCdu
   && Clone->CaptureLayoutSettings()==Config && Base->CaptureLayoutSettings()==Config && Base->GetActorTransform().Equals(Original,0)
   && VrgCduLifecycle::Identity(Base)==SourceId && Base->bRuntimeLayoutCdu==(Case==1));
 auto Physics=[&](bool Present,const TCHAR* Stage) {
  auto* Expected=Present?VrgCduLifecycle::Find(W,Id):Base;
  TestTrue(TEXT("Nearest CDU assignment rebuilt; one rack/branch retained"),Expected && Room->GetRowCdu(0)==Expected
   && Room->GetRoutedRacks().Num()==1 && Room->GetMutableNetwork().GetNumPipes()==Pipes && Room->GetMutableNetwork().GetNumPumps()==1);
  const auto& H=Room->GetLastSolveResult();
  TestTrue(TEXT("Hydraulic convergence and reference scales"),H.bConverged && !H.bIllPosed && !H.bLinearSolveFailed
   && H.NormalizedResidual<Room->SolveSettings.NormalizedResidualTarget && H.ReferenceFlowM3s>0 && H.ReferencePressurePa>0);
  MaxHydraulic=FMath::Max(MaxHydraulic,H.NormalizedResidual);
  Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
  const double ItErr=FMath::Abs(First.TotalItHeatW-BaselineIt)/BaselineIt; MaxIt=FMath::Max(MaxIt,ItErr);
  TestTrue(TEXT("CDU never becomes a rack IT heat source"),ItErr<1e-9 && First.RackHeatToAirW.Num()==1 && First.CduDutyW.Num()==1);
  TestEqual(TEXT("Fresh simulation clock"),First.SimTimeS,Sim->FixedStepS);
  for(int32 I=0;I<25;++I) Sim->Tick(.04f); const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
  TestTrue(TEXT("Coolant group and rack coupling valid"),S.RackCoolantMassFlowKgs.Num()==1 && S.RackCoolantMassFlowKgs[0]>0
   && S.CduDutyW.Num()==1 && S.CduDutyW[0]>0 && S.CduReturnTempK[0]>S.CduSupplyTempK[0]
   && S.CduSupplyTempK[0]>=Expected->FacilitySupplyTempK && S.CduFacilityReturnTempK[0]>=Expected->FacilitySupplyTempK);
  const double HotWK=S.RackCoolantMassFlowKgs[0]*Rack->Params.CoolantCpJKgK;
  const double Coolant=FMath::Abs(S.CduDutyW[0]-HotWK*(S.CduReturnTempK[0]-S.CduSupplyTempK[0]))/S.CduDutyW[0];
  TestTrue(TEXT("Existing CDU coolant transfer ledger"),Coolant<1e-12 && S.TotalRadiatorRejectedW==S.CduDutyW[0]);
  const double Pump=Room->GetTotalPumpElectricalW(),Hydro=Room->GetMutableNetwork().GetPumpHydraulicPowerW(0);
  TestTrue(TEXT("D8 pump electrical power uses operating point"),Pump>0 && FMath::Abs(Pump-Hydro/Expected->PumpEfficiency)/Pump<1e-12);
  const auto Power=Sim->GetEvaluation().GetWindowPower();
  TestTrue(TEXT("Existing electrical ledger includes pump once, separate from IT"),Power.PumpW>0 && FMath::Abs(Power.PumpW-Pump)/Pump<1e-12);
  // Existing pump/IT plausibility limit; positive branch flow and hydraulic mass closure, not new fitted hardware numbers.
  // CLAUDE.md GB200 operating point; https://docs.nvidia.com/dgx/dgxgb200-user-guide/hardware.html
  const double Flow=S.RackCoolantMassFlowKgs[0];
  TestTrue(TEXT("Flow/pump scale safety net"),Flow>0 && Pump/S.TotalItHeatW<.05);
  TestTrue(TEXT("Hydraulic mass conservation"),FMath::Abs(Room->GetMutableNetwork().GetPumpFlowM3s(0)*Sim->CoolantDensityKgM3-Flow)/Flow<1e-9);
  TestTrue(TEXT("Global conservation and pressure convergence"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9
   && S.bHydraulicsConverged && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
  MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(S.NormalizedEnergyResidual)); MaxCoolant=FMath::Max(MaxCoolant,Coolant); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
  MinPump=FMath::Min(MinPump,Pump); MaxPump=FMath::Max(MaxPump,Pump); MinFlow=FMath::Min(MinFlow,Flow); MaxFlow=FMath::Max(MaxFlow,Flow);
  AddInfo(FString::Printf(TEXT("CDU duplicate %s: IT %.6fW err%.3e hydraulic%.3e refs %.6e m3/s %.6e Pa coolant%.3e global%.3e div%.3e pump%.6fW flow%.6fkg/s t%.2fs"),
   Stage,First.TotalItHeatW,ItErr,H.NormalizedResidual,H.ReferenceFlowM3s,H.ReferencePressurePa,Coolant,S.NormalizedEnergyResidual,AS.NormalizedDivergence,Pump,Flow,S.SimTimeS));
  Sim->EnterLayoutMode();
 };

  Physics(true,TEXT("confirm")); TestTrue(TEXT("Undo removes only clone"),C->TryUndoLayout() && Count(W)==1); Physics(false,TEXT("undo"));
  TestTrue(TEXT("Redo same clone"),C->TryRedoLayout() && Count(W)==2); Physics(true,TEXT("redo"));
  TestTrue(TEXT("Save clone and source"),VrgLayoutFile::Save(W,File,Summary));
  TestTrue(TEXT("Undo then load clone"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
  TestTrue(TEXT("Same world repeat load no duplicate and history reset"),VrgLayoutFile::Load(W,File,Summary)
   && Count(W)==2 && Summary.Contains(TEXT("CDU0 생성/0 제거")) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
  Clone=VrgCduLifecycle::Find(W,Id);
  TestTrue(TEXT("Same-world full config/identity/transform"),Clone && Clone->GetFName()==Name && Clone->CaptureLayoutSettings()==Config && Clone->GetActorTransform().Equals(T,1e-9));
  Physics(true,TEXT("load"));
  TestTrue(TEXT("Before file removes clone only"),VrgLayoutFile::Load(W,BeforeFile,Summary) && Count(W)==1 && IsValid(Base)); Physics(false,TEXT("before load"));
  TestTrue(TEXT("Restore clone inventory"),VrgLayoutFile::Load(W,File,Summary));
  TestTrue(TEXT("Original and unrelated deletion invariant"),Base->CaptureLayoutSettings()==Config && Base->GetActorTransform().Equals(Original,0)
   && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(DeletedId));
  UWorld* Fresh=NewWorld(); auto* FS=Fresh->SpawnActor<ADataCenterSimulationActor>(); FS->EnterLayoutMode(); auto* FC=Fresh->SpawnActor<ADataCenterDebugController>();
  FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
  if(Case==0) { P.Name=SourceName; Fresh->SpawnActor<ACoolingDistributionUnitActor>(P); }
  P.Name=DeletedName; Fresh->SpawnActor<ACoolingDistributionUnitActor>(P);
  // Give Load a real edit record so clearing a non-empty history is checked.
  FC->BeginCduAddition(); FC->UpdateRackAdditionAlongRay(FVector(-1800,0,500),Down);
  TestNotNull(TEXT("Create fresh world edit history"),FC->ConfirmCduAddition()); TestEqual(TEXT("Fresh history initially nonempty"),FC->GetLayoutUndoCount(),1);
  TestTrue(TEXT("Fresh/repeated Load recreates clone, reconciles runtime, applies existing deletion"),VrgLayoutFile::Load(Fresh,File,Summary)
   && VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==2 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
   && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0 && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(DeletedId));
  const auto* FreshSource=VrgCduLifecycle::Find(Fresh,SourceId); const auto* FreshClone=VrgCduLifecycle::Find(Fresh,Id);
  TestTrue(TEXT("Fresh source/clone independent identities/config/provenance"),FreshSource && FreshClone && FreshSource!=FreshClone
   && FreshSource->CaptureLayoutSettings()==Config && FreshClone->CaptureLayoutSettings()==Config
   && FreshSource->bRuntimeLayoutCdu==(Case==1) && FreshClone->bRuntimeLayoutCdu
   && FreshSource->GetFName()==SourceName && FreshClone->GetFName()==Name
   && FreshSource->GetActorTransform().Equals(Original,1e-9) && FreshClone->GetActorTransform().Equals(T,1e-9));
  TestEqual(TEXT("Fresh Load never imports clock"),FS->GetSnapshot().SimTimeS,0.0);
  TestTrue(TEXT("Fresh pre-clone inventory removes only clone"),VrgLayoutFile::Load(Fresh,BeforeFile,Summary) && Count(Fresh)==1
   && VrgCduLifecycle::Find(Fresh,SourceId) && !VrgCduLifecycle::Find(Fresh,Id));
  for(const auto& F:{File,BeforeFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(F));
  Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W);
 }
 AddInfo(FString::Printf(TEXT("CDU duplication maxima: ITerr%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e pump%.6f..%.6fW flow%.6f..%.6fkg/s. Placed/default and runtime/synthetic config; nearest reassignment; fresh coupling; existing v2/deletedCdus; pump heat split remains existing debt."),
  MaxIt,MaxCoolant,MaxGlobal,MaxHydraulic,MaxDiv,MinPump,MaxPump,MinFlow,MaxFlow));
 return true;
}
#endif
