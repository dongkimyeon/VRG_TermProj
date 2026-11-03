#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrahWaterPowerResponse,"VRG.Water.Impact.CrahPowerResponse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCrahWaterPowerResponse::RunTest(const FString& Parameters)
{
 // Synthetic timing stimulus inherited from the rack/CDU response tests, NOT CRAH failure data.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Spray);Policy.SprayArrivalM3=0;
 Policy.TripPoweredContactS=.115;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;
 Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Controlled native CRAH trip fixture, not operating equipment failure criteria");
 double MaxHeat=0,MaxSite=0,MaxHydraulic=0,MaxFloor=0,MaxSpray=0,MaxInventory=0,MaxAirPressure=0,MaxChwHydraulic=0,MaxFwsHydraulic=0;
 for(ECrahUnitKind Kind:{ECrahUnitKind::InRow,ECrahUnitKind::Perimeter}) {
  TArray<double> ReferenceAir,ReferenceRack,TickAir,TickRack,TickPressure,TickFlow,TickDepth;TArray<EWaterImpactPhase> TickPhase;
  double RefFan=0,RefIt=0,RefFlow=0,TickWater=0,TickApplied=0,TickTrip=0;
  for(int32 Path=0;Path<4;++Path) {
   UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
   auto* Room=W->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
   auto* Rack=W->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->Rename(TEXT("CrahRackA"));Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
   auto* Other=W->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->Rename(TEXT("CrahRackB"));Other->ApplyGb200Nvl72Preset();Other->bShowReadout=false;
   W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);
   W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);
   auto* Crah=W->SpawnActor<ACrahUnitActor>(FVector(400,150,0),FRotator(0,180,0));Crah->Rename(TEXT("CrahA"));Crah->LayoutId=TEXT("wet-crah");Crah->Kind=Kind;
   // Native placed and previously converted origins use the identical physical path.
   if(Kind==ECrahUnitKind::InRow) {Crah->bConvertedAutomaticCrah=true;Crah->AutomaticLayoutTransform=Crah->GetActorTransform();Crah->AutomaticLayoutSizeM=Crah->SizeM;}
   auto* Healthy=W->SpawnActor<ACrahUnitActor>(FVector(400,450,0),FRotator(0,180,0));Healthy->Rename(TEXT("CrahB"));Healthy->LayoutId=TEXT("dry-crah");Healthy->bRuntimeLayoutCrah=true;
   auto* Air=W->SpawnActor<AAirFieldDebugActor>();Air->bShowSummary=false;
   auto* Sim=W->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=true;Room->RebuildRoomNow();Sim->EnterSimulationMode();
   auto Cleanup=[&]() {Sim->EnterLayoutMode();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);};
   const auto& Bridge=Sim->GetCouplingBridge();const auto& Net=Room->GetNetwork();
   TestTrue(TEXT("New run has no implicit response/policy"),!Sim->IsCrahWaterTripResponseEnabled() && !Sim->HasWaterImpactPolicy() && Crah->IsPowered());
   int32 Unit=INDEX_NONE,OtherUnit=INDEX_NONE;for(int32 U=0;U<Bridge.GetNumCrahUnits();++U) {if(Sim->GetCrahUnitActor(U)==Crah) Unit=U;if(Sim->GetCrahUnitActor(U)==Healthy) OtherUnit=U;}
   if(!TestTrue(TEXT("Native CRAH accepted with live CHW branches"),Unit!=INDEX_NONE && OtherUnit!=INDEX_NONE && Sim->GetFacilityPlant().IsChilledWaterActive())) {Cleanup();return false;}
   const int32 D=Bridge.AirSolverInput.NumDucts-Bridge.GetNumCrahUnits()+Unit;
   const auto Body=Air->GetSolver().GetGrid().bSolidCell;const auto Faces=Bridge.AirSolverInput.DuctFaceIndex;const auto DuctIds=Bridge.AirSolverInput.DuctFaceDuct;
   const int32 Nodes=Net.GetNumNodes(),Pipes=Net.GetNumPipes(),ChwBranches=Sim->GetFacilityPlant().GetChwLoop().GetNumBranches();
   const auto Settings=Crah->CaptureLayoutSettings();const double NominalFlow=Bridge.GetCrahUnits()[Unit].VolumeFlowM3s,HealthyFlow=Bridge.GetCrahUnits()[OtherUnit].VolumeFlowM3s;
   FString Error;TArray<bool> Power;Power.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
   TestFalse(TEXT("Unspecified policy rejected"),Sim->ConfigureCrahWaterTripResponse(FWaterImpactPolicy(),Power,Error));
   if(Path>0 && !TestTrue(TEXT("Explicit native CRAH policy"),Path==1?Sim->ConfigureWaterImpactObservation(Policy,Power,Error):Sim->ConfigureCrahWaterTripResponse(Policy,Power,Error,true,true))) {AddError(Error);Cleanup();return false;}
   int32 Box=INDEX_NONE,Binding=INDEX_NONE,Site=INDEX_NONE;
   for(int32 I=0;I<Sim->GetWaterExposureBindings().Num();++I) {const auto& B=Sim->GetWaterExposureBindings()[I];if(B.EquipmentId==TEXT("crah/wet-crah")) {Box=B.SprayObstacle;Binding=I;}}
   const auto& Geometry=Sim->GetLeakSpray().GetGeometry();const auto& Sites=Sim->GetLiveLeakSites();double Nearest=TNumericLimits<double>::Max();
   if(Box!=INDEX_NONE) for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {
    bool Inside=false;for(const auto& O:Geometry.Obstacles) {const FVector L=O.Rotation.UnrotateVector(Sites[I].PositionM-O.CenterM);Inside|=FMath::Abs(L.X)<=O.HalfM.X && FMath::Abs(L.Y)<=O.HalfM.Y && FMath::Abs(L.Z)<=O.HalfM.Z;}
    const double Dist=FVector::DistSquared(Sites[I].PositionM,Geometry.Obstacles[Box].CenterM);if(!Inside && Dist<Nearest) {Nearest=Dist;Site=I;}
   }
   if(!TestTrue(TEXT("Actual W1 jet and native CRAH box in fluid domain"),Site!=INDEX_NONE && Box!=INDEX_NONE && Geometry.RoomM.IsInsideOrOn(Geometry.Obstacles[Box].CenterM))) {Cleanup();return false;}
   Sim->SetLiveLeakDirection(Site,Geometry.Obstacles[Box].CenterM-Sites[Site].PositionM);Sim->SetLiveLeak(Site,.005,TEXT("Synthetic CRAH exposure fixture"));
   bool Failed=false;double HeldOpening=0,PreviousOpening=Sim->GetFacilityPlant().GetCrahOpening(Unit);bool Applied=false;
   auto Run=[&](int32 Steps) {
    if(Failed) return;const double End=Sim->GetSnapshot().SimTimeS+Steps*Sim->FixedStepS;
    for(int32 Frame=0;Frame<Steps+3 && Sim->GetSnapshot().SimTimeS<End-1e-9;++Frame) {
     const double Before=Sim->GetSnapshot().SimTimeS;
     if(Path<3) Sim->Tick(float(Sim->FixedStepS));else {Sim->QueueWarmupSteps(1);Sim->Tick(0);}
     if(Sim->GetSnapshot().SimTimeS<=Before) continue;
     const auto S=Sim->GetSnapshot();const auto A=Air->GetSolver().MakeSnapshot(S.SimTimeS);
     if(!TestTrue(TEXT("Every common step converges without duct rejection"),S.bHydraulicsConverged && S.bAirPressureConverged && !S.bAirDuctsRejected && Sim->GetFacilityPlant().GetChwResult().Hydraulics.bConverged && Sim->GetFacilityPlant().GetLoopResult().Hydraulics.bConverged)) {AddInfo(FString::Printf(TEXT("CRAH failure kind%d path%d t%.17g hyd%.17g air%.17g"),int32(Kind),Path,S.SimTimeS,Room->GetLastSolveResult().NormalizedResidual,A.NormalizedDivergence));Failed=true;return;}
     MaxChwHydraulic=FMath::Max(MaxChwHydraulic,Sim->GetFacilityPlant().GetChwResult().Hydraulics.NormalizedResidual);MaxFwsHydraulic=FMath::Max(MaxFwsHydraulic,Sim->GetFacilityPlant().GetLoopResult().Hydraulics.NormalizedResidual);
     MaxHeat=FMath::Max(MaxHeat,FMath::Abs(S.NormalizedEnergyResidual));MaxSite=FMath::Max(MaxSite,FMath::Abs(Sim->GetSiteEnergyResidual()));MaxHydraulic=FMath::Max(MaxHydraulic,Room->GetLastSolveResult().NormalizedResidual);MaxAirPressure=FMath::Max(MaxAirPressure,A.NormalizedDivergence);
     MaxFloor=FMath::Max(MaxFloor,Sim->GetFloorWater().GetTotals().Residual);MaxSpray=FMath::Max(MaxSpray,Sim->GetLeakSpray().GetTotals().Residual);
     for(const auto& T:Room->GetCoolantInventory().GetTanks()) MaxInventory=FMath::Max(MaxInventory,FMath::Abs(T.State.InitialWaterM3+T.State.MakeupM3-T.State.LeakedM3-T.State.WaterVolumeM3));
     for(double K:S.RackAirInletTempK) TestTrue(TEXT("Existing inlet plausibility"),K>=288.15 && K<=308.15);
     const double Opening=Sim->GetFacilityPlant().GetCrahOpening(Unit);
     if(!Sim->GetWaterCrahPowerCuts().IsEmpty()) {
      if(!Applied) {HeldOpening=PreviousOpening;Applied=true;}
      TestEqual(TEXT("Off CRAH does not command hydraulic isolation"),Opening,HeldOpening);
      TestEqual(TEXT("Plant updates cannot revive stopped coil"),Bridge.AirSolverInput.DuctCoilUAWK[D],0.0);TestEqual(TEXT("Stopped fan electric heat zero"),Bridge.AirSolverInput.DuctFanHeatW[D],0.0);
      const auto& In=Bridge.AirSolverInput;const auto& G=Air->GetSolver().GetGrid();
      for(int32 F=0;F<In.DuctFaceDuct.Num();++F) if(In.DuctFaceDuct[F]==D) {const int32 I=In.DuctFaceIndex[F];const double V=In.DuctFaceAxis[F]==0?G.VelocityXMs[I]:In.DuctFaceAxis[F]==1?G.VelocityYMs[I]:G.VelocityZMs[I];TestEqual(TEXT("Actual stopped unit MAC face velocity zero"),V,0.0);}
      TArray<double> Removed,Mix,Supply;TestTrue(TEXT("Per-unit readings preserve mapping"),Bridge.GetLastCrahUnitReadings(Removed,Mix,Supply));if(Removed.IsValidIndex(Unit)) TestEqual(TEXT("Actual stopped coil duty zero"),Removed[Unit],0.0);
     }
     PreviousOpening=Opening;
    }
    TestTrue(TEXT("Entire requested common interval"),FMath::Abs(Sim->GetSnapshot().SimTimeS-End)<1e-9);
   };
   Run(5);TestTrue(TEXT("No early fan shutdown"),Crah->IsPowered());Run(95);
   if(Failed) {Cleanup();return false;}
   TestTrue(TEXT("Computed spray water reaches selected CRAH"),Sim->GetWaterExposure().SprayTouchedM3[Binding]>0);
   Sim->SetLiveLeak(Site,0,TEXT("Repair is not electrical recovery"));Run(150);if(Failed) {Cleanup();return false;}
   const auto S=Sim->GetSnapshot();const auto Temp=Air->GetSolver().GetGrid().TemperatureK;const auto RackTemp=Rack->GetModel().GetState().ComponentTempK;
   TArray<double> Pressure,Flow;for(int32 N=0;N<Nodes;++N) Pressure.Add(Net.GetNodePressurePa(N));for(int32 P=0;P<Pipes;++P) Flow.Add(Net.GetPipeFlowM3s(P));
   TestTrue(TEXT("Body/duct/HX topology and settings retained"),Body==Air->GetSolver().GetGrid().bSolidCell && Faces==Bridge.AirSolverInput.DuctFaceIndex && DuctIds==Bridge.AirSolverInput.DuctFaceDuct && Bridge.GetNumCrahUnits()==2 && Nodes==Net.GetNumNodes() && Pipes==Net.GetNumPipes() && ChwBranches==Sim->GetFacilityPlant().GetChwLoop().GetNumBranches() && Settings==Crah->CaptureLayoutSettings());
   TestTrue(TEXT("No resizing or healthy-unit shutdown"),Bridge.GetCrahUnits()[Unit].VolumeFlowM3s==NominalFlow && Bridge.GetCrahUnits()[OtherUnit].VolumeFlowM3s==HealthyFlow && Healthy->IsPowered() && Bridge.GetCrahUnits()[OtherUnit].bPowered);
   if(Path==0) {ReferenceAir=Temp;ReferenceRack=RackTemp;RefFan=S.CrahFanPowerW;RefIt=S.TotalItHeatW;RefFlow=S.CrahFlowM3s;}
   else if(Path==1) TestTrue(TEXT("Read-only policy leaves physical result unchanged"),ReferenceAir==Temp && ReferenceRack==RackTemp && RefFan==S.CrahFanPowerW && Crah->IsPowered() && Sim->GetWaterCrahPowerCuts().IsEmpty());
   else {
    TestTrue(TEXT("Only target command latched off after repair"),!Crah->IsPowered() && !Bridge.GetCrahUnits()[Unit].bPowered && Sim->GetWaterCrahPowerCuts().Num()==1 && Sim->GetWaterRackPowerCuts().IsEmpty() && Sim->GetWaterCduPowerCuts().IsEmpty());
    TestTrue(TEXT("Fan electric input and active airflow decrease from existing operating points"),S.CrahFanPowerW<RefFan && S.CrahFlowM3s<RefFlow && FMath::Abs(S.CrahFlowM3s-(RefFlow-NominalFlow))<1e-12);
    TestEqual(TEXT("CRAH cutoff is not rack IT removal"),S.TotalItHeatW,RefIt);
    TestTrue(TEXT("Air response calculated without forcing its direction"),Temp!=ReferenceAir);
    const auto& State=Sim->GetWaterImpactObservation().GetState();TestTrue(TEXT("Actual electrical state fed back and damage latched"),State.LastPowered[Binding]==0 && State.Phase[Binding]==EWaterImpactPhase::Damaged);
    if(Sim->GetWaterCrahPowerCuts().Num()==1) {const auto& C=Sim->GetWaterCrahPowerCuts()[0];TestTrue(TEXT("Common-clock quantization"),C.EquipmentId==TEXT("crah/wet-crah") && C.AppliedTimeS==C.ObservedTimeS && C.AppliedTimeS>=C.TripTimeS && C.AppliedTimeS-C.TripTimeS<=Sim->FixedStepS+1e-12);if(Path==2) {TickTrip=C.TripTimeS;TickApplied=C.AppliedTimeS;}else TestTrue(TEXT("Tick/RunSteps same application timing"),TickTrip==C.TripTimeS && TickApplied==C.AppliedTimeS);}
    if(Path==2) {TickAir=Temp;TickRack=RackTemp;TickPressure=Pressure;TickFlow=Flow;TickDepth=Sim->GetFloorWater().GetDepthM();TickPhase=State.Phase;TickWater=Sim->GetLeakSpray().GetTotals().EmittedM3;}
    else TestTrue(TEXT("Tick/RunSteps same calculated physical state"),TickAir==Temp && TickRack==RackTemp && TickPressure==Pressure && TickFlow==Flow && TickDepth==Sim->GetFloorWater().GetDepthM() && TickPhase==State.Phase && TickWater==Sim->GetLeakSpray().GetTotals().EmittedM3);
   }
   double MaxAirDifference=0;for(int32 C=0;C<Temp.Num();++C) MaxAirDifference=FMath::Max(MaxAirDifference,FMath::Abs(Temp[C]-ReferenceAir[C]));
   AddInfo(FString::Printf(TEXT("W4 CRAH kind%d path%d t=%.17g s fan=%.17g W activeflow=%.17g m3/s coil=%.17g W IT=%.17g W max air difference=%.17g K emitted=%.17g m3 heldValve=%.17g trip=%.17g applied=%.17g s"),int32(Kind),Path,S.SimTimeS,S.CrahFanPowerW,S.CrahFlowM3s,S.CrahCoilRemovedW,S.TotalItHeatW,MaxAirDifference,Sim->GetLeakSpray().GetTotals().EmittedM3,HeldOpening,TickTrip,TickApplied));
   if(Path==0 || Path==3) {const int32 Cuts=Sim->GetWaterCrahPowerCuts().Num();Healthy->SetPowered(false);Sim->QueueWarmupSteps(1);Sim->Tick(0);TestFalse(TEXT("Runtime native electrical input works without water policy"),Bridge.GetCrahUnits()[OtherUnit].bPowered);TestEqual(TEXT("Cause-free actuator does not invent a W4 fault event"),Sim->GetWaterCrahPowerCuts().Num(),Cuts);}
   Sim->EnterLayoutMode();Sim->EnterSimulationMode();TestTrue(TEXT("Restart clears power and response state"),Crah->IsPowered() && Healthy->IsPowered() && !Sim->IsCrahWaterTripResponseEnabled() && !Sim->HasWaterImpactPolicy() && Sim->GetWaterCrahPowerCuts().IsEmpty());Cleanup();
  }
 }
 AddInfo(FString::Printf(TEXT("W4 CRAH every-step plant hydraulic maxima: CHW=%.17g FWS=%.17g normalized; both converge without target valve isolation"),MaxChwHydraulic,MaxFwsHydraulic));
 TestTrue(TEXT("Existing every-step conservation criteria"),MaxHeat<1e-9 && MaxSite<1e-9 && MaxFloor<1e-12 && MaxSpray<1e-12 && MaxInventory<1e-15);
 AddInfo(FString::Printf(TEXT("W4 CRAH every-step maxima: global=%.17g site=%.17g floor=%.17g spray=%.17g normalized, inventory=%.17g m3 hyd=%.17g air divergence=%.17g; Tick/RunSteps full state difference=0"),MaxHeat,MaxSite,MaxFloor,MaxSpray,MaxInventory,MaxHydraulic,MaxAirPressure));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrahResponseMapping,"VRG.Water.Impact.CrahResponseMapping",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCrahResponseMapping::RunTest(const FString& Parameters)
{
 for(bool Native:{false,true}) {
  auto* W=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
  auto* Room=W->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
  auto* Rack=W->SpawnActor<ARackThermalActor>();Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
  W->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;auto* Sim=W->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;
  if(Native) {W->SpawnActor<ACrahUnitActor>(FVector(200,0,0),FRotator(0,180,0));Sim->CrahLayout=ECrahLayout::CeilingCoil;}
  Room->RebuildRoomNow();Sim->EnterSimulationMode();
  FWaterImpactPolicy P;P.ContactSources=uint8(EWaterEvidence::Spray);P.SprayArrivalM3=0;P.TripPoweredContactS=.115;P.DamageContactAfterTripS=.1;P.MaxObservationGapS=1;P.WetRecovery=EWaterWetRecovery::LatchUntilRestart;P.Timing=EWaterContactTiming::HoldPreviousObservation;P.Basis=EWaterThresholdBasis::SyntheticTest;P.BasisNote=TEXT("Mapping fixture only");
  TArray<bool> Powered;Powered.Init(true,Sim->GetWaterExposure().EquipmentId.Num());FString Reason;
  TestTrue(TEXT("Read-only policy available without CRAH actuator support"),Sim->ConfigureWaterImpactObservation(P,Powered,Reason));
  const int32 Units=Sim->GetActiveCrahUnitCount();const auto Count=Sim->GetWaterObservationCount();
  TestFalse(TEXT("Virtual automatic or uncoupled ceiling actor cannot pretend shutdown"),Sim->ConfigureCrahWaterTripResponse(P,Powered,Reason));
  TestTrue(TEXT("Rejected mapping preserves existing policy and state with diagnostic"),!Reason.IsEmpty() && Sim->HasWaterImpactPolicy() && !Sim->IsCrahWaterTripResponseEnabled() && Units==Sim->GetActiveCrahUnitCount() && Count==Sim->GetWaterObservationCount());
  int32 Actors=0;for(TActorIterator<ACrahUnitActor> It(W);It;++It) ++Actors;
  TestEqual(TEXT("No implicit automatic conversion"),Actors,Native?1:0);
  Sim->EnterLayoutMode();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);
 }
 return true;
}
#endif
