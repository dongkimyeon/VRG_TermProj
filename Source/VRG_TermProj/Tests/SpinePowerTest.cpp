#include "Misc/AutomationTest.h"
#include "Actors/SpineCabinetActor.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpineElectricalPowerTest, "VRG.Coupling.SpineElectricalPower",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSpineElectricalPowerTest::RunTest(const FString& Parameters)
{
 UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
 GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Spine = World->SpawnActor<ASpineCabinetActor>();
 auto* Other = World->SpawnActor<ASpineCabinetActor>();
 Spine->EnsureThermalModel(); Other->EnsureThermalModel();
 TestTrue(TEXT("Native cabinets default powered"), Spine->IsPowered() && Other->IsPowered());
 const auto Settings = Spine->CaptureLayoutSettings(); const auto Transform = Spine->GetActorTransform();
 auto& Model = Spine->GetMutableModel(); auto& Input = Spine->GetMutableInput();
 const auto Params = Model.GetParams(); const auto HeatInput = Input.ComponentHeatW;
 Input.FanSpeedOverride = 1.0; Other->GetMutableInput().FanSpeedOverride = 1.0;
 // Synthetic warm initial state for residual-cooling verification, not an operating failure threshold.
 FRackThermalState Warm; Warm.ComponentTempK.Init(330.15, Model.GetNumNodes());
 Model.Initialize(Params, Warm); Other->GetMutableModel().Initialize(Params, Warm);
 FRackThermalModel Legacy; Legacy.Initialize(Params, Warm); auto LegacyInput = Input; LegacyInput.SlotPowered.Reset();
 Model.Step(Input, .02); Legacy.Step(LegacyInput, .02);
 TestTrue(TEXT("Explicit default ON preserves legacy temperatures/ledger exactly"),
  Model.GetState().ComponentTempK == Legacy.GetState().ComponentTempK && Model.GetEnergyInJ() == Legacy.GetEnergyInJ());
 Model.Initialize(Params, Warm);
 FAirGridParams GridParams; GridParams.NumCellsX=10; GridParams.NumCellsY=6; GridParams.NumCellsZ=6; GridParams.CellSizeM=.25;
 FAirSolver Air; Air.Initialize(GridParams, 297.15); auto& Grid=Air.GetMutableGrid();
 for(int32 Z=1;Z<=3;++Z) for(int32 Y=2;Y<=3;++Y) for(int32 X=4;X<=5;++X) Grid.bSolidCell[Grid.CellIndex(X,Y,Z)]=true;
 Air.OnSolidCellsChanged(); const auto Solids=Grid.bSolidCell;
 FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs=0;
 for(int32 Z=1;Z<=3;++Z) for(int32 Y=2;Y<=3;++Y) {
  Link.FanFaceAxis.Append({0,0}); Link.FanFaceIndex.Append({Grid.FaceXIndex(6,Y,Z),Grid.FaceXIndex(4,Y,Z)});
  Link.FanFaceFluidCell.Append({Grid.CellIndex(6,Y,Z),Grid.CellIndex(3,Y,Z)});
  Link.FanFaceLayer.Append({0,0}); Link.FanFaceIsExhaust.Append({0,1}); Link.FanFaceSign.Append({-1,-1});
  Link.InletCellIndices.Add(Grid.CellIndex(6,Y,Z)); Link.ExhaustCellIndices.Add(Grid.CellIndex(3,Y,Z));
 }
 TArray<double> Bottom, Height;
 for(const auto& Slot:Params.Slots) { Bottom.Add(Slot.BottomZM); Height.Add(Slot.HeightM); }
 FCouplingBridge::AssignSlotsToFanLayers(Link, {1.0}, 3.0, Bottom, Height);
 FRackCouplingLink OtherLink; OtherLink.FallbackCoolantMassFlowKgs=0;
 OtherLink.InletCellIndices={0}; OtherLink.ExhaustCellIndices={1};
 FCouplingBridge Bridge; Bridge.Initialize({{&Model,&Input,nullptr},{&Other->GetMutableModel(),&Other->GetMutableInput(),nullptr}}, {Link,OtherLink}); Bridge.SetAirSolver(&Air);
 const auto Faces=Bridge.AirSolverInput.DuctFaceIndex;
 double MaxEnergy=0, MaxDivergence=0, OnHeat=0, OtherHeat=0, ResidualHeat=0;
 for(int32 Phase=0;Phase<2;++Phase) {
  if(Phase==1) {
   const auto Before=Model.GetState().ComponentTempK; const double Stored=Model.GetEnergyStoredJ(); Spine->SetPowered(false);
   TestTrue(TEXT("Electrical change preserves temperature and stored energy"),Before==Model.GetState().ComponentTempK && Stored==Model.GetEnergyStoredJ());
  }
  for(int32 Step=0;Step<20;++Step) {
   const auto Before=Model.GetState().ComponentTempK; Bridge.Step(.02,false,true);
   const auto S=Model.MakeSnapshot(0); const auto A=Air.MakeSnapshot(0);
   MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(Bridge.GetNormalizedEnergyResidual())); MaxDivergence=FMath::Max(MaxDivergence,A.NormalizedDivergence);
   TestTrue(TEXT("Air pressure converges"),A.bPressureConverged);
   TestTrue(TEXT("Thermal step stable and finite"),S.bStepStable && FMath::IsFinite(S.ComponentTempK[0]) && S.ComponentTempK[0]>297.15 && S.ComponentTempK[0]<331.15);
   if(Phase==0) { OnHeat=S.TotalHeatW; OtherHeat=Other->GetMutableModel().GetLastTotalHeatW(); TestTrue(TEXT("ON has fan power and flow"),S.TotalFanPowerW>0 && S.SlotAirMassFlowKgs[0]>0); }
   else {
    TestEqual(TEXT("OFF electronics and fans contribute no electric heat"),S.TotalHeatW,0.0);
    TestEqual(TEXT("OFF fan power zero despite override"),S.TotalFanPowerW,0.0);
    for(double Flow:S.SlotAirMassFlowKgs) TestEqual(TEXT("OFF forced airflow zero"),Flow,0.0);
    for(double V:Bridge.AirSolverInput.DuctFaceVelocityMs) TestEqual(TEXT("Preserved duct fan command zero"),V,0.0);
    for(int32 F=0;F<Faces.Num();++F) TestEqual(TEXT("Actual MAC fan face velocity zero"),Grid.VelocityXMs[Faces[F]],0.0);
    ResidualHeat=S.TotalHeatToAmbientW; TestTrue(TEXT("Residual natural cooling continues"),ResidualHeat>0 && S.ComponentTempK[0]<Before[0]);
    TestEqual(TEXT("Other cabinet electric heat unaffected"),Other->GetMutableModel().GetLastTotalHeatW(),OtherHeat);
    TestTrue(TEXT("Other cabinet remains powered"),Other->IsPowered());
   }
  }
 }
 TestTrue(TEXT("Settings, geometry, input heat, solid cells and duct identities preserved"),Settings==Spine->CaptureLayoutSettings() && Transform.Equals(Spine->GetActorTransform()) && HeatInput==Input.ComponentHeatW && Solids==Grid.bSolidCell && Faces==Bridge.AirSolverInput.DuctFaceIndex);
 TestTrue(TEXT("Existing global energy criterion"),MaxEnergy<1e-9);
 const auto OffTemps=Model.GetState().ComponentTempK; Spine->EnsureThermalModel();
 TestTrue(TEXT("Ensure does not reset power or thermal state"),!Spine->IsPowered() && OffTemps==Model.GetState().ComponentTempK);
 Spine->SetPowered(true); Model.Step(Input,.02); TestTrue(TEXT("Explicit power restoration reuses unchanged settings"),Model.GetLastTotalHeatW()>0);
 AddInfo(FString::Printf(TEXT("Spine power: legacy delta=0 K/0 J; ON=%.17g W; OFF electric/fan/flow=0; other=%.17g W; OFF residual ambient=%.17g W; max global normalized energy=%.17g (cumulative input J); max normalized divergence=%.17g"),OnHeat,OtherHeat,ResidualHeat,MaxEnergy,MaxDivergence));
 GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return true;
}
#endif
