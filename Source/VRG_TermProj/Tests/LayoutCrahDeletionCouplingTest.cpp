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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahDeletionCouplingTest,"VRG.Interaction.Layout.CrahDeletionCoupling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahDeletionCouplingTest::RunTest(const FString& Parameters)
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
  AddInfo(FString::Printf(TEXT("CRAH delete %s: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW coil%.6fW IT-first%.6fW; sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e t%.2fs"),
   Stage,ExpectedUnits,DesignFlow,DesignUA,DesignFan,S.CrahCoilRemovedW,First.TotalItHeatW,FMath::Max3(FlowErr,UAErr,FanErr),DuctErr,Coolant,HS.NormalizedResidual,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Run(AutoCount,false,TEXT("automatic"));
 C->BeginInRowCrahAddition(); C->UpdateRackAdditionAlongRay(Destination+FVector(0,0,500),Down); auto* Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Converted fixture"),Added)) {W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return false;}
 AddedId=Added->LayoutId; Run(AutoCount+1,true,TEXT("before"));
 const auto Warm=Sim->GetSnapshot(); Added->SetRole(ROLE_SimulatedProxy);
 TestFalse(TEXT("Warm delete failure"),C->TryDeleteLayoutCrah()); Added->SetRole(ROLE_Authority);
 const auto Failed=Sim->GetSnapshot();
 TestTrue(TEXT("Failed delete keeps live bridge, time and history"),Count()==AutoCount+1 && C->GetLayoutUndoCount()==1
  && Failed.SimTimeS==Warm.SimTimeS && Failed.EnergyInJ==Warm.EnergyInJ && Failed.EnergyOutJ==Warm.EnergyOutJ
  && Failed.EnergyStoredJ==Warm.EnergyStoredJ && Failed.CrahFlowM3s==Warm.CrahFlowM3s && Failed.CrahCoilRemovedW==Warm.CrahCoilRemovedW);
 TestTrue(TEXT("Delete added cooler"),C->TryDeleteLayoutCrah() && Count()==AutoCount);
 Run(AutoCount,true,TEXT("delete"));
 TestTrue(TEXT("Undo deletion"),C->TryUndoLayout()); Run(AutoCount+1,true,TEXT("undo"));
 TestTrue(TEXT("Redo deletion"),C->TryRedoLayout()); Run(AutoCount,true,TEXT("redo"));
 FString Summary; const FString Saved=TEXT("AutomationCrahDeletionRuntime");
 TestTrue(TEXT("Save after runtime deletion"),VrgLayoutFile::Save(W,Saved,Summary));
 TestTrue(TEXT("Undo then Load deletion removes runtime again"),C->TryUndoLayout() && VrgLayoutFile::Load(W,Saved,Summary));
 TestTrue(TEXT("Repeated Load no duplicates/history"),VrgLayoutFile::Load(W,Saved,Summary) && Count()==AutoCount && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 Run(AutoCount,true,TEXT("reload"));
 // Each remaining converted cooler is deleted singly. Final zero actual actors restores the unchanged automatic policy.
 while(Count()>0) {
  ACrahUnitActor* Target=nullptr; for(TActorIterator<ACrahUnitActor> It(W);It;++It) {Target=*It;break;}
  C->SelectLayoutAlongRay(Target->GetActorLocation()+FVector(0,0,500),Down);
  if(!TestTrue(TEXT("Delete one converted unit"),C->TryDeleteLayoutCrah())) break;
 }
 TestEqual(TEXT("No actual units remain"),Count(),0); Run(AutoCount,false,TEXT("last-delete-auto"));
 TestTrue(TEXT("Last delete Undo restores exactly one actual unit"),C->TryUndoLayout() && Count()==1);
 Run(1,true,TEXT("last-undo"));
 TestTrue(TEXT("Last delete Redo restores automatic"),C->TryRedoLayout() && Count()==0); Run(AutoCount,false,TEXT("last-redo-auto"));
 AddInfo(FString::Printf(TEXT("CRAH delete maxima: sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e; inlet%.6f..%.6fK coil%.6f..%.6fW; IT change0W, no new physical parameters."),
  MaxSize,MaxDuct,MaxCoolant,MaxHydraulic,MaxGlobal,MaxDiv,MinInlet,MaxInlet,MinCoil,MaxCoil));
 IFileManager::Get().Delete(*VrgLayoutFile::PathFor(Saved)); W->DestroyWorld(false);GEngine->DestroyWorldContext(W);return true;
}
#endif
