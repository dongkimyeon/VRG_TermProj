#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCrahLifecycle.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahAdditionCouplingTest,"VRG.Interaction.Layout.CrahAdditionCoupling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahAdditionCouplingTest::RunTest(const FString& Parameters)
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
 auto MeshCount=[&]() {TInlineComponentArray<UStaticMeshComponent*> Components(Sim); return Components.Num();};
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
  if(Placed) {
   auto* Added=VrgCrahLifecycle::Find(W,AddedId);
   if(TestNotNull(TEXT("Added cooler stays present"),Added)) {
    const FVector Point=(Added->GetActorLocation()+FVector(0,0,Added->SizeM.Z*50)-Air->GetActorLocation())/100;
    const double H=G.GetCellSizeM();
    TestTrue(TEXT("Actual added box is a solid in air grid"),G.IsSolidCell(FMath::FloorToInt32(Point.X/H),FMath::FloorToInt32(Point.Y/H),FMath::FloorToInt32(Point.Z/H)));
   }
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
  AddInfo(FString::Printf(TEXT("CRAH add %s: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW coil%.6fW IT-first%.6fW; sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e t%.2fs"),
   Stage,ExpectedUnits,DesignFlow,DesignUA,DesignFan,S.CrahCoilRemovedW,First.TotalItHeatW,FMath::Max3(FlowErr,UAErr,FanErr),DuctErr,Coolant,HS.NormalizedResidual,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Run(AutoCount,false,TEXT("automatic"));
 C->BeginInRowCrahAddition(); C->UpdateRackAdditionAlongRay(Destination+FVector(0,0,500),Down); auto* Added=C->ConfirmCrahAddition();
 if(!TestNotNull(TEXT("Addition confirmed"),Added)) {W->DestroyWorld(false); GEngine->DestroyWorldContext(W);return false;}
 AddedId=Added->LayoutId;
 TestTrue(TEXT("No duplicate virtual meshes after conversion"),Count()==AutoCount+1 && MeshCount()==0);
 Run(AutoCount+1,true,TEXT("confirm"));
 const auto Warm=Sim->GetSnapshot(); TArray<ACrahUnitActor*> Converted;
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah) Converted.Add(*It);
 auto* FirstConverted=Converted[0]; auto* SecondConverted=Converted[1];
 const auto FailUndo=W->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* A) {
  if(A==FirstConverted) SecondConverted->SetRole(ROLE_SimulatedProxy);
 }));
 TestFalse(TEXT("Warm middle removal fails the whole Undo"),C->TryUndoLayout()); W->RemoveOnActorDestroyedHandler(FailUndo);
 const auto AfterFailure=Sim->GetSnapshot();
 TestTrue(TEXT("Warm failed Undo preserves coupling/clock/energy/duty/selection/stacks"),Count()==AutoCount+1
  && C->GetLayoutUndoCount()==1 && C->GetLayoutRedoCount()==0 && C->GetSelectedLayoutActor()==VrgCrahLifecycle::Find(W,AddedId)
  && AfterFailure.SimTimeS==Warm.SimTimeS && AfterFailure.EnergyInJ==Warm.EnergyInJ && AfterFailure.EnergyStoredJ==Warm.EnergyStoredJ
  && AfterFailure.EnergyOutJ==Warm.EnergyOutJ && AfterFailure.CrahCoilRemovedW==Warm.CrahCoilRemovedW
  && AfterFailure.CrahFlowM3s==Warm.CrahFlowM3s && AfterFailure.CduDutyW==Warm.CduDutyW);
 TestTrue(TEXT("Undo reinstates automatic display and removes all actual coolers"),C->TryUndoLayout() && Count()==0 && MeshCount()==AutoCount);
 Run(AutoCount,false,TEXT("undo"));
 TestTrue(TEXT("Redo returns same cooler identity with no virtual duplicates"),C->TryRedoLayout() && VrgCrahLifecycle::Find(W,AddedId) && MeshCount()==0);
 Run(AutoCount+1,true,TEXT("redo"));
 // Existing actual authored flow retains its value; zeros share only the design remainder.
 for(TActorIterator<ACrahUnitActor> It(W);It;++It) if(It->bConvertedAutomaticCrah) {It->VolumeFlowM3s=DesignFlow*.25; break;}
 C->BeginInRowCrahAddition(); C->UpdateRackAdditionAlongRay(FVector(180,180,500),Down);
 TestNotNull(TEXT("Existing actual world accepts exactly one further cooler"),C->ConfirmCrahAddition());
 Run(AutoCount+2,true,TEXT("placed-set-flow"));
 TestTrue(TEXT("Placed-world Undo removes only newest unit"),C->TryUndoLayout() && Count()==AutoCount+1);
 Run(AutoCount+1,true,TEXT("placed-undo"));
 TestTrue(TEXT("Placed-world Redo restores only newest unit"),C->TryRedoLayout() && Count()==AutoCount+2);
 Run(AutoCount+2,true,TEXT("placed-redo"));
 AddInfo(FString::Printf(TEXT("CRAH add maxima: sizing%.3e duct%.3e coolant%.3e hydraulic%.3e global%.3e div%.3e; inlet%.6f..%.6fK coil%.6f..%.6fW. No new physical parameters/thresholds; geometry/quarter-flow are synthetic integration inputs."),
  MaxSize,MaxDuct,MaxCoolant,MaxHydraulic,MaxGlobal,MaxDiv,MinInlet,MaxInlet,MinCoil,MaxCoil));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W);return true;
}
// Automatic perimeter units must retain their different intake kind/dimensions as well as in-row units.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutPerimeterCrahConversionTest,"VRG.Interaction.Layout.CrahPerimeterConversion",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutPerimeterCrahConversionTest::RunTest(const FString& Parameters)
{
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->CrahLayout=ECrahLayout::Perimeter;
 auto* Room=W->SpawnActor<ADataCenterRoomActor>(); W->SpawnActor<AAirFieldDebugActor>();
 auto* A=W->SpawnActor<ARackThermalActor>(FVector(-150,0,0),FRotator::ZeroRotator);
 auto* B=W->SpawnActor<ARackThermalActor>(FVector(150,0,0),FRotator(0,180,0));
 for(auto* Rack:{A,B}) {Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
  Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);}
 W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400,100,0),FRotator::ZeroRotator);
 const auto InitialA=A->Input,InitialB=B->Input; Room->RebuildRoomNow();
 auto Run=[&]() {A->Input=InitialA; B->Input=InitialB; Sim->EnterSimulationMode(); Sim->Tick(.021f); for(int I=0;I<10;++I) Sim->Tick(.04f); return Sim->GetSnapshot();};
 const auto Before=Run(); const double Flow=Sim->GetCrahTotalFlowM3s(),UA=Sim->GetCrahTotalUAWK(),Fan=Sim->GetCrahTotalFanW();
 Sim->EnterLayoutMode(); TArray<FCrahLayoutRecord> Records;
 TestTrue(TEXT("Perimeter automatic plan captured"),Sim->CaptureAutomaticCrahLayout(Records) && Records.Num()==2 && Sim->GetActiveCrahUnitCount()==2);
 for(auto R:Records) {
  TestTrue(TEXT("Top intake and perimeter size retained rather than in-row defaults"),R.Settings.Kind==ECrahUnitKind::Perimeter
   && R.Settings.SizeM==Sim->CrahPerimeterSizeM && R.Settings.VolumeFlowM3s==0 && R.bConvertedAutomatic);
  R.Name=FName(*(TEXT("ConvertedCrah_")+R.Id)); TestNotNull(TEXT("Perimeter actual actor"),VrgCrahLifecycle::Spawn(W,R));
 }
 VrgRackLifecycle::Rebuild(W); const auto After=Run();
 const double Ledger=FMath::Abs(After.EnergyStoredJ-Before.EnergyStoredJ)/FMath::Abs(Before.EnergyStoredJ);
 const double FlowErr=FMath::Abs(Sim->GetCrahTotalFlowM3s()-Flow)/Flow,UAErr=FMath::Abs(Sim->GetCrahTotalUAWK()-UA)/UA,FanErr=FMath::Abs(Sim->GetCrahTotalFanW()-Fan)/Fan;
 TestTrue(TEXT("Perimeter conversion preserves operation and thermal result"),Sim->IsUsingPlacedCrahs() && Sim->GetActiveCrahUnitCount()==2
  && Ledger<1e-9 && FlowErr<1e-9 && UAErr<1e-9 && FanErr<1e-9 && Before.bAirPressureConverged && After.bAirPressureConverged
  && FMath::Abs(Before.NormalizedEnergyResidual)<1e-9 && FMath::Abs(After.NormalizedEnergyResidual)<1e-9);
 AddInfo(FString::Printf(TEXT("Perimeter conversion t%.2fs: flow%.6fm3/s UA%.6fW/K fan%.6fW relative flow%.3e UA%.3e fan%.3e stored%.3e global before%.3e after%.3e"),
  After.SimTimeS,Flow,UA,Fan,FlowErr,UAErr,FanErr,Ledger,Before.NormalizedEnergyResidual,After.NormalizedEnergyResidual));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W);return true;
}
#endif
