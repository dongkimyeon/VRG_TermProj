#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/ElapsedTimer.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/MultigridPoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags MgFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams GridParams(int32 X, int32 Y, int32 Z, double H)
	{
		FAirGridParams P; P.NumCellsX = X; P.NumCellsY = Y; P.NumCellsZ = Z; P.CellSizeM = H; return P;
	}

	// Rack-row like obstacles: two rows of solid boxes with a gap, leaving the front/back layers open.
	void AddRackRows(FUniformAirGrid& Grid)
	{
		const int32 NX = Grid.GetNumCellsX(), NY = Grid.GetNumCellsY(), NZ = Grid.GetNumCellsZ();
		for (int32 Z = 0; Z < NZ * 3 / 5; ++Z)
			for (int32 Y = NY / 6; Y < NY * 5 / 6; ++Y)
				for (int32 X = 0; X < NX; ++X)
				{
					const bool bRowA = X >= NX / 5 && X < NX / 5 + 3, bRowB = X >= NX * 4 / 5 - 3 && X < NX * 4 / 5;
					if ((bRowA || bRowB) && (Y % 4) != 3) Grid.bSolidCell[Grid.CellIndex(X, Y, Z)] = true;
				}
	}

	// Manufactured right-hand side B = L(Xref) with the reference S3 operator: always compatible.
	void Manufacture(const FUniformAirGrid& Grid, TArray<double>& Ref, TArray<double>& B)
	{
		Ref.Init(0.0, Grid.GetNumCells()); B.Init(0.0, Grid.GetNumCells());
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) Ref[Grid.CellIndex(X, Y, Z)] = FMath::Sin(.37 * X + .11 * Y) * FMath::Cos(.23 * Z - .5 * Y) + .01 * X * Z;
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z) for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y) for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			if (Grid.IsFluidCell(X, Y, Z)) B[Grid.CellIndex(X, Y, Z)] = JacobiPoissonSolver::LaplacianAt(Grid, Ref, X, Y, Z);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMultigridPoissonTest, "VRG.Air.Poisson.MultigridSolidsAndScaling", MgFlags)
bool FMultigridPoissonTest::RunTest(const FString& Parameters)
{
	FPoissonSolveSettings S; S.NormalizedResidualTarget = 1.0e-10; S.MaxIterations = 200;

	// 1) Analytic: cell-centred cosine is an exact eigenvector of the Neumann operator (no discretization error).
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(16, 16, 16, .25));
		TArray<double> A, B; A.SetNum(Grid.GetNumCells()); B.SetNum(Grid.GetNumCells());
		const double Ev = -3.0 * 4.0 / (.25 * .25) * FMath::Square(FMath::Sin(UE_DOUBLE_PI / 32.0));
		for (int32 Z = 0; Z < 16; ++Z) for (int32 Y = 0; Y < 16; ++Y) for (int32 X = 0; X < 16; ++X)
		{
			const int32 I = Grid.CellIndex(X, Y, Z);
			A[I] = FMath::Cos(UE_DOUBLE_PI * (X + .5) / 16) * FMath::Cos(UE_DOUBLE_PI * (Y + .5) / 16) * FMath::Cos(UE_DOUBLE_PI * (Z + .5) / 16);
			B[I] = Ev * A[I];
		}
		JacobiPoissonSolver::RemoveMean(A);
		for (const FMultigridPoisson::EMode Mode : {FMultigridPoisson::EMode::VCycle, FMultigridPoisson::EMode::ConjugateGradient})
		{
			MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
			const FPoissonSolveResult R = MultigridPoissonSolver::Solve(Grid, B, X, Cache, S, Mode);
			double Err = 0.0, Mx = 0.0;
			for (int32 I = 0; I < A.Num(); ++I) { Err = FMath::Max(Err, FMath::Abs(X[I] - A[I])); Mx = FMath::Max(Mx, FMath::Abs(A[I])); }
			AddInfo(FString::Printf(TEXT("16^3 코사인 해석해 | %s | %d층 | %s %d회 | 정규화 잔차 %.3e | 해 최대 %.4f 기준 오차 %.3e"),
				Mode == FMultigridPoisson::EMode::VCycle ? TEXT("V-cycle") : TEXT("MG-PCG"), Cache.Solver.GetNumLevels(),
				R.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), R.IterationsUsed, R.NormalizedResidual, Mx, Err / Mx));
			TestTrue(TEXT("Analytic mode converges"), R.bConverged);
			TestTrue(TEXT("Analytic mode error"), Err / Mx < 1e-8);
		}
	}

	// 2) Rack-row solids: residual checked with the independent S3 operator, not the MG operator itself.
	//    3) The iteration count must not grow with the grid (the reason for multigrid).
	int32 ItersSmall = 0, ItersLarge = 0;
	for (const int32 Scale : {1, 2})
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(20 * Scale, 30 * Scale, 10 * Scale, .25 / Scale));
		AddRackRows(Grid);
		TArray<double> Ref, B; Manufacture(Grid, Ref, B);
		MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
		const FPoissonSolveResult R = MultigridPoissonSolver::Solve(Grid, B, X, Cache, S);
		const double ResidualS3 = JacobiPoissonSolver::MaxAbsResidual(Grid, X, B) / R.ReferenceScale;
		JacobiPoissonSolver::RemoveFluidMean(Grid, Ref); JacobiPoissonSolver::RemoveFluidMean(Grid, X);
		double Err = 0.0, Mx = 0.0;
		for (int32 I = 0; I < X.Num(); ++I) if (!Grid.bSolidCell[I]) { Err = FMath::Max(Err, FMath::Abs(X[I] - Ref[I])); Mx = FMath::Max(Mx, FMath::Abs(Ref[I])); }
		AddInfo(FString::Printf(TEXT("랙 줄 고체 %dx%dx%d | 유체 %d셀 %d층 | MG-PCG %s %d회 | S3 연산자로 잰 정규화 잔차 %.3e | 제조해 최대 %.4f 기준 오차 %.3e"),
			Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ(), Cache.Solver.GetNumCells(), Cache.Solver.GetNumLevels(),
			R.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), R.IterationsUsed, ResidualS3, Mx, Err / Mx));
		TestTrue(TEXT("Solid case converges"), R.bConverged);
		TestTrue(TEXT("Residual agrees with the S3 operator"), ResidualS3 < 1e-9);
		TestTrue(TEXT("Manufactured solution recovered"), Err / Mx < 1e-6);
		(Scale == 1 ? ItersSmall : ItersLarge) = R.IterationsUsed;

		// Jacobi reference on the small grid only, to record the gap (it hits its cap).
		if (Scale == 1)
		{
			FPoissonSolveSettings J = S; J.MaxIterations = 4000; J.NormalizedResidualTarget = 1e-6;
			TArray<double> XJ, Scratch;
			const FPoissonSolveResult RJ = JacobiPoissonSolver::Solve(Grid, B, XJ, Scratch, J);
			AddInfo(FString::Printf(TEXT("같은 문제 완화 Jacobi: 목표 1e-6에 %s %d회, 정규화 잔차 %.3e"),
				RJ.bConverged ? TEXT("수렴") : TEXT("수렴 실패"), RJ.IterationsUsed, RJ.NormalizedResidual));
		}
	}
	TestTrue(TEXT("Iterations roughly grid independent (8x cells <= 2x iterations)"), ItersLarge <= 2 * FMath::Max(ItersSmall, 1));

	// D5: a cap that is too small is reported, not hidden.
	{
		FUniformAirGrid Grid; Grid.Initialize(GridParams(20, 30, 10, .25)); AddRackRows(Grid);
		TArray<double> Ref, B; Manufacture(Grid, Ref, B);
		FPoissonSolveSettings Tight = S; Tight.MaxIterations = 2;
		MultigridPoissonSolver::FUniformCache Cache; TArray<double> X;
		TestFalse(TEXT("Cap reached is reported"), MultigridPoissonSolver::Solve(Grid, B, X, Cache, Tight).bConverged);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPressureScanProfile,"VRG.Air.Poisson.ScanProfileAndEquivalence",MgFlags)
bool FPressureScanProfile::RunTest(const FString& Parameters)
{
 double ObserverMs=0;const double ObserverStart=FPlatformTime::Seconds();
 for(int32 I=0;I<100000;++I) {FElapsedTimer Timer(ObserverMs);}
 AddInfo(FString::Printf(TEXT("pressure observer empty scope %.6f ns average (100000, includes loop)"),(FPlatformTime::Seconds()-ObserverStart)*1e9/100000));
 FUniformAirGrid Grid;Grid.Initialize(GridParams(54,41,15,.25));AddRackRows(Grid);
 MultigridPoissonSolver::FUniformCache Cache[2];FPoissonSolveSettings Settings;
 double MaxDiff=0,MaxRelative=0;
 for(int32 Case=0;Case<4;++Case) {
  if(Case==1) Grid.bSolidCell[Grid.CellIndex(4,4,4)]=true;
  if(Case==2) {Grid.Initialize(GridParams(54,41,15,.3));AddRackRows(Grid);}
  if(Case==3) {Grid.Initialize(GridParams(32,24,12,.25));AddRackRows(Grid);}
  TArray<double> Ref,B,X[2];Manufacture(Grid,Ref,B);
  double TimeMs[2]={};FPoissonCpuCosts Total[2];int32 Iterations[2]={};
  for(int32 Path=0;Path<2;++Path) {
   const double BuildStart=FPlatformTime::Seconds();const bool Rebuilt=MultigridPoissonSolver::UpdateCache(Grid,Cache[Path]);
   AddInfo(FString::Printf(TEXT("cache case%d path%d topology build %.6f ms rebuilt%d"),Case,Path,(FPlatformTime::Seconds()-BuildStart)*1000,Rebuilt?1:0));
   Cache[Path].Solver.SetFusedScansForTesting(Path==1); // gauge/dot only: update/norm fusion was slower
  }
  for(int32 Repeat=0;Repeat<20;++Repeat) for(int32 Order=0;Order<2;++Order) {
   const int32 Path=(Repeat+Order)%2; // alternate order to reduce sequential clock/load bias
   X[Path].Init(0,Grid.GetNumCells());const double Start=FPlatformTime::Seconds();
   const auto R=MultigridPoissonSolver::Solve(Grid,B,X[Path],Cache[Path],Settings);
   TimeMs[Path]+=(FPlatformTime::Seconds()-Start)*1000;
   TestTrue(TEXT("unchanged target and convergence"),R.bConverged && R.NormalizedResidual<=Settings.NormalizedResidualTarget);
   Iterations[Path]+=R.IterationsUsed;
   const auto& C=Cache[Path].LastCosts;for(int32 I=0;I<10;++I) Total[Path].Ms[I]+=C.Ms[I];Total[Path].Builds+=C.Builds;
  }
  double RefScale=0;for(int32 I=0;I<Ref.Num();++I) {MaxDiff=FMath::Max(MaxDiff,FMath::Abs(X[0][I]-X[1][I]));RefScale=FMath::Max(RefScale,FMath::Abs(Ref[I]));}
  MaxRelative=FMath::Max(MaxRelative,MaxDiff/RefScale);
  AddInfo(FString::Printf(TEXT("pressure scan case%d cells%d 20 solves old/new %.6f/%.6f ms iterations %d/%d pressure diff %.12g Pa reference-normalized %.12g; rebuilt in repeats %d/%d"),Case,Grid.GetNumCells(),TimeMs[0],TimeMs[1],Iterations[0],Iterations[1],MaxDiff,MaxRelative,Total[0].Builds,Total[1].Builds));
  for(int32 I=0;I<10;++I) AddInfo(FString::Printf(TEXT("pressure stage%d old/new %.6f/%.6f ms/solve"),I,Total[0].Ms[I]/20,Total[1].Ms[I]/20));
  TestEqual(TEXT("existing hierarchy already reused"),Total[0].Builds+Total[1].Builds,0);
  TestTrue(TEXT("measured same-order pressure result"),MaxDiff==0 && MaxRelative==0);
 }
 // D5 failure must stay visible, even after a prior successful solve.
 TArray<double> Ref,B;Manufacture(Grid,Ref,B);Settings.MaxIterations=1;Settings.NormalizedResidualTarget=1e-15;
 for(int32 Path=0;Path<2;++Path) {TArray<double> X;const auto R=MultigridPoissonSolver::Solve(Grid,B,X,Cache[Path],Settings);TestFalse(TEXT("tight cap fails visibly"),R.bConverged);TestEqual(TEXT("one shared cap"),R.IterationsUsed,1);}
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPressureScanAirLifecycle,"VRG.Air.Projection.ScanLifecycle",MgFlags)
bool FPressureScanAirLifecycle::RunTest(const FString& Parameters)
{
 FAirSolver Solver[2];FAirSolverInput Input;double MaxP=0,MaxV=0,MaxT=0,MaxD=0;
 auto Compare=[&](const TArray<double>& A,const TArray<double>& B,double& Out) {for(int32 I=0;I<A.Num();++I) Out=FMath::Max(Out,FMath::Abs(A[I]-B[I]));};
 for(int32 Case=0;Case<4;++Case) {
  auto P=GridParams(24,20,12,Case==2 ? .3:.25);if(Case==3) {P.NumCellsX=28;P.DensityKgM3=1.3;} // synthetic boundary/property variation, not defaults
  for(int32 Path=0;Path<2;++Path) {
   Solver[Path].Initialize(P,297.15);Solver[Path].SetUseMultigridPressure(true);
   Solver[Path].SetFusedPressureScansForTesting(Path==1);
   auto& G=Solver[Path].GetMutableGrid();AddRackRows(G);
   if(Case==1) G.bSolidCell[G.CellIndex(4,4,4)]=true;
   for(int32 I=0;I<G.TemperatureK.Num();++I) G.TemperatureK[I]+=2*FMath::Sin(.1*I);
   for(int32 I=0;I<G.VelocityXMs.Num();++I) G.VelocityXMs[I]=.2*FMath::Sin(.13*I);
   for(int32 I=0;I<G.VelocityYMs.Num();++I) G.VelocityYMs[I]=.1*FMath::Cos(.17*I);
  }
  Input.CellHeatW.Init(0,Solver[0].GetGrid().GetNumCells());Input.CellHeatW[0]=10; // synthetic test source W
  for(int32 Step=0;Step<25;++Step) {
   for(auto& A:Solver) {A.Step(Input,.02);TestTrue(TEXT("pressure convergence unchanged"),A.GetLastPressureResult().bConverged);}
   const auto& A=Solver[0].GetGrid();const auto& B=Solver[1].GetGrid();
   Compare(A.PressurePa,B.PressurePa,MaxP);Compare(A.VelocityXMs,B.VelocityXMs,MaxV);Compare(A.VelocityYMs,B.VelocityYMs,MaxV);Compare(A.VelocityZMs,B.VelocityZMs,MaxV);Compare(A.TemperatureK,B.TemperatureK,MaxT);
   for(int32 Z=0;Z<P.NumCellsZ;++Z) for(int32 Y=0;Y<P.NumCellsY;++Y) for(int32 X=0;X<P.NumCellsX;++X)
    if(A.IsFluidCell(X,Y,Z)) MaxD=FMath::Max(MaxD,FMath::Abs(A.CellDivergencePerS(X,Y,Z)-B.CellDivergencePerS(X,Y,Z)));
  }
 }
 AddInfo(FString::Printf(TEXT("air scan lifecycle pressure %.12g Pa velocity %.12g m/s temperature %.12g K divergence diff %.12g 1/s; changed solids/h/dims/density and restart"),MaxP,MaxV,MaxT,MaxD));
 TestTrue(TEXT("measured same-order projection fields"),MaxP==0 && MaxV==0 && MaxT==0 && MaxD==0);
 return true;
}
#endif
