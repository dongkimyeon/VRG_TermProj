#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/OctreeAirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	FAirGridParams Params()
	{
		FAirGridParams P; P.NumCellsX = 6; P.NumCellsY = 4; P.NumCellsZ = 4; P.CellSizeM = .25; return P;
	}
	TArray<FBox> Boxes() { return {FBox(FVector(.2,.2,.2), FVector(.7,.6,.6))}; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeUniformStepTest, "VRG.Air.Octree.Step.UniformHeatReference", Flags)
bool FOctreeUniformStepTest::RunTest(const FString& Parameters)
{
	FAirSolver Uniform, Tree; Uniform.Initialize(Params(), 297.15); Tree.Initialize(Params(), 297.15);
	FFixedOctreeSettings Settings; Settings.MaxDepth = 0; FString Error;
	TestTrue(TEXT("Configure"), Tree.ConfigureOctree(Settings, {}, Error));
	FAirSolverInput Input; Input.bEnableBuoyancy = false;
	Input.CellHeatW.Init(12.5, Uniform.GetGrid().GetNumCells());
	Input.CellCoolingUAWK.Init(.5, Uniform.GetGrid().GetNumCells()); Input.CoolingSupplyTempK = 297.15;
	for (int32 I = 0; I < 200; ++I) { Uniform.Step(Input,.01); Tree.Step(Input,.01); }
	double MaxErrorK = 0;
	for (int32 I = 0; I < Uniform.GetGrid().GetNumCells(); ++I)
		MaxErrorK = FMath::Max(MaxErrorK, FMath::Abs(Uniform.GetGrid().TemperatureK[I] - Tree.GetGrid().TemperatureK[I]));
	AddInfo(FString::Printf(TEXT("Uniform heat/cooling step max difference %.12e K; ledger %.12e / input %.3f J"), MaxErrorK, Tree.GetNormalizedEnergyResidual(), Tree.GetEnergyInJ()));
	TestTrue(TEXT("Pressure converged"), Tree.GetLastPressureResult().bConverged);
	// Measured 0 K and 1.49e-12 dimensionless; allow rounding/platform margin.
	TestTrue(TEXT("Uniform reference temperature [K]"), MaxErrorK < 1e-11);
	TestTrue(TEXT("Heat/cooling ledger / 2400 J input"), FMath::Abs(Tree.GetNormalizedEnergyResidual()) < 1e-10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeProjectionTest, "VRG.Air.Octree.Step.ProjectionAndRegrid", Flags)
bool FOctreeProjectionTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Source; Source.Initialize(Params()); Source.ResetToUniform(297.15);
	for (int32 I = 0; I < Source.GetNumCells(); ++I) Source.TemperatureK[I] += .125 * (I % 7);
	FOctreeAirSolver S; FFixedOctreeSettings Settings; Settings.MaxDepth = 2; FString Error;
	if (!TestTrue(TEXT("Build"), S.Initialize(Source, Settings, Boxes(), Error))) return false;
	S.PressureSettings.NormalizedResidualTarget = 1e-9; S.PressureSettings.MaxIterations = 500;
	// Discrete manufactured gradient: projection must remove it, including every hanging face.
	const auto& G = S.GetGrid();
	auto Potential = [&](int32 I) { const FVector C = G.LeafCenterM(I); return FMath::Cos(PI*C.X/1.5) * FMath::Cos(PI*C.Y); };
	for (int32 I = 0; I < G.GetFaces().Num(); ++I)
	{
		const auto& F = G.GetFaces()[I];
		if (F.LeafLow != INDEX_NONE && F.LeafHigh != INDEX_NONE)
			S.GetMutableFaceVelocityMs()[I] = (Potential(F.LeafHigh) - Potential(F.LeafLow)) / F.DistanceM;
	}
	const double Before = S.MaxDivergencePerS();
	TestTrue(TEXT("MG projection converges"), S.Project(.02).bConverged);
	double MaxVelocity = 0; for (double V : S.GetFaceVelocityMs()) MaxVelocity = FMath::Max(MaxVelocity, FMath::Abs(V));
	AddInfo(FString::Printf(TEXT("Manufactured gradient: max velocity %.12e m/s; divergence ratio %.12e / initial %.6f 1/s"), MaxVelocity, S.MaxDivergencePerS()/Before, Before));
	TestTrue(TEXT("Gradient removed [m/s], divergence / original divergence"), MaxVelocity < 1e-8 && S.MaxDivergencePerS()/Before < 1e-8);
	const double Energy = G.TotalThermalEnergyJ(297.15);
	const int32 Refined = G.GetNumLeaves();
	TestTrue(TEXT("Runtime merge"), S.Adapt({}, .02, Error));
	TestEqual(TEXT("All roots restored"), S.GetGrid().GetNumLeaves(), Source.GetNumCells());
	TestTrue(TEXT("Runtime split"), S.Adapt(Boxes(), .02, Error));
	TestEqual(TEXT("Original topology rebuilt"), S.GetGrid().GetNumLeaves(), Refined);
	const double TransferError = FMath::Abs(S.GetGrid().TotalThermalEnergyJ(297.15) - Energy) / Energy;
	AddInfo(FString::Printf(TEXT("Split/merge heat error %.12e / initial sensible heat %.9f J; final divergence %.12e 1/s"), TransferError, Energy, S.MaxDivergencePerS()));
	TestTrue(TEXT("Conservative split/merge / initial sensible heat"), TransferError < 1e-11);
	TestTrue(TEXT("Reprojected divergence [1/s]"), S.MaxDivergencePerS() < 1e-8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePlumeTest, "VRG.Air.Octree.Step.BuoyancyHeatAndSolids", Flags)
bool FOctreePlumeTest::RunTest(const FString& Parameters)
{
	FAirSolver Air; Air.Initialize(Params(), 297.15);
	Air.GetMutableGrid().bSolidCell[Air.GetGrid().CellIndex(0,0,0)] = true; Air.OnSolidCellsChanged();
	FFixedOctreeSettings Settings; Settings.MaxDepth = 1; FString Error;
	if (!TestTrue(TEXT("Configure"), Air.ConfigureOctree(Settings, Boxes(), Error))) return false;
	Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-8;
	FAirSolverInput Input; Input.CellHeatW.Init(0, Air.GetGrid().GetNumCells());
	Input.CellHeatW[Air.GetGrid().CellIndex(2,2,0)] = 25.0;
	for (int32 I = 0; I < 300; ++I)
	{
		Air.Step(Input,.01);
		if (I == 100) TestTrue(TEXT("Merge during heat integration"), Air.AdaptOctree({}, .01, Error));
		if (I == 200) TestTrue(TEXT("Split during heat integration"), Air.AdaptOctree(Boxes(), .01, Error));
	}
	const auto Snap = Air.MakeSnapshot(3);
	AddInfo(FString::Printf(TEXT("Plume: ledger %.12e / input %.3f J, divergence %.12e (max u/min h), Tmin %.9f K Tmax %.9f K, speed %.9f m/s, CFL %.9f"),
		Snap.NormalizedEnergyResidual, Air.GetEnergyInJ(), Snap.NormalizedDivergence, Snap.MinTemperatureK, Snap.MaxTemperatureK, Snap.MaxSpeedMs, Snap.CourantNumber));
	TestEqual(TEXT("All pressure solves converge"), Air.GetNumPressureFailures(), int64(0));
	TestTrue(TEXT("Plume ledger / 75 J and divergence / max(u/h)"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-10 && Snap.NormalizedDivergence < 1e-8);
	TestTrue(TEXT("Plausible low-power plume and CFL stability"), Snap.MaxTemperatureK > 297.15 && Snap.MaxTemperatureK < 330 && Snap.MaxSpeedMs > .001 && Snap.MaxSpeedMs < 2 && Snap.CourantNumber <= .5 + 1e-12);
	TestEqual(TEXT("Solid remains at initial temperature"), Air.GetGrid().TemperatureK[0], 297.15);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeAdaptFailureTest, "VRG.Air.Octree.Step.RegridFailureIsAtomic", Flags)
bool FOctreeAdaptFailureTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid G; G.Initialize(Params()); G.ResetToUniform(300.0);
	FOctreeAirSolver S; FFixedOctreeSettings Settings; Settings.MaxDepth=2; Settings.MaxLeaves=G.GetNumCells()+7;
	FString Error; TestTrue(TEXT("Coarse grid"), S.Initialize(G,Settings,{},Error));
	const double Energy=S.GetGrid().TotalThermalEnergyJ(297.15);
	TestFalse(TEXT("Insufficient budget rejects refinement"),S.Adapt(Boxes(),.02,Error));
	TestEqual(TEXT("Old topology retained"),S.GetGrid().GetNumLeaves(),G.GetNumCells());
	TestEqual(TEXT("Old energy retained"),S.GetGrid().TotalThermalEnergyJ(297.15),Energy);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeMgCompatibilityTest, "VRG.Air.Octree.Step.VolumeWeightedCompatibility", Flags)
bool FOctreeMgCompatibilityTest::RunTest(const FString& Parameters)
{
	FMultigridPoisson S; FMultigridPoisson::FLevelInput L;
	L.Volume={1,8}; L.Centre={FVector(0),FVector(1,0,0)};
	L.BoxMin={FIntVector(0),FIntVector(1,0,0)}; L.BoxSize={1,2};
	L.RowStart={0,1,2}; L.Col={1,0}; L.Trans={1,1}; S.Build(MoveTemp(L));
	TArray<double> X; FPoissonSolveSettings Settings;
	const auto Good=S.Solve({8,-1},X,Settings);
	TestTrue(TEXT("Volume-balanced RHS is compatible"),Good.bConverged && !Good.bIncompatibleRightHandSide);
	const auto Bad=S.Solve({1,1},X,Settings);
	TestTrue(TEXT("Nonzero integral must report failure"),!Bad.bConverged && Bad.bIncompatibleRightHandSide);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeCoolingAnalyticTest, "VRG.Air.Octree.Step.CoolingAnalyticTimeRefinement", Flags)
bool FOctreeCoolingAnalyticTest::RunTest(const FString& Parameters)
{
	const auto P = Params();
	const double Capacity = P.DensityKgM3 * P.SpecificHeatJKgK * FMath::Pow(P.CellSizeM,3);
	// Independent lumped ODE: C*dT/dt=12.5-0.5*(T-297.15), T(0)=297.15 K.
	const double Exact = 297.15 + 25.0 * (1.0-FMath::Exp(-.5*2.0/Capacity));
	auto Run = [&](double Dt)
	{
		FAirSolver S; S.Initialize(P,297.15); FFixedOctreeSettings Settings; FString Error;
		S.ConfigureOctree(Settings,Boxes(),Error);
		FAirSolverInput In; In.bEnableBuoyancy=false; In.CellHeatW.Init(12.5,S.GetGrid().GetNumCells());
		In.CellCoolingUAWK.Init(.5,S.GetGrid().GetNumCells()); In.CoolingSupplyTempK=297.15;
		for(int32 I=0;I<FMath::RoundToInt32(2.0/Dt);++I) S.Step(In,Dt);
		return FMath::Abs(S.GetGrid().TemperatureK[0]-Exact);
	};
	const double Coarse=Run(.04), Fine=Run(.02);
	AddInfo(FString::Printf(TEXT("Cooling ODE errors %.12e / %.12e K; refinement ratio %.9f; analytic %.9f K"),Coarse,Fine,Coarse/Fine,Exact));
	TestTrue(TEXT("Refining time improves analytic agreement"),Fine<Coarse);
	// Measured 3.615e-4 K at dt=.02 s and ratio 2.00035 (first-order splitting/Euler).
	TestTrue(TEXT("Measured ODE error margin [K] and first-order convergence"),Fine<5e-4 && Coarse/Fine>1.9 && Coarse/Fine<2.1);
	return true;
}
#endif
