#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Interaction/VrgLayoutFile.h"
#include "HAL/FileManager.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Visualization/GjRackLayout.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahBatchDeletionCouplingTest,"VRG.Interaction.Layout.CrahBatchDeletionPersistenceCoupling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahBatchDeletionCouplingTest::RunTest(const FString& Parameters)
{
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>();
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator);
 const auto InitialInput=Rack->Input;
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineIt=Sim->GetSnapshot().TotalItHeatW;
 const int32 AutoCount=Sim->GetActiveCrahUnitCount(); const int32 Pipes=Room->GetMutableNetwork().GetNumPipes(); Sim->EnterLayoutMode();
 const FVector Down(0,0,-1); const FVector Destination(180,0,0); FString AddedId;
 double MaxGlobal=0,MaxDiv=0,MaxDuct=0,MaxSize=0,MaxCoolant=0,MaxHydraulic=0;
 double MinInlet=1e9,MaxInlet=0,MinCoil=1e9,MaxCoil=0; double DesignFlow=0,DesignUA=0,DesignFan=0;
 auto Count=[&]() {int N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N;};

 auto Run=[&](int32 ExpectedUnits,bool Placed,const TCHAR* Stage) {
  // Equal authored initial input, not a copied live temperature/integral/clock.
  Rack->Input=InitialInput; Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
  for(int I=0;I<25;++I) Sim->Tick(.04f);
  const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
  const auto& G=Air->GetMutableSolver().GetGrid(); const auto& GP=G.GetParams();
  DesignFlow=Sim->GetDesignAirLoadW()/(GP.DensityKgM3*GP.SpecificHeatJKgK*Sim->CrahDesignAirDeltaTK);
  const double Eps=Sim->CrahDesignAirDeltaTK/(Sim->CrahDesignAirDeltaTK+Sim->CrahDesignApproachK);
  DesignUA=-FMath::Loge(1-Eps)*GP.DensityKgM3*GP.SpecificHeatJKgK*DesignFlow;
  DesignFan=Sim->CrahFanPressurePa*DesignFlow/Sim->CrahFanEfficiency;
  const double FlowErr=FMath::Abs(S.CrahFlowM3s-DesignFlow)/DesignFlow;
  const double UAErr=FMath::Abs(Sim->GetCrahTotalUAWK()-DesignUA)/DesignUA;
  const double FanErr=FMath::Abs(S.CrahFanPowerW-DesignFan)/DesignFan;
  MaxSize=FMath::Max(MaxSize,FMath::Max3(FlowErr,UAErr,FanErr));
  TestTrue(TEXT("All original/new coolers remain active ducts; no fallback or duplication"),Sim->IsUsingPlacedCrahs()==Placed
   && Sim->GetNumCrahUnits()==ExpectedUnits && Sim->GetActiveCrahUnitCount()==ExpectedUnits && !AS.bDuctsRejected && AS.NumDuctFaces>0);
  TestTrue(TEXT("Existing design flow/UA/fan policy reused after count change"),FlowErr<1e-12 && UAErr<1e-12 && FanErr<1e-12);
  TestTrue(TEXT("Fresh clock and cooler is not IT; fan heat enters global ledger once"),First.SimTimeS==Sim->FixedStepS
   && FMath::Abs(First.TotalItHeatW-BaselineIt)/BaselineIt<1e-9
   && FMath::Abs(First.EnergyInJ-(First.TotalItHeatW+First.CrahFanPowerW)*First.SimTimeS)/First.EnergyInJ<1e-12);
  const auto Power=Sim->GetEvaluation().GetWindowPower();
  TestTrue(TEXT("CRAH fan electrical input in separate evaluation category"),FMath::Abs(Power.CrahFanW-DesignFan)/DesignFan<1e-12 && Power.ItW>DesignFan);
  const double DuctFlow=S.RackAirMassFlowKgs[0]/GP.DensityKgM3+DesignFlow;
  const double DuctErr=FMath::Abs(AS.DuctFlowM3s-DuctFlow)/DuctFlow; MaxDuct=FMath::Max(MaxDuct,DuctErr);
  TestTrue(TEXT("Rack plus cooler flow actually passes through air ducts"),DuctErr<1e-9
   && FMath::Abs(S.RackDuctMassFlowKgs[0]-S.RackAirMassFlowKgs[0])/S.RackAirMassFlowKgs[0]<1e-12);
  TestTrue(TEXT("Positive coil duty; cooling solely via CRAH ducts"),S.CrahCoilRemovedW>0
   && FMath::Abs(S.TotalAirCoolingRemovedW-S.CrahCoilRemovedW)/S.CrahCoilRemovedW<1e-9);
  TestTrue(TEXT("Cooling edit preserves rack branch/CDU assignment"),Room->GetRoutedRacks().Num()==1 && Room->GetRowCdu(0)==Cdu
   && Room->GetMutableNetwork().GetNumPipes()==Pipes && S.CduDutyW.Num()==1 && S.RackCoolantMassFlowKgs[0]>0);
  const double Coolant=FMath::Abs(S.CduDutyW[0]-S.RackCoolantMassFlowKgs[0]*Rack->Params.CoolantCpJKgK*(S.CduReturnTempK[0]-S.CduSupplyTempK[0]))/S.CduDutyW[0];
  MaxCoolant=FMath::Max(MaxCoolant,Coolant);
  const auto& HS=Room->GetLastSolveResult(); MaxHydraulic=FMath::Max(MaxHydraulic,HS.NormalizedResidual);
  TestTrue(TEXT("Existing coolant/hydraulic conservation and scale safety net"),Coolant<1e-12 && HS.bConverged
   && HS.ReferenceFlowM3s>0 && HS.ReferencePressurePa>0 && HS.NormalizedResidual<Room->SolveSettings.NormalizedResidualTarget
   && S.CduReturnTempK[0]>S.CduSupplyTempK[0] && S.CduSupplyTempK[0]>=Cdu->FacilitySupplyTempK
   && Room->GetTotalPumpElectricalW()/S.TotalItHeatW<.05);
  // Reuse existing 15~35 C inlet plausibility, not a new calibrated hardware range.
  // CLAUDE.md CRAH validation / existing CrahUnitCarriesAirLoad; transient at t1.02s, not steady state.
  for(const double K:Rack->Input.SlotAirInletTempK) { MinInlet=FMath::Min(MinInlet,K); MaxInlet=FMath::Max(MaxInlet,K);
   TestTrue(TEXT("Existing inlet plausibility range 15~35C"),FMath::IsFinite(K) && K>288.15 && K<308.15); }
  TestTrue(TEXT("Global conservation and pressure converge"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9
   && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
  MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
  MinCoil=FMath::Min(MinCoil,S.CrahCoilRemovedW); MaxCoil=FMath::Max(MaxCoil,S.CrahCoilRemovedW);
  AddInfo(FString::Printf(TEXT("CRAH batch delete %s: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW coil%.6fW IT-first%.6fW; sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e t%.2fs"),
   Stage,ExpectedUnits,DesignFlow,DesignUA,DesignFan,S.CrahCoilRemovedW,First.TotalItHeatW,FMath::Max3(FlowErr,UAErr,FanErr),DuctErr,Coolant,HS.NormalizedResidual,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Run(AutoCount,false,TEXT("automatic"));
 C->BeginInRowCrahAddition(); C->UpdateRackAdditionAlongRay(Destination+FVector(0,0,500),Down); auto* Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Conversion and runtime addition fixture"),Added)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 // Synthetic native placed perimeter cooler plus authored flow share; not a hardware constant/default or measurement.
 auto* Placed=W->SpawnActor<ACrahUnitActor>(FVector(180,150,0),FRotator::ZeroRotator);
 Placed->Kind=ECrahUnitKind::Perimeter; Placed->VolumeFlowM3s=DesignFlow*.1; Placed->RefreshVisuals();
 const FString PlacedId=VrgCrahLifecycle::Identity(Placed),RuntimeId=VrgCrahLifecycle::Identity(Added);
 const FName RackName=Rack->GetFName(),CduName=Cdu->GetFName(),PlacedName=Placed->GetFName();
 const FTransform PlacedTransform=Placed->GetActorTransform();
 TArray<FCrahLayoutRecord> Records;
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) Records.Add({VrgCrahLifecycle::Identity(*It),It->GetFName(),It->GetActorTransform(),It->CaptureLayoutSettings(),
  It->bRuntimeLayoutCrah,It->bConvertedAutomaticCrah,It->AutomaticLayoutTransform,It->AutomaticLayoutSizeM});
 auto Select=[&](AActor* Actor,bool Toggle=false) {C->SelectLayoutAlongRay(Actor->GetActorLocation()+FVector(0,0,500),Down,Toggle);};
 auto Both=[&]() {Placed=VrgCrahLifecycle::Find(W,PlacedId);Added=VrgCrahLifecycle::Find(W,RuntimeId);Select(Placed);Select(Added,true);};
 auto SelectAll=[&]() {bool First=true;for(TActorIterator<ACrahUnitActor> It(W);It;++It) {Select(*It,!First);First=false;}};
 auto CheckInventory=[&]() {
  TestEqual(TEXT("Restored complete inventory count"),Count(),AutoCount+2);
  for(const auto& R:Records) {
   auto* Unit=VrgCrahLifecycle::Find(W,R.Id);
   TestTrue(TEXT("ID/name/config/full transform/origin/original bounds restored"),Unit && Unit->GetFName()==R.Name
    && Unit->CaptureLayoutSettings()==R.Settings && Unit->GetActorTransform().Equals(R.Transform,1e-9)
    && Unit->bRuntimeLayoutCrah==R.bRuntime && Unit->bConvertedAutomaticCrah==R.bConvertedAutomatic
    && (!R.bConvertedAutomatic || (Unit->AutomaticLayoutTransform.Equals(R.AutomaticTransform,1e-9)
     && Unit->AutomaticLayoutSizeM.Equals(R.AutomaticSizeM,1e-9))));
  }
 };
 const FString Before=TEXT("AutomationCrahBatchBefore"),After=Before+TEXT("After"),None=Before+TEXT("None"); FString Summary;
 TestTrue(TEXT("Save before mixed provenance batch"),VrgLayoutFile::Save(W,Before,Summary));
 Run(AutoCount+2,true,TEXT("before")); C->ClearLayoutHistory(); Both();
 const auto Warm=Sim->GetSnapshot();
 const auto Handler=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor) {
  if(Actor==Placed) Added->SetRole(ROLE_SimulatedProxy);
 }));
 TestFalse(TEXT("Warm second deletion failure rolls back whole batch"),C->TryDeleteLayoutCrah()); W->RemoveOnActorDestroyedHandler(Handler);
 const auto Failed=Sim->GetSnapshot(); Placed=VrgCrahLifecycle::Find(W,PlacedId); Added=VrgCrahLifecycle::Find(W,RuntimeId);
 TestTrue(TEXT("Warm rollback keeps actors/selection/intent/history and bridge ledger/time exactly"),Count()==AutoCount+2
  && C->GetSelectedLayoutActors().Num()==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0
  && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.IsEmpty()
  && Warm.SimTimeS==Failed.SimTimeS && Warm.EnergyInJ==Failed.EnergyInJ && Warm.EnergyOutJ==Failed.EnergyOutJ
  && Warm.EnergyStoredJ==Failed.EnergyStoredJ && Warm.CrahFlowM3s==Failed.CrahFlowM3s && Warm.CrahCoilRemovedW==Failed.CrahCoilRemovedW
  && Warm.RackAirMassFlowKgs==Failed.RackAirMassFlowKgs && Warm.RackCoolantMassFlowKgs==Failed.RackCoolantMassFlowKgs
  && Warm.CduDutyW==Failed.CduDutyW);
 CheckInventory();
 TestTrue(TEXT("Mixed placed/runtime deletion one command"),C->TryDeleteLayoutCrah() && Count()==AutoCount && C->GetLayoutUndoCount()==1
  && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
 Run(AutoCount,true,TEXT("delete")); TestTrue(TEXT("Save deleted mixed batch"),VrgLayoutFile::Save(W,After,Summary));
 TestTrue(TEXT("Whole Undo restores both"),C->TryUndoLayout()); CheckInventory(); Run(AutoCount+2,true,TEXT("undo"));
 TestTrue(TEXT("Whole Redo deletes both"),C->TryRedoLayout() && Count()==AutoCount); Run(AutoCount,true,TEXT("redo"));
 TestTrue(TEXT("Load before restores original inventory"),VrgLayoutFile::Load(W,Before,Summary)); CheckInventory(); Run(AutoCount+2,true,TEXT("before-load"));
 TestTrue(TEXT("Repeat deleted Load no duplicates/old history"),VrgLayoutFile::Load(W,After,Summary) && VrgLayoutFile::Load(W,After,Summary)
  && Count()==AutoCount && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0
  && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1); Run(AutoCount,true,TEXT("deleted-load"));
 SelectAll(); TestTrue(TEXT("Delete last converted batch"),C->TryDeleteLayoutCrah() && Count()==0);
 Run(AutoCount,false,TEXT("last-delete-automatic"));
 TestTrue(TEXT("Undo last batch restores original automatic boundaries"),C->TryUndoLayout() && Count()==AutoCount);
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) TestTrue(TEXT("Unchanged converted bounds survive whole Undo"),It->PreservesAutomaticAirBounds());
 Run(AutoCount,true,TEXT("last-undo"));
 TestTrue(TEXT("Redo last batch returns automatic"),C->TryRedoLayout() && Count()==0); Run(AutoCount,false,TEXT("last-redo"));
 TestTrue(TEXT("Save empty actual inventory with placed deletion intent"),VrgLayoutFile::Save(W,None,Summary));
 TestTrue(TEXT("Restore then Load empty inventory twice"),VrgLayoutFile::Load(W,Before,Summary) && VrgLayoutFile::Load(W,None,Summary)
  && VrgLayoutFile::Load(W,None,Summary) && Count()==0 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 Run(AutoCount,false,TEXT("empty-load"));
 UWorld* Previous=W;
 W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; C=W->SpawnActor<ADataCenterDebugController>();
 Room=W->SpawnActor<ADataCenterRoomActor>(); Air=W->SpawnActor<AAirFieldDebugActor>();
 FActorSpawnParameters RP; RP.Name=RackName; Rack=W->SpawnActor<ARackThermalActor>(RP);
 Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false; Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 FActorSpawnParameters CP; CP.Name=CduName; Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator,CP);
 FActorSpawnParameters PP; PP.Name=PlacedName; Placed=W->SpawnActor<ACrahUnitActor>(ACrahUnitActor::StaticClass(),PlacedTransform,PP);
 Room->RebuildRoomNow(); Sim->EnterLayoutMode(); Select(Placed); C->OnResetSim();
 TestEqual(TEXT("Fresh world prior edit exists"),C->GetLayoutUndoCount(),1);
 TestTrue(TEXT("Fresh deleted group Load/repeat clears history and applies placed intent"),VrgLayoutFile::Load(W,After,Summary)
  && VrgLayoutFile::Load(W,After,Summary) && Count()==AutoCount && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0
  && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1);
 TestEqual(TEXT("Load never copies warm clock"),Sim->GetSnapshot().SimTimeS,0.0);
 Run(AutoCount,true,TEXT("fresh-deleted"));
 TestTrue(TEXT("Fresh Load before twice restores original inventory"),VrgLayoutFile::Load(W,Before,Summary) && VrgLayoutFile::Load(W,Before,Summary));
 CheckInventory(); Run(AutoCount+2,true,TEXT("fresh-before"));
 TestTrue(TEXT("Fresh empty Load twice maintains intent"),VrgLayoutFile::Load(W,None,Summary) && VrgLayoutFile::Load(W,None,Summary)
  && Count()==0 && W->GetSubsystem<UVrgCrahLayoutState>()->DeletedPlaced.Num()==1
  && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); Run(AutoCount,false,TEXT("fresh-empty"));
 AddInfo(FString::Printf(TEXT("CRAH batch delete maxima: sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e; inlet%.6f..%.6fK coil%.6f..%.6fW; IT change0W, warm rollback exact; no new physical parameters."),
  MaxSize,MaxDuct,MaxCoolant,MaxHydraulic,MaxGlobal,MaxDiv,MinInlet,MaxInlet,MinCoil,MaxCoil));
 for(const auto& File:{Before,After,None}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
 W->DestroyWorld(false);GEngine->DestroyWorldContext(W);Previous->DestroyWorld(false);GEngine->DestroyWorldContext(Previous);return true;
}
#endif