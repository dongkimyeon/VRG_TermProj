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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahDuplicationCouplingTest,"VRG.Interaction.Layout.CrahDuplicationPersistenceCoupling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahDuplicationCouplingTest::RunTest(const FString& Parameters)
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
 auto MeshCount=[&]() {return Sim->GetNumAutomaticCrahMeshes();};  // not every mesh on Sim: the room shell adds its own
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
  if(auto* Copy=VrgCrahLifecycle::Find(W,AddedId)) {
   const FVector Point=(Copy->GetActorTransform().TransformPosition(FVector(0,0,Copy->SizeM.Z*50))-Air->GetActorLocation())/100;
   const double H=G.GetCellSizeM();
   TestTrue(TEXT("Duplicate participates as a solid in rebuilt air grid"),G.IsSolidCell(FMath::FloorToInt32(Point.X/H),FMath::FloorToInt32(Point.Y/H),FMath::FloorToInt32(Point.Z/H)));
  }
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
  AddInfo(FString::Printf(TEXT("CRAH duplicate %s: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW coil%.6fW IT-first%.6fW; sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e t%.2fs"),
   Stage,ExpectedUnits,DesignFlow,DesignUA,DesignFan,S.CrahCoilRemovedW,First.TotalItHeatW,FMath::Max3(FlowErr,UAErr,FanErr),DuctErr,Coolant,HS.NormalizedResidual,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Run(AutoCount,false,TEXT("automatic"));
 C->BeginInRowCrahAddition(); C->UpdateRackAdditionAlongRay(Destination+FVector(0,0,500),Down); auto* Source=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Converted layout with runtime source"),Source)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 // Synthetic authored perimeter source and explicit share; no new physical defaults, measurements or fitting.
 Source->Kind=ECrahUnitKind::Perimeter; Source->VolumeFlowM3s=DesignFlow*.1; Source->RefreshVisuals();
 const auto SourceSettings=Source->CaptureLayoutSettings(); const auto SourceTransform=Source->GetActorTransform();
 const FString SourceId=VrgCrahLifecycle::Identity(Source); const FName RackName=Rack->GetFName(),CduName=Cdu->GetFName();
 Run(AutoCount+1,true,TEXT("source")); C->ClearLayoutHistory();
 C->SelectLayoutAlongRay(Source->GetActorLocation()+FVector(0,0,500),Down);
 TestTrue(TEXT("Runtime source duplicate preview"),C->BeginCrahDuplication());
 C->UpdateRackAdditionAlongRay(FVector(180,150,500),Down);
 const auto Warm=Sim->GetSnapshot();
 const auto Handler=W->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor) {
  if(Cast<ACrahUnitActor>(Actor) && Actor->GetName().StartsWith(TEXT("DuplicatedCrah_"))) Actor->Destroy(false,false);
 }));
 TestNull(TEXT("Warm duplication actual spawn failure"),C->ConfirmCrahAddition()); W->RemoveOnActorSpawnedHandler(Handler);
 const auto Failed=Sim->GetSnapshot();
 TestTrue(TEXT("Failed duplication preserves live time/energy/flow/coil/history exactly"),Count()==AutoCount+1 && C->GetLayoutUndoCount()==0
  && Failed.SimTimeS==Warm.SimTimeS && Failed.EnergyInJ==Warm.EnergyInJ && Failed.EnergyOutJ==Warm.EnergyOutJ
  && Failed.EnergyStoredJ==Warm.EnergyStoredJ && Failed.CrahFlowM3s==Warm.CrahFlowM3s && Failed.CrahCoilRemovedW==Warm.CrahCoilRemovedW);
 auto* Clone=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Confirm one clone"),Clone)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 const FString CloneId=VrgCrahLifecycle::Identity(Clone); const FName CloneName=Clone->GetFName(); const auto CloneTransform=Clone->GetActorTransform();
 AddedId=CloneId;
 auto CheckCopy=[&]() {
  auto* S=VrgCrahLifecycle::Find(W,SourceId); auto* D=VrgCrahLifecycle::Find(W,CloneId);
  TestTrue(TEXT("Independent runtime clone/config/full pose; no inherited automatic boundaries"),S && D && S!=D
   && S->CaptureLayoutSettings()==SourceSettings && S->GetActorTransform().Equals(SourceTransform,1e-9)
   && D->GetFName()==CloneName && D->CaptureLayoutSettings()==SourceSettings && D->GetActorTransform().Equals(CloneTransform,1e-9)
   && D->bRuntimeLayoutCrah && !D->bConvertedAutomaticCrah && !D->PreservesAutomaticAirBounds());
 };
 CheckCopy(); Run(AutoCount+2,true,TEXT("duplicate"));
 TestTrue(TEXT("Undo removes clone only"),C->TryUndoLayout() && Count()==AutoCount+1); Run(AutoCount+1,true,TEXT("undo"));
 TestTrue(TEXT("Redo restores same clone"),C->TryRedoLayout() && Count()==AutoCount+2); CheckCopy(); Run(AutoCount+2,true,TEXT("redo"));
 FString Summary; const FString Saved=TEXT("AutomationCrahDuplication");
 TestTrue(TEXT("Save duplicate using existing v2 inventory"),VrgLayoutFile::Save(W,Saved,Summary));
 TestTrue(TEXT("Undo then Load restores duplicate"),C->TryUndoLayout() && VrgLayoutFile::Load(W,Saved,Summary));
 TestTrue(TEXT("Repeated Load no duplicates; histories empty"),VrgLayoutFile::Load(W,Saved,Summary) && Count()==AutoCount+2
  && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 CheckCopy(); Run(AutoCount+2,true,TEXT("reload"));
 // A newly created execution world has only the same placed rack/CDU. Inventory Load recreates all runtime CRAHs.
 UWorld* Previous=W;
 W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 C=W->SpawnActor<ADataCenterDebugController>(); Room=W->SpawnActor<ADataCenterRoomActor>(); Air=W->SpawnActor<AAirFieldDebugActor>();
 FActorSpawnParameters RP; RP.Name=RackName; Rack=W->SpawnActor<ARackThermalActor>(RP);
 Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false; Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 FActorSpawnParameters CP; CP.Name=CduName; Cdu=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator,CP);
 Room->RebuildRoomNow(); Sim->EnterLayoutMode();
 TestTrue(TEXT("New world Load restores duplicate with no previous dynamics"),VrgLayoutFile::Load(W,Saved,Summary) && Count()==AutoCount+2
  && Sim->GetSnapshot().SimTimeS==0 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 TestTrue(TEXT("New world repeated Load no duplicates"),VrgLayoutFile::Load(W,Saved,Summary) && Count()==AutoCount+2);
 CheckCopy(); Run(AutoCount+2,true,TEXT("newworld"));
 AddInfo(FString::Printf(TEXT("CRAH duplicate maxima: sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e; inlet%.6f..%.6fK coil%.6f..%.6fW; IT change0W, no new physical parameters."),
  MaxSize,MaxDuct,MaxCoolant,MaxHydraulic,MaxGlobal,MaxDiv,MinInlet,MaxInlet,MinCoil,MaxCoil));
 IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Saved));
 W->DestroyWorld(false);GEngine->DestroyWorldContext(W); Previous->DestroyWorld(false);GEngine->DestroyWorldContext(Previous);return true;
}
#endif
