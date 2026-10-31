#include "Misc/AutomationTest.h"
#include "Simulation/Coupling/InRackTransport.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS
constexpr auto TrayTraversalFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter;
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrayTransportTraversal,"VRG.Coupling.InRack.TransportTraversal",TrayTraversalFlags)
bool FTrayTransportTraversal::RunTest(const FString& Parameters)
{
 const auto Params=FRackThermalModel::MakeGb200Nvl72Params();FRackThermalInput Input;
 Input.ComponentHeatW=FRackThermalModel::MakeDefaultHeatW(Params);
 FHydraulicNetwork Net;TArray<FInRowLoopHandles> Rows;
 for(int32 R=0;R<4;++R) {FInRowLoopSpec S;S.NumRacks=8;TArray<FInRackSpec> In;In.Init(InRackNetwork::FromThermal(Params,Input),8);Rows.Add(InRowLoopBuilder::AddLoop(Net,S,&In));}
 auto Same=[](const FTrayCoolantInput& A,const FTrayCoolantInput& B) {
  return A.Network.Reservoirs==B.Network.Reservoirs && A.Network.NetWK==B.Network.NetWK && A.Network.BoundaryWK==B.Network.BoundaryWK
   && A.Network.LeakWK==B.Network.LeakWK && A.Network.ClassLeakWK==B.Network.ClassLeakWK && A.Network.ClassLeakKgs==B.Network.ClassLeakKgs
   && A.Network.LeakKgs==B.Network.LeakKgs && A.Network.MassResidualKgs==B.Network.MassResidualKgs
   && A.InletMassFlowKgs==B.InletMassFlowKgs && A.OutletMassFlowKgs==B.OutletMassFlowKgs && A.LeakMassFlowKgs==B.LeakMassFlowKgs
   && A.ReturnMassFlowKgs==B.ReturnMassFlowKgs && A.SupplyTempK==B.SupplyTempK && A.ReturnBoundaryTempK==B.ReturnBoundaryTempK;
 };
 for(int32 Mode=0;Mode<5;++Mode) {
  const auto& H=Rows[0].InRacks[0];
  if(Mode==1) Net.SetLeakHoleDiameterM(H.Leaks[13*5+2],.003);
  if(Mode==2) H.SetDocked(Net,4,false);
  if(Mode==3) {H.SetDocked(Net,4,true);Net.SetLeakHoleDiameterM(H.Leaks[13*5+2],0);}
  if(Mode==4) {Net.InvalidateInitialGuess();Net.SetFluidDensityKgM3(998);}
  const auto Solve=Net.Solve(FHydraulicSolveSettings());TestTrue(TEXT("same operating point converges"),Solve.bConverged);
  if(Mode==1) TestTrue(TEXT("signed reverse flow retained"),Net.GetPipeFlowM3s(H.OutletPipes[13])<0);
  TArray<double> Bulk;Net.GetNodeImbalancesM3s(Bulk);
  double OldMs=0,NewMs=0;InRackTransport::FBuildCosts Costs[2];
  for(const auto& Row:Rows) for(int32 R=0;R<Row.InRacks.Num();++R) {
   FTrayCoolantInput Ref,Got;
   for(int32 Path=0;Path<2;++Path) {
    const double Start=FPlatformTime::Seconds();
    for(int32 Repeat=0;Repeat<10;++Repeat) TestTrue(TEXT("transport builds"),InRackTransport::Build(Net,Row.InRacks[R],Row.RackReturnPipeIndices[R],Net.GetFluidDensityKgM3(),Params.CoolantCpJKgK,Path ? Got:Ref,&Bulk,Path!=0));
    (Path ? NewMs:OldMs)+=(FPlatformTime::Seconds()-Start)*1000;
    InRackTransport::FBuildCosts C;FTrayCoolantInput Profile;
    InRackTransport::Build(Net,Row.InRacks[R],Row.RackReturnPipeIndices[R],Net.GetFluidDensityKgM3(),Params.CoolantCpJKgK,Profile,&Bulk,Path!=0,&C);
    Costs[Path].SetupMs+=C.SetupMs;Costs[Path].LookupMs+=C.LookupMs;Costs[Path].MixingMs+=C.MixingMs;Costs[Path].AssemblyMs+=C.AssemblyMs;Costs[Path].EdgeVisits+=C.EdgeVisits;
   }
   TestTrue(TEXT("same ordered operator, signed flows and mass ledger exactly"),Same(Ref,Got));
  }
  AddInfo(FString::Printf(TEXT("transport mode%d 32racks x10 old/new %.6f/%.6f ms; separate single-build profile setup %.6f/%.6f lookup %.6f/%.6f mixing %.6f/%.6f assembly+validation %.6f/%.6f ms; edge visits %d/%d"),Mode,OldMs,NewMs,Costs[0].SetupMs,Costs[1].SetupMs,Costs[0].LookupMs,Costs[1].LookupMs,Costs[0].MixingMs,Costs[1].MixingMs,Costs[0].AssemblyMs,Costs[1].AssemblyMs,Costs[0].EdgeVisits,Costs[1].EdgeVisits));
 }
 // Fresh handle/slot topology; both failure paths leave caller output unchanged.
 FTrayCoolantInput Sentinel;Sentinel.SupplyTempK=321.15;
 TestFalse(TEXT("invalid traversal is transactional"),InRackTransport::Build(Net,Rows[0].InRacks[0],-1,997,4182,Sentinel,nullptr,true));
 TestEqual(TEXT("failed build keeps output"),Sentinel.SupplyTempK,321.15);
 Net.Reset();FInRowLoopSpec S;S.NumRacks=1;auto Spec=InRackNetwork::FromThermal(Params,Input);Spec.Slots.Pop();Spec.HeightM.Pop();Spec.DesignFraction.Pop();Spec.Docked.Pop();
 TArray<FInRackSpec> In={Spec};auto Fresh=InRowLoopBuilder::AddLoop(Net,S,&In);TestTrue(TEXT("new network/slot topology"),Net.Solve(FHydraulicSolveSettings()).bConverged);
 TArray<double> Bulk;Net.GetNodeImbalancesM3s(Bulk);FTrayCoolantInput Ref,Got;
 TestTrue(TEXT("fresh old/new builds"),InRackTransport::Build(Net,Fresh.InRacks[0],Fresh.RackReturnPipeIndices[0],997,4182,Ref,&Bulk,false) && InRackTransport::Build(Net,Fresh.InRacks[0],Fresh.RackReturnPipeIndices[0],997,4182,Got,&Bulk,true));
 TestTrue(TEXT("fresh geometry cannot reuse stale indices"),Same(Ref,Got));return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrayStabilityTraversal,"VRG.Thermal.Tray.StabilityTraversal",TrayTraversalFlags)
bool FTrayStabilityTraversal::RunTest(const FString& Parameters)
{
 auto Params=FRackThermalModel::MakeGb200Nvl72Params();FRackThermalInput Input;
 Input.ComponentHeatW=FRackThermalModel::MakeDefaultHeatW(Params);Input.SledDocked.Init(true,Params.NumSleds);Input.FanSpeedOverride=1;
 FRackThermalState State;State.ComponentTempK.Init(300.15,Input.ComponentHeatW.Num());State.CoolantTempK=300.15;State.SlotFanSpeedRatio.Init(1,Params.NumSleds);
 TArray<FRackThermalModel> Models;Models.SetNum(32);for(auto& M:Models) M.Initialize(Params,State);
 for(int32 Mode=0;Mode<4;++Mode) {
  if(Mode==1) {Input.SledDocked[0]=false;Input.FanSpeedOverride=.4;}
  if(Mode==2) {Input.SledDocked[0]=true;Input.FanSpeedOverride=0;}
  if(Mode==3) {Input.FanSpeedOverride=-1;Params.DeviceTypes[0].Components[0].AirStage=-1;for(auto& M:Models) M.Initialize(Params,State);}
  double Ms[2]={},Rates[2]={};
  for(int32 Path=0;Path<2;++Path) {
   for(auto& M:Models) M.SetGroupedStabilityForTesting(Path!=0);
   const double Start=FPlatformTime::Seconds();
   for(int32 Repeat=0;Repeat<100;++Repeat) for(auto& M:Models) Rates[Path]+=M.GetStabilityRateForTesting(Input);
   Ms[Path]=(FPlatformTime::Seconds()-Start)*1000;
  }
  TestEqual(TEXT("same ordered stability rate exactly"),Rates[1],Rates[0]);
  for(auto& M:Models) {M.SetGroupedStabilityForTesting(false);const bool Before=M.IsStepStable(Input,.02);M.SetGroupedStabilityForTesting(true);TestEqual(TEXT("same explicit interval decision"),M.IsStepStable(Input,.02),Before);}
  AddInfo(FString::Printf(TEXT("stability mode%d 32racks x100 old/new %.6f/%.6f ms; summed rate difference %.12g 1/s"),Mode,Ms[0],Ms[1],Rates[1]-Rates[0]));
 }
 return true;
}
#endif
