#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#if WITH_DEV_AUTOMATION_TESTS
constexpr auto WarmTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter;
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicWarmPoints,"VRG.Hydraulics.WarmStart.OperatingPoints",WarmTestFlags)
bool FHydraulicWarmPoints::RunTest(const FString& Parameters)
{
 const auto Params=FRackThermalModel::MakeGb200Nvl72Params(); FRackThermalInput Input;
 Input.ComponentHeatW=FRackThermalModel::MakeDefaultHeatW(Params);
 FHydraulicNetwork Cold; TArray<FInRowLoopHandles> H;
 for(int32 R=0;R<4;++R) { FInRowLoopSpec S; S.NumRacks=8; TArray<FInRackSpec> Internal;
  Internal.Init(InRackNetwork::FromThermal(Params,Input),8); H.Add(InRowLoopBuilder::AddLoop(Cold,S,&Internal)); }
 auto Warm=Cold; Warm.SetInitialGuessReuseEnabled(true);
 FHydraulicSolveSettings Settings; double MaxPa=0,MaxM3s=0,MaxPRel=0,MaxFRel=0,ColdMs=0,WarmMs=0; int32 ColdIt=0,WarmIt=0;
 auto Compare=[&](const TCHAR* Name,bool ExpectWarm,bool Bench=false) {
  const auto C=Cold.Solve(Settings); const auto W=Warm.Solve(Settings);
  TestTrue(Name,C.bConverged && W.bConverged && W.bUsedWarmStart==ExpectWarm && W.IterationsUsed<=Settings.MaxIterations);
  double DP=0,DF=0;
  for(int32 N=0;N<Cold.GetNumNodes();++N) DP=FMath::Max(DP,FMath::Abs(Cold.GetNodePressurePa(N)-Warm.GetNodePressurePa(N)));
  for(int32 E=0;E<Cold.GetNumPipes();++E) DF=FMath::Max(DF,FMath::Abs(Cold.GetPipeFlowM3s(E)-Warm.GetPipeFlowM3s(E)));
  for(int32 E=0;E<Cold.GetNumPumps();++E) DF=FMath::Max(DF,FMath::Abs(Cold.GetPumpFlowM3s(E)-Warm.GetPumpFlowM3s(E)));
  MaxPa=FMath::Max(MaxPa,DP); MaxM3s=FMath::Max(MaxM3s,DF); MaxPRel=FMath::Max(MaxPRel,DP/C.ReferencePressurePa); MaxFRel=FMath::Max(MaxFRel,DF/C.ReferenceFlowM3s);
  TestTrue(TEXT("unchanged normalized convergence criterion"),C.NormalizedResidual<=Settings.NormalizedResidualTarget && W.NormalizedResidual<=Settings.NormalizedResidualTarget);
  if(Bench) { ColdMs+=Cold.GetLastCosts().TotalMs; WarmMs+=Warm.GetLastCosts().TotalMs; ColdIt+=C.IterationsUsed;WarmIt+=W.IterationsUsed; }
 };
 Compare(TEXT("first execution is cold"),false); Compare(TEXT("unchanged converged state reusable"),true);
 for(auto* N:{&Cold,&Warm}) N->SetLeakHoleDiameterM(H[0].InRacks[0].Leaks[13*5+2],.003);
 Compare(TEXT("hole onset uses original seed"),false);
 TestTrue(TEXT("actual reverse outlet retained"),Warm.GetPipeFlowM3s(H[0].InRacks[0].OutletPipes[13])<0);
 const double Base=Cold.GetNodeSetPressurePa(H[0].CduNodeIndex);
 for(int32 S=0;S<20;++S) { // synthetic +10 Pa increments for identical boundary-input comparisons, not new equipment defaults
  for(auto* N:{&Cold,&Warm}) N->SetNodeSetPressurePa(H[0].CduNodeIndex,Base-10*(S+1));
  Compare(TEXT("continuous positive boundary updates reuse only converged states"),true,true);
 }
 TestTrue(TEXT("measured iteration reduction"),WarmIt<ColdIt);
 for(auto* N:{&Cold,&Warm}) { N->SetPipeClosed(H[0].InRacks[1].InletPipes[0],true); N->SetPipeClosed(H[0].InRacks[1].OutletPipes[0],true); }
 Compare(TEXT("detach/isolation uses cold seed"),false);
 for(auto* N:{&Cold,&Warm}) { N->SetPipeClosed(H[0].InRacks[1].InletPipes[0],false); N->SetPipeClosed(H[0].InRacks[1].OutletPipes[0],false); }
 Compare(TEXT("reattach uses cold seed"),false);
 for(auto* N:{&Cold,&Warm}) { N->SetPumpSpeedRatio(H[0].PumpIndex,0); N->SetNodeSetPressurePa(H[0].CduNodeIndex,0); N->SetLeakHoleDiameterM(H[0].InRacks[0].Leaks[13*5+2],0); }
 Compare(TEXT("depleted stopped loop retains per-loop hydrostatic seed"),false);
 TestTrue(TEXT("stopped row flow zero"),FMath::Abs(Warm.GetPumpFlowM3s(H[0].PumpIndex))<1e-12);
 for(auto* N:{&Cold,&Warm}) { N->SetPumpSpeedRatio(H[0].PumpIndex,1); N->SetNodeSetPressurePa(H[0].CduNodeIndex,Base); }
 Compare(TEXT("pump resume is cold"),false);
 for(auto* N:{&Cold,&Warm}) N->InvalidateInitialGuess(); Compare(TEXT("explicit restart invalidation"),false);
 for(auto* N:{&Cold,&Warm}) { FHydraulicLeak L;L.NodeIndex=H[0].SupplyNodeIndices[0];N->AddLeak(L); }
 Compare(TEXT("network edit invalidates seed"),false);
 AddInfo(FString::Printf(TEXT("warm/cold 20 steps %.6f/%.6f ms; Newton %d/%d; max pressure %.12g Pa (reference pressure normalized %.12g), flow %.12g m3/s (reference flow normalized %.12g)"),WarmMs,ColdMs,WarmIt,ColdIt,MaxPa,MaxPRel,MaxM3s,MaxFRel));
 // Measured normalized maxima: pressure 2.84e-15, flow 2.06e-12.
 // Reuse existing analytic hydraulic comparison bound (1e-9), do not alter solve tolerances.
 TestTrue(TEXT("existing normalized pressure/flow comparison bound"),MaxPRel<1e-9 && MaxFRel<1e-9);
 // Adversarial finite test-only guesses force overflow in the linearized system, never production parameters.
 TArray<double> P,F;P.Init(1e100,Warm.GetNumNodes());F.Init(1e200,Warm.GetNumPumps());
 Warm.SetInitialGuessForTesting(P,F); const auto Recovered=Warm.Solve(Settings);
 TestTrue(TEXT("failed warm attempt observable, cold recovery within shared budget"),Recovered.bConverged && Recovered.bWarmStartFailed && Recovered.bColdStartRetry && Recovered.WarmIterations+Recovered.ColdIterations==Recovered.IterationsUsed && Recovered.IterationsUsed<=Settings.MaxIterations);
 Settings.MaxIterations=1;Warm.SetInitialGuessForTesting(P,F);const auto Failed=Warm.Solve(Settings);
 TestTrue(TEXT("no extra retry budget, failure visible"),!Failed.bConverged && Failed.bUsedWarmStart && Failed.bWarmStartFailed && !Failed.bColdStartRetry && Failed.IterationsUsed==1);
 Settings=FHydraulicSolveSettings();const auto Next=Warm.Solve(Settings);
 TestTrue(TEXT("previous failure forces original seed next time"),Next.bConverged && !Next.bUsedWarmStart);
 AddInfo(FString::Printf(TEXT("fallback warm %d+cold %d=%d iterations; limited failure %d; next cold %d"),Recovered.WarmIterations,Recovered.ColdIterations,Recovered.IterationsUsed,Failed.IterationsUsed,Next.IterationsUsed));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicWarmThermal,"VRG.Coupling.InRack.WarmStartThermal",WarmTestFlags)
bool FHydraulicWarmThermal::RunTest(const FString& Parameters)
{
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;Room->bShowSummary=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>();Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
 World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,0,0),FRotator::ZeroRotator);
 World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
 auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->MaxStepsPerFrame=16;
 Room->RebuildRoomNow();TArray<double> ColdK;double ColdWaterK=0,MaxK=0,WaterDiff=0,MaxGlobal=0,ColdLeakM3=0,LeakDiff=0;
 for(int32 Path=0;Path<2;++Path) {
  Sim->EnterSimulationMode();Room->GetMutableNetwork().SetInitialGuessReuseEnabled(Path==1);
  for(int32 S=0;S<300;++S) {
   if(S==10) Room->SetInternalLeakHoleDiameterM(Rack,13,2,.003);
   if(S==150) Room->SetInternalLeakHoleDiameterM(Rack,13,2,0);
   Sim->Tick(.0201f);
   const auto Snap=Sim->GetSnapshot();MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(Snap.NormalizedEnergyResidual));
   TestTrue(TEXT("existing convergence/global conservation"),Snap.bHydraulicsConverged && Snap.bAirPressureConverged && FMath::Abs(Snap.NormalizedEnergyResidual)<1e-9);
  }
  const auto& State=Rack->GetMutableModel().GetState(); const double Leaked=Room->GetCoolantInventory().GetTanks()[0].State.LeakedM3;
  if(Path==0) {ColdK=State.ComponentTempK;ColdWaterK=State.CoolantTempK;ColdLeakM3=Leaked;}
  else {for(int32 I=0;I<ColdK.Num();++I) MaxK=FMath::Max(MaxK,FMath::Abs(ColdK[I]-State.ComponentTempK[I]));WaterDiff=FMath::Abs(ColdWaterK-State.CoolantTempK);LeakDiff=FMath::Abs(Leaked-ColdLeakM3);}
  AddInfo(FString::Printf(TEXT("thermal path%d time %.12g s leaked %.12g m3 global %.12g"),Path,Sim->GetSnapshot().SimTimeS,Leaked,MaxGlobal));
 }
 AddInfo(FString::Printf(TEXT("warm/cold component max difference %.12g K, return water %.12g K, leaked volume %.12g m3, global %.12g"),MaxK,WaterDiff,LeakDiff,MaxGlobal));
 // Measured component delta1.08e-12 K/return0 K/leak0 m3; post-measurement safety margin.
 TestTrue(TEXT("measured absolute thermal comparison bound"),MaxK<1e-9 && WaterDiff<1e-9);
 TestTrue(TEXT("existing absolute water comparison bound"),LeakDiff<1e-15);
 GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return true;
}
#endif
