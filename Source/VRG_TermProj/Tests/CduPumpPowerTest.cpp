#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduPumpElectricalCurve,"VRG.Hydraulics.Network.PumpElectricalPower",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCduPumpElectricalCurve::RunTest(const FString& Parameters)
{
 // Synthetic fixed-pressure boundary, not a CDU operating rating. The existing curve supplies all pump constants.
 FHydraulicNetwork Net;FHydraulicNode A;A.bFixedPressure=true;A.PressurePa=3e5;
 FHydraulicNode B=A;B.PressurePa=2e5;const int32 From=Net.AddNode(A),To=Net.AddNode(B);
 FHydraulicPump Pump;Pump.FromNodeIndex=From;Pump.ToNodeIndex=To;Pump.SpeedRatio=.8;const int32 P=Net.AddPump(Pump);
 const auto On=Net.Solve(FHydraulicSolveSettings());const double OnFlow=Net.GetPumpFlowM3s(P),OnW=Net.GetPumpElectricalInputPowerW(P);
 TestTrue(TEXT("Normal powered curve converges"),On.bConverged);
 Net.SetPumpPowered(P,false);const auto Off=Net.Solve(FHydraulicSolveSettings());
 const double Expected=FMath::Sqrt((A.PressurePa-B.PressurePa)/Pump.CurveCoeffPaS2M6);
 const double Error=FMath::Abs(Net.GetPumpFlowM3s(P)-Expected)/Expected;
 TestTrue(TEXT("Stopped pump with external head converges"),Off.bConverged);
 TestTrue(TEXT("Passive through-flow is solved, never forced to zero"),Net.GetPumpFlowM3s(P)>0 && Error<1e-6);
 TestEqual(TEXT("Off has no electric generation from passive pressure loss"),Net.GetPumpElectricalInputPowerW(P),0.0);
 TestEqual(TEXT("Electrical command preserves speed"),Net.GetPumpSpeedRatio(P),Pump.SpeedRatio);
 TestEqual(TEXT("Electrical command preserves pump topology"),Net.GetNumPumps(),1);
 Net.SetNodeSetPressurePa(From,B.PressurePa-(A.PressurePa-B.PressurePa));const auto Reverse=Net.Solve(FHydraulicSolveSettings());
 TestTrue(TEXT("Stopped pump also solves reverse through-flow"),Reverse.bConverged && FMath::Abs(Net.GetPumpFlowM3s(P)+Expected)/Expected<1e-6);
 TestEqual(TEXT("Reverse passive flow consumes no pump electricity"),Net.GetPumpElectricalInputPowerW(P),0.0);
 Net.SetNodeSetPressurePa(From,A.PressurePa);
 Net.SetPumpPowered(P,true);const auto Again=Net.Solve(FHydraulicSolveSettings());
 TestTrue(TEXT("Explicit re-energization preserves original equations"),Again.bConverged && FMath::Abs(Net.GetPumpFlowM3s(P)-OnFlow)/OnFlow<1e-10 && FMath::Abs(Net.GetPumpElectricalInputPowerW(P)-OnW)/FMath::Abs(OnW)<1e-10);
 AddInfo(FString::Printf(TEXT("CDU passive pump flow=%.17g m3/s normalized flow error=%.17g (reference %.17g m3/s), off hydraulic power=%.17g W, electrical=0 W, residual=%.17g flowRef=%.17g m3/s pressureRef=%.17g Pa"),Expected,Error,Expected,-(A.PressurePa-B.PressurePa)*Expected,Off.NormalizedResidual,Off.ReferenceFlowM3s,Off.ReferencePressurePa));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCduRoomPumpPower,"VRG.Hydraulics.Network.CduRuntimePower",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCduRoomPumpPower::RunTest(const FString& Parameters)
{
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
 auto* Cdu=World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100,150,0),FRotator::ZeroRotator);Room->RebuildRoomNow();
 const auto& Net=Room->GetNetwork();const int32 Pump=Room->GetRowPumpIndex(0),Nodes=Net.GetNumNodes(),Pipes=Net.GetNumPipes(),Leaks=Net.GetNumLeaks();
 const double Flow=Net.GetPumpFlowM3s(Pump),Speed=Cdu->PumpSpeedRatio,Water=Room->GetCoolantInventory().GetTanks()[0].State.WaterVolumeM3;
 TArray<double> Resistance;for(int32 P=0;P<Pipes;++P) Resistance.Add(Net.GetPipeResistancePaS2M6(P));
 Cdu->SetPumpPowered(false);Room->StepCoolantInventory(.02,0);
 TestTrue(TEXT("Off command re-solves idle sealed inventory"),Room->GetLastSolveResult().bConverged && !Net.IsPumpPowered(Pump));
 TestEqual(TEXT("Off electrical input"),Room->GetTotalPumpElectricalW(),0.0);
 TestTrue(TEXT("Closed-loop flow decreases by equations"),FMath::Abs(Net.GetPumpFlowM3s(Pump))<Flow);
 TestEqual(TEXT("No replacement CDU"),Room->GetRowCdu(0),Cdu);TestEqual(TEXT("Speed command unmodified"),Cdu->PumpSpeedRatio,Speed);
 TestEqual(TEXT("No inventory loss without leak"),Room->GetCoolantInventory().GetTanks()[0].State.WaterVolumeM3,Water);
 TestTrue(TEXT("Topology/indices unchanged"),Nodes==Net.GetNumNodes() && Pipes==Net.GetNumPipes() && Leaks==Net.GetNumLeaks() && Pump==Room->GetRowPumpIndex(0));
 for(int32 P=0;P<Pipes;++P) TestEqual(TEXT("No valve isolation"),Net.GetPipeResistancePaS2M6(P),Resistance[P]);
 AddInfo(FString::Printf(TEXT("CDU sealed loop on=%.17g off=%.17g m3/s residual=%.17g flowRef=%.17g m3/s pressureRef=%.17g Pa inventory=%.17g m3"),Flow,Net.GetPumpFlowM3s(Pump),Room->GetLastSolveResult().NormalizedResidual,Room->GetLastSolveResult().ReferenceFlowM3s,Room->GetLastSolveResult().ReferencePressurePa,Water));
 int32 Site=INDEX_NONE;for(int32 I=0;I<Room->GetNumLeakSpecs();++I) if(Room->GetLeakSpecRack(I)==Rack && !Room->IsLeakSpecReturnBranch(I)) {Site=I;break;}
 TestTrue(TEXT("Cold off-pump live hole available"),Site!=INDEX_NONE && Room->SetLeakHoleDiameterM(Site,.005));
 Room->StepCoolantInventory(.02,.02);TestTrue(TEXT("New leak while already stopped converges from cold coordinates"),Room->GetLastSolveResult().bConverged && !Room->GetLastSolveResult().bUsedWarmStart && !Net.IsPumpPowered(Pump));
 TestTrue(TEXT("Stopped network has calculated leak loss"),Room->GetCoolantInventory().GetTanks()[0].State.LeakedM3>0);
 Room->SetLeakHoleDiameterM(Site,0);Room->StepCoolantInventory(.02,.04);TestTrue(TEXT("Repair after cold stopped leak converges without repowering"),Room->GetLastSolveResult().bConverged && !Cdu->IsPumpPowered());
 Room->RestartCoolantInventory();TestTrue(TEXT("New run restores electrical command and solve"),Cdu->IsPumpPowered() && Net.IsPumpPowered(Pump) && Room->GetLastSolveResult().bConverged && FMath::Abs(Net.GetPumpFlowM3s(Pump)-Flow)<1e-10);
 GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStoppedPumpPressureDatum,"VRG.Hydraulics.Network.StoppedPumpPressureDatum",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FStoppedPumpPressureDatum::RunTest(const FString& Parameters)
{
 // Numerical translation fixture, not a device operating condition. Same double equations:
 // a 1e-6 m3/s demand creates micro-Pascal pipe drops even on a 200kPa fill pedestal.
 const double Demand=1e-6;double PreviousFlow=0;
 for(double Fill:{0.0,2e5}) {
  FHydraulicNetwork Net;FHydraulicNode A;A.bFixedPressure=true;A.PressurePa=Fill;
  const int32 From=Net.AddNode(A);FHydraulicNode Free;const int32 Middle=Net.AddNode(Free);Free.ExternalInflowM3s=-Demand;const int32 End=Net.AddNode(Free);
  FHydraulicPump Pump;Pump.FromNodeIndex=From;Pump.ToNodeIndex=Middle;Pump.bPowered=false;Net.AddPump(Pump);
  FHydraulicPipe Pipe;Pipe.FromNodeIndex=Middle;Pipe.ToNodeIndex=End;Pipe.ResistancePaS2M6=Pump.CurveCoeffPaS2M6;Net.AddPipe(Pipe);
  const auto Result=Net.Solve(FHydraulicSolveSettings());const double Flow=Net.GetPipeFlowM3s(0),ExpectedDrop=Pipe.ResistancePaS2M6*Demand*Demand;
  const double FlowError=FMath::Abs(Flow-Demand)/Demand,DropError=FMath::Abs(Net.PiezometricDropPa(Middle,End)-ExpectedDrop)/ExpectedDrop;
  TestTrue(TEXT("Stopped low-flow network converges from cold, without lowering target"),Result.bConverged && !Result.bUsedWarmStart);
  TestTrue(TEXT("Micro-drop mass balance and physical equation"),FlowError<1e-10 && DropError<1e-9 && FMath::Abs(Net.GetNodeImbalanceM3s(Middle))/Demand<1e-10 && FMath::Abs(Net.GetNodeImbalanceM3s(End))/Demand<1e-10);
  if(Fill!=0) TestTrue(TEXT("Changing fill datum preserves solved flow"),FMath::Abs(Flow-PreviousFlow)/Demand<1e-10);
  PreviousFlow=Flow;
  AddInfo(FString::Printf(TEXT("CDU pressure datum=%.17g Pa flow=%.17g m3/s normalized flow error=%.17g normalized drop error=%.17g (reference %.17g Pa); solver residual=%.17g, public gauge roundoff=%.17g Pa"),Fill,Flow,FlowError,DropError,ExpectedDrop,Result.NormalizedResidual,FMath::Abs((Net.GetNodePiezometricPa(Middle)-Net.GetNodePiezometricPa(End))-Net.PiezometricDropPa(Middle,End))));
 }
 return true;
}
#endif
