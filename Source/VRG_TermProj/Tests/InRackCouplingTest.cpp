#include "Misc/AutomationTest.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
struct FCase
{
	FRackThermalModel Rack;
	FRackThermalInput Input;
	FHydraulicNetwork Net;
	FInRowLoopHandles Loop;
	FAirSolver Air;
	FCouplingBridge Bridge;
	FCase(bool Internal = true)
	{
		const auto Params = FRackThermalModel::MakeGb200Nvl72Params();
		Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params); Input.SledDocked.Init(true, Params.Slots.Num());
		Input.CoolantInletTempK = 300.15; Input.AirInletTempK = 297.15; Input.FanSpeedOverride = 1.0;
		FRackThermalState Initial; Initial.CoolantTempK = 300.15;
		Initial.ComponentTempK.Init(300.15, Input.ComponentHeatW.Num()); Initial.SlotFanSpeedRatio.Init(1.0, Params.Slots.Num());
		Rack.Initialize(Params, Initial);
		FInRowLoopSpec Spec; Spec.NumRacks = 1;
		TArray<FInRackSpec> InternalSpecs; if (Internal) InternalSpecs.Add(InRackNetwork::FromThermal(Params, Input));
		Loop = InRowLoopBuilder::AddLoop(Net, Spec, &InternalSpecs); Net.Solve(FHydraulicSolveSettings());
		// Synthetic small air reservoir/strong coil isolates the liquid comparison. These
		// are test boundary conditions, not new equipment properties or production defaults.
		FAirGridParams Grid; Grid.NumCellsX = Grid.NumCellsY = Grid.NumCellsZ = 2; Grid.CellSizeM = .5;
		Air.Initialize(Grid, 297.15);
		FRackCouplingLink Link; Link.BranchPipeIndex = Loop.RackBranchPipeIndices[0]; Link.ReturnPipeIndex = Loop.RackReturnPipeIndices[0];
		Link.Internal = Loop.InRacks[0]; Link.InletCellIndices.Add(0);
		for (int32 C = 0; C < Air.GetGrid().GetNumCells(); ++C) Link.ExhaustCellIndices.Add(C);
		Bridge.Initialize({{&Rack, &Input, nullptr}}, {Link}); Bridge.SetHydraulicNetwork(&Net); Bridge.SetAirSolver(&Air);
		FCduCouplingGroup Group; Group.RackIndices.Add(0);
		// Same controlled HX boundary as the existing coupling fixtures; not equipment defaults.
		Group.Hx.UAWK = 1.1e4; Group.Hx.FacilityMassFlowKgs = 2.3;
		Bridge.SetCduGroups({Group}); Bridge.AirSolverInput.bEnableBuoyancy = false;
		Bridge.AirSolverInput.CellCoolingUAWK.Init(1e4, Air.GetGrid().GetNumCells()); Bridge.AirSolverInput.CoolingSupplyTempK = 297.15;
	}
	void Run(double Seconds, double Dt = .02) { for (int32 S = 0; S < FMath::RoundToInt(Seconds / Dt); ++S) Bridge.Step(Dt, S == 0, true); }
	double ChipK() const
	{
		double Peak = 0.0;
		for (int32 P = 0; P < Rack.GetNumNodes(); ++P)
		{
			const auto K = Rack.GetNodeParams(P).Kind;
			if (K == ERackPartKind::Cpu || K == ERackPartKind::Gpu || K == ERackPartKind::Switch) Peak = FMath::Max(Peak, Rack.GetState().ComponentTempK[P]);
		}
		return Peak;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRackCoupledNormal, "VRG.Coupling.InRack.Normal", Flags)
bool FInRackCoupledNormal::RunTest(const FString& Parameters)
{
	FCase Old(false), New;
	Old.Run(400.0); New.Run(400.0);
	const double Flow = New.Net.GetPipeFlowM3s(New.Loop.RackBranchPipeIndices[0]);
	const double OldFlow = Old.Net.GetPipeFlowM3s(Old.Loop.RackBranchPipeIndices[0]);
	const double Drop = New.Net.GetNodePiezometricPa(New.Loop.SupplyNodeIndices[0]) - New.Net.GetNodePiezometricPa(New.Loop.ReturnNodeIndices[0]);
	const double OldDrop = Old.Net.GetNodePiezometricPa(Old.Loop.SupplyNodeIndices[0]) - Old.Net.GetNodePiezometricPa(Old.Loop.ReturnNodeIndices[0]);
	double Capacity = New.Rack.GetTrayCoolant().GetReturnCapacityJK();
	for (double C : New.Rack.GetTrayCoolant().GetTrayCapacityJK()) Capacity += C;
	AddInfo(FString::Printf(TEXT("normal400s flow old/new %.12g/%.12g m3/s; drop %.12g/%.12g Pa; outlet %.12g/%.12g K; chip %.12g/%.12g K; global %.12g; mass %.12g kg/s; capacity %.12g J/K; delta %.12g K; pump/IT %.12g"),
		OldFlow, Flow, OldDrop, Drop, Old.Rack.GetState().CoolantTempK, New.Rack.GetState().CoolantTempK,
		Old.ChipK(), New.ChipK(), New.Bridge.GetNormalizedEnergyResidual(), New.Rack.GetTrayCoolant().GetLastMassResidualKgs(), Capacity,
		New.Rack.GetState().CoolantTempK - New.Input.CoolantInletTempK, New.Net.GetPumpElectricalInputPowerW(0) / New.Bridge.GetLastItHeatW()));
	TestEqual(TEXT("no duplicate water capacity"), Capacity, New.Rack.GetParams().CoolantHeatCapacityJK);
	TestTrue(TEXT("normal global conservation, existing criterion"), FMath::Abs(New.Bridge.GetNormalizedEnergyResidual()) < 1e-9);
	TestTrue(TEXT("chip plausibility"), New.ChipK() > 273.15 && New.ChipK() < 358.15);
	TestTrue(TEXT("coolant rise plausibility"), New.Rack.GetState().CoolantTempK - New.Input.CoolantInletTempK >= 5.0 && New.Rack.GetState().CoolantTempK - New.Input.CoolantInletTempK <= 15.0);
	TestTrue(TEXT("pump power plausibility"), New.Net.GetPumpElectricalInputPowerW(0) / New.Bridge.GetLastItHeatW() < .05);
	TestTrue(TEXT("unchanged IT"), FMath::Abs(New.Bridge.GetLastItHeatW() - Old.Bridge.GetLastItHeatW()) < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRackCoupledLeak, "VRG.Coupling.InRack.LeaksAndLoss", Flags)
bool FInRackCoupledLeak::RunTest(const FString& Parameters)
{
	FCase C; C.Run(50.0);
	const auto& H = C.Loop.InRacks[0];
	double MaxGlobal = 0.0, MaxMass = 0.0, MaxClassErrorW = 0.0;
	double RackBalance = 0.0, TrayBalance = 0.0, ManifoldBalance = 0.0;
	for (int32 Kind = 0; Kind < 5; ++Kind)
	{
		const int32 L = H.Leaks[5 * 13 + Kind]; C.Net.SetLeakHoleDiameterM(L, .003);
		TestTrue(TEXT("local leak convergence"), C.Net.Solve(FHydraulicSolveSettings()).bConverged);
		const double In = C.Net.GetPipeFlowM3s(H.InletPipes[13]), Out = C.Net.GetPipeFlowM3s(H.OutletPipes[13]);
		if (Kind == 2) TestTrue(TEXT("actual reverse outlet is not clipped"), Out < 0.0);
		C.Run(1.0);
		double Leaks = 0.0; for (int32 Leak : H.Leaks) Leaks += C.Net.GetLeakFlowM3s(Leak);
		RackBalance = FMath::Max(RackBalance, FMath::Abs(C.Net.GetPipeFlowM3s(C.Loop.RackBranchPipeIndices[0])
			- C.Net.GetPipeFlowM3s(C.Loop.RackReturnPipeIndices[0]) - Leaks));
		for (int32 T = 0; T < H.Slots.Num(); ++T)
		{
			double TrayLeak = 0.0; for (int32 K = 1; K <= 3; ++K) TrayLeak += C.Net.GetLeakFlowM3s(H.Leaks[5 * T + K]);
			TrayBalance = FMath::Max(TrayBalance, FMath::Abs(C.Net.GetPipeFlowM3s(H.InletPipes[T]) - C.Net.GetPipeFlowM3s(H.OutletPipes[T]) - TrayLeak));
			for (int32 K : {0, 4}) ManifoldBalance = FMath::Max(ManifoldBalance, FMath::Abs(C.Net.GetNodeImbalanceM3s(H.Nodes[5 * T + K])));
		}
		const double Mass = C.Rack.GetTrayCoolant().GetLastMassResidualKgs(); MaxMass = FMath::Max(MaxMass, Mass);
		MaxGlobal = FMath::Max(MaxGlobal, FMath::Abs(C.Bridge.GetNormalizedEnergyResidual()));
		double ClassW = 0.0; for (double W : C.Bridge.GetLastInternalLeakHeatW()) ClassW += W;
		MaxClassErrorW = FMath::Max(MaxClassErrorW, FMath::Abs(ClassW - C.Bridge.GetLastLeakHeatW()));
		AddInfo(FString::Printf(TEXT("site%d inlet %.12g outlet %.12g m3/s; local leak heat %.12g W; mass %.12g kg/s; global %.12g"), Kind, In, Out, ClassW, Mass, C.Bridge.GetNormalizedEnergyResidual()));
		TestTrue(TEXT("only intended leak classification"), C.Bridge.GetLastInternalLeakKgs()[Kind < 2 ? 0 : Kind == 2 ? 1 : 2] > 0.0);
		C.Net.SetLeakHoleDiameterM(L, 0.0); C.Net.Solve(FHydraulicSolveSettings());
	}
	// Cooling loss by closing the actual rack isolation valves (no temperature forcing).
	C.Net.SetPipeClosed(C.Loop.RackBranchPipeIndices[0], true); C.Net.SetPipeClosed(C.Loop.RackReturnPipeIndices[0], true);
	TestTrue(TEXT("isolated hydraulic convergence"), C.Net.Solve(FHydraulicSolveSettings()).bConverged);
	const double BeforeK = C.ChipK(); C.Run(2.0);
	TestTrue(TEXT("cooling loss calculated chip response"), C.ChipK() > BeforeK);
	TestEqual(TEXT("isolated branch flow"), C.Input.CoolantMassFlowKgs, 0.0);
	// A large interval must subcycle both HX and rack using the same old state.
	const double LargeIntervalS = 2.0 * C.Rack.LimitTrayStepS(C.Input, C.Bridge.GetTrayInputs()[0], 1000.0);
	C.Bridge.Step(LargeIntervalS, false, true);
	TestTrue(TEXT("shared thermal substeps"), C.Bridge.GetLastCoupledSubsteps() > 1);
	MaxGlobal = FMath::Max(MaxGlobal, FMath::Abs(C.Bridge.GetNormalizedEnergyResidual()));
	AddInfo(FString::Printf(TEXT("leak/loss maxima global %.12g; mass %.12g kg/s; class single-book difference %.12g W; loss chip %.12g -> %.12g K; common substeps %d"),
		MaxGlobal, MaxMass, MaxClassErrorW, BeforeK, C.ChipK(), C.Bridge.GetLastCoupledSubsteps()));
	TestTrue(TEXT("existing global conservation"), MaxGlobal < 1e-9);
	TestTrue(TEXT("single booked local leak heat, measured then bounded"), MaxClassErrorW < 1e-9);
	TestTrue(TEXT("solved mass residual uses existing hydraulic volume bound x density"), MaxMass < C.Net.GetFluidDensityKgM3() * 1e-12);
	AddInfo(FString::Printf(TEXT("rack supply-return-allLeaks %.12g; tray inlet-outlet-localLeaks %.12g; manifold tee %.12g m3/s"), RackBalance, TrayBalance, ManifoldBalance));
	TestTrue(TEXT("separate rack/tray/manifold conservation, existing hydraulic bound"), RackBalance < 1e-12 && TrayBalance < 1e-12 && ManifoldBalance < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInRackTraversalCoupled,"VRG.Coupling.InRack.TraversalEquivalence",Flags)
bool FInRackTraversalCoupled::RunTest(const FString& Parameters)
{
 FCase Old,New;Old.Rack.SetGroupedStabilityForTesting(false);Old.Bridge.SetIndexedTransportForTesting(false);
 New.Rack.SetGroupedStabilityForTesting(true);New.Bridge.SetIndexedTransportForTesting(true);
 double MaxK=0,MaxHeatW=0,MaxGlobal=0,MaxPressurePa=0,MaxFlowM3s=0;
 for(int32 S=0;S<500;++S) {
  for(FCase* C:{&Old,&New}) {
   const auto& H=C->Loop.InRacks[0];bool Changed=false;
   if(S==20) {C->Net.SetLeakHoleDiameterM(H.Leaks[13*5+2],.003);Changed=true;}
   if(S==150 || S==250) {const bool Docked=S==250;H.SetDocked(C->Net,4,Docked);C->Input.SledDocked[H.Slots[4]]=Docked;Changed=true;}
   if(S==350) {C->Net.SetLeakHoleDiameterM(H.Leaks[13*5+2],0);Changed=true;}
   if(Changed) TestTrue(TEXT("updated hydraulic input converges"),C->Net.Solve(FHydraulicSolveSettings()).bConverged);
   if(S==400) C->Input.FanSpeedOverride=.5;
   C->Bridge.Step(.02,Changed,true);
   MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(C->Bridge.GetNormalizedEnergyResidual()));
  }
  for(int32 N=0;N<Old.Rack.GetNumNodes();++N) MaxK=FMath::Max(MaxK,FMath::Abs(Old.Rack.GetState().ComponentTempK[N]-New.Rack.GetState().ComponentTempK[N]));
  MaxK=FMath::Max(MaxK,FMath::Abs(Old.Rack.GetState().CoolantTempK-New.Rack.GetState().CoolantTempK));
  MaxHeatW=FMath::Max(MaxHeatW,FMath::Abs(Old.Bridge.GetLastLeakHeatW()-New.Bridge.GetLastLeakHeatW()));
  MaxHeatW=FMath::Max(MaxHeatW,FMath::Abs(Old.Rack.GetLastHeatRemovedByCoolantW()-New.Rack.GetLastHeatRemovedByCoolantW()));
  for(int32 N=0;N<Old.Net.GetNumNodes();++N) MaxPressurePa=FMath::Max(MaxPressurePa,FMath::Abs(Old.Net.GetNodePressurePa(N)-New.Net.GetNodePressurePa(N)));
  for(int32 P=0;P<Old.Net.GetNumPipes();++P) MaxFlowM3s=FMath::Max(MaxFlowM3s,FMath::Abs(Old.Net.GetPipeFlowM3s(P)-New.Net.GetPipeFlowM3s(P)));
  TestEqual(TEXT("same substep policy"),Old.Bridge.GetLastCoupledSubsteps(),New.Bridge.GetLastCoupledSubsteps());
 }
 AddInfo(FString::Printf(TEXT("traversal 10s pressure diff %.12g Pa flow %.12g m3/s temperature %.12g K heat %.12g W global %.12g"),MaxPressurePa,MaxFlowM3s,MaxK,MaxHeatW,MaxGlobal));
 TestTrue(TEXT("same input exact ordered physics"),MaxK==0 && MaxHeatW==0 && MaxPressurePa==0 && MaxFlowM3s==0);
 TestTrue(TEXT("existing normalized global conservation"),MaxGlobal<1e-9);
 TestTrue(TEXT("existing temperature plausibility"),New.ChipK()>273.15 && New.ChipK()<358.15);
 return true;
}
#endif
