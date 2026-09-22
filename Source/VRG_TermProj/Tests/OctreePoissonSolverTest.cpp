// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/FixedOctreeGrid.h"
#include "Simulation/Air/JacobiPoissonSolver.h"
#include "Simulation/Air/OctreePoissonSolver.h"
#include "Simulation/Air/UniformAirGrid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgOctreeSolverTestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams SolverTestParams(int32 Cells, double CellSizeM)
	{
		FAirGridParams P;
		P.NumCellsX = Cells;
		P.NumCellsY = Cells;
		P.NumCellsZ = Cells;
		P.CellSizeM = CellSizeM;
		return P;
	}

	// 재현 가능한 의사난수. 시드가 고정이라 실패를 그대로 다시 만들 수 있다.
	double PseudoRandom(int32 Index, int32 Salt)
	{
		const double Value = FMath::Sin(double(Index) * 12.9898 + double(Salt) * 78.233) * 43758.5453;
		return Value - FMath::Floor(Value) - 0.5;
	}

	// 매끄러운 제조해. 비정합 면에서도 값이 튀지 않게 저주파만 쓴다.
	double ManufacturedField(const FVector& PositionM)
	{
		return FMath::Cos(UE_DOUBLE_PI * PositionM.X) * FMath::Cos(UE_DOUBLE_PI * PositionM.Y)
			+ 0.5 * FMath::Cos(UE_DOUBLE_PI * PositionM.Z);
	}
}

// 2:1 밸런스: 면을 맞댄 두 리프의 깊이 차가 1을 넘으면 안 된다.
// 깊이 3짜리 좁은 영역을 주면 밸런스 분할이 실제로 일어나는 구성이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeBalanceTest,
	"VRG.Air.Octree.Fixed.BalanceTwoToOne", VrgOctreeSolverTestFlags)

bool FFixedOctreeBalanceTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	const FAirGridParams P = SolverTestParams(6, 0.25);
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 3;
	Settings.MaxLeaves = 200000;

	// 도메인 한가운데 한 셀보다 작은 영역. 밸런스가 없으면 깊이 0과 3이 바로 맞닿는다.
	const TArray<FBox> Regions = { FBox(FVector(0.76, 0.76, 0.76), FVector(0.82, 0.82, 0.82)) };
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, Regions, Error))) { return false; }

	int32 MaxDepthGap = 0;
	for (const FFixedOctreeFace& Face : Tree.GetFaces())
	{
		if (Face.LeafLow == INDEX_NONE || Face.LeafHigh == INDEX_NONE)
		{
			continue;
		}
		MaxDepthGap = FMath::Max(MaxDepthGap,
			FMath::Abs(Tree.LeafDepth(Face.LeafLow) - Tree.LeafDepth(Face.LeafHigh)));
	}

	AddInfo(FString::Printf(TEXT("리프 %d개 | 밸런스 분할 %d회 | 비정합 면 %d개 | 최대 깊이 차 %d"),
		Tree.GetNumLeaves(), Tree.GetNumBalanceSplits(), Tree.GetNumHangingFaces(), MaxDepthGap));

	TestTrue(TEXT("밸런스 분할이 실제로 일어나야 한다"), Tree.GetNumBalanceSplits() > 0);
	TestTrue(TEXT("비정합 면이 있어야 한다"), Tree.GetNumHangingFaces() > 0);
	TestEqual(TEXT("맞닿은 리프의 깊이 차는 1 이하여야 한다"), MaxDepthGap, 1);

	return true;
}

// 면 목록의 무결성. 리프마다 면적 합이 정확히 6h^2이면 면이 빠지지도 겹치지도 않았다.
// 총 면적만 보면 누락과 중복이 상쇄될 수 있으므로 리프별로 본다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeFaceClosureTest,
	"VRG.Air.Octree.Fixed.FaceAreaClosure", VrgOctreeSolverTestFlags)

bool FFixedOctreeFaceClosureTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	const FAirGridParams P = SolverTestParams(4, 0.3);
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	Settings.MaxLeaves = 200000;
	const TArray<FBox> Regions = { FBox(FVector(0.31, 0.31, 0.31), FVector(0.61, 0.61, 0.61)) };
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, Regions, Error))) { return false; }

	double MaxNormalizedAreaError = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		const double SizeM = Tree.LeafSizeM(Leaf);
		const double ExpectedM2 = 6.0 * SizeM * SizeM;
		double SumM2 = 0.0;
		for (int32 Slot = Tree.GetLeafFaceStart()[Leaf]; Slot < Tree.GetLeafFaceStart()[Leaf + 1]; ++Slot)
		{
			SumM2 += Tree.GetFaces()[Tree.GetLeafFaceIndices()[Slot]].AreaM2;
		}
		MaxNormalizedAreaError = FMath::Max(MaxNormalizedAreaError,
			FMath::Abs(SumM2 - ExpectedM2) / ExpectedM2);
	}

	// 안쪽 면은 두 리프 목록에, 벽면은 한 리프 목록에 정확히 한 번씩 들어가야 한다.
	int32 ExpectedSlots = 0;
	int32 NumWallFaces = 0;
	for (const FFixedOctreeFace& Face : Tree.GetFaces())
	{
		const bool bWall = Face.LeafLow == INDEX_NONE || Face.LeafHigh == INDEX_NONE;
		ExpectedSlots += bWall ? 1 : 2;
		NumWallFaces += bWall ? 1 : 0;
	}

	// 도메인 바깥 면적은 직육면체 겉넓이와 같아야 한다.
	const FVector DomainM = FVector(P.NumCellsX, P.NumCellsY, P.NumCellsZ) * P.CellSizeM;
	const double ExpectedWallM2 = 2.0 * (DomainM.X * DomainM.Y + DomainM.Y * DomainM.Z + DomainM.Z * DomainM.X);
	double WallM2 = 0.0;
	for (const FFixedOctreeFace& Face : Tree.GetFaces())
	{
		if (Face.LeafLow == INDEX_NONE || Face.LeafHigh == INDEX_NONE)
		{
			WallM2 += Face.AreaM2;
		}
	}
	const double NormalizedWallError = FMath::Abs(WallM2 - ExpectedWallM2) / ExpectedWallM2;

	AddInfo(FString::Printf(TEXT("리프 %d개 / 면 %d개 (벽 %d개, 비정합 %d개)"),
		Tree.GetNumLeaves(), Tree.GetFaces().Num(), NumWallFaces, Tree.GetNumHangingFaces()));
	AddInfo(FString::Printf(TEXT("리프별 면적 합 정규화 오차 %.3e | 벽 면적 %.6f m^2 (기대 %.6f) 정규화 오차 %.3e"),
		MaxNormalizedAreaError, WallM2, ExpectedWallM2, NormalizedWallError));

	TestEqual(TEXT("CSR 슬롯 수가 면 참조 수와 같아야 한다"),
		Tree.GetLeafFaceIndices().Num(), ExpectedSlots);
	TestTrue(TEXT("리프마다 면적 합이 6h^2여야 한다"), MaxNormalizedAreaError < 1.0e-14);
	TestTrue(TEXT("벽 면적이 도메인 겉넓이와 같아야 한다"), NormalizedWallError < 1.0e-14);

	return true;
}

// 깊이 0에서는 비균일 연산자가 기존 균일 연산자와 **같은 식**이어야 한다.
// 고체 셀을 섞어 Neumann 처리까지 같이 본다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePoissonUniformIdentityTest,
	"VRG.Air.Octree.Poisson.UniformOperatorIdentity", VrgOctreeSolverTestFlags)

bool FOctreePoissonUniformIdentityTest::RunTest(const FString& Parameters)
{
	const FAirGridParams P = SolverTestParams(6, 0.25);
	FUniformAirGrid Grid;
	Grid.Initialize(P);
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		Grid.TemperatureK[Index] = 297.15;
	}
	// 가운데 기둥 하나를 고체로 막는다.
	for (int32 Z = 1; Z < 5; ++Z)
	{
		Grid.bSolidCell[Grid.CellIndex(2, 3, Z)] = true;
	}

	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 0;
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, {}, Error))) { return false; }
	if (!TestTrue(TEXT("Import succeeds"), Tree.ImportUniform(Grid, Error))) { return false; }

	TArray<double> UniformField;
	UniformField.SetNum(Grid.GetNumCells());
	for (int32 Index = 0; Index < UniformField.Num(); ++Index)
	{
		UniformField[Index] = PseudoRandom(Index, 7);
	}
	TArray<double> LeafField;
	LeafField.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		LeafField[Leaf] = UniformField[Tree.GetNodes()[Tree.GetLeafNodeIndices()[Leaf]].RootCellIndex];
	}

	double MaxAbsError = 0.0;
	double MaxAbsExpected = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (!Tree.IsLeafFluid(Leaf))
		{
			continue;
		}
		const int32 Cell = Tree.GetNodes()[Tree.GetLeafNodeIndices()[Leaf]].RootCellIndex;
		const int32 X = Cell % P.NumCellsX;
		const int32 Y = (Cell / P.NumCellsX) % P.NumCellsY;
		const int32 Z = Cell / (P.NumCellsX * P.NumCellsY);
		const double Expected = JacobiPoissonSolver::LaplacianAt(Grid, UniformField, X, Y, Z);
		const double Actual = OctreePoissonSolver::LaplacianAt(Tree, LeafField, Leaf);
		MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(Actual - Expected));
		MaxAbsExpected = FMath::Max(MaxAbsExpected, FMath::Abs(Expected));
	}
	const double NormalizedError = MaxAbsError / MaxAbsExpected;

	AddInfo(FString::Printf(TEXT("6^3 격자, h=0.25 m, 고체 4셀 | 균일 Laplacian 최대 크기 %.6f 1/m^2"),
		MaxAbsExpected));
	AddInfo(FString::Printf(TEXT("깊이 0 연산자 절대 차이 %.3e / 정규화 %.3e"),
		MaxAbsError, NormalizedError));

	TestTrue(TEXT("깊이 0에서 두 연산자가 같아야 한다"), NormalizedError < 1.0e-14);

	return true;
}

// 규약 6의 결과: 면 플럭스를 한 번 계산해 양쪽에 쓰므로 sum(V_i * (LX)_i)가 0이고
// 연산자가 체적 가중 내적에서 대칭이다. 정제 격자에서 본다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePoissonConservationTest,
	"VRG.Air.Octree.Poisson.ConservationAndSymmetry", VrgOctreeSolverTestFlags)

bool FOctreePoissonConservationTest::RunTest(const FString& Parameters)
{
	const FAirGridParams P = SolverTestParams(6, 0.25);
	FUniformAirGrid Grid;
	Grid.Initialize(P);
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		Grid.TemperatureK[Index] = 297.15;
	}
	Grid.bSolidCell[Grid.CellIndex(4, 4, 2)] = true;

	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	Settings.MaxLeaves = 200000;
	const TArray<FBox> Regions = { FBox(FVector(0.26, 0.26, 0.26), FVector(0.74, 0.74, 0.74)) };
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, Regions, Error))) { return false; }
	if (!TestTrue(TEXT("Import succeeds"), Tree.ImportUniform(Grid, Error))) { return false; }

	TArray<double> FieldA;
	TArray<double> FieldB;
	FieldA.SetNum(Tree.GetNumLeaves());
	FieldB.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		FieldA[Leaf] = PseudoRandom(Leaf, 11);
		FieldB[Leaf] = PseudoRandom(Leaf, 29);
	}

	// 상수장은 연산자의 핵이다. 남는 것은 큰 항끼리 빼면서 생기는 반올림뿐이다.
	// 정규화 기준은 연산자 자신의 크기 (최소 셀에서의 대각 계수 x 상수값)다.
	// 절대값으로 기준을 걸면 셀 크기와 상수값에 따라 달라져 테스트가 불안정해진다.
	const double ConstantValue = 3.75;
	TArray<double> Constant;
	Constant.Init(ConstantValue, Tree.GetNumLeaves());
	double MinLeafSizeM = TNumericLimits<double>::Max();
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		MinLeafSizeM = FMath::Min(MinLeafSizeM, Tree.LeafSizeM(Leaf));
	}
	const double OperatorScale = 6.0 * ConstantValue / (MinLeafSizeM * MinLeafSizeM);
	double MaxAbsConstant = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (Tree.IsLeafFluid(Leaf))
		{
			MaxAbsConstant = FMath::Max(MaxAbsConstant,
				FMath::Abs(OctreePoissonSolver::LaplacianAt(Tree, Constant, Leaf)));
		}
	}
	const double NormalizedConstant = MaxAbsConstant / OperatorScale;

	// 보존. 정규화 기준은 항을 모두 양수로 더했을 때의 크기다.
	const double LaplacianSum = OctreePoissonSolver::VolumeWeightedLaplacianSum(Tree, FieldA);
	double AbsScale = 0.0;
	double InnerAB = 0.0;
	double InnerBA = 0.0;
	double InnerScale = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		if (!Tree.IsLeafFluid(Leaf))
		{
			continue;
		}
		const double VolumeM3 = Tree.LeafVolumeM3(Leaf);
		const double LapA = OctreePoissonSolver::LaplacianAt(Tree, FieldA, Leaf);
		const double LapB = OctreePoissonSolver::LaplacianAt(Tree, FieldB, Leaf);
		AbsScale += VolumeM3 * FMath::Abs(LapA);
		InnerAB += VolumeM3 * FieldB[Leaf] * LapA;
		InnerBA += VolumeM3 * FieldA[Leaf] * LapB;
		InnerScale += VolumeM3 * FMath::Abs(FieldB[Leaf] * LapA);
	}
	const double NormalizedConservation = FMath::Abs(LaplacianSum) / AbsScale;
	const double NormalizedSymmetry = FMath::Abs(InnerAB - InnerBA) / InnerScale;

	AddInfo(FString::Printf(TEXT("리프 %d개 / 면 %d개 (비정합 %d개, 밸런스 분할 %d회)"),
		Tree.GetNumLeaves(), Tree.GetFaces().Num(), Tree.GetNumHangingFaces(), Tree.GetNumBalanceSplits()));
	AddInfo(FString::Printf(TEXT("상수장 Laplacian 최대 %.3e 1/m^2 / 연산자 크기 %.3f 기준 정규화 %.3e"),
		MaxAbsConstant, OperatorScale, NormalizedConstant));
	AddInfo(FString::Printf(TEXT("보존 잔차 %.3e (절대값 합 %.6f 기준) | 대칭성 %.3e (내적 크기 합 %.6f 기준)"),
		NormalizedConservation, AbsScale, NormalizedSymmetry, InnerScale));

	TestTrue(TEXT("상수장의 Laplacian은 0이어야 한다"), NormalizedConstant < 1.0e-14);
	TestTrue(TEXT("체적 가중 Laplacian 합이 0이어야 한다"), NormalizedConservation < 1.0e-12);
	TestTrue(TEXT("연산자가 체적 가중 내적에서 대칭이어야 한다"), NormalizedSymmetry < 1.0e-12);

	return true;
}

// 해석해 비교. 깊이 0에서 셀 중심 코사인은 이산 Neumann Laplacian의 정확한 고유벡터다.
// 이산화 오차가 0이므로 남는 것은 반복 솔버의 수렴 한계뿐이다 (균일 솔버와 같은 성질).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePoissonAnalyticTest,
	"VRG.Air.Octree.Poisson.CosineModeAnalytic", VrgOctreeSolverTestFlags)

bool FOctreePoissonAnalyticTest::RunTest(const FString& Parameters)
{
	const int32 Cells = 12;
	const FAirGridParams P = SolverTestParams(Cells, 0.25);
	FUniformAirGrid Grid;
	Grid.Initialize(P);
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		Grid.TemperatureK[Index] = 297.15;
	}

	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 0;
	Settings.MaxLeaves = 200000;
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, {}, Error))) { return false; }
	if (!TestTrue(TEXT("Import succeeds"), Tree.ImportUniform(Grid, Error))) { return false; }

	// cos(pi*(i+0.5)/N)은 거울상 경계에서 고유값 -(4/h^2)sin^2(pi/2N)을 갖는다 (축마다).
	const double Eigenvalue = -3.0 * (4.0 / (P.CellSizeM * P.CellSizeM))
		* FMath::Square(FMath::Sin(UE_DOUBLE_PI / (2.0 * Cells)));

	TArray<double> Analytic;
	TArray<double> RightHandSide;
	Analytic.SetNum(Tree.GetNumLeaves());
	RightHandSide.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		const int32 Cell = Tree.GetNodes()[Tree.GetLeafNodeIndices()[Leaf]].RootCellIndex;
		const int32 X = Cell % Cells;
		const int32 Y = (Cell / Cells) % Cells;
		const int32 Z = Cell / (Cells * Cells);
		Analytic[Leaf] = FMath::Cos(UE_DOUBLE_PI * (X + 0.5) / Cells)
			* FMath::Cos(UE_DOUBLE_PI * (Y + 0.5) / Cells)
			* FMath::Cos(UE_DOUBLE_PI * (Z + 0.5) / Cells);
		RightHandSide[Leaf] = Eigenvalue * Analytic[Leaf];
	}

	FPoissonSolveSettings SolveSettings;
	SolveSettings.NormalizedResidualTarget = 1.0e-8;
	SolveSettings.MaxIterations = 20000;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result =
		OctreePoissonSolver::Solve(Tree, RightHandSide, Solved, Scratch, SolveSettings);

	TArray<double> AnalyticZeroMean = Analytic;
	OctreePoissonSolver::RemoveFluidVolumeMean(Tree, AnalyticZeroMean);

	double MaxAbsError = 0.0;
	double MaxAbsAnalytic = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(Solved[Leaf] - AnalyticZeroMean[Leaf]));
		MaxAbsAnalytic = FMath::Max(MaxAbsAnalytic, FMath::Abs(AnalyticZeroMean[Leaf]));
	}
	const double NormalizedError = MaxAbsError / MaxAbsAnalytic;

	AddInfo(FString::Printf(TEXT("깊이 0, %d^3 격자, h=0.25 m | 고유값 %.9f 1/m^2 | %s (반복 %d회, 정규화 잔차 %.3e)"),
		Cells, Eigenvalue, Result.bConverged ? TEXT("수렴") : TEXT("수렴 실패"),
		Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("해석해 대비 절대 오차 %.3e / 해 최대 크기 %.6f 기준 정규화 오차 %.3e"),
		MaxAbsError, MaxAbsAnalytic, NormalizedError));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestTrue(TEXT("옥트리 솔버가 해석해를 재현해야 한다"), NormalizedError < 1.0e-6);

	return true;
}

// 정제 격자에서의 제조해. B = L(X_manufactured)로 두면 우변이 반드시 적합하다.
// 이건 **이산화 정확도가 아니라 솔버의 일관성**을 재는 것이다.
// 옥트리 이산화 자체의 정확도를 재려면 독립 CFD 해가 필요하고, 그건 여기 없다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePoissonRefinedSolveTest,
	"VRG.Air.Octree.Poisson.RefinedSolveConsistency", VrgOctreeSolverTestFlags)

bool FOctreePoissonRefinedSolveTest::RunTest(const FString& Parameters)
{
	const FAirGridParams P = SolverTestParams(8, 0.25);
	FUniformAirGrid Grid;
	Grid.Initialize(P);
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		Grid.TemperatureK[Index] = 297.15;
	}

	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	Settings.MaxLeaves = 200000;
	const TArray<FBox> Regions = { FBox(FVector(0.51, 0.51, 0.51), FVector(1.24, 1.24, 1.24)) };
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, Regions, Error))) { return false; }
	if (!TestTrue(TEXT("Import succeeds"), Tree.ImportUniform(Grid, Error))) { return false; }

	TArray<double> Manufactured;
	Manufactured.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		Manufactured[Leaf] = ManufacturedField(Tree.LeafCenterM(Leaf));
	}
	OctreePoissonSolver::RemoveFluidVolumeMean(Tree, Manufactured);

	TArray<double> RightHandSide;
	RightHandSide.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		RightHandSide[Leaf] = OctreePoissonSolver::LaplacianAt(Tree, Manufactured, Leaf);
	}

	FPoissonSolveSettings SolveSettings;
	SolveSettings.NormalizedResidualTarget = 1.0e-9;
	SolveSettings.MaxIterations = 60000;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result =
		OctreePoissonSolver::Solve(Tree, RightHandSide, Solved, Scratch, SolveSettings);

	double MaxAbsError = 0.0;
	double MaxAbsManufactured = 0.0;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(Solved[Leaf] - Manufactured[Leaf]));
		MaxAbsManufactured = FMath::Max(MaxAbsManufactured, FMath::Abs(Manufactured[Leaf]));
	}
	const double NormalizedError = MaxAbsError / MaxAbsManufactured;

	AddInfo(FString::Printf(TEXT("리프 %d개 / 면 %d개 (비정합 %d개) | %s (반복 %d회, 정규화 잔차 %.3e)"),
		Tree.GetNumLeaves(), Tree.GetFaces().Num(), Tree.GetNumHangingFaces(),
		Result.bConverged ? TEXT("수렴") : TEXT("수렴 실패"),
		Result.IterationsUsed, Result.NormalizedResidual));
	AddInfo(FString::Printf(TEXT("제조해 대비 절대 오차 %.3e / 해 최대 크기 %.6f 기준 정규화 오차 %.3e"),
		MaxAbsError, MaxAbsManufactured, NormalizedError));
	AddInfo(FString::Printf(TEXT("우변 적합성: 체적 가중 평균 %.3e (기준 %.6f), 부적합 표시 %s"),
		Result.MeanRightHandSide, Result.ReferenceScale,
		Result.bIncompatibleRightHandSide ? TEXT("있음") : TEXT("없음")));

	TestTrue(TEXT("수렴해야 한다"), Result.bConverged);
	TestFalse(TEXT("제조한 우변은 적합해야 한다"), Result.bIncompatibleRightHandSide);
	TestTrue(TEXT("해가 제조해를 재현해야 한다"), NormalizedError < 1.0e-5);

	return true;
}

// D5: 반복 상한에 걸리면 수렴 실패를 표시해야 한다. 적합하지 않은 우변도 표시해야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreePoissonReportingTest,
	"VRG.Air.Octree.Poisson.FailureIsReported", VrgOctreeSolverTestFlags)

bool FOctreePoissonReportingTest::RunTest(const FString& Parameters)
{
	const FAirGridParams P = SolverTestParams(6, 0.25);
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 1;
	Settings.MaxLeaves = 200000;
	const TArray<FBox> Regions = { FBox(FVector(0.26, 0.26, 0.26), FVector(0.74, 0.74, 0.74)) };
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, Regions, Error))) { return false; }

	TArray<double> RightHandSide;
	RightHandSide.SetNum(Tree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		RightHandSide[Leaf] = PseudoRandom(Leaf, 3) + 1.0;
	}

	FPoissonSolveSettings SolveSettings;
	SolveSettings.NormalizedResidualTarget = 1.0e-12;
	SolveSettings.MaxIterations = 5;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result =
		OctreePoissonSolver::Solve(Tree, RightHandSide, Solved, Scratch, SolveSettings);

	AddInfo(FString::Printf(TEXT("반복 %d회에서 정지, 정규화 잔차 %.3e, 체적 가중 평균 우변 %.6f"),
		Result.IterationsUsed, Result.NormalizedResidual, Result.MeanRightHandSide));

	TestFalse(TEXT("5회로는 1e-12에 못 간다"), Result.bConverged);
	TestEqual(TEXT("반복 상한에서 멈춰야 한다"), Result.IterationsUsed, 5);
	TestTrue(TEXT("적합하지 않은 우변을 표시해야 한다"), Result.bIncompatibleRightHandSide);

	return true;
}

#endif
