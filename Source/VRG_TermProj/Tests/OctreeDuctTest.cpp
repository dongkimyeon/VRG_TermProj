// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr auto OctreeDuctTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}

// S5 finish (2026-09-27): fan ducts on the octree path. The scene of VRG.Air.Duct.ThroughFlowConservation (a solid body
// with a -X duct through it), plus a duct coil and fan heat, at octree depth 0 and depth 1 refined around the body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOctreeDuctTest, "VRG.Air.Octree.Duct.ConservationCoilAndMapping", OctreeDuctTestFlags)
bool FOctreeDuctTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 12, NY = 6, NZ = 6;
	constexpr double H = .25, Q = .02;
	FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;

	// Depth -1: uniform (no octree). Heat / coil / fan / buoyancy as flags.
	auto Setup = [&](FAirSolver& Air, FAirSolverInput& In, int32 Depth, bool bHeat, bool bSolidBody = true)
	{
		Air.Initialize(GP, 297.15);
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-10;
		Air.GetMutablePressureSettings().MaxIterations = 200;
		FUniformAirGrid& G = Air.GetMutableGrid();
		if (bSolidBody)
			for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y) for (int32 X = 5; X <= 6; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		In = FAirSolverInput(); In.bEnableBuoyancy = bHeat; In.CellHeatW.Init(0.0, G.GetNumCells());
		In.NumDucts = 1;
		for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y)
		{
			const double V = -Q / (6 * H * H);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(7, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(7, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(0); In.DuctFaceVelocityMs.Add(V);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(5, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(4, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(1); In.DuctFaceVelocityMs.Add(V);
			if (bHeat) In.CellHeatW[G.CellIndex(8, Y, Z)] = 500.0 / 6.0;  // heat upstream of the intake
		}
		if (bHeat) { In.DuctCoilUAWK = { 20.0 }; In.DuctCoilWaterTempK = { 291.15 }; In.DuctFanHeatW = { 30.0 }; }
		if (Depth >= 0)
		{
			FFixedOctreeSettings S; S.MaxDepth = Depth; S.MaxLeaves = 100000;
			TArray<FBox> Boxes;
			if (Depth > 0) Boxes.Add(FBox(FVector(.75, .25, 0.0), FVector(2.0, 1.25, 1.25)));  // body and the cells on both faces
			FString Error;
			if (!Air.ConfigureOctree(S, Boxes, Error)) AddError(Error);
		}
	};

	// 1) No heat: the duct moves uniform air around without changing any temperature, mapped and divergence free.
	for (const int32 Depth : { 0, 1 })
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, Depth, false);
		for (int32 S = 0; S < 200; ++S) Air.Step(In, .02);
		const FUniformAirGrid& G = Air.GetGrid();
		double MaxDevK = 0.0;
		for (int32 I = 0; I < G.GetNumCells(); ++I) if (!G.bSolidCell[I]) MaxDevK = FMath::Max(MaxDevK, FMath::Abs(G.TemperatureK[I] - 297.15));
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(4.0);
		const FOctreeAirSolver& O = Air.GetOctreeSolver();
		AddInfo(FString::Printf(TEXT("옥트리 깊이 %d, 열 없음 200스텝: 최대 온도 변화 %.3e K | 옥트리 덕트 면 %d (입력 12) | 정규화 발산 %.3e | 거부 %d | 리프 %d | 배기 다음 면 속도 %.4f / %.4f / %.4f m/s (X=4,3,2; 덕트 %.4f)"),
			Depth, MaxDevK, O.LastNumMappedDuctFaces, Snap.NormalizedDivergence, Snap.bDuctsRejected, O.GetGrid().GetNumLeaves(),
			G.VelocityXMs[G.FaceXIndex(4, 2, 2)], G.VelocityXMs[G.FaceXIndex(3, 2, 2)], G.VelocityXMs[G.FaceXIndex(2, 2, 2)], -Q / (6 * H * H)));
		TestTrue(TEXT("Octree path in use"), Air.IsUsingOctree());
		TestFalse(TEXT("Ducts accepted"), Snap.bDuctsRejected);
		TestEqual(TEXT("Mapped octree duct faces"), O.LastNumMappedDuctFaces, Depth == 0 ? 12 : 48);
		TestTrue(TEXT("No spurious heat"), MaxDevK < 1e-10);
		TestTrue(TEXT("Divergence free with duct sources"), Snap.NormalizedDivergence < 1e-6);
		// Air really moves through the body: a -X jet leaves the exhaust and decays downstream. The octree's first-order
		// upwind momentum is more diffusive than the uniform semi-Lagrangian path (whose test asks for half the duct speed
		// at this face), so the octree check asks for a quarter; the values are recorded in CLAUDE.md.
		const double U4 = G.VelocityXMs[G.FaceXIndex(4, 2, 2)], U3 = G.VelocityXMs[G.FaceXIndex(3, 2, 2)], U2 = G.VelocityXMs[G.FaceXIndex(2, 2, 2)];
		TestTrue(TEXT("Jet leaves the exhaust face and decays"), U4 < -.25 * Q / (6 * H * H) && U4 < U3 && U3 < U2 && U2 < 0.0);
	}

	// 2) Heat, coil, fan, buoyancy; 3) the same on the uniform grid (A/B, not the same momentum scheme).
	double CoilW[3] = { 0, 0, 0 }, OutK[3] = { 0, 0, 0 };
	for (const int32 Depth : { -1, 0, 1 })
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, Depth, true);
		for (int32 S = 0; S < 500; ++S) Air.Step(In, .02);
		const FUniformAirGrid& G = Air.GetGrid();
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(10.0);
		CoilW[Depth + 1] = Air.GetLastDuctCoilRemovedW();
		OutK[Depth + 1] = G.TemperatureK[G.CellIndex(4, 2, 2)];
		AddInfo(FString::Printf(TEXT("%s: 열수지 %.3e (유입 %.1f J) | 덕트 코일 %.3f W, 팬 %.3f W | 배기 셀 %.4f C | 정규화 발산 %.3e | 압력 %s"),
			Depth < 0 ? TEXT("균일") : *FString::Printf(TEXT("옥트리 깊이 %d"), Depth), Air.GetNormalizedEnergyResidual(), Air.GetEnergyInJ(),
			CoilW[Depth + 1], Air.GetLastDuctFanHeatW(), OutK[Depth + 1] - 273.15, Snap.NormalizedDivergence, Snap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
		TestTrue(TEXT("Ledger closes (heat, coil, fan)"), FMath::Abs(Air.GetNormalizedEnergyResidual()) < 1e-10);
		TestTrue(TEXT("Coil removes heat"), CoilW[Depth + 1] > 0.0);
		TestTrue(TEXT("Fan heat booked"), FMath::IsNearlyEqual(Air.GetLastDuctFanHeatW(), 30.0, 1e-9));
		TestTrue(TEXT("Divergence free"), Snap.NormalizedDivergence < 1e-6);
	}
	AddInfo(FString::Printf(TEXT("A/B 코일 제거 균일 %.3f / 깊이0 %.3f / 깊이1 %.3f W (균일 대비 %.1f%% / %.1f%%)"), CoilW[0], CoilW[1], CoilW[2],
		100.0 * (CoilW[1] / CoilW[0] - 1.0), 100.0 * (CoilW[2] / CoilW[0] - 1.0)));

	// 4) A duct face with no solid behind it is not a duct face of this octree: rejected and reported (D5 spirit).
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 0, false, false);
		AddExpectedMessage(TEXT("옥트리 덕트 연결 실패"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		Air.Step(In, .02);
		TestTrue(TEXT("Unmappable ducts are rejected"), Air.WereDuctsRejected());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
