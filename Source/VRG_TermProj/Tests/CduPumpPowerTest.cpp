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
 TestTrue(TEXT("Explicit re-energization preserves original equations"),Again.bConverged && Net.GetPumpFlowM3s(P)==OnFlow && Net.GetPumpElectricalInputPowerW(P)==OnW);
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
 Room->RestartCoolantInventory();TestTrue(TEXT("New run restores electrical command and solve"),Cdu->IsPumpPowered() && Net.IsPumpPowered(Pump) && Room->GetLastSolveResult().bConverged && FMath::Abs(Net.GetPumpFlowM3s(Pump)-Flow)<1e-10);
 GEngine->DestroyWorldContext(World);World->DestroyWorld(false);return true;
}
#endif
