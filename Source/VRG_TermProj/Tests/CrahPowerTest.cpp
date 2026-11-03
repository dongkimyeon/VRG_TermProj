#include "Misc/AutomationTest.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Air/AirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrahElectricalPower,"VRG.Coupling.CrahElectricalPower",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FCrahElectricalPower::RunTest(const FString& Parameters)
{
 // Synthetic duct fixture from the existing duct conservation tests, not CRAH catalog ratings.
 FAirGridParams Params;Params.NumCellsX=12;Params.NumCellsY=6;Params.NumCellsZ=6;Params.CellSizeM=.25;
 FAirSolver Air;Air.Initialize(Params,297.15);auto& Grid=Air.GetMutableGrid();
 for(int32 Z=1;Z<=3;++Z) for(int32 Y=2;Y<=3;++Y) for(int32 X=5;X<=6;++X) Grid.bSolidCell[Grid.CellIndex(X,Y,Z)]=true;
 Air.OnSolidCellsChanged();const auto Solid=Grid.bSolidCell;
 FCrahCouplingUnit Unit;Unit.VolumeFlowM3s=.02;Unit.CoilUAWK=20;Unit.FanPowerW=30;Unit.WaterSupplyTempK=291.15;
 for(int32 Z=1;Z<=3;++Z) for(int32 Y=2;Y<=3;++Y) {
  Unit.FaceAxis.Append({0,0});Unit.FaceIndex.Append({Grid.FaceXIndex(7,Y,Z),Grid.FaceXIndex(5,Y,Z)});
  Unit.FaceFluidCell.Append({Grid.CellIndex(7,Y,Z),Grid.CellIndex(4,Y,Z)});Unit.FaceIsExhaust.Append({0,1});Unit.FaceSign.Append({-1,-1});
 }
 FCouplingBridge Bridge;Bridge.Initialize({},{});Bridge.SetAirSolver(&Air);Bridge.SetCrahUnits({Unit});
 const auto Faces=Bridge.AirSolverInput.DuctFaceIndex;const int32 Ducts=Bridge.AirSolverInput.NumDucts;
 double MaxEnergy=0,MaxPressure=0,OnCoil=0;
 for(int32 Phase=0;Phase<3;++Phase) {
  if(Phase==1) {TestTrue(TEXT("Explicit electrical command"),Bridge.SetCrahPowered(0,false));Bridge.SetCrahCoil(0,40,292.15);}
  if(Phase==2) TestTrue(TEXT("Explicit re-energization"),Bridge.SetCrahPowered(0,true));
  for(int32 Step=0;Step<30;++Step) {
   Bridge.Step(.02,false,true);const auto S=Air.MakeSnapshot((Phase*30+Step+1)*.02);
   TestTrue(TEXT("Pressure converges during on/off transition"),S.bPressureConverged);MaxPressure=FMath::Max(MaxPressure,S.NormalizedDivergence);
   MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(Bridge.GetNormalizedEnergyResidual()));
   if(Phase==1) {
    TestEqual(TEXT("Stopped fan electric input absent from ledger"),Air.GetLastDuctFanHeatW(),0.0);
    TestEqual(TEXT("Stopped coil removes no heat"),Air.GetLastDuctCoilRemovedW(),0.0);
    for(double V:Bridge.AirSolverInput.DuctFaceVelocityMs) TestEqual(TEXT("Active fan command zero on preserved faces"),V,0.0);
    TestEqual(TEXT("Plant coil update cannot reactivate stopped unit"),Bridge.AirSolverInput.DuctCoilUAWK[0],0.0);
    const auto& In=Bridge.AirSolverInput;for(int32 F=0;F<In.DuctFaceIndex.Num();++F) {
     const int32 I=In.DuctFaceIndex[F];const double V=In.DuctFaceAxis[F]==0?Grid.VelocityXMs[I]:In.DuctFaceAxis[F]==1?Grid.VelocityYMs[I]:Grid.VelocityZMs[I];
     TestEqual(TEXT("Actual MAC face velocity follows stopped fan"),V,0.0);
    }
   } else {TestTrue(TEXT("On fan electric heat follows original input"),FMath::Abs(Air.GetLastDuctFanHeatW()-Unit.FanPowerW)<1e-9);OnCoil=FMath::Max(OnCoil,Air.GetLastDuctCoilRemovedW());}
  }
 }
 TestTrue(TEXT("Powered coil actually exchanges heat"),OnCoil>0);
 TestTrue(TEXT("Body/faces/duct identity retained"),Solid==Grid.bSolidCell && Faces==Bridge.AirSolverInput.DuctFaceIndex && Ducts==Bridge.AirSolverInput.NumDucts && Bridge.GetNumCrahUnits()==1);
 TestEqual(TEXT("Nominal airflow is never overwritten"),Bridge.GetCrahUnits()[0].VolumeFlowM3s,Unit.VolumeFlowM3s);
 TestEqual(TEXT("Nominal fan power is never overwritten"),Bridge.GetCrahUnits()[0].FanPowerW,Unit.FanPowerW);
 TestEqual(TEXT("Restarting uses current coil setting"),Bridge.AirSolverInput.DuctCoilUAWK[0],40.0);
 TestFalse(TEXT("Invalid unit rejected"),Bridge.SetCrahPowered(1,false));
 TestTrue(TEXT("Existing energy conservation criterion"),MaxEnergy<1e-9);
 AddInfo(FString::Printf(TEXT("W4 CRAH electrical max normalized energy=%.17g max divergence=%.17g; on fan=30 W off fan/coil=0 W peak on coil=%.17g W; body/faces/ducts unchanged"),MaxEnergy,MaxPressure,OnCoil));
 return true;
}
#endif
