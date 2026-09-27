// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/WallDistance.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr auto TurbulenceTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams MakeBox(int32 NX, int32 NY, int32 NZ, double H)
	{
		FAirGridParams P;
		P.NumCellsX = NX; P.NumCellsY = NY; P.NumCellsZ = NZ; P.CellSizeM = H;
		return P;
	}
}

// Wall distance (2026-09-27): exact on the grid for axis-aligned walls, sqrt(2)h - h/2 / sqrt(3)h - h/2 next to a
// lone solid cell diagonally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurbulenceWallDistanceTest, "VRG.Air.Turbulence.WallDistanceAnalytic", TurbulenceTestFlags)
bool FTurbulenceWallDistanceTest::RunTest(const FString& Parameters)
{
	constexpr double H = .2;
	FUniformAirGrid Box;
	Box.Initialize(MakeBox(7, 6, 5, H));
	TArray<double> L;
	AirWallDistance::Compute(Box, L);
	double MaxErr = 0.0;
	for (int32 Z = 0; Z < 5; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 7; ++X)
	{
		const int32 M = FMath::Min3(FMath::Min(X, 6 - X), FMath::Min(Y, 5 - Y), FMath::Min(Z, 4 - Z));
		MaxErr = FMath::Max(MaxErr, FMath::Abs(L[Box.CellIndex(X, Y, Z)] - (M + .5) * H));
	}

	FUniformAirGrid Solid;
	Solid.Initialize(MakeBox(15, 15, 15, H));
	Solid.bSolidCell[Solid.CellIndex(7, 7, 7)] = true;
	TArray<double> LS;
	AirWallDistance::Compute(Solid, LS);
	const double Face = LS[Solid.CellIndex(8, 7, 7)], Edge = LS[Solid.CellIndex(8, 8, 7)], Corner = LS[Solid.CellIndex(8, 8, 8)];
	const double SolidErr = FMath::Max3(FMath::Abs(Face - .5 * H), FMath::Abs(Edge - (FMath::Sqrt(2.0) - .5) * H), FMath::Abs(Corner - (FMath::Sqrt(3.0) - .5) * H));
	AddInfo(FString::Printf(TEXT("빈 상자 최대 오차 %.3e m | 고체 한 칸 옆 면/모서리/꼭짓점 %.6f / %.6f / %.6f m, 오차 %.3e m | 고체 셀 %.1f"),
		MaxErr, Face, Edge, Corner, SolidErr, LS[Solid.CellIndex(7, 7, 7)]));
	TestTrue(TEXT("Empty box: distance to the nearest wall"), MaxErr < 1e-12);
	TestTrue(TEXT("Lone solid cell: face / edge / corner neighbours"), SolidErr < 1e-12);
	TestEqual(TEXT("Solid cell has zero distance"), LS[Solid.CellIndex(7, 7, 7)], 0.0);
	return true;
}

// Momentum diffusion: u_z = sin(pi(i+.5)/NX) sin(pi(j+.5)/NY) sin(pi k/NZ) is an exact eigenvector of the discrete
// operator with no-slip walls (ghost -u half a cell out, wall faces 0), eigenvalue -(4/h^2) sum sin^2(pi/2N).
// With uniform viscosity each explicit step multiplies it by exactly 1 + nu dt lambda.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurbulenceMomentumEigenTest, "VRG.Air.Turbulence.MomentumEigenmodeDecay", TurbulenceTestFlags)
bool FTurbulenceMomentumEigenTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 8, NY = 6, NZ = 5, Steps = 50;
	constexpr double H = .25, Nu = .01, Dt = .2, A = .7;
	FAirSolver Air;
	Air.Initialize(MakeBox(NX, NY, NZ, H), 297.15);
	FAirTurbulenceSettings& T = Air.GetMutableTurbulenceSettings();
	T.bEnabled = true; T.ChenXuCoefficient = 0.0; T.MolecularViscosityM2s = Nu;  // uniform viscosity
	FUniformAirGrid& G = Air.GetMutableGrid();
	auto Mode = [&](int32 X, int32 Y, int32 Z)
	{
		return FMath::Sin(UE_DOUBLE_PI * (X + .5) / NX) * FMath::Sin(UE_DOUBLE_PI * (Y + .5) / NY) * FMath::Sin(UE_DOUBLE_PI * double(Z) / NZ);
	};
	for (int32 Z = 0; Z <= NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
		G.VelocityZMs[G.FaceZIndex(X, Y, Z)] = (Z == 0 || Z == NZ) ? 0.0 : A * Mode(X, Y, Z);
	Air.UpdateTurbulentViscosity();
	for (int32 S = 0; S < Steps; ++S) Air.DiffuseMomentum(Dt);

	auto S2 = [](int32 N) { const double V = FMath::Sin(UE_DOUBLE_PI / (2.0 * N)); return V * V; };
	const double Lambda = -4.0 / (H * H) * (S2(NX) + S2(NY) + S2(NZ));
	const double Factor = FMath::Pow(1.0 + Nu * Dt * Lambda, Steps);
	double MaxErr = 0.0, MaxOther = 0.0;
	for (int32 Z = 0; Z <= NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
		MaxErr = FMath::Max(MaxErr, FMath::Abs(G.VelocityZMs[G.FaceZIndex(X, Y, Z)] - A * Factor * Mode(X, Y, Z)));
	for (const double V : G.VelocityXMs) MaxOther = FMath::Max(MaxOther, FMath::Abs(V));
	for (const double V : G.VelocityYMs) MaxOther = FMath::Max(MaxOther, FMath::Abs(V));
	const double Normalized = MaxErr / (A * Factor);
	AddInfo(FString::Printf(TEXT("감쇠 배율 %.9f (연속 해 exp(nu lambda t) %.9f) | 정규화 오차 %.3e (기대 진폭 %.6f m/s 기준) | 다른 성분 %.1e | 부분 스텝 %d"),
		Factor, FMath::Exp(Nu * Lambda * Dt * Steps), Normalized, A * Factor, MaxOther, Air.GetLastMomentumDiffusionSubsteps()));
	TestEqual(TEXT("One sub-step per step at this dt"), Air.GetLastMomentumDiffusionSubsteps(), 1);
	TestTrue(TEXT("Mode decays at the exact discrete rate"), Normalized < 1e-12);
	TestTrue(TEXT("Other components stay zero"), MaxOther == 0.0);
	return true;
}

// Heat diffusion: T = T0 + A cos(pi(i+.5)/NX) is an exact eigenvector of the adiabatic (zero-flux) operator,
// eigenvalue -(4/h^2) sin^2(pi/2NX). Still air, so nu_t = 0 even with the Chen-Xu coefficient on. Heat is conserved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurbulenceHeatEigenTest, "VRG.Air.Turbulence.HeatEigenmodeDecay", TurbulenceTestFlags)
bool FTurbulenceHeatEigenTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 10, NY = 4, NZ = 3, Steps = 60;
	constexpr double H = .25, Alpha = .01, Dt = .2, A = 5.0, T0 = 300.0;
	FAirSolver Air;
	Air.Initialize(MakeBox(NX, NY, NZ, H), T0);
	FAirTurbulenceSettings& T = Air.GetMutableTurbulenceSettings();
	T.bEnabled = true; T.MolecularDiffusivityM2s = Alpha;
	FUniformAirGrid& G = Air.GetMutableGrid();
	for (int32 Z = 0; Z < NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
		G.TemperatureK[G.CellIndex(X, Y, Z)] = T0 + A * FMath::Cos(UE_DOUBLE_PI * (X + .5) / NX);
	const double E0 = G.TotalThermalEnergyJ(T0);
	Air.UpdateTurbulentViscosity();
	for (int32 S = 0; S < Steps; ++S) Air.AdvectTemperature(Dt);

	const double SinH = FMath::Sin(UE_DOUBLE_PI / (2.0 * NX));
	const double Lambda = -4.0 / (H * H) * SinH * SinH;
	const double Factor = FMath::Pow(1.0 + Alpha * Dt * Lambda, Steps);
	double MaxErr = 0.0;
	for (int32 Z = 0; Z < NZ; ++Z) for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X)
		MaxErr = FMath::Max(MaxErr, FMath::Abs(G.TemperatureK[G.CellIndex(X, Y, Z)] - (T0 + A * Factor * FMath::Cos(UE_DOUBLE_PI * (X + .5) / NX))));
	// Heat content: A cos sums to zero, so compare against the total heat relative to 0 K instead.
	const double EAbs = G.GetParams().DensityKgM3 * G.GetParams().SpecificHeatJKgK * G.GetCellVolumeM3() * T0 * G.GetNumCells();
	const double Drift = FMath::Abs(G.TotalThermalEnergyJ(T0) - E0) / EAbs;
	AddInfo(FString::Printf(TEXT("감쇠 배율 %.9f (연속 %.9f) | 온도 최대 오차 %.3e K (기대 진폭 %.6f K) | 열량 변화 %.3e (0 K 기준 열량 %.4e J 기준)"),
		Factor, FMath::Exp(Alpha * Lambda * Dt * Steps), MaxErr, A * Factor, Drift, EAbs));
	TestTrue(TEXT("nu_t is zero in still air"), Air.GetMaxTurbulentViscosityM2s() == 0.0);
	TestTrue(TEXT("Mode decays at the exact discrete rate"), MaxErr / (A * Factor) < 1e-11);
	TestTrue(TEXT("Heat is conserved"), Drift < 1e-14);
	return true;
}

// Closed box with a heater, a solid block and buoyancy, turbulence on: the ledger closes, the flow stays
// divergence-free, nu_t lands in a plausible range for a room, and the turbulent heat spread lowers the peak.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTurbulenceClosedBoxTest, "VRG.Air.Turbulence.ClosedBoxConservation", TurbulenceTestFlags)
bool FTurbulenceClosedBoxTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 12, NY = 12, NZ = 8, Steps = 500;
	constexpr double H = .25, Dt = .02;
	auto Run = [&](bool bTurbulence, double& OutPeakK, double& OutResidual, double& OutDivergence, double& OutNuMax, double& OutNuMean, int32& OutTempSub)
	{
		FAirSolver Air;
		Air.Initialize(MakeBox(NX, NY, NZ, H), 297.15);
		Air.GetMutableTurbulenceSettings().bEnabled = bTurbulence;
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-9;
		FUniformAirGrid& G = Air.GetMutableGrid();
		for (int32 Z = 0; Z < 5; ++Z) for (int32 Y = 2; Y < 5; ++Y) for (int32 X = 7; X < 9; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		FAirSolverInput In;
		In.CellHeatW.Init(0.0, G.GetNumCells());
		for (int32 Y = 5; Y < 7; ++Y) for (int32 X = 3; X < 5; ++X) In.CellHeatW[G.CellIndex(X, Y, 0)] = 500.0;  // 2000 W
		for (int32 S = 0; S < Steps; ++S) Air.Step(In, Dt);
		OutPeakK = 0.0;
		for (int32 I = 0; I < G.GetNumCells(); ++I) if (!G.bSolidCell[I]) OutPeakK = FMath::Max(OutPeakK, G.TemperatureK[I]);
		OutResidual = Air.GetNormalizedEnergyResidual();
		OutDivergence = G.MaxAbsDivergencePerS() / FMath::Max(G.MaxAbsVelocityMs() / H, 1e-30);
		OutNuMax = Air.GetMaxTurbulentViscosityM2s();
		OutNuMean = Air.GetMeanTurbulentViscosityM2s();
		OutTempSub = Air.GetLastTemperatureSubsteps();
	};
	double PeakT, ResT, DivT, NuMax, NuMean, PeakI, ResI, DivI, D0, D1;
	int32 SubT, SubI;
	Run(true, PeakT, ResT, DivT, NuMax, NuMean, SubT);
	Run(false, PeakI, ResI, DivI, D0, D1, SubI);
	AddInfo(FString::Printf(TEXT("난류: 최고 %.3f C, 열수지 %.3e, 정규화 발산 %.3e, nu_t 최대 %.3e / 평균 %.3e m2/s, 온도 부분 스텝 %d | 비점성: 최고 %.3f C, 열수지 %.3e"),
		PeakT - 273.15, ResT, DivT, NuMax, NuMean, SubT, PeakI - 273.15, ResI));
	TestTrue(TEXT("Ledger closes with turbulence"), FMath::Abs(ResT) < 1e-12);
	TestTrue(TEXT("Divergence-free with turbulence"), DivT < 1e-6);
	TestTrue(TEXT("nu_t in a room-scale range (1e-4 .. 0.5 m^2/s)"), NuMax > 1e-4 && NuMax < .5 && NuMean > 0.0);
	TestTrue(TEXT("Turbulent spreading lowers the peak"), PeakT < PeakI);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
