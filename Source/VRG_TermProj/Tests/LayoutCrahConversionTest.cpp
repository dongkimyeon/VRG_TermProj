#include "Misc/AutomationTest.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCrahConversionTest,"VRG.Interaction.Layout.CrahConversion",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FLayoutCrahConversionTest::RunTest(const FString& Parameters)
{
 auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
 auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* Room=W->SpawnActor<ADataCenterRoomActor>(); auto* Air=W->SpawnActor<AAirFieldDebugActor>();
 auto* Rack=W->SpawnActor<ARackThermalActor>(); Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300,100,0),FRotator::ZeroRotator);
 const auto InitialInput=Rack->Input;
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f); for(int I=0;I<25;++I) Sim->Tick(.04f);
 const auto Auto=Sim->GetSnapshot(); const int32 Count=Sim->GetActiveCrahUnitCount();
 const double Flow=Sim->GetCrahTotalFlowM3s(),UA=Sim->GetCrahTotalUAWK(),Fan=Sim->GetCrahTotalFanW();
 Sim->EnterLayoutMode(); TArray<FCrahLayoutRecord> Records;
 TestTrue(TEXT("Automatic layout captured"),Sim->CaptureAutomaticCrahLayout(Records));
 TestTrue(TEXT("Planner query preserves warm bridge and geometry caches"),Records.Num()==Count && Count>0
  && Sim->GetActiveCrahUnitCount()==Count && Sim->GetSnapshot().SimTimeS==Auto.SimTimeS
  && Sim->GetSnapshot().EnergyInJ==Auto.EnergyInJ && Sim->GetSnapshot().EnergyStoredJ==Auto.EnergyStoredJ);
 TArray<ACrahUnitActor*> Units;
 for(auto& R:Records) { R.Name=FName(*(TEXT("ConvertedCrah_")+R.Id)); auto* A=VrgCrahLifecycle::Spawn(W,R);
  if(TestNotNull(TEXT("Converted unit"),A)) { Units.Add(A); TestTrue(TEXT("Authored geometry/zero-flow policy retained"),A->CaptureLayoutSettings()==R.Settings
   && A->GetActorTransform().Equals(R.Transform,1e-9) && A->VolumeFlowM3s==0 && A->bRuntimeLayoutCrah); } }
 VrgRackLifecycle::Rebuild(W);
 // Equal authored starting input, including coolant flow used by the existing CDU sizing policy.
 Rack->Input=InitialInput; Sim->EnterSimulationMode(); Sim->Tick(.021f); for(int I=0;I<25;++I) Sim->Tick(.04f);
 const auto Placed=Sim->GetSnapshot();
 const double FlowErr=FMath::Abs(Sim->GetCrahTotalFlowM3s()-Flow)/Flow,UAErr=FMath::Abs(Sim->GetCrahTotalUAWK()-UA)/UA;
 const double FanErr=FMath::Abs(Sim->GetCrahTotalFanW()-Fan)/Fan;
 const double LedgerErr=FMath::Abs(Placed.EnergyStoredJ-Auto.EnergyStoredJ)/FMath::Abs(Auto.EnergyStoredJ);
 TestTrue(TEXT("Conversion retains active ducts and design sizing"),Sim->IsUsingPlacedCrahs() && Sim->GetActiveCrahUnitCount()==Count
  && FlowErr<1e-9 && UAErr<1e-9 && FanErr<1e-9 && LedgerErr<1e-9);
 TestTrue(TEXT("Both global ledgers and pressure converge"),FMath::Abs(Auto.NormalizedEnergyResidual)<1e-9 && FMath::Abs(Placed.NormalizedEnergyResidual)<1e-9
  && Auto.bAirPressureConverged && Placed.bAirPressureConverged);
 Sim->EnterLayoutMode(); TestTrue(TEXT("Remove converted group"),VrgCrahLifecycle::RemoveBatch(Units)); VrgRackLifecycle::Rebuild(W);
 Sim->EnterSimulationMode(); Sim->Tick(.021f); TestTrue(TEXT("Automatic layout returns without duplicates"),!Sim->IsUsingPlacedCrahs() && Sim->GetActiveCrahUnitCount()==Count);
 AddInfo(FString::Printf(TEXT("CRAH conversion: units%d flow%.6fm3/s UA%.6fW/K fan%.6fW; relative flow%.3e UA%.3e fan%.3e storedJ%.3e; global auto%.3e placed%.3e."),
  Count,Flow,UA,Fan,FlowErr,UAErr,FanErr,LedgerErr,Auto.NormalizedEnergyResidual,Placed.NormalizedEnergyResidual));
 W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif
