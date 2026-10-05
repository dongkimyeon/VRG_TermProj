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


#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduBatchDuplicationCouplingTest,"VRG.Interaction.Layout.CduBatchDuplicationPersistenceCoupling",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduBatchDuplicationCouplingTest::RunTest(const FString& Parameters)
{
 auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
 auto Count=[](UWorld* World) { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(World);It;++It) ++N; return N; };
 double MaxIt=0,MaxGlobal=0,MaxCoolant=0,MaxDiv=0,MaxHydraulic=0,MinPump=DBL_MAX,MaxPump=0,MinFlow=DBL_MAX,MaxFlow=0;
 UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->EnterLayoutMode();
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 const FVector Down(0,0,-1);
 auto Select=[&](AActor* A,bool Add=false) { C->SelectLayoutAlongRay(A->GetActorLocation()+FVector(0,0,500),Down,Add); };
 auto* Base=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-700,-200,25),FRotator::ZeroRotator);
 auto Config=GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings();
 // Synthetic authored copy/serialization conditions, not measured or new hardware defaults.
 Config.PumpSpeedRatio=.8; Config.PumpEfficiency=.65; Config.SizeM*=1.1; Config.bUseReferenceModel=false;
 Config.FacilitySupplyTempK+=2; Config.FacilityMassFlowKgs=3; Config.HxUAWK=5000;
 Config.ConnectionHeightM=.3; Config.ConnectionSpacingM=.4;
 auto* Runtime=VrgCduLifecycle::Spawn(W,TEXT("RuntimeCduBatchPhysicsSource"),FGuid::NewGuid().ToString(),
  FTransform(FRotator(0,90,0),FVector(-1200,500,40),FVector(.9,1.1,1.2)),Config);
 auto* Deleted=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(1000,1000,0),FRotator::ZeroRotator);
 const FString DeletedId=VrgCduLifecycle::Identity(Deleted); const FName DeletedName=Deleted->GetFName();
 Select(Deleted); TestTrue(TEXT("Unrelated deletion intent"),C->TryDeleteLayoutCdu()); C->ClearLayoutHistory();
 // Runtime first: translating the group near the rack changes the assigned CDU from default to nondefault.
 const FString SourceIds[]={VrgCduLifecycle::Identity(Runtime),VrgCduLifecycle::Identity(Base)};
 const FName SourceNames[]={Runtime->GetFName(),Base->GetFName()};
 const FCduLayoutSettings Configs[]={Runtime->CaptureLayoutSettings(),Base->CaptureLayoutSettings()};
 const FTransform Originals[]={Runtime->GetActorTransform(),Base->GetActorTransform()};
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineIt=Sim->GetSnapshot().TotalItHeatW;
 for(int32 I=0;I<25;++I) Sim->Tick(.04f); Sim->EnterLayoutMode(); const auto Warm=Sim->GetSnapshot();
 const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
 const FString File=TEXT("automation_cdu_batch_copy_")+FGuid::NewGuid().ToString(),BeforeFile=File+TEXT("_before"); FString Summary;
 TestTrue(TEXT("Save source inventory with unrelated placed deletion"),VrgLayoutFile::Save(W,BeforeFile,Summary));
 Select(Runtime); Select(Base,true); TestTrue(TEXT("Begin whole duplicate"),C->BeginCduDuplication());
 C->UpdateRackAdditionAlongRay(FVector(-200,0,500),Down);
 int32 Spawned=0;
 const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* A) {
  if(Cast<ACoolingDistributionUnitActor>(A) && A->GetName().StartsWith(TEXT("DuplicatedCdu_")) && ++Spawned==2) A->Destroy(false,false);
 }));
 TestNull(TEXT("Warm second spawn failure refuses whole confirmation"),C->ConfirmCduAddition()); W->RemoveOnActorSpawnedHandler(Handler);
 const auto AfterFail=Sim->GetSnapshot();
 TestTrue(TEXT("Failed creation preserves warm ledger/clock/coupling and inventory/history/deletion"),Warm.SimTimeS>1
  && AfterFail.SimTimeS==Warm.SimTimeS && AfterFail.EnergyInJ==Warm.EnergyInJ && AfterFail.EnergyOutJ==Warm.EnergyOutJ
  && AfterFail.EnergyStoredJ==Warm.EnergyStoredJ && AfterFail.CduSupplyTempK==Warm.CduSupplyTempK && AfterFail.CduDutyW==Warm.CduDutyW
  && AfterFail.RackCoolantMassFlowKgs==Warm.RackCoolantMassFlowKgs && AfterFail.RackHeatToAirW==Warm.RackHeatToAirW
  && Count(W)==2 && Room->GetRowCdu(0)==Base && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0
  && C->GetSelectedLayoutActors().Num()==2 && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(DeletedId));
 auto* Anchor=C->ConfirmCduAddition();
 if(!TestNotNull(TEXT("Whole confirmation creates two CDU clones"),Anchor)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
 FString CloneIds[2]; FName CloneNames[2]; FTransform CloneTransforms[2]; int32 Index=0;
 for(auto* A:C->GetSelectedLayoutActors()) { auto* Clone=CastChecked<ACoolingDistributionUnitActor>(A);
  CloneIds[Index]=VrgCduLifecycle::Identity(Clone); CloneNames[Index]=Clone->GetFName(); CloneTransforms[Index]=Clone->GetActorTransform();
  TestTrue(TEXT("Copied settings/full transform with independent runtime identity"),CloneIds[Index]!=SourceIds[0] && CloneIds[Index]!=SourceIds[1]
   && Clone->bRuntimeLayoutCdu && Clone->CaptureLayoutSettings()==Configs[Index]
   && CloneTransforms[Index].GetLocation().Equals(Originals[Index].GetLocation()+FVector(1000,-500,0),1e-9)
   && CloneTransforms[Index].GetRotation().Equals(Originals[Index].GetRotation(),1e-9)
   && CloneTransforms[Index].GetScale3D().Equals(Originals[Index].GetScale3D(),1e-9)); ++Index; }
 TestTrue(TEXT("Two distinct clones in one record"),Count(W)==4 && CloneIds[0]!=CloneIds[1] && C->GetLayoutUndoCount()==1);
 auto Physics=[&](bool Present,const TCHAR* Stage) {
  auto* Expected=Present?VrgCduLifecycle::Find(W,CloneIds[0]):Base;
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
  AddInfo(FString::Printf(TEXT("CDU batch duplicate %s: IT %.6fW err%.3e hydraulic%.3e refs %.6e m3/s %.6e Pa coolant%.3e global%.3e div%.3e pump%.6fW flow%.6fkg/s t%.2fs"),
   Stage,First.TotalItHeatW,ItErr,H.NormalizedResidual,H.ReferenceFlowM3s,H.ReferencePressurePa,Coolant,S.NormalizedEnergyResidual,AS.NormalizedDivergence,Pump,Flow,S.SimTimeS));
  Sim->EnterLayoutMode();
 };

 Physics(true,TEXT("confirm"));
 TestTrue(TEXT("Whole Undo removes clones only"),C->TryUndoLayout() && Count(W)==2); Physics(false,TEXT("undo"));
 TestTrue(TEXT("Whole Redo restores stable clone identities"),C->TryRedoLayout() && Count(W)==4); Physics(true,TEXT("redo"));
 TestTrue(TEXT("Save full clone inventory"),VrgLayoutFile::Save(W,File,Summary));
 TestTrue(TEXT("Undo then Load restores both clones"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
 TestTrue(TEXT("Repeat Load has no duplicate and clears history"),VrgLayoutFile::Load(W,File,Summary) && Count(W)==4
  && Summary.Contains(TEXT("CDU0 생성/0 제거")) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Physics(true,TEXT("load"));
 TestTrue(TEXT("Before Load removes clones only"),VrgLayoutFile::Load(W,BeforeFile,Summary) && Count(W)==2); Physics(false,TEXT("before load"));
 TestTrue(TEXT("Restore whole clone inventory"),VrgLayoutFile::Load(W,File,Summary));
 for(int32 I=0;I<2;++I) { auto* Source=VrgCduLifecycle::Find(W,SourceIds[I]); auto* Clone=VrgCduLifecycle::Find(W,CloneIds[I]);
  TestTrue(TEXT("Original and both clones restored independently"),Source && Clone && Source!=Clone
   && Source->CaptureLayoutSettings()==Configs[I] && Clone->CaptureLayoutSettings()==Configs[I]
   && Source->GetActorTransform().Equals(Originals[I],0) && Source->GetFName()==SourceNames[I] && Source->bRuntimeLayoutCdu==(I==0)
   && Clone->GetActorTransform().Equals(CloneTransforms[I],1e-9) && Clone->GetFName()==CloneNames[I] && Clone->bRuntimeLayoutCdu); }
 TestTrue(TEXT("Unrelated placed deletion survives all group edits"),W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Num()==1
  && W->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(DeletedId));
 UWorld* Fresh=NewWorld(); auto* FS=Fresh->SpawnActor<ADataCenterSimulationActor>(); FS->bShowSummary=false; FS->EnterLayoutMode();
 auto* FC=Fresh->SpawnActor<ADataCenterDebugController>();
 FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
 P.Name=SourceNames[1]; Fresh->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(),Originals[1],P);
 P.Name=DeletedName; Fresh->SpawnActor<ACoolingDistributionUnitActor>(ACoolingDistributionUnitActor::StaticClass(),FTransform(FVector(1000,1000,0)),P);
 FC->BeginCduAddition(); FC->UpdateRackAdditionAlongRay(FVector(-1800,0,500),Down);
 TestNotNull(TEXT("Fresh fixture nonempty history"),FC->ConfirmCduAddition()); TestEqual(TEXT("Fresh history one"),FC->GetLayoutUndoCount(),1);
 TestTrue(TEXT("Fresh Load restores group/runtime source and deletion; repeat idempotent/history clear"),VrgLayoutFile::Load(Fresh,File,Summary)
  && VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==4 && Summary.Contains(TEXT("CDU0 생성/0 제거"))
  && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0
  && Fresh->GetSubsystem<UVrgCduLayoutState>()->DeletedPlaced.Contains(DeletedId));
 for(int32 I=0;I<2;++I) { auto* Source=VrgCduLifecycle::Find(Fresh,SourceIds[I]); auto* Clone=VrgCduLifecycle::Find(Fresh,CloneIds[I]);
  TestTrue(TEXT("Fresh full settings/transform/provenance and independent identities"),Source && Clone && Source!=Clone
   && Source->GetFName()==SourceNames[I] && Clone->GetFName()==CloneNames[I]
   && Source->CaptureLayoutSettings()==Configs[I] && Clone->CaptureLayoutSettings()==Configs[I]
   && Source->GetActorTransform().Equals(Originals[I],1e-9) && Clone->GetActorTransform().Equals(CloneTransforms[I],1e-9)
   && Source->bRuntimeLayoutCdu==(I==0) && Clone->bRuntimeLayoutCdu); }
 TestTrue(TEXT("Fresh Load never imports warm state/time"),FS->GetSnapshot().SimTimeS==0 && FS->GetSnapshot().EnergyInJ==0);
 TestTrue(TEXT("Fresh pre-clone Load removes only clones"),VrgLayoutFile::Load(Fresh,BeforeFile,Summary)
  && VrgLayoutFile::Load(Fresh,BeforeFile,Summary) && Count(Fresh)==2 && !VrgCduLifecycle::Find(Fresh,CloneIds[0]) && !VrgCduLifecycle::Find(Fresh,CloneIds[1]));
 AddInfo(FString::Printf(TEXT("CDU batch duplication maxima: ITerr%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e pump%.6f..%.6fW flow%.6f..%.6fkg/s. 2/4/2/4 inventory, default/modified mixed sources, nearest reassignment, warm failed creation rollback at %.2fs, fresh coupling/v2/deletedCdus; existing pump heat split debt."),
  MaxIt,MaxCoolant,MaxGlobal,MaxHydraulic,MaxDiv,MinPump,MaxPump,MinFlow,MaxFlow,Warm.SimTimeS));
 for(const auto& F:{File,BeforeFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(F));
 Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
