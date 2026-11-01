#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterExposureRoomLifecycle,"VRG.Water.Impact.RoomLifecycle",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWaterExposureRoomLifecycle::RunTest(const FString& Parameters)
{
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->ApplyGb200Nvl72Preset();Rack->LayoutId=TEXT("water-A");Rack->bShowReadout=false;Rack->SetActorScale3D(FVector(1.1,.9,1.2));
 auto* Other=World->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->ApplyGb200Nvl72Preset();Other->LayoutId=TEXT("water-B");Other->bShowReadout=false;
 World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100,150,0),FRotator::ZeroRotator);
 World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
 auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->MaxStepsPerFrame=16;Room->RebuildRoomNow();
 // SYNTHETIC observation policy only: these short durations exercise transitions in a small integration fixture.
 // They are not published/calibrated GB200 failure criteria. Power is explicit, not inferred from docking or heat.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Interior)|uint8(EWaterEvidence::Rear);
 Policy.InteriorPresentM3=Policy.RearPresentM3=0;Policy.TripPoweredContactS=.1;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Integration fixture only, not equipment failure limits");
 TArray<double> RefHeld,RefMoving,RefRear,RefPassed,RefDepth,RefTemp,RefPressure,RefFlow;
 FWaterExposureSnapshot TickExposure;FWaterImpactState TickImpact;TArray<FWaterImpactEvent> TickEvents;
 double RefReceived=0,RefFloor=0,RefEnergy=0,MaxHeat=0,MaxFloor=0,MaxInternal=0,MaxObserverDifference=0;
 for(int32 Path=0;Path<3;++Path) {
  Sim->bWaterExposureObservation=Path!=0;Sim->EnterSimulationMode();
  TestFalse(TEXT("New execution clears old impact policy"),Sim->HasWaterImpactPolicy());
  TestEqual(TEXT("Restart clears observation count"),Sim->GetWaterObservationCount(),uint64(0));
  if(Path!=0) {
   TestTrue(TEXT("Actual baseline current"),Sim->IsWaterExposureCurrent());FString Error;
   TestFalse(TEXT("No operational default supplied"),Sim->ConfigureWaterImpactObservation(FWaterImpactPolicy(),{},Error));
   TArray<bool> Power;Power.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
   TestTrue(TEXT("Explicit policy/power registration"),Sim->ConfigureWaterImpactObservation(Policy,Power,Error));
   for(auto Phase:Sim->GetWaterImpactObservation().GetState().Phase) TestTrue(TEXT("No arrival: no transition"),Phase==EWaterImpactPhase::Normal);
   TestFalse(TEXT("Malformed power input rejected"),Sim->SetWaterImpactPowerObservation({},Error));
  }
  const auto H=*Room->GetInRackHandles(Rack);
  TestTrue(TEXT("First-step leaks open in existing topology"),Room->SetInternalLeakHoleDiameterM(Rack,20,0,.003) && Room->SetInternalLeakHoleDiameterM(Rack,0,2,InRackNetwork::HoseDiameterM));
  auto Run=[&](int32 Steps) {
   const double End=Sim->GetSnapshot().SimTimeS+Steps*Sim->FixedStepS;
   if(Path<2) for(int32 Frame=0;Frame<Steps/5;++Frame) Sim->Tick(.1f);
   else {Sim->QueueWarmupSteps(Steps);for(int32 Frame=0;Frame<100 && Sim->GetSnapshot().SimTimeS<End-1e-9;++Frame) Sim->Tick(0);}
   TestTrue(TEXT("Same integrated interval"),FMath::Abs(Sim->GetSnapshot().SimTimeS-End)<1e-9);
  };
  Run(100);
  Room->SetInternalLeakHoleDiameterM(Rack,20,0,0);Room->SetInternalLeakHoleDiameterM(Rack,0,2,0);Run(50);
  const auto& W=Sim->GetRackInteriorCoupling().GetWater();const auto& Floor=Sim->GetFloorWater();const auto& Snapshot=Sim->GetSnapshot();
  TArray<double> Pressure,Flow;const auto& Net=Room->GetNetwork();
  for(int32 N=0;N<Net.GetNumNodes();++N) Pressure.Add(Net.GetNodePressurePa(N));
  for(int32 P=0;P<Net.GetNumPipes();++P) Flow.Add(Net.GetPipeFlowM3s(P));
  MaxHeat=FMath::Max(MaxHeat,FMath::Abs(Snapshot.NormalizedEnergyResidual));MaxFloor=FMath::Max(MaxFloor,Floor.GetTotals().Residual);
  MaxInternal=FMath::Max(MaxInternal,FMath::Abs(W.BalanceErrorM3()));
  TestTrue(TEXT("Existing pressure/hydraulic convergence"),Snapshot.bAirPressureConverged && Snapshot.bHydraulicsConverged);
  for(double Temp:Snapshot.RackAirInletTempK) TestTrue(TEXT("Existing inlet plausibility"),Temp>=288.15 && Temp<=308.15);
  TestEqual(TEXT("Internal leak never re-enters W2"),Sim->GetLeakSpray().GetTotals().EmittedM3,0.0);
  if(Path==0) {RefHeld=W.HeldM3;RefMoving=W.TransitM3;RefRear=W.GetRearTransitM3();RefPassed=W.PassedM3;RefDepth=Floor.GetDepthM();RefTemp=Rack->GetModel().GetState().ComponentTempK;RefPressure=Pressure;RefFlow=Flow;RefReceived=W.ReceivedM3;RefFloor=Floor.GetTotals().ReceivedM3;RefEnergy=Snapshot.NormalizedEnergyResidual;}
  else {
   TestTrue(TEXT("Observation does not change water arrays"),RefHeld==W.HeldM3 && RefMoving==W.TransitM3 && RefRear==W.GetRearTransitM3() && RefPassed==W.PassedM3 && RefDepth==Floor.GetDepthM());
   TestTrue(TEXT("Observation does not change thermal/hydraulic states"),RefTemp==Rack->GetModel().GetState().ComponentTempK && RefPressure==Pressure && RefFlow==Flow);
   MaxObserverDifference=FMath::Max(MaxObserverDifference,FMath::Abs(RefReceived-W.ReceivedM3));
   TestEqual(TEXT("Same floor received volume"),RefFloor,Floor.GetTotals().ReceivedM3);TestEqual(TEXT("Same energy ledger"),RefEnergy,Snapshot.NormalizedEnergyResidual);
   const auto& Exposure=Sim->GetWaterExposure();const auto& Impact=Sim->GetWaterImpactObservation().GetState();
   TestTrue(TEXT("Water endpoint equals shared SimTime"),Sim->IsWaterExposureCurrent() && Exposure.SimTimeS==Snapshot.SimTimeS && Sim->GetWaterImpactObservation().GetSimTimeS()==Snapshot.SimTimeS);
   TestEqual(TEXT("Exactly one observation per integrated step"),Sim->GetWaterObservationCount(),uint64(150));
   int32 WetSlots=0,DamagedSlots=0;
   for(int32 I=0;I<Exposure.EquipmentId.Num();++I) {
    if(Exposure.EquipmentId[I]==TEXT("rack/water-B")) {TestEqual(TEXT("Other rack has no interior arrivals"),Exposure.InteriorPassedM3[I],0.0);TestTrue(TEXT("No other-rack false trip"),Impact.Phase[I]==EWaterImpactPhase::Normal);}
    if(Exposure.EquipmentId[I]==TEXT("rack/water-A") && Exposure.Slot[I]!=INDEX_NONE) {
     if(Exposure.InteriorPassedM3[I]>0) ++WetSlots;if(Impact.Phase[I]==EWaterImpactPhase::Damaged) ++DamagedSlots;
     TestEqual(TEXT("Unlocalized box spray never assigned to slot"),Exposure.SprayTouchedM3[I],0.0);
    }
   }
   TestTrue(TEXT("Calculated native arrival drives synthetic state"),WetSlots>0 && DamagedSlots>0);
   TestTrue(TEXT("Floor arrival exercised"),Floor.GetTotals().ReceivedM3>0);
   AddInfo(FString::Printf(TEXT("W4 room path%d t=%.17gs observed=%llu wet native slots=%d synthetic damaged slots=%d water=%.17g m3 floor=%.17g m3 energy=%.17g floor residual=%.17g internal residual=%.17g m3"),Path,Exposure.SimTimeS,Sim->GetWaterObservationCount(),WetSlots,DamagedSlots,W.ReceivedM3,Floor.GetTotals().ReceivedM3,Snapshot.NormalizedEnergyResidual,Floor.GetTotals().Residual,W.BalanceErrorM3()));
   if(Path==1) {TickExposure=Exposure;TickImpact=Impact;TickEvents=Sim->GetWaterImpactObservation().GetEvents();}
   else {
    TestTrue(TEXT("Tick/RunSteps same exposure"),TickExposure.InteriorPassedM3==Exposure.InteriorPassedM3 && TickExposure.InteriorHeldM3==Exposure.InteriorHeldM3 && TickExposure.RearPassedM3==Exposure.RearPassedM3 && TickExposure.FloodImmersionM==Exposure.FloodImmersionM);
    TestTrue(TEXT("Tick/RunSteps same phase/timing"),TickImpact.Phase==Impact.Phase && TickImpact.FirstWetS==Impact.FirstWetS && TickImpact.TripS==Impact.TripS && TickImpact.DamageS==Impact.DamageS);
    const auto& Events=Sim->GetWaterImpactObservation().GetEvents();TestEqual(TEXT("Same event count"),TickEvents.Num(),Events.Num());
    for(int32 I=0;I<FMath::Min(TickEvents.Num(),Events.Num());++I) TestTrue(TEXT("Same event target/phase/time"),TickEvents[I].Target==Events[I].Target && TickEvents[I].To==Events[I].To && TickEvents[I].SimTimeS==Events[I].SimTimeS);
    const auto OldState=Impact;const auto OldTime=Exposure.SimTimeS;const auto OldPassed=Exposure.InteriorPassedM3;const auto OldCount=Sim->GetWaterObservationCount();
    Rack->LayoutId=TEXT("changed-under-observer");Run(5);
    TestFalse(TEXT("Stale mapping diagnosed"),Sim->IsWaterExposureCurrent());TestFalse(TEXT("Diagnostic present"),Sim->GetWaterObservationDiagnostic().IsEmpty());
    TestEqual(TEXT("Rejected sample does not advance snapshot"),Sim->GetWaterExposure().SimTimeS,OldTime);TestEqual(TEXT("Rejected sample does not advance count"),Sim->GetWaterObservationCount(),OldCount);
    TestTrue(TEXT("No partial state/exposure publish"),OldPassed==Sim->GetWaterExposure().InteriorPassedM3 && OldState.Phase==Sim->GetWaterImpactObservation().GetState().Phase && OldState.DamageS==Sim->GetWaterImpactObservation().GetState().DamageS);
    AddInfo(FString::Printf(TEXT("W4 rejected mapping: physical clock=%.17gs, retained exposure=%.17gs, retained observations=%llu"),Sim->GetSnapshot().SimTimeS,Sim->GetWaterExposure().SimTimeS,Sim->GetWaterObservationCount()));
    Rack->LayoutId=TEXT("water-A");
   }

  }
  Sim->EnterLayoutMode();
 }
 TestEqual(TEXT("Observer water difference"),MaxObserverDifference,0.0);
 TestTrue(TEXT("Existing conservation criteria"),MaxHeat<1e-9 && MaxFloor<1e-12 && MaxInternal<1e-15);
 AddInfo(FString::Printf(TEXT("W4 observer A/B max water difference=%.17g m3 / reference %.17g m3; thermal arrays difference=0K, pressure=0Pa, flow=0m3/s; max energy residual=%.17g, floor normalized residual=%.17g, interior residual=%.17g m3"),MaxObserverDifference,RefReceived,MaxHeat,MaxFloor,MaxInternal));
 Sim->bWaterExposureObservation=true;Sim->EnterSimulationMode();
 TestTrue(TEXT("Restart reconstructs mapping"),Sim->IsWaterExposureCurrent());TestFalse(TEXT("Restart clears configured policy/state"),Sim->HasWaterImpactPolicy() || Sim->GetWaterImpactObservation().IsInitialized());
 TestEqual(TEXT("Restart cumulative baseline dry"),Sim->GetRackInteriorCoupling().GetWater().ReceivedM3,0.0);TestEqual(TEXT("Restart observation count zero"),Sim->GetWaterObservationCount(),uint64(0));
 // W2 actual room branch: aim at the existing rack box, then read its computed contact history.
 FString Error;auto SprayPolicy=Policy;SprayPolicy.ContactSources=uint8(EWaterEvidence::Spray);SprayPolicy.SprayArrivalM3=0;
 TArray<bool> Power;Power.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
 TestTrue(TEXT("Explicit spray-only fixture policy"),Sim->ConfigureWaterImpactObservation(SprayPolicy,Power,Error));
 int32 Site=INDEX_NONE;const auto& Sites=Sim->GetLiveLeakSites();
 for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {Site=I;break;}
 int32 RackBox=INDEX_NONE;for(const auto& B:Sim->GetWaterExposureBindings()) if(B.EquipmentId==TEXT("rack/water-A") && B.Slot==INDEX_NONE) RackBox=B.SprayObstacle;
 TestTrue(TEXT("Actual W1 branch and W2 box available"),Site!=INDEX_NONE && RackBox!=INDEX_NONE);
 if(Site!=INDEX_NONE && RackBox!=INDEX_NONE) {
  Sim->SetLiveLeakDirection(Site,Sim->GetLeakSpray().GetGeometry().Obstacles[RackBox].CenterM-Sites[Site].PositionM);
  Sim->SetLiveLeak(Site,.005,TEXT("W4 observer fixture"));for(int32 I=0;I<10;++I) Sim->Tick(.1f);
  const auto& E=Sim->GetWaterExposure();bool RootContact=false;int32 Diagnostics=0;
  for(int32 I=0;I<E.EquipmentId.Num();++I) {
   const auto& B=Sim->GetWaterExposureBindings()[I];
   if(E.Slot[I]==INDEX_NONE) {TestEqual(TEXT("Live W2 contact copied once"),E.SprayTouchedM3[I],Sim->GetLeakSpray().GetWetting()[B.SprayObstacle].WaterM3);RootContact|=E.SprayTouchedM3[I]>0;}
   else {TestEqual(TEXT("Live box contact never distributed among native slots"),E.SprayTouchedM3[I],0.0);TestTrue(TEXT("Spray-only policy cannot trip unlocalized slots"),Sim->GetWaterImpactObservation().GetState().Phase[I]==EWaterImpactPhase::Normal);}
   if(B.FloorFlood!=INDEX_NONE) TestEqual(TEXT("Live W3 immersion from the exact footprint"),E.FloodImmersionM[I],FMath::Max(0.0,Sim->GetFloorWater().GetFloods()[B.FloorFlood].MeanDepthM-B.FloorContactDepthM));
  }
  for(const auto& D:E.Diagnostics) if(D.Reason.Contains(TEXT("cannot localize"))) ++Diagnostics;
  TestTrue(TEXT("Live spray reached equipment and unlocalized diagnostic retained"),RootContact && Diagnostics>0);
  TestTrue(TEXT("Existing W2/W3 conservation"),Sim->GetLeakSpray().GetTotals().Residual<1e-12 && Sim->GetFloorWater().GetTotals().Residual<1e-12);
  AddInfo(FString::Printf(TEXT("W4 actual W2 emitted=%.17g m3 residual=%.17g; actual W3 received=%.17g m3 normalized residual=%.17g; unlocalized diagnostics=%d"),Sim->GetLeakSpray().GetTotals().EmittedM3,Sim->GetLeakSpray().GetTotals().Residual,Sim->GetFloorWater().GetTotals().ReceivedM3,Sim->GetFloorWater().GetTotals().Residual,Diagnostics));
 }
 Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return true;
}
#endif
