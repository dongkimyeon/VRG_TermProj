#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackWaterPowerResponse,"VRG.Water.Impact.RackPowerResponse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FRackWaterPowerResponse::RunTest(const FString& Parameters)
{
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->ApplyGb200Nvl72Preset();Rack->LayoutId=TEXT("trip-A");Rack->bShowReadout=false;
 auto* Other=World->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->ApplyGb200Nvl72Preset();Other->LayoutId=TEXT("trip-B");Other->bShowReadout=false;
 World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100,150,0),FRotator::ZeroRotator);World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
 auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->MaxStepsPerFrame=16;Room->RebuildRoomNow();
 // SYNTHETIC fixture only: threshold zero / trip .115s (deliberately between endpoints) / damage .1s.
 // No operating failure limits are defined. 1mm plate leak stays below one slot's existing 30mL holding capacity.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Interior);Policy.InteriorPresentM3=0;Policy.TripPoweredContactS=.115;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Controlled rack power test, not hardware failure data");
 TArray<double> RefTemp,RefPressure,RefFlow,RefHeld,RefDepth,TickTemp,TickDamage;
 double RefIt=0,RefLoss=0,TickIt=0,RefWater=0,RefFloor=0,MaxEnergy=0,MaxWater=0,MaxFloor=0,MaxApplyDelay=0;
 TArray<EWaterImpactPhase> TickPhase;TArray<ADataCenterSimulationActor::FWaterRackPowerCut> TickCuts;
 for(int32 Path=0;Path<4;++Path) {
  Sim->EnterSimulationMode();TestFalse(TEXT("New execution clears response"),Sim->IsRackWaterTripResponseEnabled());
  for(bool P:Rack->GetInput().SlotPowered) TestTrue(TEXT("New execution clears runtime cuts"),P);
  const auto H=*Room->GetInRackHandles(Rack);const int32 Slot=H.Slots[0];
  const auto Docked=Rack->GetInput().SledDocked;const int32 Pipes=Room->GetNetwork().GetNumPipes(),Nodes=Room->GetNetwork().GetNumNodes();
  TArray<double> Resistance;for(int32 P=0;P<Pipes;++P) Resistance.Add(Room->GetNetwork().GetPipeResistancePaS2M6(P));
  if(Path>0) {
   TArray<bool> Power;Power.Init(true,Sim->GetWaterExposure().EquipmentId.Num());FString Error;
   TestTrue(TEXT("Explicit policy configuration"),Path==1?Sim->ConfigureWaterImpactObservation(Policy,Power,Error):Sim->ConfigureRackWaterTripResponse(Policy,Power,Error));
   auto ModelCopy=Sim->GetWaterImpactObservation();const auto Events=ModelCopy.GetEvents().Num();
   TestFalse(TEXT("Power cannot update past/future observation time"),ModelCopy.SetPowerAtCurrentTime(1,Power,Error));
   TestFalse(TEXT("Bad power array rejected"),ModelCopy.SetPowerAtCurrentTime(0,{},Error));
   Power.Init(false,Power.Num());TestTrue(TEXT("Known power at current endpoint accepted"),ModelCopy.SetPowerAtCurrentTime(0,Power,Error));
   TestEqual(TEXT("Power synchronization does not advance clock/events"),ModelCopy.GetSimTimeS(),0.0);TestEqual(TEXT("Power synchronization does not replay events"),ModelCopy.GetEvents().Num(),Events);
  }
  TestTrue(TEXT("Actual plate leak opened"),Room->SetInternalLeakHoleDiameterM(Rack,0,2,.001));
  auto Run=[&](int32 Steps) {const double End=Sim->GetSnapshot().SimTimeS+Steps*Sim->FixedStepS;if(Path<3) for(int32 I=0;I<Steps/5;++I) Sim->Tick(.1f);else {Sim->QueueWarmupSteps(Steps);for(int32 I=0;I<100 && Sim->GetSnapshot().SimTimeS<End-1e-9;++I) Sim->Tick(0);}TestTrue(TEXT("Requested common interval"),FMath::Abs(Sim->GetSnapshot().SimTimeS-End)<1e-9);};
  Run(5);TestTrue(TEXT("No early physical trip before duration"),Rack->GetInput().SlotPowered[Slot]);TestEqual(TEXT("No early cut event"),Sim->GetWaterRackPowerCuts().Num(),0);
  Run(45);const double Arrived=Sim->GetRackInteriorCoupling().GetWater().ReceivedM3;TestTrue(TEXT("Real water arrival exists"),Arrived>0);
  Room->SetInternalLeakHoleDiameterM(Rack,0,2,0);Run(50);
  const auto Snap=Sim->GetSnapshot();const auto Thermal=Rack->GetModel().MakeSnapshot(Snap.SimTimeS);const auto& W=Sim->GetRackInteriorCoupling().GetWater();
  TArray<double> Pressure,Flow;for(int32 N=0;N<Nodes;++N) Pressure.Add(Room->GetNetwork().GetNodePressurePa(N));for(int32 P=0;P<Pipes;++P) {Flow.Add(Room->GetNetwork().GetPipeFlowM3s(P));TestEqual(TEXT("Electrical cut does not change hydraulic resistance/valves"),Room->GetNetwork().GetPipeResistancePaS2M6(P),Resistance[P]);}
  TestEqual(TEXT("Pipe numbering/topology unchanged"),Room->GetNetwork().GetNumPipes(),Pipes);TestEqual(TEXT("Node topology unchanged"),Room->GetNetwork().GetNumNodes(),Nodes);
  TestTrue(TEXT("Mechanical docking unchanged"),Rack->GetInput().SledDocked==Docked);TestEqual(TEXT("Repair does not create water"),W.ReceivedM3,Arrived);
  TestTrue(TEXT("Existing pressure/hydraulic convergence"),Snap.bAirPressureConverged && Snap.bHydraulicsConverged);for(double K:Snap.RackAirInletTempK) TestTrue(TEXT("Existing inlet plausibility"),K>=288.15 && K<=308.15);
  MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(Snap.NormalizedEnergyResidual));MaxWater=FMath::Max(MaxWater,FMath::Abs(W.BalanceErrorM3()));MaxFloor=FMath::Max(MaxFloor,Sim->GetFloorWater().GetTotals().Residual);
  if(Path==0) {RefTemp=Thermal.ComponentTempK;RefIt=Snap.TotalItHeatW;RefLoss=Rack->GetModel().GetLastConversionLossW();RefPressure=Pressure;RefFlow=Flow;RefWater=W.ReceivedM3;RefFloor=Sim->GetFloorWater().GetTotals().ReceivedM3;RefHeld=W.HeldM3;RefDepth=Sim->GetFloorWater().GetDepthM();}
  else if(Path==1) TestTrue(TEXT("Read-only policy preserves legacy thermal behaviour"),RefTemp==Thermal.ComponentTempK && RefIt==Snap.TotalItHeatW && Sim->GetWaterRackPowerCuts().IsEmpty());
  else {
   TestEqual(TEXT("Exactly the arrived slot is physically cut"),Sim->GetWaterRackPowerCuts().Num(),1);
   TestFalse(TEXT("Repair does not restore electrical command"),Rack->GetInput().SlotPowered[Slot]);
   for(int32 S=0;S<Rack->Params.Slots.Num();++S) if(S!=Slot) TestTrue(TEXT("No unrelated slot power cut"),Rack->GetInput().SlotPowered[S]);
   for(bool P:Other->GetInput().SlotPowered) TestTrue(TEXT("No other rack false cutoff"),P);
   TestEqual(TEXT("Tripped fan electric input zero"),Thermal.SlotFanPowerW[Slot],0.0);TestEqual(TEXT("Tripped slot forced air zero"),Thermal.SlotAirMassFlowKgs[Slot],0.0);
   for(int32 N=Rack->GetModel().GetSlotNodeStart(Slot);N<Rack->GetModel().GetSlotNodeStart(Slot)+Rack->GetModel().GetSlotNodeCount(Slot);++N) TestEqual(TEXT("Tripped slot electrical heat zero"),Thermal.ComponentHeatW[N],0.0);
   TestTrue(TEXT("Actual IT and related PSU loss decrease"),Snap.TotalItHeatW<RefIt && Rack->GetModel().GetLastConversionLossW()<RefLoss);
   TestTrue(TEXT("Power cut preserves water and hydraulics"),RefPressure==Pressure && RefFlow==Flow && RefWater==W.ReceivedM3 && RefFloor==Sim->GetFloorWater().GetTotals().ReceivedM3 && RefHeld==W.HeldM3 && RefDepth==Sim->GetFloorWater().GetDepthM());
   const auto& E=Sim->GetWaterExposure();const auto& State=Sim->GetWaterImpactObservation().GetState();
   for(int32 I=0;I<E.EquipmentId.Num();++I) if(E.EquipmentId[I]==TEXT("rack/trip-A") && E.Slot[I]==Slot) {TestTrue(TEXT("Damage persists after physical trip/repair"),State.Phase[I]==EWaterImpactPhase::Damaged);TestEqual(TEXT("Actual power fed back to observation"),State.LastPowered[I],uint8(0));}
   for(const auto& C:Sim->GetWaterRackPowerCuts()) {TestEqual(TEXT("Correct native slot identity"),C.Slot,Slot);TestEqual(TEXT("Correct native rack identity"),C.EquipmentId,FString(TEXT("rack/trip-A")));const double Delay=C.AppliedTimeS-C.TripTimeS;MaxApplyDelay=FMath::Max(MaxApplyDelay,Delay);TestTrue(TEXT("Detection quantization at most one unchanged dt"),Delay>=0 && Delay<=Sim->FixedStepS+1e-12 && C.AppliedTimeS==C.ObservedTimeS);}
   if(Path==2) {TickTemp=Thermal.ComponentTempK;TickPhase=State.Phase;TickDamage=State.DamageS;TickCuts=Sim->GetWaterRackPowerCuts();TickIt=Snap.TotalItHeatW;}
   else {TestTrue(TEXT("Tick/RunSteps same state, heat and temperatures"),TickTemp==Thermal.ComponentTempK && TickPhase==State.Phase && TickDamage==State.DamageS && TickIt==Snap.TotalItHeatW);TestEqual(TEXT("Same cut count"),TickCuts.Num(),Sim->GetWaterRackPowerCuts().Num());if(TickCuts.Num()==1 && Sim->GetWaterRackPowerCuts().Num()==1) TestTrue(TEXT("Same application/detection times"),TickCuts[0].TripTimeS==Sim->GetWaterRackPowerCuts()[0].TripTimeS && TickCuts[0].AppliedTimeS==Sim->GetWaterRackPowerCuts()[0].AppliedTimeS);}
  }
  AddInfo(FString::Printf(TEXT("W4 rack power path%d t=%.17gs IT=%.17g W PSUloss=%.17g W water=%.17g m3 energy=%.17g floor=%.17g interior=%.17g m3 cuts=%d"),Path,Snap.SimTimeS,Snap.TotalItHeatW,Rack->GetModel().GetLastConversionLossW(),W.ReceivedM3,Snap.NormalizedEnergyResidual,Sim->GetFloorWater().GetTotals().Residual,W.BalanceErrorM3(),Sim->GetWaterRackPowerCuts().Num()));
  Sim->EnterLayoutMode();
 }
 TestTrue(TEXT("Existing conservation criteria"),MaxEnergy<1e-9 && MaxWater<1e-15 && MaxFloor<1e-12);
 AddInfo(FString::Printf(TEXT("W4 rack power max trip->apply delay=%.17g s / dt %.17g s; max energy residual=%.17g; water abs=%.17g m3; floor relative=%.17g; water/pressure/flow and Tick/RunSteps thermal difference=0"),MaxApplyDelay,Sim->FixedStepS,MaxEnergy,MaxWater,MaxFloor));
 Sim->EnterSimulationMode();TestFalse(TEXT("Restart clears response and policy"),Sim->IsRackWaterTripResponseEnabled() || Sim->HasWaterImpactPolicy());TestTrue(TEXT("Restart clears cut records"),Sim->GetWaterRackPowerCuts().IsEmpty());for(bool P:Rack->GetInput().SlotPowered) TestTrue(TEXT("Restart resets slot power"),P);
 Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return true;
}
#endif
