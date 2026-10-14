#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutPerimeterCrahAdditionCouplingTest,"VRG.Interaction.Layout.PerimeterCrahAdditionPersistenceCoupling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutPerimeterCrahAdditionCouplingTest::RunTest(const FString& Parameters)
{
 for(int32 Mode=0;Mode<2;++Mode) {
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->CrahLayout=Mode==0?ECrahLayout::InRow:ECrahLayout::Perimeter;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>();
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 // Synthetic paired-row fixture: automatic perimeter planning requires facing rows.
 auto* OtherRack=W->SpawnActor<ARackThermalActor>(FVector(300,0,0),FRotator(0,180,0));
 OtherRack->ApplyGb200Nvl72Preset(); OtherRack->bShowReadout=false; OtherRack->RackSizeM=Rack->RackSizeM;
 auto* Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator);
 const auto InitialInput=Rack->Input;
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); const double BaselineIt=Sim->GetSnapshot().TotalItHeatW;
 const int32 AutoCount=Sim->GetActiveCrahUnitCount(); const int32 Pipes=Room->GetMutableNetwork().GetNumPipes(); Sim->EnterLayoutMode();
 const FVector Down(0,0,-1); const FVector Destination(300,300,0); FString AddedId;
 double MaxGlobal=0,MaxDiv=0,MaxDuct=0,MaxSize=0,MaxCoolant=0,MaxHydraulic=0;
 double MinInlet=1e9,MaxInlet=0,MinCoil=1e9,MaxCoil=0; double DesignFlow=0,DesignUA=0,DesignFan=0;
 auto Count=[&]() {int N=0; for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++N; return N;};
 auto MeshCount=[&]() {return Sim->GetNumAutomaticCrahMeshes();};  // not every mesh on Sim: the room shell adds its own
 auto Run=[&](int32 ExpectedUnits,bool Placed,const TCHAR* Stage) {
  // Equal authored initial input, not a copied live temperature/integral/clock.
  Rack->Input=InitialInput; OtherRack->Input=InitialInput; Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
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
  double RackFlow=0;
  for(const double Flow:S.RackAirMassFlowKgs) RackFlow+=Flow;
  const double DuctFlow=RackFlow/GP.DensityKgM3+DesignFlow;
  const double DuctErr=FMath::Abs(AS.DuctFlowM3s-DuctFlow)/DuctFlow; MaxDuct=FMath::Max(MaxDuct,DuctErr);
  TestTrue(TEXT("Rack plus cooler flow actually passes through air ducts"),DuctErr<1e-9
   && FMath::Abs(S.RackDuctMassFlowKgs[0]-S.RackAirMassFlowKgs[0])/S.RackAirMassFlowKgs[0]<1e-12);
  TestTrue(TEXT("Positive coil duty; cooling solely via CRAH ducts"),S.CrahCoilRemovedW>0
   && FMath::Abs(S.TotalAirCoolingRemovedW-S.CrahCoilRemovedW)/S.CrahCoilRemovedW<1e-9);
  if(Placed) {
   auto* Added=VrgCrahLifecycle::Find(W,AddedId);
   if(TestNotNull(TEXT("Added cooler stays present"),Added)) {
    const FVector Point=(Added->GetActorLocation()+FVector(0,0,Added->SizeM.Z*50)-Air->GetActorLocation())/100;
    const double H=G.GetCellSizeM();
   TestTrue(TEXT("Actual added box is a solid in air grid"),G.IsSolidCell(FMath::FloorToInt32(Point.X/H),FMath::FloorToInt32(Point.Y/H),FMath::FloorToInt32(Point.Z/H)));
    // Inspect actual solver velocities, not another copy of the duct-builder inputs.
    const FVector Base=Added->GetActorLocation()/100, Forward=Added->GetActorForwardVector();
    const FVector Side(-Forward.Y,Forward.X,0); const int32 Axis=FMath::Abs(Forward.X)>.5?0:1;
    const int32 Sign=(Axis==0?Forward.X:Forward.Y)>0?1:-1;
    double Intake=0,Supply=0,Upper=0; int32 TopFaces=0,SupplyFaces=0;
    for(int32 Z=0;Z<G.GetNumCellsZ();++Z) for(int32 Y=0;Y<G.GetNumCellsY();++Y) for(int32 X=0;X<G.GetNumCellsX();++X) {
     const FVector P=Air->GetActorLocation()/100+G.CellCenterM(X,Y,Z)-Base;
     if(!G.IsSolidCell(X,Y,Z) || FMath::Abs(FVector::DotProduct(P,Forward))>Added->SizeM.X*.5
      || FMath::Abs(FVector::DotProduct(P,Side))>Added->SizeM.Y*.5 || P.Z<0 || P.Z>Added->SizeM.Z) continue;
     if(G.IsFluidCell(X,Y,Z+1)) {Intake-=G.VelocityZMs[G.FaceZIndex(X,Y,Z+1)]*H*H; ++TopFaces;}
     const int32 FX=X+(Axis==0?Sign:0),FY=Y+(Axis==1?Sign:0);
     if(G.IsFluidCell(FX,FY,Z)) {
      const double Speed=Sign*(Axis==0?G.VelocityXMs[G.FaceXIndex(FMath::Max(X,FX),Y,Z)]
       :G.VelocityYMs[G.FaceYIndex(X,FMath::Max(Y,FY),Z)]);
      if(P.Z<Added->SizeM.Z*.5) {Supply+=Speed*H*H; ++SupplyFaces;}
      else Upper=FMath::Max(Upper,FMath::Abs(Speed));
     }
    }
    double Fixed=0; int32 Sharing=0;
    for(TActorIterator<ACrahUnitActor> It(W);It;++It) {if(It->VolumeFlowM3s>0) Fixed+=It->VolumeFlowM3s; else ++Sharing;}
    const double Expected=Added->VolumeFlowM3s>0?Added->VolumeFlowM3s:(DesignFlow-Fixed)/Sharing;
    const double FaceErr=FMath::Max(FMath::Abs(Intake-Expected),FMath::Abs(Supply-Expected))/Expected;
    MaxDuct=FMath::Max(MaxDuct,FaceErr);
    TestTrue(TEXT("Perimeter top intake/downward flow, lower-front supply, upper front closed"),Added->Kind==ECrahUnitKind::Perimeter
     && TopFaces>0 && SupplyFaces>0 && FaceErr<1e-9 && Upper==0);
    AddInfo(FString::Printf(TEXT("Perimeter port flow: intake%.6f supply%.6f expected%.6fm3/s relative%.3e upper%.6fm/s"),Intake,Supply,Expected,FaceErr,Upper));
   }
  }
  TestTrue(TEXT("Cooling edit preserves rack branch/CDU assignment"),Room->GetRoutedRacks().Num()==2 && Room->GetRowCdu(0)==Cdu
   && Room->GetMutableNetwork().GetNumPipes()==Pipes && S.CduDutyW.Num()==2 && S.RackCoolantMassFlowKgs[0]>0);
  // One rack in each of two rows: existing CDU heat-exchanger groups are per row, not per CDU actor.
  double Coolant=0; bool ValidWater=true;
  for(int32 Group=0;Group<S.CduDutyW.Num();++Group) {
   if(!S.RackCoolantMassFlowKgs.IsValidIndex(Group) || S.CduDutyW[Group]<=0) {ValidWater=false;continue;}
   const double Duty=S.RackCoolantMassFlowKgs[Group]*Rack->Params.CoolantCpJKgK*(S.CduReturnTempK[Group]-S.CduSupplyTempK[Group]);
   Coolant=FMath::Max(Coolant,FMath::Abs(S.CduDutyW[Group]-Duty)/S.CduDutyW[Group]);
   ValidWater&=S.CduReturnTempK[Group]>S.CduSupplyTempK[Group] && S.CduSupplyTempK[Group]>=Cdu->FacilitySupplyTempK;
  }
  MaxCoolant=FMath::Max(MaxCoolant,Coolant);
  const auto& HS=Room->GetLastSolveResult(); MaxHydraulic=FMath::Max(MaxHydraulic,HS.NormalizedResidual);
  TestTrue(TEXT("Existing coolant/hydraulic conservation and scale safety net"),ValidWater && Coolant<1e-12 && HS.bConverged
   && HS.ReferenceFlowM3s>0 && HS.ReferencePressurePa>0 && HS.NormalizedResidual<Room->SolveSettings.NormalizedResidualTarget
   && S.CduReturnTempK[0]>S.CduSupplyTempK[0] && S.CduSupplyTempK[0]>=Cdu->FacilitySupplyTempK
   && Room->GetTotalPumpElectricalW()/S.TotalItHeatW<.05);
  // Reuse existing 15~35 C inlet plausibility, not a new calibrated hardware range.
  // CLAUDE.md CRAH validation / existing CrahUnitCarriesAirLoad; transient at t1.02s, not steady state.
  for(auto* R:{Rack,OtherRack}) for(const double K:R->Input.SlotAirInletTempK) { MinInlet=FMath::Min(MinInlet,K); MaxInlet=FMath::Max(MaxInlet,K);
   TestTrue(TEXT("Existing inlet plausibility range 15~35C"),FMath::IsFinite(K) && K>288.15 && K<308.15); }
  TestTrue(TEXT("Global conservation and pressure converge"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9
   && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
  MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
  MinCoil=FMath::Min(MinCoil,S.CrahCoilRemovedW); MaxCoil=FMath::Max(MaxCoil,S.CrahCoilRemovedW);
  AddInfo(FString::Printf(TEXT("Perimeter add %s: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW coil%.6fW IT-first%.6fW; sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e t%.2fs"),
   Stage,ExpectedUnits,DesignFlow,DesignUA,DesignFan,S.CrahCoilRemovedW,First.TotalItHeatW,FMath::Max3(FlowErr,UAErr,FanErr),DuctErr,Coolant,HS.NormalizedResidual,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Run(AutoCount,false,TEXT("automatic"));
 FString Summary; const FString BaseFile=FString::Printf(TEXT("AutomationPerimeterBase%d"),Mode),Saved=BaseFile+TEXT("Added");
 TestTrue(TEXT("Save original automatic state"),VrgLayoutFile::Save(W,BaseFile,Summary));
 TArray<FCrahLayoutRecord> Automatic; Sim->CaptureAutomaticCrahLayout(Automatic);
 C->BeginPerimeterCrahAddition(); C->UpdateRackAdditionAlongRay(Destination+FVector(0,0,500),Down); C->TryRotateRackAddition();
 auto* Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Wall-kind addition confirmed"),Added)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 AddedId=VrgCrahLifecycle::Identity(Added);
 auto Defaults=GetDefault<ACrahUnitActor>()->CaptureLayoutSettings(); Defaults.Kind=ECrahUnitKind::Perimeter;
 TestTrue(TEXT("Actor defaults reused, fresh runtime origin"),Added->CaptureLayoutSettings()==Defaults && Added->bRuntimeLayoutCrah && !Added->bConvertedAutomaticCrah);
 TestTrue(TEXT("No duplicate automatic displays"),Count()==AutoCount+1 && MeshCount()==0);
 for(const auto& R:Automatic) {
  // Each virtual plan capture allocates fresh IDs; compare planned inputs, not unrelated captures' GUIDs.
  bool Found=false;
  for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah && It->CaptureLayoutSettings()==R.Settings
   && It->GetActorTransform().Equals(R.Transform,1e-9) && It->PreservesAutomaticAirBounds()) Found=true;
  TestTrue(TEXT("Every automatic kind/config/pose/original boundary retained"),Found);
 }
 Run(AutoCount+1,true,TEXT("confirm"));
 const auto Warm=Sim->GetSnapshot(); TArray<ACrahUnitActor*> Converted;
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah) Converted.Add(*It);
 if(!TestTrue(TEXT("Paired automatic fixture provides two failure-injection targets"),Converted.Num()>=2)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 const auto FailUndo=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* A) {
  if(A==Converted[0]) Converted[1]->SetRole(ROLE_SimulatedProxy);
 }));
 TestFalse(TEXT("Warm middle Undo failure rolls back conversion and added unit"),C->TryUndoLayout()); W->RemoveOnActorDestroyedHandler(FailUndo);
 const auto Failed=Sim->GetSnapshot();
 TestTrue(TEXT("Failure preserves warm bridge, actor count, selection and history"),Count()==AutoCount+1
  && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0 && C->GetSelectedLayoutActor()==VrgCrahLifecycle::Find(W,AddedId)
  && Failed.SimTimeS==Warm.SimTimeS && Failed.EnergyInJ==Warm.EnergyInJ && Failed.EnergyOutJ==Warm.EnergyOutJ
  && Failed.EnergyStoredJ==Warm.EnergyStoredJ && Failed.CrahCoilRemovedW==Warm.CrahCoilRemovedW && Failed.CrahFlowM3s==Warm.CrahFlowM3s
  && Failed.CduDutyW==Warm.CduDutyW && Failed.RackAirMassFlowKgs==Warm.RackAirMassFlowKgs && Failed.RackCoolantMassFlowKgs==Warm.RackCoolantMassFlowKgs);
 TestTrue(TEXT("Undo restores original automatic mode"),C->TryUndoLayout() && Count()==0 && MeshCount()==AutoCount);
 Run(AutoCount,false,TEXT("undo")); TestTrue(TEXT("Redo restores same wall-kind unit"),C->TryRedoLayout()); Run(AutoCount+1,true,TEXT("redo"));
 // Synthetic explicit quarter-flow policy check; no new hardware property/default or calibration.
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah) {It->VolumeFlowM3s=DesignFlow*.25; break;}
 C->BeginPerimeterCrahAddition(); C->UpdateRackAdditionAlongRay(FVector(300,480,500),Down); Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Actual world adds only one further perimeter unit"),Added)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 AddedId=VrgCrahLifecycle::Identity(Added);
 const FString SecondAddedId=AddedId;
 Run(AutoCount+2,true,TEXT("actual-add")); TestTrue(TEXT("Undo affects only newest unit"),C->TryUndoLayout() && Count()==AutoCount+1);
 // Only the first added perimeter is present while the second is undone.
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(!It->bConvertedAutomaticCrah) {AddedId=VrgCrahLifecycle::Identity(*It);break;}
 Run(AutoCount+1,true,TEXT("actual-undo")); TestTrue(TEXT("Redo affects only newest unit"),C->TryRedoLayout());
 AddedId=SecondAddedId; Run(AutoCount+2,true,TEXT("actual-redo"));
 TArray<FCrahLayoutRecord> Records;
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) Records.Add({VrgCrahLifecycle::Identity(*It),It->GetFName(),It->GetActorTransform(),It->CaptureLayoutSettings(),
  It->bRuntimeLayoutCrah,It->bConvertedAutomaticCrah,It->AutomaticLayoutTransform,It->AutomaticLayoutSizeM});
 const FString LastId=AddedId; const FName RackName=Rack->GetFName(),OtherRackName=OtherRack->GetFName(),CduName=Cdu->GetFName();
 auto CheckInventory=[&]() {
  TestEqual(TEXT("Whole saved inventory no duplicates"),Count(),Records.Num());
  for(const auto& R:Records) {
   auto* A=VrgCrahLifecycle::Find(W,R.Id);
   TestTrue(TEXT("Saved identity/config/full pose/provenance/original bounds"),A && A->GetFName()==R.Name && A->CaptureLayoutSettings()==R.Settings
    && A->GetActorTransform().Equals(R.Transform,1e-9) && A->bRuntimeLayoutCrah==R.bRuntime && A->bConvertedAutomaticCrah==R.bConvertedAutomatic
    && (!R.bConvertedAutomatic || (A->AutomaticLayoutTransform.Equals(R.AutomaticTransform,1e-9) && A->AutomaticLayoutSizeM==R.AutomaticSizeM)));
  }
 };
 TestTrue(TEXT("Save perimeter additions"),VrgLayoutFile::Save(W,Saved,Summary));
 TestTrue(TEXT("Load base twice restores automatic state"),VrgLayoutFile::Load(W,BaseFile,Summary) && VrgLayoutFile::Load(W,BaseFile,Summary) && Count()==0);
 TestTrue(TEXT("Load additions twice restores units"),VrgLayoutFile::Load(W,Saved,Summary) && VrgLayoutFile::Load(W,Saved,Summary));
 TestTrue(TEXT("Load clears Undo/Redo"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0); CheckInventory(); AddedId=LastId;
 Run(AutoCount+2,true,TEXT("reload"));
 UWorld* Previous=W;
 W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->CrahLayout=Mode==0?ECrahLayout::InRow:ECrahLayout::Perimeter;
 C=W->SpawnActor<ADataCenterDebugController>(); Room=W->SpawnActor<ADataCenterRoomActor>(); Air=W->SpawnActor<AAirFieldDebugActor>();
 FActorSpawnParameters RP; RP.Name=RackName; Rack=W->SpawnActor<ARackThermalActor>(RP); Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 FActorSpawnParameters OP; OP.Name=OtherRackName; OtherRack=W->SpawnActor<ARackThermalActor>(FVector(300,0,0),FRotator(0,180,0),OP);
 OtherRack->ApplyGb200Nvl72Preset(); OtherRack->bShowReadout=false; OtherRack->RackSizeM=Rack->RackSizeM;
 FActorSpawnParameters CP; CP.Name=CduName; Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator,CP);
 Room->RebuildRoomNow(); Sim->EnterLayoutMode();
 C->SelectLayoutAlongRay(FVector(0,0,500),Down); C->OnResetSim(); TestEqual(TEXT("Prior fresh-world history exists"),C->GetLayoutUndoCount(),1);
 TestTrue(TEXT("Fresh Load/repeat no duplicates/dynamics; clears history"),VrgLayoutFile::Load(W,Saved,Summary) && VrgLayoutFile::Load(W,Saved,Summary)
  && Sim->GetSnapshot().SimTimeS==0 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 CheckInventory(); Run(AutoCount+2,true,TEXT("newworld"));
 AddInfo(FString::Printf(TEXT("Perimeter add mode%d maxima: sizing%.3e duct/ports%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e; inlet%.6f..%.6fK coil%.6f..%.6fW; IT change0W, no new physical parameters."),
  Mode,MaxSize,MaxDuct,MaxCoolant,MaxHydraulic,MaxGlobal,MaxDiv,MinInlet,MaxInlet,MinCoil,MaxCoil));
 for(const auto& File:{BaseFile,Saved}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
 W->DestroyWorld(false);GEngine->DestroyWorldContext(W); Previous->DestroyWorld(false);GEngine->DestroyWorldContext(Previous);
 }
 return true;
}
#endif
