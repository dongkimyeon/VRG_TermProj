#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduWaterPowerResponse,"VRG.Water.Impact.CduPowerResponse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCduWaterPowerResponse::RunTest(const FString& Parameters)
{
 // SYNTHETIC TEST ONLY. Reuse the rack-response timing fixture, not operating CDU failure limits.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Spray);Policy.SprayArrivalM3=0;
 Policy.TripPoweredContactS=.115;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Controlled CDU shutdown fixture; not equipment failure data");
 TArray<double> ReferenceTemp,TickTemp,TickPressure,TickFlow,TickDepth;TArray<EWaterImpactPhase> TickPhase;
 double RefCoolant=0,RefChip=0,RefIt=0,RefFlow=0,TickWater=0,TickHeat=0,MaxHeat=0,MaxSite=0,MaxSpray=0,MaxFloor=0,MaxInventory=0,MaxHydraulic=0;
 TArray<ADataCenterSimulationActor::FWaterCduPowerCut> TickCuts;
 for(int32 Path=0;Path<4;++Path) {
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->ApplyGb200Nvl72Preset();Rack->LayoutId=TEXT("cdu-rack-A");Rack->bShowReadout=false;
 auto* Other=World->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->ApplyGb200Nvl72Preset();Other->LayoutId=TEXT("cdu-rack-B");Other->bShowReadout=false;
 // Fixture cabinets are inside the existing rack-fitted fluid domain; no room/grid policy changes.
 auto* Cdu=World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);Cdu->LayoutId=TEXT("pump-A");
 auto* Healthy=World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);Healthy->LayoutId=TEXT("pump-B");
 World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
 auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=true;Sim->MaxStepsPerFrame=16;Rack->Rename(TEXT("CduRackA"));Other->Rename(TEXT("CduRackB"));Cdu->Rename(TEXT("CduA"));Healthy->Rename(TEXT("CduB"));Room->RebuildRoomNow();

  Sim->EnterSimulationMode();TestTrue(TEXT("New execution restores both pumps"),Cdu->IsPumpPowered() && Healthy->IsPumpPowered());
  TestFalse(TEXT("No implicit CDU response or operational policy"),Sim->IsCduWaterTripResponseEnabled() || Sim->HasWaterImpactPolicy());
  const double InitialCoolant=Rack->GetModel().GetState().CoolantTempK;
  double InitialChip=0;for(double K:Rack->GetModel().GetState().ComponentTempK) InitialChip=FMath::Max(InitialChip,K);
  TestTrue(TEXT("Facility pump heat ledger active"),Sim->GetFacilityPlant().IsThermalActive());
  int32 RowA=INDEX_NONE,RowB=INDEX_NONE;for(int32 R=0;R<Room->GetNumRows();++R) {if(Room->GetRowCdu(R)==Cdu) RowA=R;if(Room->GetRowCdu(R)==Healthy) RowB=R;}
  if(!TestTrue(TEXT("Distinct native CDU loops available"),RowA!=INDEX_NONE && RowB!=INDEX_NONE && RowA!=RowB)) {GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return false;}
  const int32 Pump=Room->GetRowPumpIndex(RowA),OtherPump=Room->GetRowPumpIndex(RowB);const auto& Net=Room->GetNetwork();
  const int32 Nodes=Net.GetNumNodes(),Pipes=Net.GetNumPipes(),Leaks=Net.GetNumLeaks();const double Speed=Cdu->PumpSpeedRatio,UA=Cdu->HxUAWK;
  TArray<double> Resistance;for(int32 P=0;P<Pipes;++P) Resistance.Add(Net.GetPipeResistancePaS2M6(P));
  FString Error;TArray<bool> Power;Power.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
  TestFalse(TEXT("Unspecified policy never enables response"),Sim->ConfigureCduWaterTripResponse(FWaterImpactPolicy(),Power,Error));
  if(Path>0) TestTrue(TEXT("Explicit policy/actuator opt-in"),Path==1?Sim->ConfigureWaterImpactObservation(Policy,Power,Error):Sim->ConfigureCduWaterTripResponse(Policy,Power,Error,true));
  int32 Site=INDEX_NONE,Box=INDEX_NONE,Binding=INDEX_NONE;
  const auto& Sites=Sim->GetLiveLeakSites();
  for(int32 I=0;I<Sim->GetWaterExposureBindings().Num();++I) {const auto& B=Sim->GetWaterExposureBindings()[I];if(B.EquipmentId==TEXT("cdu/pump-A")) {Box=B.SprayObstacle;Binding=I;}}
  // W2 intentionally deposits an inside-equipment source in that equipment; aiming cannot
  // turn it into an external jet. Select an actual W1 site outside ALL boxes, nearest the CDU.
  double Nearest=TNumericLimits<double>::Max();
  const auto& Geometry=Sim->GetLeakSpray().GetGeometry();
  if(Box!=INDEX_NONE) for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {
   bool Inside=false;for(const auto& O:Geometry.Obstacles) {const FVector L=O.Rotation.UnrotateVector(Sites[I].PositionM-O.CenterM);Inside|=FMath::Abs(L.X)<=O.HalfM.X && FMath::Abs(L.Y)<=O.HalfM.Y && FMath::Abs(L.Z)<=O.HalfM.Z;}
   const double Distance=FVector::DistSquared(Sites[I].PositionM,Geometry.Obstacles[Box].CenterM);
   if(!Inside && Distance<Nearest) {Nearest=Distance;Site=I;}
  }
  if(!TestTrue(TEXT("Real external leak and CDU contact box available"),Site!=INDEX_NONE && Box!=INDEX_NONE)) {GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return false;}
  TestTrue(TEXT("CDU is inside actual water domain"),Geometry.RoomM.IsInsideOrOn(Geometry.Obstacles[Box].CenterM));
  TestTrue(TEXT("Real jet aimed at target CDU"),Sim->SetLiveLeakDirection(Site,Sim->GetLeakSpray().GetGeometry().Obstacles[Box].CenterM-Sites[Site].PositionM));
  TestTrue(TEXT("Actual external hole opens"),Sim->SetLiveLeak(Site,.005,TEXT("Synthetic CDU response test")));
  bool bIntervalRejected=false;
  auto Run=[&](int32 Steps) {
   if(bIntervalRejected) return;
   const double End=Sim->GetSnapshot().SimTimeS+Steps*Sim->FixedStepS;
   for(int32 Frame=0;Frame<Steps+3 && Sim->GetSnapshot().SimTimeS<End-1e-9;++Frame) {
    const double Before=Sim->GetSnapshot().SimTimeS;
    if(Path<3) Sim->Tick(float(Sim->FixedStepS));else {Sim->QueueWarmupSteps(1);Sim->Tick(0);}
    if(Sim->GetSnapshot().SimTimeS>Before) {
     const auto S=Sim->GetSnapshot();
     if(!TestTrue(TEXT("Every common step converges, including live leak after pump stop"),S.bHydraulicsConverged && S.bAirPressureConverged)) {
      const auto& Result=Room->GetLastSolveResult();
      AddInfo(FString::Printf(TEXT("CDU failed step t=%.17g residual=%.17g warm=%d iterations=%d flowRef=%.17g pressureRef=%.17g pumpFlow=%.17g pumpHead=%.17g Pa"),S.SimTimeS,Result.NormalizedResidual,Result.bUsedWarmStart,Result.IterationsUsed,Result.ReferenceFlowM3s,Result.ReferencePressurePa,Net.GetPumpFlowM3s(Pump),Net.GetPumpDeltaPPa(Pump)));
      int32 Worst=INDEX_NONE;double Imbalance=0;
      for(int32 N=0;N<Nodes;++N) {bool Fixed=false;for(const auto& T:Room->GetCoolantInventory().GetTanks()) Fixed|=N==T.CduNodeIndex;if(!Fixed && FMath::Abs(Net.GetNodeImbalanceM3s(N))>Imbalance) {Worst=N;Imbalance=FMath::Abs(Net.GetNodeImbalanceM3s(N));}}
      AddInfo(FString::Printf(TEXT("CDU worst free node=%d mass error=%.17g m3/s pressure=%.17g Pa"),Worst,Imbalance,Net.GetNodePressurePa(Worst)));
      for(int32 P=0;P<Pipes;++P) if(Net.GetPipeFromNodeIndex(P)==Worst || Net.GetPipeToNodeIndex(P)==Worst) AddInfo(FString::Printf(TEXT("CDU worst-node pipe=%d from=%d to=%d drop=%.17g Pa flow=%.17g m3/s resistance=%.17g"),P,Net.GetPipeFromNodeIndex(P),Net.GetPipeToNodeIndex(P),Net.GetNodePiezometricPa(Net.GetPipeFromNodeIndex(P))-Net.GetNodePiezometricPa(Net.GetPipeToNodeIndex(P)),Net.GetPipeFlowM3s(P),Net.GetPipeResistancePaS2M6(P)));
      bIntervalRejected=true;return;
     }
     MaxHydraulic=FMath::Max(MaxHydraulic,Room->GetLastSolveResult().NormalizedResidual);
     MaxHeat=FMath::Max(MaxHeat,FMath::Abs(S.NormalizedEnergyResidual));
     MaxSite=FMath::Max(MaxSite,FMath::Abs(Sim->GetSiteEnergyResidual()));
     MaxSpray=FMath::Max(MaxSpray,Sim->GetLeakSpray().GetTotals().Residual);MaxFloor=FMath::Max(MaxFloor,Sim->GetFloorWater().GetTotals().Residual);
     for(const auto& T:Room->GetCoolantInventory().GetTanks()) MaxInventory=FMath::Max(MaxInventory,FMath::Abs(T.State.InitialWaterM3+T.State.MakeupM3-T.State.LeakedM3-T.State.WaterVolumeM3));
    }
   }
   TestTrue(TEXT("Entire common interval integrated"),FMath::Abs(Sim->GetSnapshot().SimTimeS-End)<1e-9);
  };
  AddInfo(FString::Printf(TEXT("CDU real source %.6g %.6g %.6g m target %.6g %.6g %.6g m"),Sites[Site].PositionM.X,Sites[Site].PositionM.Y,Sites[Site].PositionM.Z,Geometry.Obstacles[Box].CenterM.X,Geometry.Obstacles[Box].CenterM.Y,Geometry.Obstacles[Box].CenterM.Z));
  Run(5);TestTrue(TEXT("No early pump shutdown"),Cdu->IsPumpPowered());
  Run(95);if(bIntervalRejected) {Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return false;}TestTrue(TEXT("Computed water actually reached CDU"),Sim->GetWaterExposure().SprayTouchedM3[Binding]>0);
  Sim->SetLiveLeak(Site,0,TEXT("Fixture repair, not electrical recovery"));Run(150);
  const auto Snap=Sim->GetSnapshot();const auto& State=Sim->GetWaterImpactObservation().GetState();
  const auto Temp=Rack->GetModel().GetState().ComponentTempK;double MaxChip=0;for(double K:Temp) MaxChip=FMath::Max(MaxChip,K);
  const double Coolant=Rack->GetModel().GetState().CoolantTempK;
  TArray<double> Pressure,Flow;for(int32 N=0;N<Nodes;++N) Pressure.Add(Net.GetNodePressurePa(N));for(int32 P=0;P<Pipes;++P) {Flow.Add(Net.GetPipeFlowM3s(P));TestEqual(TEXT("Valves/resistance unchanged"),Net.GetPipeResistancePaS2M6(P),Resistance[P]);}
  TestTrue(TEXT("CDU, exchanger, inventory and topology retained"),IsValid(Cdu) && Room->GetRowCdu(RowA)==Cdu && UA==Cdu->HxUAWK && Sim->GetCouplingBridge().GetNumCduGroups()==2 && Room->GetCoolantInventory().GetTanks().Num()==2 && Nodes==Net.GetNumNodes() && Pipes==Net.GetNumPipes() && Leaks==Net.GetNumLeaks() && Pump==Room->GetRowPumpIndex(RowA));
  TestEqual(TEXT("Speed command distinct from power"),Cdu->PumpSpeedRatio,Speed);
  TestTrue(TEXT("Other CDU not spuriously stopped"),Healthy->IsPumpPowered() && Net.IsPumpPowered(OtherPump));
  TestTrue(TEXT("Hydraulic and air pressure convergence"),Snap.bHydraulicsConverged && Snap.bAirPressureConverged);
  for(double K:Snap.RackAirInletTempK) TestTrue(TEXT("Existing inlet plausibility"),K>=288.15 && K<=308.15);
  TestTrue(TEXT("Coolant/chips remain finite"),FMath::IsFinite(Coolant) && FMath::IsFinite(MaxChip));
  const double Water=Sim->GetLeakSpray().GetTotals().EmittedM3;
  MaxHeat=FMath::Max(MaxHeat,FMath::Abs(Snap.NormalizedEnergyResidual));MaxSite=FMath::Max(MaxSite,FMath::Abs(Sim->GetSiteEnergyResidual()));MaxSpray=FMath::Max(MaxSpray,Sim->GetLeakSpray().GetTotals().Residual);MaxFloor=FMath::Max(MaxFloor,Sim->GetFloorWater().GetTotals().Residual);
  for(const auto& T:Room->GetCoolantInventory().GetTanks()) MaxInventory=FMath::Max(MaxInventory,FMath::Abs(T.State.InitialWaterM3+T.State.MakeupM3-T.State.LeakedM3-T.State.WaterVolumeM3));
  TestTrue(TEXT("Existing pump heat and electric ledgers use same solved input"),FMath::Abs(Sim->GetCouplingBridge().GetLastCduPumpHeatW()-Room->GetTotalPumpElectricalW())<1e-8);
  if(Path==0) {ReferenceTemp=Temp;RefCoolant=Coolant;RefChip=MaxChip;RefIt=Snap.TotalItHeatW;RefFlow=Net.GetPumpFlowM3s(Pump);}
  else if(Path==1) TestTrue(TEXT("Read-only policy never shuts down or changes thermal results"),Cdu->IsPumpPowered() && ReferenceTemp==Temp && RefIt==Snap.TotalItHeatW && Sim->GetWaterCduPowerCuts().IsEmpty());
  else {
   TestTrue(TEXT("Trip stops only actual target pump after repair"),!Cdu->IsPumpPowered() && !Net.IsPumpPowered(Pump));
   TestEqual(TEXT("Stopped pump electric input zero"),Net.GetPumpElectricalInputPowerW(Pump),0.0);
   TestEqual(TEXT("Exactly one CDU application"),Sim->GetWaterCduPowerCuts().Num(),1);
   TestTrue(TEXT("No inferred rack slot trip from box spray"),Sim->GetWaterRackPowerCuts().IsEmpty());
   TestTrue(TEXT("Latched synthetic damage and actual power feedback"),State.Phase[Binding]==EWaterImpactPhase::Damaged && State.LastPowered[Binding]==0);
   TestEqual(TEXT("Pump shutdown never removes rack IT load"),Snap.TotalItHeatW,RefIt);
   // Facility supply starts warmer than the rack: the stopped transient can be cooler
   // than the circulating reference. Test calculated response/accumulation, not a forced sign.
   TestTrue(TEXT("Hydraulic/thermal response comes from solved loss of circulation"),FMath::Abs(Net.GetPumpFlowM3s(Pump))<RefFlow && Coolant!=RefCoolant && Temp!=ReferenceTemp);
   TestTrue(TEXT("Unpowered pump does not erase residual heat accumulation"),Coolant>InitialCoolant && MaxChip>InitialChip);
   for(const auto& C:Sim->GetWaterCduPowerCuts()) {TestEqual(TEXT("Correct actuator identity"),C.EquipmentId,FString(TEXT("cdu/pump-A")));TestTrue(TEXT("Trip quantized to next common step start"),C.AppliedTimeS==C.ObservedTimeS && C.AppliedTimeS>=C.TripTimeS && C.AppliedTimeS-C.TripTimeS<=Sim->FixedStepS+1e-12);AddInfo(FString::Printf(TEXT("CDU trip=%.17g observed=%.17g applied=%.17g s dt=%.17g s"),C.TripTimeS,C.ObservedTimeS,C.AppliedTimeS,Sim->FixedStepS));}
   if(Path==2) {TickTemp=Temp;TickPressure=Pressure;TickFlow=Flow;TickDepth=Sim->GetFloorWater().GetDepthM();TickPhase=State.Phase;TickCuts=Sim->GetWaterCduPowerCuts();TickWater=Water;TickHeat=Snap.NormalizedEnergyResidual;}
   else {TestTrue(TEXT("Tick/RunSteps exact common-step physical/state results"),Temp==TickTemp && Pressure==TickPressure && Flow==TickFlow && Sim->GetFloorWater().GetDepthM()==TickDepth && State.Phase==TickPhase && Water==TickWater && Snap.NormalizedEnergyResidual==TickHeat);if(TickCuts.Num()==1 && Sim->GetWaterCduPowerCuts().Num()==1) TestTrue(TEXT("Tick/RunSteps exact detection/application times"),TickCuts[0].TripTimeS==Sim->GetWaterCduPowerCuts()[0].TripTimeS && TickCuts[0].AppliedTimeS==Sim->GetWaterCduPowerCuts()[0].AppliedTimeS);}
  }
  AddInfo(FString::Printf(TEXT("W4 CDU path%d t=%.17g s flow=%.17g m3/s other=%.17g m3/s electric=%.17g W pumpHeat=%.17g W IT=%.17g W coolant=%.17g K maxChip=%.17g K deltaCoolant=%.17g K deltaChip=%.17g K water=%.17g m3 energy=%.17g site=%.17g hydResidual=%.17g hydFlowRef=%.17g m3/s hydPressureRef=%.17g Pa"),Path,Snap.SimTimeS,Net.GetPumpFlowM3s(Pump),Net.GetPumpFlowM3s(OtherPump),Room->GetTotalPumpElectricalW(),Sim->GetCouplingBridge().GetLastCduPumpHeatW(),Snap.TotalItHeatW,Coolant,MaxChip,Coolant-RefCoolant,MaxChip-RefChip,Water,Snap.NormalizedEnergyResidual,Sim->GetSiteEnergyResidual(),Room->GetLastSolveResult().NormalizedResidual,Room->GetLastSolveResult().ReferenceFlowM3s,Room->GetLastSolveResult().ReferencePressurePa));
  Sim->EnterLayoutMode();
  Sim->EnterSimulationMode();TestTrue(TEXT("Restart clears electrical cuts"),Cdu->IsPumpPowered() && Healthy->IsPumpPowered() && Sim->GetWaterCduPowerCuts().IsEmpty());TestFalse(TEXT("Restart clears response/policy"),Sim->IsCduWaterTripResponseEnabled() || Sim->IsRackWaterTripResponseEnabled() || Sim->HasWaterImpactPolicy());
  Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
 }
 AddInfo(FString::Printf(TEXT("W4 CDU every-step max hydraulic normalized residual=%.17g"),MaxHydraulic));
 TestTrue(TEXT("Existing conservation criteria preserved"),MaxHeat<1e-9 && MaxSite<1e-9 && MaxSpray<1e-12 && MaxFloor<1e-12 && MaxInventory<1e-15);
 AddInfo(FString::Printf(TEXT("W4 CDU max energy=%.17g site=%.17g spray=%.17g floor=%.17g normalized; inventory=%.17g m3; Tick/RunSteps temperature/pressure/flow/water difference=0"),MaxHeat,MaxSite,MaxSpray,MaxFloor,MaxInventory));
 return true;
}
#endif
