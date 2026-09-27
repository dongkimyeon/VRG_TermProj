// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/OctreeAdaptController.h"

#if WITH_DEV_AUTOMATION_TESTS

// S5-D: automatic octree adaptation from temperature / velocity indicators, without chattering.

namespace
{
	constexpr EAutomationTestFlags AdaptFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FAirGridParams SmallGrid(int32 NX, int32 NY, int32 NZ)
	{
		FAirGridParams P; P.NumCellsX = NX; P.NumCellsY = NY; P.NumCellsZ = NZ; P.CellSizeM = .25; return P;
	}

	// Temperature linear in X with the given slope [K/m]: the indicator is exactly that slope in every cell
	// (central differences inside, one-sided at the walls, all equal for a linear field).
	void SetSlope(FUniformAirGrid& G, double SlopeKPerM)
	{
		for (int32 Z = 0; Z < G.GetNumCellsZ(); ++Z)
			for (int32 Y = 0; Y < G.GetNumCellsY(); ++Y)
				for (int32 X = 0; X < G.GetNumCellsX(); ++X)
					G.TemperatureK[G.CellIndex(X, Y, Z)] = 297.15 + SlopeKPerM * (X + .5) * G.GetCellSizeM();
	}
}

// The controller alone: hysteresis, hold time, evaluation interval, commit / discard and the budget.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeAdaptHysteresisTest, "VRG.Air.Octree.AutoAdapt.HysteresisAndHold", AdaptFlags)
bool FOctreeAdaptHysteresisTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid G; G.Initialize(SmallGrid(8, 4, 4)); G.ResetToUniform(297.15);
	const int32 N = G.GetNumCells();
	FOctreeAdaptSettings S; S.RefineGradientKPerM = 8.0; S.CoarsenGradientKPerM = 4.0; S.RefineSpeedMs = 100.0; S.CoarsenSpeedMs = 50.0;
	S.MinHoldS = 2.0; S.IntervalS = .5;
	TestTrue(TEXT("Settings valid"), S.Validate().IsEmpty());
	FOctreeAdaptSettings Bad = S; Bad.CoarsenGradientKPerM = 9.0;
	TestFalse(TEXT("No hysteresis is refused"), Bad.Validate().IsEmpty());

	FOctreeAdaptController C; TArray<FBox> Boxes;
	int32 Proposals = 0;
	auto Eval = [&](double T, double Slope) { SetSlope(G, Slope); const bool bP = C.Update(G, S, T, Boxes); if (bP) { ++Proposals; C.Commit(); } return bP; };

	// 1) Below refine, then oscillating between the thresholds: nothing happens.
	double T = 0.0;
	Eval(T, 6.0);
	for (int32 K = 0; K < 10; ++K) Eval(T += .5, K % 2 ? 7.0 : 5.0);
	const int32 QuietProposals = Proposals;
	// 2) Above refine: every root refines in one change.
	TestTrue(TEXT("Refines above threshold"), Eval(T += .5, 10.0));
	const int32 AfterRefine = C.GetNumRefined();
	// 3) Oscillating between the thresholds again: stays refined, no change.
	const int32 BeforeOsc = Proposals;
	for (int32 K = 0; K < 10; ++K) Eval(T += .5, K % 2 ? 7.0 : 5.0);
	const int32 OscProposals = Proposals - BeforeOsc;
	// 4) Below coarsen (hold elapsed): merges.
	TestTrue(TEXT("Merges below coarsen threshold"), Eval(T += .5, 2.0));
	// 5) Straight back up within the hold time: refused; after the hold: refines again.
	const bool bWithinHold = Eval(T += .5, 10.0);
	const bool bAfterHold = Eval(T += 2.0, 10.0);
	// 6) The interval: a second call at the same time does not evaluate.
	SetSlope(G, 2.0);
	const bool bSameTime = C.Update(G, S, T, Boxes);
	// 7) Discard: a proposal that is not committed is proposed again at the next evaluation.
	SetSlope(G, 2.0);
	const bool bP1 = C.Update(G, S, T += 2.5, Boxes); C.Discard();
	const bool bP2 = C.Update(G, S, T += .5, Boxes);
	if (bP2) C.Commit();

	AddInfo(FString::Printf(TEXT("사이 진동 제안 %d/%d회 | 정밀 루트 %d/%d | 보류 중 재정밀 %d, 보류 뒤 %d | 같은 시각 재평가 %d | 버림 뒤 재제안 %d/%d | 변경 %lld회, 루트 전환 %lld"),
		QuietProposals, OscProposals, AfterRefine, N, bWithinHold, bAfterHold, bSameTime, bP1, bP2, C.GetNumTopologyChanges(), C.GetNumRootFlips()));
	TestEqual(TEXT("No change while oscillating below refine"), QuietProposals, 0);
	TestEqual(TEXT("All roots refined"), AfterRefine, N);
	TestEqual(TEXT("No change while oscillating between thresholds"), OscProposals, 0);
	TestFalse(TEXT("Hold time blocks an immediate flip back"), bWithinHold);
	TestTrue(TEXT("After the hold it may flip"), bAfterHold);
	TestFalse(TEXT("One evaluation per interval"), bSameTime);
	TestTrue(TEXT("Discarded proposal is proposed again"), bP1 && bP2);
	TestEqual(TEXT("Exactly four committed changes"), C.GetNumTopologyChanges(), int64(4));

	// 8) Budget: slope grows with X (T = a x^2), so the largest gradients are at high X; cap 8 keeps the top 8.
	{
		FOctreeAdaptController B; FOctreeAdaptSettings SB = S; SB.MaxRefinedRoots = 8;
		for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y < 4; ++Y) for (int32 X = 0; X < 8; ++X)
		{
			const double XM = (X + .5) * .25;
			G.TemperatureK[G.CellIndex(X, Y, Z)] = 297.15 + 10.0 * XM * XM;
		}
		B.Update(G, SB, 0.0, Boxes); B.Commit();
		double MinOn = 1e30, MaxOff = 0.0;
		for (int32 I = 0; I < N; ++I)
			(B.IsRefined(I) ? MinOn : MaxOff) = B.IsRefined(I) ? FMath::Min(MinOn, B.GetLastGradientKPerM()[I]) : FMath::Max(MaxOff, B.GetLastGradientKPerM()[I]);
		AddInfo(FString::Printf(TEXT("예산 8: 정밀 %d개, 켜진 최소 기울기 %.3f K/m >= 꺼진 최대 %.3f K/m"), B.GetNumRefined(), MinOn, MaxOff));
		TestEqual(TEXT("Budget respected"), B.GetNumRefined(), 8);
		TestTrue(TEXT("Largest indicators win"), MinOn >= MaxOff);
	}
	return true;
}

// The solver with automatic adaptation: a heat source refines around itself, the refinement follows when the source
// moves, energy is conserved through every regrid, and the hottest cell is where the uniform reference puts it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeAdaptPlumeTest, "VRG.Air.Octree.AutoAdapt.FollowsPlumeConserving", AdaptFlags)
bool FOctreeAdaptPlumeTest::RunTest(const FString& Parameters)
{
	const FAirGridParams P = SmallGrid(12, 6, 8);
	FAirSolver Tree, Uniform;
	Tree.Initialize(P, 297.15); Uniform.Initialize(P, 297.15);
	for (FAirSolver* A : {&Tree, &Uniform})
	{
		A->GetMutablePressureSettings().NormalizedResidualTarget = 1e-8;
		A->GetMutablePressureSettings().MaxIterations = 200;
	}
	FFixedOctreeSettings OS; OS.MaxDepth = 1; FString Error;
	if (!TestTrue(TEXT("Configure"), Tree.ConfigureOctree(OS, {}, Error))) return false;
	FOctreeAdaptSettings S; S.RefineGradientKPerM = 6.0; S.CoarsenGradientKPerM = 3.0; S.RefineSpeedMs = .3; S.CoarsenSpeedMs = .15;
	S.MinHoldS = 1.0; S.IntervalS = .2;
	if (!TestTrue(TEXT("Auto adapt on"), Tree.SetOctreeAutoAdapt(true, S, Error))) return false;

	const FUniformAirGrid& G = Tree.GetGrid();
	const int32 SourceA = G.CellIndex(2, 3, 1), SourceB = G.CellIndex(9, 2, 1);
	FAirSolverInput In; In.CellHeatW.Init(0.0, G.GetNumCells());
	In.CellHeatW[SourceA] = 150.0;
	constexpr double Dt = .02;
	for (int32 I = 0; I < 300; ++I) { Tree.Step(In, Dt); Uniform.Step(In, Dt); }
	const bool bARefined = Tree.GetOctreeAdaptController().IsRefined(SourceA);
	const int64 ChangesA = Tree.GetOctreeAdaptController().GetNumTopologyChanges();
	const int32 LeavesA = Tree.GetOctreeSolver().GetGrid().GetNumLeaves();

	// Hot spot judgement against the uniform reference (same input, same steps).
	auto Hottest = [](const FAirSolver& A) { int32 Best = 0; for (int32 I = 1; I < A.GetGrid().GetNumCells(); ++I) if (A.GetGrid().TemperatureK[I] > A.GetGrid().TemperatureK[Best]) Best = I; return Best; };
	const int32 HotTree = Hottest(Tree), HotUniform = Hottest(Uniform);
	const double MaxTree = G.TemperatureK[HotTree], MaxUniform = Uniform.GetGrid().TemperatureK[HotUniform];

	// Move the source to the other side.
	In.CellHeatW[SourceA] = 0.0; In.CellHeatW[SourceB] = 150.0;
	for (int32 I = 0; I < 300; ++I) Tree.Step(In, Dt);
	const FOctreeAdaptController& C = Tree.GetOctreeAdaptController();
	const FAirSolverSnapshot Snap = Tree.MakeSnapshot(600 * Dt);

	AddInfo(FString::Printf(TEXT("6 s: 원 A 정밀 %d, 변경 %lld회, 리프 %d | 최고 온도 셀 옥트리 %d / 균일 %d, %.4f / %.4f K (차 %.4f K, 상승분 %.3f K)"),
		bARefined, ChangesA, LeavesA, HotTree, HotUniform, MaxTree, MaxUniform, MaxTree - MaxUniform, MaxUniform - 297.15));
	AddInfo(FString::Printf(TEXT("12 s: 원 B 정밀 %d, 정밀 루트 %d개, 변경 %lld회 (루트 전환 %lld), 실패 %lld | 열수지 %.3e (유입 %.1f J) | 발산 %.3e | 압력 실패 %lld회"),
		C.IsRefined(SourceB), C.GetNumRefined(), C.GetNumTopologyChanges(), C.GetNumRootFlips(), Tree.GetNumAutoAdaptFailures(),
		Tree.GetNormalizedEnergyResidual(), Tree.GetEnergyInJ(), Snap.NormalizedDivergence, Tree.GetNumPressureFailures()));

	TestTrue(TEXT("Source A refined"), bARefined);
	TestTrue(TEXT("Refinement follows to source B"), C.IsRefined(SourceB));
	TestTrue(TEXT("Leaves added"), LeavesA > G.GetNumCells());
	TestEqual(TEXT("No failed adaptation"), Tree.GetNumAutoAdaptFailures(), int64(0));
	TestEqual(TEXT("All pressure solves converge"), Tree.GetNumPressureFailures(), int64(0));
	TestTrue(TEXT("Energy conserved through every regrid"), FMath::Abs(Tree.GetNormalizedEnergyResidual()) < 1e-10);
	TestTrue(TEXT("Divergence free"), Snap.NormalizedDivergence < 1e-6);
	// At most one change per interval by construction; 12 s / 0.2 s = 60.
	TestTrue(TEXT("Changes bounded by the interval"), C.GetNumTopologyChanges() <= 60);
	TestEqual(TEXT("Hot spot in the same root cell as the uniform reference"), HotTree, HotUniform);
	return true;
}

// S5-E: the step cost steers the root budget into a dead band and stays there; every evaluation is recorded.
// The control law runs on a synthetic cost (0.01 ms per leaf) so it is deterministic; a real solver run then
// records leaves and measured time without asserting on them (D10: measured, machine dependent).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeLodTest, "VRG.Air.Octree.AutoAdapt.PerformanceLod", AdaptFlags)
bool FOctreeLodTest::RunTest(const FString& Parameters)
{
	FUniformAirGrid G; G.Initialize(SmallGrid(8, 4, 4)); G.ResetToUniform(297.15);
	SetSlope(G, 10.0);  // every root wants refining (above 8 K/m)
	const int32 N = G.GetNumCells();
	FOctreeAdaptSettings S; S.RefineGradientKPerM = 8.0; S.CoarsenGradientKPerM = 4.0; S.RefineSpeedMs = 100.0; S.CoarsenSpeedMs = 50.0;
	S.MinHoldS = 2.0; S.IntervalS = .5; S.TargetStepMs = 4.0; S.LodBand = .2;
	// Depth 1: a refined root is 8 leaves -> leaves = N + 7 R. Cost 0.01 ms per leaf -> band [3.2, 4.8] ms
	// means R in [(320 - 128) / 7, (480 - 128) / 7] = [27.4, 50.3].
	auto LeavesOf = [&](int32 R) { return N + 7 * R; };
	FOctreeAdaptController C; TArray<FBox> Boxes;
	TArray<int32> Refined;
	int32 ChangesInLastHalf = 0;
	for (int32 K = 0; K < 80; ++K)
	{
		const int32 Leaves = LeavesOf(C.GetNumRefined());
		for (int32 Step = 0; Step < 25; ++Step) C.ReportStepCost(.01 * Leaves, Leaves);
		const int32 BudgetChangesBefore = C.GetNumBudgetChanges();
		if (C.Update(G, S, K * .5, Boxes)) C.Commit();
		if (K >= 40) ChangesInLastHalf += C.GetNumBudgetChanges() - BudgetChangesBefore;
		Refined.Add(C.GetNumRefined());
	}
	const int32 Final = C.GetNumRefined();
	AddInfo(FString::Printf(TEXT("목표 4.0 ms ±20%%: 정밀 루트 %d개 (리프 %d, 비용 %.2f ms) | 예산 %d | 예산 변경 %d회 (뒤 절반 %d회) | 기록 %d개"),
		Final, LeavesOf(Final), .01 * LeavesOf(Final), C.GetEffectiveBudget(), C.GetNumBudgetChanges(), ChangesInLastHalf, C.GetLodHistory().Num()));
	TestTrue(TEXT("Settles inside the band"), Final >= 28 && Final <= 50);
	TestEqual(TEXT("No budget changes once settled"), ChangesInLastHalf, 0);
	TestEqual(TEXT("Every evaluation recorded"), C.GetLodHistory().Num(), 80);

	// Off: no budget, no record.
	{
		FOctreeAdaptController Off; FOctreeAdaptSettings SO = S; SO.TargetStepMs = 0.0;
		Off.ReportStepCost(100.0, 1000);
		if (Off.Update(G, SO, 0.0, Boxes)) Off.Commit();
		TestTrue(TEXT("LOD off: all refine, nothing recorded"), Off.GetNumRefined() == N && Off.GetLodHistory().Num() == 0);
	}

	// Real solver: record leaves and measured cost (not asserted; machine dependent).
	{
		const FAirGridParams P = SmallGrid(12, 6, 8);
		FAirSolver Tree; Tree.Initialize(P, 297.15);
		Tree.GetMutablePressureSettings().NormalizedResidualTarget = 1e-8;
		FFixedOctreeSettings OS; OS.MaxDepth = 1; FString Error;
		Tree.ConfigureOctree(OS, {}, Error);
		FOctreeAdaptSettings SR; SR.RefineGradientKPerM = 2.0; SR.CoarsenGradientKPerM = 1.0; SR.RefineSpeedMs = .1; SR.CoarsenSpeedMs = .05;
		SR.MinHoldS = .5; SR.IntervalS = .2; SR.TargetStepMs = 1.0;
		Tree.SetOctreeAutoAdapt(true, SR, Error);
		FAirSolverInput In; In.CellHeatW.Init(0.0, Tree.GetGrid().GetNumCells());
		In.CellHeatW[Tree.GetGrid().CellIndex(6, 3, 1)] = 400.0;
		for (int32 I = 0; I < 300; ++I) Tree.Step(In, .02);
		const TArray<FOctreeLodSample>& H = Tree.GetOctreeAdaptController().GetLodHistory();
		FString Trace;
		for (int32 I = 0; I < H.Num(); I += FMath::Max(1, H.Num() / 6))
			Trace += FString::Printf(TEXT(" [%.1f s 리프 %d, %.3f ms, 예산 %d]"), H[I].SimTimeS, H[I].NumLeaves, H[I].MeanStepMs, H[I].BudgetRoots);
		AddInfo(FString::Printf(TEXT("실제 솔버(목표 1.0 ms, 측정값이며 보장값 아님):%s | 열수지 %.3e | 실패 %lld"),
			*Trace, Tree.GetNormalizedEnergyResidual(), Tree.GetNumAutoAdaptFailures()));
		TestTrue(TEXT("Real run recorded"), H.Num() > 0);
		TestTrue(TEXT("Energy conserved with LOD regrids"), FMath::Abs(Tree.GetNormalizedEnergyResidual()) < 1e-10);
		TestEqual(TEXT("No failed adaptation"), Tree.GetNumAutoAdaptFailures(), int64(0));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
