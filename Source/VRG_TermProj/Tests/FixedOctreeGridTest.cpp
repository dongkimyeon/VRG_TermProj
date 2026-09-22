// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/FixedOctreeGrid.h"
#include "Simulation/Air/AirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgOctreeTestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams OctreeTestParams()
	{
		FAirGridParams P;
		P.NumCellsX = 3;
		P.NumCellsY = 2;
		P.NumCellsZ = 1;
		P.CellSizeM = 0.3; // 이진수로 정확히 표현되지 않는 셀 폭에서도 측정한다.
		return P;
	}

	TArray<FBox> OctreeTestRegions()
	{
		return { FBox(FVector(0.08), FVector(0.12)) };
	}
}

// 최정밀 정수 격자를 독립적으로 훑는다. 총 체적만 보면 중복과 구멍이 상쇄될 수 있다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreePartitionTest,
	"VRG.Air.Octree.Fixed.PartitionAndMorton", VrgOctreeTestFlags)

bool FFixedOctreePartitionTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	const FAirGridParams P = OctreeTestParams();
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	FString Error;
	if (!TestTrue(TEXT("Build succeeds"), Tree.Build(P, Settings, OctreeTestRegions(), Error))) { return false; }
	const FIntVector FineCount(P.NumCellsX * 4, P.NumCellsY * 4, P.NumCellsZ * 4);
	TArray<int32> Coverage;
	Coverage.Init(0, FineCount.X * FineCount.Y * FineCount.Z);
	double VolumeM3 = 0.0;
	uint64 PreviousKey = 0;
	bool bUniqueSorted = true;
	bool bLinksValid = true;
	bool bKeysValid = true;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		const int32 NodeIndex = Tree.GetLeafNodeIndices()[Leaf];
		const FFixedOctreeNode& Node = Tree.GetNodes()[NodeIndex];
		const int32 Width = 1 << (Settings.MaxDepth - Node.Depth);
		VolumeM3 += Tree.LeafVolumeM3(Leaf);
		bLinksValid &= Node.FirstChildIndex == INDEX_NONE && Node.LeafIndex == Leaf;
		bUniqueSorted &= Leaf == 0 || PreviousKey < Node.MortonCode;
		PreviousKey = Node.MortonCode;
		FIntVector Decoded(0);
		for (int32 Bit = 0; Bit < 21; ++Bit)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Decoded[Axis] |= int32((Node.MortonCode >> (3 * Bit + Axis)) & 1) << Bit;
			}
		}
		bKeysValid &= Decoded == Node.MinFine;
		for (int32 Z = Node.MinFine.Z; Z < Node.MinFine.Z + Width; ++Z)
		{
			for (int32 Y = Node.MinFine.Y; Y < Node.MinFine.Y + Width; ++Y)
			{
				for (int32 X = Node.MinFine.X; X < Node.MinFine.X + Width; ++X)
				{
					const bool bInside = X >= 0 && Y >= 0 && Z >= 0
						&& X < FineCount.X && Y < FineCount.Y && Z < FineCount.Z;
					if (!bInside) { AddError(TEXT("Leaf extends outside domain")); return false; }
					++Coverage[X + FineCount.X * (Y + FineCount.Y * Z)];
				}
			}
		}
	}
	for (int32 Index = 0; Index < Tree.GetNodes().Num(); ++Index)
	{
		const FFixedOctreeNode& Node = Tree.GetNodes()[Index];
		if (Node.FirstChildIndex == INDEX_NONE) { continue; }
		bLinksValid &= Node.LeafIndex == INDEX_NONE;
		for (int32 Child = 0; Child < 8; ++Child)
		{
			const FFixedOctreeNode& C = Tree.GetNodes()[Node.FirstChildIndex + Child];
			bLinksValid &= C.ParentIndex == Index && C.Depth == Node.Depth + 1
				&& C.RootCellIndex == Node.RootCellIndex;
		}
	}
	TestTrue(TEXT("Every finest voxel is covered exactly once"), !Coverage.ContainsByPredicate([](int32 N) { return N != 1; }));
	TestTrue(TEXT("Parent/child/leaf indices are consistent"), bLinksValid);
	TestTrue(TEXT("Leaf Morton keys are unique and sorted"), bUniqueSorted);
	TestTrue(TEXT("Morton keys decode to spatial coordinates"), bKeysValid);
	const double ExpectedM3 = 6.0 * 0.3 * 0.3 * 0.3;
	const double RelativeError = FMath::Abs(VolumeM3 - ExpectedM3) / ExpectedM3;
	AddInfo(FString::Printf(TEXT("Octree volume %.17g m3 / reference %.17g m3 / normalized error %.17g"), VolumeM3, ExpectedM3, RelativeError));
	// UE 5.8.2 측정 1.71e-16, 영역 체적 0.162 m3로 정규화 (CLAUDE.md).
	TestTrue(TEXT("Normalized domain volume error < 1e-14"), RelativeError < 1.0e-14);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeRefinementTest,
	"VRG.Air.Octree.Fixed.LocalRefinement", VrgOctreeTestFlags)

bool FFixedOctreeRefinementTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	FString Error;
	const FAirGridParams P = OctreeTestParams();
	TestTrue(TEXT("Local build"), Tree.Build(P, Settings, OctreeTestRegions(), Error));
	TestEqual(TEXT("Two splits add 14 leaves to six roots"), Tree.GetNumLeaves(), 20);
	bool bFarCoarse = true;
	int32 FineLeaves = 0;
	for (int32 Index : Tree.GetLeafNodeIndices())
	{
		const FFixedOctreeNode& Node = Tree.GetNodes()[Index];
		if (Node.RootCellIndex != 0) { bFarCoarse &= Node.Depth == 0; }
		if (Node.Depth == 2) { ++FineLeaves; }
	}
	TestTrue(TEXT("Unselected roots remain coarse"), bFarCoarse);
	TestEqual(TEXT("Selected child is split into eight fine cells"), FineLeaves, 8);
	const TArray<FFixedOctreeNode> FirstNodes = Tree.GetNodes();
	TestTrue(TEXT("Repeated regions do not duplicate leaves"), Tree.Build(P, Settings,
		{ OctreeTestRegions()[0], OctreeTestRegions()[0] }, Error));
	TestEqual(TEXT("Deterministic node count"), Tree.GetNodes().Num(), FirstNodes.Num());
	for (int32 I = 0; I < FMath::Min(FirstNodes.Num(), Tree.GetNodes().Num()); ++I)
	{
		TestEqual(TEXT("Deterministic Morton key"), Tree.GetNodes()[I].MortonCode, FirstNodes[I].MortonCode);
	}
	const FVector SizeM(P.NumCellsX * P.CellSizeM, P.NumCellsY * P.CellSizeM, P.NumCellsZ * P.CellSizeM);
	TestTrue(TEXT("Outside/touching regions accepted"), Tree.Build(P, Settings,
		{ FBox(SizeM, SizeM + FVector(1.0)), FBox(FVector(-2.0), FVector(-1.0)) }, Error));
	TestEqual(TEXT("Outside boxes do not refine boundary cells"), Tree.GetNumLeaves(), 6);
	TestTrue(TEXT("Whole domain refinement"), Tree.Build(P, Settings, { FBox(FVector::ZeroVector, SizeM) }, Error));
	TestEqual(TEXT("Fine uniform equivalent count"), Tree.GetNumLeaves(), 384);
	AddInfo(TEXT("Local 20 leaves vs finest uniform 384 cells; h_min=0.075 m (fixture only)."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeUniformIdentityTest,
	"VRG.Air.Octree.Fixed.UniformIdentity", VrgOctreeTestFlags)

bool FFixedOctreeUniformIdentityTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Uniform;
	Uniform.Initialize(OctreeTestParams());
	for (int32 I = 0; I < Uniform.GetNumCells(); ++I) { Uniform.TemperatureK[I] = 280.0 + I * 7.25; }
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 0;
	FString Error;
	TestTrue(TEXT("Depth zero build"), Tree.Build(Uniform.GetParams(), Settings, OctreeTestRegions(), Error));
	TestEqual(TEXT("Depth zero matches uniform cell count"), Tree.GetNumLeaves(), Uniform.GetNumCells());
	TestTrue(TEXT("Import"), Tree.ImportUniform(Uniform, Error));
	TArray<double> Restored;
	TestTrue(TEXT("Restriction"), Tree.RestrictTemperatureToUniform(Restored));
	TestTrue(TEXT("Depth zero fields match exactly"), Restored == Uniform.TemperatureK);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeThermalTransferTest,
	"VRG.Air.Octree.Fixed.ThermalTransferConservation", VrgOctreeTestFlags)

bool FFixedOctreeThermalTransferTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid Uniform;
	FAirGridParams P = OctreeTestParams();
	P.NumCellsX = 5; P.NumCellsY = 3; P.NumCellsZ = 2;
	Uniform.Initialize(P);
	for (int32 I = 0; I < Uniform.GetNumCells(); ++I)
	{
		Uniform.TemperatureK[I] = 301.15 + 0.37 * I;
		Uniform.bSolidCell[I] = I % 7 == 0;
	}
	const TArray<double> Before = Uniform.TemperatureK;
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 3;
	FString Error;
	if (!TestTrue(TEXT("Mixed solid/fluid tree build"), Tree.Build(P, Settings,
		{ FBox(FVector(0.05), FVector(0.44, 0.53, 0.25)) }, Error))) { return false; }
	if (!TestTrue(TEXT("Import"), Tree.ImportUniform(Uniform, Error))) { return false; }
	const double ReferenceJ = Uniform.TotalThermalEnergyJ(P.ReferenceTempK);
	const double RelativeError = FMath::Abs(Tree.TotalThermalEnergyJ(P.ReferenceTempK) - ReferenceJ) / ReferenceJ;
	TArray<double> Restored;
	TestTrue(TEXT("Restriction"), Tree.RestrictTemperatureToUniform(Restored));
	double MaxErrorK = 0.0;
	for (int32 I = 0; I < Restored.Num(); ++I) { MaxErrorK = FMath::Max(MaxErrorK, FMath::Abs(Restored[I] - Before[I])); }
	bool bSolidsMatch = true;
	for (int32 Leaf = 0; Leaf < Tree.GetNumLeaves(); ++Leaf)
	{
		const int32 Root = Tree.GetNodes()[Tree.GetLeafNodeIndices()[Leaf]].RootCellIndex;
		bSolidsMatch &= Tree.GetFields().bSolidCell[Leaf] == Uniform.bSolidCell[Root];
	}
	TestTrue(TEXT("Solid classification inherited"), bSolidsMatch);
	TestTrue(TEXT("Uniform source not modified"), Before == Uniform.TemperatureK);
	AddInfo(FString::Printf(TEXT("Transfer reference %.17g J / normalized energy error %.17g / restriction max error %.17g K / leaves %d"),
		ReferenceJ, RelativeError, MaxErrorK, Tree.GetNumLeaves()));
	// 측정: 열량 1.57e-15 (7551.65 J 기준), 왕복 온도 2.90e-12 K.
	TestTrue(TEXT("Normalized transferred energy error < 1e-12"), RelativeError < 1.0e-12);
	TestTrue(TEXT("Restriction temperature error < 1e-10 K"), MaxErrorK < 1.0e-10);
	return true;
}

// S5-A의 동일 문제 비교는 정지 열원/셀 평균 전달까지다. 옥트리 CFD 해를 주장하지 않는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeHeatSourceTest,
	"VRG.Air.Octree.Fixed.StationaryHeatReference", VrgOctreeTestFlags)

bool FFixedOctreeHeatSourceTest::RunTest(const FString& Parameters)
{
	FAirSolver Solver;
	const FAirGridParams P = OctreeTestParams();
	Solver.Initialize(P, P.ReferenceTempK);
	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, 6);
	Input.CellHeatW[0] = 137.0;
	Input.CellHeatW[4] = 83.0;
	const double Dt = 0.017;
	for (int32 Step = 0; Step < 37; ++Step) { Solver.InjectHeat(Input, Dt); }
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = 2;
	FString Error;
	TestTrue(TEXT("Build"), Tree.Build(P, Settings, OctreeTestRegions(), Error));
	TestTrue(TEXT("Import uniform heat-source solution"), Tree.ImportUniform(Solver.GetGrid(), Error));
	const double ExpectedJ = 220.0 * Dt * 37;
	const double RelativeError = FMath::Abs(Tree.TotalThermalEnergyJ(P.ReferenceTempK) - ExpectedJ) / ExpectedJ;
	const double UniformRelativeError = FMath::Abs(Solver.GetGrid().TotalThermalEnergyJ(P.ReferenceTempK) - ExpectedJ) / ExpectedJ;
	const double ExpectedPeakK = P.ReferenceTempK + 137.0 * Dt * 37 / (P.DensityKgM3 * P.SpecificHeatJKgK * 0.027);
	double MaxTempK = 0.0;
	for (double TempK : Tree.GetFields().TemperatureK) { MaxTempK = FMath::Max(MaxTempK, TempK); }
	const double PeakErrorK = FMath::Abs(MaxTempK - ExpectedPeakK);
	AddInfo(FString::Printf(TEXT("Stationary heat reference %.17g J / octree normalized error %.17g / uniform normalized error %.17g / peak error %.17g K"),
		ExpectedJ, RelativeError, UniformRelativeError, PeakErrorK));
	// 측정: 138.38 J 기준 7.27e-14, 최대 온도 절대 오차 3.98e-13 K.
	TestTrue(TEXT("Octree heat matches analytic integral"), RelativeError < 1.0e-11);
	TestTrue(TEXT("Uniform reference matches analytic integral"), UniformRelativeError < 1.0e-11);
	TestTrue(TEXT("Peak temperature matches analytic value"), PeakErrorK < 1.0e-10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeInvalidInputTest,
	"VRG.Air.Octree.Fixed.RejectsInvalidAndBudget", VrgOctreeTestFlags)

bool FFixedOctreeInvalidInputTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	FAirGridParams P = OctreeTestParams();
	FString Error;
	TestTrue(TEXT("Initial valid build"), Tree.Build(P, Settings, {}, Error));
	Settings.MaxLeaves = 6;
	TestFalse(TEXT("Split exceeding budget fails"), Tree.Build(P, Settings, OctreeTestRegions(), Error));
	TestEqual(TEXT("No stale/partial leaves"), Tree.GetNumLeaves(), 0);
	TestEqual(TEXT("No stale/partial nodes"), Tree.GetNodes().Num(), 0);
	TestFalse(TEXT("Budget diagnostic"), Error.IsEmpty());
	Settings.MaxLeaves = 5;
	TestFalse(TEXT("Root budget failure"), Tree.Build(P, Settings, {}, Error));
	Settings.MaxLeaves = 1000;
	Settings.MaxDepth = 7;
	TestFalse(TEXT("Excessive Morton depth rejected"), Tree.Build(P, Settings, {}, Error));
	Settings.MaxDepth = 1;
	TestFalse(TEXT("Empty bounds rejected"), Tree.Build(P, Settings, { FBox(ForceInit) }, Error));
	P.CellSizeM = 0.0;
	TestFalse(TEXT("Zero cell size rejected"), Tree.Build(P, Settings, {}, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFixedOctreeInvalidTransferTest,
	"VRG.Air.Octree.Fixed.RejectsIncompatibleTransfer", VrgOctreeTestFlags)

bool FFixedOctreeInvalidTransferTest::RunTest(const FString& Parameters)
{
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings Settings;
	FAirGridParams P = OctreeTestParams();
	FUniformAirGrid Uniform;
	Uniform.Initialize(P);
	FString Error;
	TestTrue(TEXT("Build"), Tree.Build(P, Settings, OctreeTestRegions(), Error));
	TestTrue(TEXT("Valid import"), Tree.ImportUniform(Uniform, Error));
	Uniform.TemperatureK[0] = -1.0;
	TestFalse(TEXT("Nonphysical temperature rejected"), Tree.ImportUniform(Uniform, Error));
	TestFalse(TEXT("Stale fields cleared"), Tree.HasFields());
	TArray<double> Out;
	TestFalse(TEXT("Cannot restrict missing fields"), Tree.RestrictTemperatureToUniform(Out));
	P.CellSizeM = 0.5;
	Uniform.Initialize(P);
	TestFalse(TEXT("Different domain rejected"), Tree.ImportUniform(Uniform, Error));
	P = OctreeTestParams();
	P.DensityKgM3 *= 2.0;
	Uniform.Initialize(P);
	TestFalse(TEXT("Different heat capacity rejected"), Tree.ImportUniform(Uniform, Error));
	Uniform.Initialize(OctreeTestParams());
	Uniform.bSolidCell.Pop();
	TestFalse(TEXT("Incomplete solid mask rejected"), Tree.ImportUniform(Uniform, Error));
	return true;
}

#endif
