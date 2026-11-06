#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// W3.5 step cost (2026-11-06): the tray-transport reuse and the rack-parallel loops must give exactly the same results as
// the serial rebuild-every-step path. Exact equality (not a tolerance): every changed loop keeps its arithmetic and order.
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStepCostExactness,"VRG.Coupling.StepCostExactness",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FStepCostExactness::RunTest(const FString& Parameters)
{
 IConsoleVariable* Reuse=IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Perf.TrayTransportReuse"));
 IConsoleVariable* Parallel=IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Perf.RackParallel"));
 if(!TestNotNull(TEXT("Switches exist"),Reuse) || !TestNotNull(TEXT("Switches exist"),Parallel)) return false;
 struct FResult { TArray<double> PartK,AirK,PressurePa,FlowM3s,FloorM;double ItW=0,Residual=0,LeakedM3=0;int32 Reuses=0,Builds=0; };
 auto RunCase=[&](int32 bReuse,int32 bParallel,bool bWaterImpact)->FResult
 {
  Reuse->Set(bReuse,ECVF_SetByCode);Parallel->Set(bParallel,ECVF_SetByCode);
  FResult Out;
  UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
  auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
  TArray<ARackThermalActor*> Racks;
  // The two-row fixture of the existing W4 power-response tests (no overlapping equipment).
  for(int32 I=0;I<2;++I) {auto* R=World->SpawnActor<ARackThermalActor>(FVector(200,150+I*300,0),FRotator(0,90,0));R->ApplyGb200Nvl72Preset();R->bShowReadout=false;Racks.Add(R);}
  World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
  auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->MaxStepsPerFrame=16;
  Room->RebuildRoomNow();Sim->EnterSimulationMode();
  if(bWaterImpact) GEngine->Exec(World,TEXT("vrg.Water.Impact 1 0.3 0.4"));
  // 1 s static network (reuse path), then a plate leak (re-solved every step), then repaired.
  auto Steps=[&](int32 N){const double End=Sim->GetSnapshot().SimTimeS+N*Sim->FixedStepS;Sim->QueueWarmupSteps(N);for(int32 I=0;I<400 && Sim->GetSnapshot().SimTimeS<End-1e-9;++I) Sim->Tick(0.f);};
  Steps(50);
  Room->SetInternalLeakHoleDiameterM(Racks[0],0,2,.003);
  Steps(50);
  Out.Reuses=Sim->GetCouplingBridge().GetLastTrayTransportReuses();Out.Builds=Sim->GetCouplingBridge().GetLastTrayTransportBuilds();
  Room->SetInternalLeakHoleDiameterM(Racks[0],0,2,0);
  Steps(30);
  for(auto* R:Racks) Out.PartK.Append(R->GetModel().GetState().ComponentTempK);
  for(auto* R:Racks) {Out.PartK.Append(R->GetModel().GetTrayCoolant().GetTrayTempK());Out.PartK.Add(R->GetModel().GetTrayCoolant().GetReturnTempK());}
  const auto Snap=Sim->GetSnapshot();Out.AirK=Snap.RackAirInletTempK;Out.ItW=Snap.TotalItHeatW;Out.Residual=Snap.NormalizedEnergyResidual;
  for(int32 N=0;N<Room->GetNetwork().GetNumNodes();++N) Out.PressurePa.Add(Room->GetNetwork().GetNodePressurePa(N));
  for(int32 P=0;P<Room->GetNetwork().GetNumPipes();++P) Out.FlowM3s.Add(Room->GetNetwork().GetPipeFlowM3s(P));
  Out.FloorM=Sim->GetFloorWater().GetDepthM();Out.LeakedM3=Sim->GetRackInteriorCoupling().GetWater().ReceivedM3;
  TestEqual(TEXT("Same time reached"),Snap.SimTimeS,130*Sim->FixedStepS,1e-9);
  Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
  return Out;
 };
 for(bool bImpact:{false,true}) {
  const FResult Ref=RunCase(0,0,bImpact);
  TestTrue(TEXT("Leak exercised"),Ref.LeakedM3>0);
  TestTrue(TEXT("Existing heat ledger criterion"),FMath::Abs(Ref.Residual)<1e-9);
  for(const FIntPoint Mode:{FIntPoint(1,0),FIntPoint(0,1),FIntPoint(1,1)}) {
   const FResult R=RunCase(Mode.X,Mode.Y,bImpact);
   const FString What=FString::Printf(TEXT("reuse=%d parallel=%d impact=%d"),Mode.X,Mode.Y,bImpact?1:0);
   TestTrue(What+TEXT(": rack part and tray water temperatures identical"),R.PartK==Ref.PartK);
   TestTrue(What+TEXT(": inlet air identical"),R.AirK==Ref.AirK);
   TestTrue(What+TEXT(": pressures and flows identical"),R.PressurePa==Ref.PressurePa && R.FlowM3s==Ref.FlowM3s);
   TestTrue(What+TEXT(": floor water identical"),R.FloorM==Ref.FloorM && R.LeakedM3==Ref.LeakedM3);
   TestTrue(What+TEXT(": IT and ledger identical"),R.ItW==Ref.ItW && R.Residual==Ref.Residual);
   AddInfo(FString::Printf(TEXT("W3.5 cost exactness %s: last leak step reuses=%d builds=%d (reference %d/%d), energy residual %.3e"),*What,R.Reuses,R.Builds,Ref.Reuses,Ref.Builds,R.Residual));
  }
 }
 Reuse->Set(1,ECVF_SetByCode);Parallel->Set(1,ECVF_SetByCode);
 return true;
}
#endif
