#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Water/WaterImpactModel.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
 constexpr EAutomationTestFlags SlotPowerTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackSlotPower,"VRG.Thermal.Power.SlotElectricalGate",SlotPowerTestFlags)
bool FRackSlotPower::RunTest(const FString& Parameters)
{
 const auto Params=FRackThermalModel::MakeGb200Nvl72Params();const int32 Count=Params.Slots.Num();
 FRackThermalInput In;In.ComponentHeatW=FRackThermalModel::MakeDefaultHeatW(Params);In.SledDocked.Init(true,Count);In.FanSpeedOverride=1;
 FRackThermalState Initial;Initial.CoolantTempK=In.CoolantInletTempK;Initial.ComponentTempK.Init(330.15,FRackThermalModel::CountNodes(Params)); // synthetic warm initial condition, not a trip threshold
 FRackThermalModel Legacy,Explicit;Legacy.Initialize(Params,Initial);Explicit.Initialize(Params,Initial);
 auto Powered=In;Powered.SlotPowered.Init(true,Count);Legacy.Step(In,.02);Explicit.Step(Powered,.02);
 TestTrue(TEXT("All-on input exactly preserves legacy state/energy"),Legacy.GetState().ComponentTempK==Explicit.GetState().ComponentTempK && Legacy.GetEnergyInJ()==Explicit.GetEnergyInJ());
 const auto On=Legacy.MakeSnapshot(.02);double MaxLossError=0;
 for(int32 Type:{FRackThermalModel::Gb200DevCompute,FRackThermalModel::Gb200DevNvSwitch,FRackThermalModel::Gb200DevManagement,FRackThermalModel::Gb200DevPowerShelf}) {
  const int32 Slot=Params.Slots.IndexOfByPredicate([&](const FRackSlot& S){return S.DeviceType==Type;});if(!TestTrue(TEXT("Preset slot exists"),Slot!=INDEX_NONE)) continue;
  auto Off=Powered;Off.SlotPowered[Slot]=false;FRackThermalModel Model;Model.Initialize(Params,Initial);Model.Step(Powered,.02);const auto Before=Model.GetState().ComponentTempK;Model.Step(Off,.02);const auto S=Model.MakeSnapshot(.04);
  const int32 Begin=Model.GetSlotNodeStart(Slot),End=Begin+Model.GetSlotNodeCount(Slot);double RemovedDc=0;
  for(int32 N=Begin;N<End;++N) {TestEqual(TEXT("Electrical heat of off slot zero"),S.ComponentHeatW[N],0.0);RemovedDc+=On.ComponentHeatW[N];}
  TestEqual(TEXT("Off fan cannot be overridden"),S.SlotFanPowerW[Slot],0.0);TestEqual(TEXT("Off forced airflow zero"),S.SlotAirMassFlowKgs[Slot],0.0);
  if(FRackThermalModel::FansOf(Params,Slot)) TestTrue(TEXT("Off fan controller ratio zero"),S.SlotFanSpeedRatio[Slot]==0);
  TestTrue(TEXT("Mechanical docking unaffected"),Off.SledDocked==In.SledDocked);
  if(Type==FRackThermalModel::Gb200DevCompute) {
   double LossFraction=0;for(int32 N=0;N<Model.GetNumNodes();++N) LossFraction+=Model.GetNodeParams(N).LossFractionOfRackLoad;
   const double Expected=RemovedDc*LossFraction;MaxLossError=FMath::Abs((Legacy.GetLastConversionLossW()-Model.GetLastConversionLossW())-Expected);
   TestTrue(TEXT("Existing PSU conversion loss follows removed DC load"),MaxLossError<1e-9);
   int32 LiquidNode=INDEX_NONE;for(int32 N=Begin;N<End;++N) if(Model.GetNodeParams(N).ToCoolantUAWK>0) {LiquidNode=N;break;}
   TestTrue(TEXT("Liquid-cooled component available"),LiquidNode!=INDEX_NONE);
   if(LiquidNode!=INDEX_NONE) {
    TestTrue(TEXT("Warm powered-off chip continues cooling"),S.ComponentTempK[LiquidNode]<Before[LiquidNode]);
    TestTrue(TEXT("Connected cold plate retains positive residual heat transfer"),S.ComponentHeatToCoolantW[LiquidNode]>0);
   }
   auto Detached=Off;Detached.SledDocked[Slot]=false;FRackThermalModel D;D.Initialize(Params,Initial);D.Step(Detached,.02);
   TestEqual(TEXT("Only actual detachment disconnects cold plate"),D.MakeSnapshot(.02).ComponentHeatToCoolantW[LiquidNode!=INDEX_NONE?LiquidNode:Begin],0.0);
  }
  if(Type==FRackThermalModel::Gb200DevPowerShelf) {
   for(int32 I=0;I<Count;++I) if(Params.Slots[I].DeviceType!=Type) {
    TestTrue(TEXT("No shelf-to-slot cascade"),Off.SlotPowered[I]);
    for(int32 N=Model.GetSlotNodeStart(I);N<Model.GetSlotNodeStart(I)+Model.GetSlotNodeCount(I);++N) TestEqual(TEXT("Other device electrical heat not cut by shelf gate"),S.ComponentHeatW[N],On.ComponentHeatW[N]);
   }
  }
 }
 auto AllOff=Powered;AllOff.SlotPowered.Init(false,Count);FRackThermalModel Cooling;Cooling.Initialize(Params,Initial);Cooling.Step(AllOff,.02);
 TestEqual(TEXT("All-off IT zero"),Cooling.GetLastTotalHeatW(),0.0);TestEqual(TEXT("All-off fan zero"),Cooling.GetLastTotalFanPowerW(),0.0);TestEqual(TEXT("All-off conversion loss zero"),Cooling.GetLastConversionLossW(),0.0);
 const double Norm=FMath::Max(1.0,FMath::Max(FMath::Abs(Cooling.GetEnergyOutJ()),FMath::Abs(Cooling.GetEnergyStoredJ())));
 const double Residual=FMath::Abs(Cooling.GetEnergyResidualJ())/Norm;
 TestTrue(TEXT("Zero-input residual includes released stored energy"),Residual<1e-9);
 auto Bad=Powered;Bad.SlotPowered.SetNum(1);const auto Saved=Cooling.GetState().ComponentTempK;const double Ledger=Cooling.GetEnergyOutJ();Cooling.Step(Bad,.02);
 TestFalse(TEXT("Malformed electrical layout invalid"),FRackThermalModel::IsSlotPowerInputValid(Bad,Count));TestTrue(TEXT("Bad power layout leaves state/ledger untouched"),Saved==Cooling.GetState().ComponentTempK && Ledger==Cooling.GetEnergyOutJ());
 AddInfo(FString::Printf(TEXT("W4 power all-on difference=0K/0J; conversion-loss formula error=%.17g W (1W reference); all-off residual=%.17g J normalized by %.17g J = %.17g"),MaxLossError,FMath::Abs(Cooling.GetEnergyResidualJ()),Norm,Residual));
 return true;
}
#endif
