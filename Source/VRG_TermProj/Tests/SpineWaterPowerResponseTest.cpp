#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpineWaterPowerResponse,"VRG.Water.Impact.SpinePowerResponse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FSpineWaterPowerResponse::RunTest(const FString& Parameters)
{
 // Synthetic timing stimulus inherited from the rack/CDU response tests, NOT 스파인 failure data.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Spray);Policy.SprayArrivalM3=0;
 Policy.TripPoweredContactS=.115;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;
 Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Controlled native 스파인 trip fixture, not operating equipment failure criteria");
 // Retain every common endpoint, not just the final state. Units are kept in separate arrays.
 struct FEndpoint { TArray<double> TempK, PressurePa, FlowM3s, WaterM3; TArray<double> DepthM; double TimeS=0; };
 TArray<FEndpoint> TickHistory;
 double TickTrip=-1,TickApplied=-1;
 double MaxHeat=0,MaxSite=0,MaxHydraulic=0,MaxChw=0,MaxFws=0,MaxAir=0,MaxFloor=0,MaxSpray=0,MaxInventory=0;
 double MaxTempDifferenceK=0,MaxPressureDifferencePa=0,MaxFlowDifferenceM3s=0,MaxWaterDifferenceM3=0,MaxDepthDifferenceM=0;
 double MinInletK=TNumericLimits<double>::Max(),MaxInletK=0,OnHeatW=0,OnFanW=0,PeakResidualW=0,MaxPumpRatio=0;
 int32 TotalEndpoints=0,OffEndpoints=0;
 for(int32 Path=0;Path<2;++Path) {
   UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
   auto* Room=W->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
   auto* Rack=W->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->Rename(TEXT("CrahRackA"));Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
   auto* Other=W->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->Rename(TEXT("CrahRackB"));Other->ApplyGb200Nvl72Preset();Other->bShowReadout=false;
   auto* CduA=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);
   auto* CduB=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);
   auto* Wet=W->SpawnActor<ASpineCabinetActor>(FVector(315,70,0),FRotator::ZeroRotator);
   auto* Dry=W->SpawnActor<ASpineCabinetActor>(FVector(400,450,0),FRotator::ZeroRotator);
   Wet->LayoutId=TEXT("wet-spine"); Dry->LayoutId=TEXT("dry-spine");
   auto* Air=W->SpawnActor<AAirFieldDebugActor>(); Air->bShowSummary=false;
   auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false; Sim->bFacilityPlant=true; // Extend the existing fixture to the current site ledger/CHW/FWS path.
   Room->RebuildRoomNow(); Sim->EnterSimulationMode();
   auto Cleanup=[&]() {Sim->EnterLayoutMode();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);};
   const auto& Bridge=Sim->GetCouplingBridge(); const auto& Net=Room->GetNetwork();
   const int32 Nodes=Net.GetNumNodes(),Pipes=Net.GetNumPipes();
   const auto Body=Air->GetSolver().GetGrid().bSolidCell;
   const auto Faces=Bridge.AirSolverInput.DuctFaceIndex, Ducts=Bridge.AirSolverInput.DuctFaceDuct;
   const auto WetSettings=Wet->CaptureLayoutSettings(),DrySettings=Dry->CaptureLayoutSettings();
   const auto WetTransform=Wet->GetActorTransform(),DryTransform=Dry->GetActorTransform();
   const auto RackDocked=Rack->Input.SledDocked,OtherDocked=Other->Input.SledDocked;
   TArray<uint8> PipeKinds; TArray<FIntPoint> PipeEnds;
   for(int32 P=0;P<Pipes;++P) {PipeKinds.Add(uint8(Net.IsRackInternalPipe(P)) | (uint8(Net.IsPipeClosed(P))<<1));PipeEnds.Add({Net.GetPipeFromNodeIndex(P),Net.GetPipeToNodeIndex(P)});}
   bool Failed=false; int32 Endpoint=0; double PreviousStored=Wet->GetMutableModel().GetEnergyStoredJ();
   auto CleanupOnFailure=[&]() { Cleanup(); return false; };
   if(!TestTrue(TEXT("Existing facility cooling active for the extended site checks"),Sim->GetFacilityPlant().IsChilledWaterActive())) return CleanupOnFailure();
   FString Reason; TArray<bool> Powered; Powered.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
   if(!TestTrue(TEXT("침수 정책 활성화"),Sim->ConfigureSpineWaterTripResponse(Policy,Powered,Reason))) {AddError(Reason); Cleanup(); return false;}
   int32 Box=INDEX_NONE,Binding=INDEX_NONE,Site=INDEX_NONE;
   for(int32 I=0;I<Sim->GetWaterExposureBindings().Num();++I) {const auto& B=Sim->GetWaterExposureBindings()[I];if(B.EquipmentId==TEXT("spine/wet-spine")) {Box=B.SprayObstacle;Binding=I;}}
   const auto& Geometry=Sim->GetLeakSpray().GetGeometry();const auto& Sites=Sim->GetLiveLeakSites();double Nearest=TNumericLimits<double>::Max();
   if(Box!=INDEX_NONE) for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {
    bool Inside=false;for(const auto& O:Geometry.Obstacles) {const FVector L=O.Rotation.UnrotateVector(Sites[I].PositionM-O.CenterM);Inside|=FMath::Abs(L.X)<=O.HalfM.X && FMath::Abs(L.Y)<=O.HalfM.Y && FMath::Abs(L.Z)<=O.HalfM.Z;}
    const double Dist=FVector::DistSquared(Sites[I].PositionM,Geometry.Obstacles[Box].CenterM);if(!Inside && Dist<Nearest) {Nearest=Dist;Site=I;}
   }
   if(!TestTrue(TEXT("Actual W1 jet and native 스파인 box in fluid domain"),Site!=INDEX_NONE && Box!=INDEX_NONE && Geometry.RoomM.IsInsideOrOn(Geometry.Obstacles[Box].CenterM))) {Cleanup();return false;}
   Sim->SetLiveLeakDirection(Site,Geometry.Obstacles[Box].CenterM-Sites[Site].PositionM);Sim->SetLiveLeak(Site,.005,TEXT("Synthetic 스파인 exposure fixture"));
   auto Difference=[&](const TArray<double>& A,const TArray<double>& B,double& MaxDifference) {
    if(A.Num()!=B.Num()) return false;
    for(int32 I=0;I<A.Num();++I) {if(!FMath::IsFinite(A[I]) || !FMath::IsFinite(B[I])) return false;MaxDifference=FMath::Max(MaxDifference,FMath::Abs(A[I]-B[I]));}
    return A==B; // Same fixed-step inputs, existing deterministic Tick/RunSteps contract.
   };
   auto Run=[&](int32 Count) {
    const double End=Sim->GetSnapshot().SimTimeS+Count*Sim->FixedStepS;
    // Tick takes a float frame interval; its first frame may only fill the accumulator.
    // Reuse the CRAH test runner: inspect actual common endpoints and still require the full interval.
    for(int32 I=0;I<Count+3 && Sim->GetSnapshot().SimTimeS<End-1e-9 && !Failed;++I) {
     const double Before=Sim->GetSnapshot().SimTimeS;
     if(Path==0) Sim->Tick(float(Sim->FixedStepS)); else {Sim->QueueWarmupSteps(1);Sim->Tick(0);}
     const auto S=Sim->GetSnapshot(); if(S.SimTimeS<=Before) continue;
     const auto A=Air->GetSolver().MakeSnapshot(S.SimTimeS);
     const auto Spray=Sim->GetLeakSpray().GetTotals(); const auto Floor=Sim->GetFloorWater().GetTotals();
     const auto& Plant=Sim->GetFacilityPlant();
     const double Site=Sim->GetSiteEnergyResidual();
     if(!TestTrue(TEXT("Each requested common step advances and all active solvers converge"),
      FMath::Abs(S.SimTimeS-Before-Sim->FixedStepS)<1e-12 && S.bHydraulicsConverged && S.bAirPressureConverged && !S.bAirDuctsRejected &&
      Plant.GetChwResult().Hydraulics.bConverged && Plant.GetLoopResult().Hydraulics.bConverged &&
      FMath::IsFinite(S.NormalizedEnergyResidual) && FMath::IsFinite(Site) && FMath::IsFinite(Spray.Residual) && FMath::IsFinite(Floor.Residual))) {
      AddInfo(FString::Printf(TEXT("Spine physical failure path%d endpoint%d t%.17g TCS%.17g air%.17g CHW%.17g FWS%.17g"),Path,Endpoint,S.SimTimeS,Room->GetLastSolveResult().NormalizedResidual,A.NormalizedDivergence,Plant.GetChwResult().Hydraulics.NormalizedResidual,Plant.GetLoopResult().Hydraulics.NormalizedResidual));Failed=true;break;
     }
     ++TotalEndpoints; MaxHeat=FMath::Max(MaxHeat,FMath::Abs(S.NormalizedEnergyResidual));MaxSite=FMath::Max(MaxSite,FMath::Abs(Site));
     MaxHydraulic=FMath::Max(MaxHydraulic,Room->GetLastSolveResult().NormalizedResidual);MaxAir=FMath::Max(MaxAir,A.NormalizedDivergence);
     MaxChw=FMath::Max(MaxChw,Plant.GetChwResult().Hydraulics.NormalizedResidual);MaxFws=FMath::Max(MaxFws,Plant.GetLoopResult().Hydraulics.NormalizedResidual);
     MaxFloor=FMath::Max(MaxFloor,Floor.Residual);MaxSpray=FMath::Max(MaxSpray,Spray.Residual);
     FEndpoint Frame; Frame.TimeS=S.SimTimeS; Frame.TempK=Air->GetSolver().GetGrid().TemperatureK;Frame.DepthM=Sim->GetFloorWater().GetDepthM();
     for(ARackThermalActor* R:{Rack,Other}) {Frame.TempK.Append(R->GetModel().GetState().ComponentTempK);Frame.TempK.Add(R->GetModel().GetState().CoolantTempK);if(R->GetModel().UsesTrayCoolant()) Frame.TempK.Append(R->GetModel().GetTrayCoolant().GetTrayTempK());}
     for(ASpineCabinetActor* Spine:{Wet,Dry}) Frame.TempK.Append(Spine->GetMutableModel().GetState().ComponentTempK);
     Frame.TempK.Append({Plant.GetSupplyTempK(),Plant.GetChwSupplyTempK()});
     for(int32 N=0;N<Nodes;++N) Frame.PressurePa.Add(Net.GetNodePressurePa(N));
     for(int32 P=0;P<Pipes;++P) Frame.FlowM3s.Add(Net.GetPipeFlowM3s(P));
     for(const auto& Tank:Room->GetCoolantInventory().GetTanks()) {
      MaxInventory=FMath::Max(MaxInventory,FMath::Abs(Tank.State.InitialWaterM3+Tank.State.MakeupM3-Tank.State.LeakedM3-Tank.State.WaterVolumeM3));
      Frame.WaterM3.Append({Tank.State.WaterVolumeM3,Tank.State.MakeupM3,Tank.State.LeakedM3});
     }
     Frame.WaterM3.Append({Spray.EmittedM3,Spray.AirborneM3,Spray.WaitingM3,Spray.FilmM3,Spray.FloorM3,Spray.GroundM3,Floor.ReceivedM3,Floor.StoredM3,Floor.DrainedM3,Floor.TrayM3,Floor.ClippedM3});
     if(Path==0) TickHistory.Add(Frame);
     else if(!TestTrue(TEXT("Every Tick/RunSteps endpoint has identical temperatures, pressure, flow and water inventory/depth"),
      TickHistory.IsValidIndex(Endpoint) && Frame.TimeS==TickHistory[Endpoint].TimeS &&
      Difference(Frame.TempK,TickHistory[Endpoint].TempK,MaxTempDifferenceK) && Difference(Frame.PressurePa,TickHistory[Endpoint].PressurePa,MaxPressureDifferencePa) &&
      Difference(Frame.FlowM3s,TickHistory[Endpoint].FlowM3s,MaxFlowDifferenceM3s) && Difference(Frame.WaterM3,TickHistory[Endpoint].WaterM3,MaxWaterDifferenceM3) &&
      Difference(Frame.DepthM,TickHistory[Endpoint].DepthM,MaxDepthDifferenceM))) {Failed=true;break;}
     ++Endpoint;
     for(double K:S.RackAirInletTempK) {MinInletK=FMath::Min(MinInletK,K);MaxInletK=FMath::Max(MaxInletK,K);TestTrue(TEXT("Existing 15..35 C inlet plausibility"),FMath::IsFinite(K) && K>=288.15 && K<=308.15);}
     if(S.TotalItHeatW>0) {double PumpW=0;for(int32 P=0;P<Net.GetNumPumps();++P) PumpW+=Net.GetPumpElectricalInputPowerW(P);const double Ratio=PumpW/S.TotalItHeatW;MaxPumpRatio=FMath::Max(MaxPumpRatio,Ratio);TestTrue(TEXT("Existing pump electric/IT plausibility"),FMath::IsFinite(Ratio) && Ratio>=0 && Ratio<.05);}
     TestTrue(TEXT("Healthy cabinet/CDU powers and rack docking retained"),Dry->IsPowered() && CduA->IsPumpPowered() && CduB->IsPumpPowered() && RackDocked==Rack->Input.SledDocked && OtherDocked==Other->Input.SledDocked);
     // The new shared busbar can cut a rack independently of docking: check actual power too.
     for(ARackThermalActor* R:{Rack,Other}) for(int32 Slot=0;Slot<R->GetModel().GetNumSleds();++Slot)
      TestTrue(TEXT("Every other rack slot remains electrically live, including the shared busbar"),R->GetModel().IsSlotLive(R->Input,Slot,R->GetModel().GetNumSleds()));
     const auto Thermal=Wet->GetMutableModel().MakeSnapshot(S.SimTimeS);const double Stored=Wet->GetMutableModel().GetEnergyStoredJ();
     TestTrue(TEXT("Existing thermal integration remains stable"),Thermal.bStepStable);
     if(Wet->IsPowered()) {OnHeatW=FMath::Max(OnHeatW,Thermal.TotalHeatW);OnFanW=FMath::Max(OnFanW,Thermal.TotalFanPowerW);}
     else {
      ++OffEndpoints;TestEqual(TEXT("Tripped spine electric heat zero"),Thermal.TotalHeatW,0.0);TestEqual(TEXT("Tripped spine fan electric input zero"),Thermal.TotalFanPowerW,0.0);
      for(double Flow:Thermal.SlotAirMassFlowKgs) TestEqual(TEXT("Tripped spine active airflow zero"),Flow,0.0);
      TestTrue(TEXT("Healthy spine still produces heat and fan airflow"),Dry->GetMutableModel().GetLastTotalHeatW()>0 && Dry->GetMutableModel().GetLastTotalFanPowerW()>0);
      PeakResidualW=FMath::Max(PeakResidualW,Thermal.TotalHeatToAmbientW);
      if(Thermal.TotalHeatToAmbientW>0) TestTrue(TEXT("Calculated positive residual heat release reduces stored energy"),Stored<PreviousStored);
     }
     PreviousStored=Stored;
    }
    if(!Failed) TestTrue(TEXT("Entire requested common interval integrated"),FMath::Abs(Sim->GetSnapshot().SimTimeS-End)<1e-9);
   };
   Run(100); if(Failed) return CleanupOnFailure();
   TestFalse(TEXT("노출 스파인 차단"),Wet->IsPowered());
   TestTrue(TEXT("건조 스파인 통전"),Dry->IsPowered());
   Sim->SetLiveLeak(Site,0,TEXT("수리")); Run(150); if(Failed) return CleanupOnFailure();
   TestFalse(TEXT("수리 후 차단 유지"),Wet->IsPowered());
   TestEqual(TEXT("Both execution paths cover all 250 common endpoints"),Endpoint,250);
   TArray<uint8> FinalKinds; TArray<FIntPoint> FinalEnds;
   for(int32 P=0;P<Net.GetNumPipes();++P) {FinalKinds.Add(uint8(Net.IsRackInternalPipe(P)) | (uint8(Net.IsPipeClosed(P))<<1));FinalEnds.Add({Net.GetPipeFromNodeIndex(P),Net.GetPipeToNodeIndex(P)});}
   TestTrue(TEXT("Existing hydraulic/solid/duct/cabinet connections and settings retained"),Nodes==Net.GetNumNodes() && Pipes==Net.GetNumPipes() && PipeKinds==FinalKinds && PipeEnds==FinalEnds && Body==Air->GetSolver().GetGrid().bSolidCell && Faces==Bridge.AirSolverInput.DuctFaceIndex && Ducts==Bridge.AirSolverInput.DuctFaceDuct && WetSettings==Wet->CaptureLayoutSettings() && DrySettings==Dry->CaptureLayoutSettings() && WetTransform.Equals(Wet->GetActorTransform()) && DryTransform.Equals(Dry->GetActorTransform()));
   const auto& Cuts=Sim->GetWaterSpinePowerCuts();
   if(TestEqual(TEXT("단일 차단 기록"),Cuts.Num(),1)) {
    const auto& Cut=Cuts[0];
    TestEqual(TEXT("대상"),Cut.EquipmentId,FString(TEXT("spine/wet-spine")));
    TestTrue(TEXT("판정 뒤 한 스텝 안에 적용"),Cut.AppliedTimeS==Cut.ObservedTimeS && Cut.AppliedTimeS>=Cut.TripTimeS && Cut.AppliedTimeS-Cut.TripTimeS<=Sim->FixedStepS+1e-12);
    if(Path==0) {TickTrip=Cut.TripTimeS; TickApplied=Cut.AppliedTimeS;}
    else {TestEqual(TEXT("판정 시각 일치"),Cut.TripTimeS,TickTrip);TestEqual(TEXT("차단 시각 일치"),Cut.AppliedTimeS,TickApplied);}
   }
   Sim->EnterLayoutMode(); Sim->EnterSimulationMode();
   TestTrue(TEXT("재시작 통전"),Wet->IsPowered() && Dry->IsPowered());
   TestEqual(TEXT("재시작 사건 초기화"),Sim->GetWaterSpinePowerCuts().Num(),0);
   Cleanup();
 }
 TestTrue(TEXT("Existing every-step energy/water conservation criteria"),MaxHeat<1e-9 && MaxSite<1e-9 && MaxFloor<1e-12 && MaxSpray<1e-12 && MaxInventory<1e-15);
 TestTrue(TEXT("Nonvacuous ON/OFF and residual cooling observations"),OnHeatW>0 && OnFanW>0 && PeakResidualW>0 && OffEndpoints>0 && TotalEndpoints==500);
 AddInfo(FString::Printf(TEXT("Spine every-step: endpoints%d off%d global%.17g site%.17g spray%.17g floor%.17g normalized inventory%.17g m3 TCS%.17g CHW%.17g FWS%.17g normalized air divergence%.17g inlet[%.17g,%.17g] K pump/IT max%.17g"),TotalEndpoints,OffEndpoints,MaxHeat,MaxSite,MaxSpray,MaxFloor,MaxInventory,MaxHydraulic,MaxChw,MaxFws,MaxAir,MinInletK,MaxInletK,MaxPumpRatio));
 AddInfo(FString::Printf(TEXT("Spine response: ON heat%.17g W fan%.17g W -> OFF0; peak residual%.17g W trip%.17g applied%.17g s; Tick/RunSteps max differences temperature%.17g K pressure%.17g Pa flow%.17g m3/s water%.17g m3 depth%.17g m"),OnHeatW,OnFanW,PeakResidualW,TickTrip,TickApplied,MaxTempDifferenceK,MaxPressureDifferencePa,MaxFlowDifferenceM3s,MaxWaterDifferenceM3,MaxDepthDifferenceM));
 return true;
}
#endif
