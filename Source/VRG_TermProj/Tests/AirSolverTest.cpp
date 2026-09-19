// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgAirSolverTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	FAirGridParams MakeGridParams(int32 NumCells, double CellSizeM)
	{
		FAirGridParams Params;
		Params.NumCellsX = NumCells;
		Params.NumCellsY = NumCells;
		Params.NumCellsZ = NumCells;
		Params.CellSizeM = CellSizeM;
		return Params;
	}
}

// 해석해 1: 균일한 X방향 흐름에서 U*dt를 정확히 셀 한 칸으로 맞추면
// 온도장이 수치 확산 없이 정확히 한 칸 밀린다.
//
// 풍상차분은 Courant 수가 1보다 작을 때 수치 확산이 생기지만, 정확히 1이면
// 상류 셀의 값이 그대로 건너와 오차가 0이다. 이류 구현이 맞는지 보는 깨끗한 시험이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirAdvectionShiftTest,
	"VRG.Air.Advection.ExactCellShiftAnalytic", VrgAirSolverTestFlags)

bool FAirAdvectionShiftTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 16;
	const double CellSizeM = 0.25;

	FAirSolver Solver;
	Solver.Initialize(MakeGridParams(NumCells, CellSizeM), 297.15);

	FUniformAirGrid& Grid = Solver.GetMutableGrid();

	// X방향 균일 흐름. 모든 X면에 같은 속도를 준다.
	const double SpeedMs = 1.0;
	for (double& Value : Grid.VelocityXMs)
	{
		Value = SpeedMs;
	}

	// 계단 모양 온도장. 한 칸 밀리면 눈에 띄게 달라진다.
	const int32 StepX = 4;
	const double HotTempK = 320.0;
	const double ColdTempK = 297.15;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = 0; X < NumCells; ++X)
			{
				Grid.TemperatureK[Grid.CellIndex(X, Y, Z)] = (X < StepX) ? HotTempK : ColdTempK;
			}
		}
	}

	// U*dt = h 가 되게 스텝을 잡는다.
	// 풍상차분에서 Courant 수가 정확히 1이면 수치 확산이 0이고 정확히 한 칸 밀린다.
	const double DeltaTimeS = CellSizeM / SpeedMs;

	// Step()이 아니라 이류 단계만 직접 부른다.
	// 막힌 상자에서는 균일 관통류가 애초에 불가능해서(벽면 법선 속도가 0이어야 한다)
	// 투영이 이 속도장을 통째로 없앤다. 이류 구현만 보려면 그 단계를 건너뛰어야 한다.
	Solver.AdvectTemperature(DeltaTimeS);

	// 기대: 계단이 한 칸 오른쪽으로 밀린다.
	// 왼쪽 벽면은 플럭스가 0이라 첫 칸의 뜨거운 값이 그대로 남는다.
	double MaxAbsErrorK = 0.0;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = 0; X < NumCells; ++X)
			{
				const double ExpectedK = (X < StepX + 1) ? HotTempK : ColdTempK;
				const double ActualK = Grid.TemperatureK[Grid.CellIndex(X, Y, Z)];
				MaxAbsErrorK = FMath::Max(MaxAbsErrorK, FMath::Abs(ActualK - ExpectedK));
			}
		}
	}

	const double StepSizeK = HotTempK - ColdTempK;
	AddInfo(FString::Printf(TEXT("U=%.1f m/s, dt=%.4f s, h=%.2f m -> U*dt/h = %.1f"),
		SpeedMs, DeltaTimeS, CellSizeM, SpeedMs * DeltaTimeS / CellSizeM));
	AddInfo(FString::Printf(TEXT("계단 높이 %.2f K 기준 절대 오차 %.3e K (정규화 %.3e)"),
		StepSizeK, MaxAbsErrorK, MaxAbsErrorK / StepSizeK));

	TestTrue(TEXT("정확히 한 칸 밀려야 한다 (수치 확산 없음)"), MaxAbsErrorK / StepSizeK < 1.0e-12);

	return true;
}

// 투영 뒤에는 속도장의 발산이 0이어야 한다.
// 발산이 남으면 공기가 어디선가 생기거나 사라진다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirProjectionDivergenceFreeTest,
	"VRG.Air.Projection.DivergenceFree", VrgAirSolverTestFlags)

bool FAirProjectionDivergenceFreeTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 16;
	const double CellSizeM = 0.25;

	FAirSolver Solver;
	Solver.Initialize(MakeGridParams(NumCells, CellSizeM), 297.15);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-8;
	Solver.GetMutablePressureSettings().MaxIterations = 4000;

	FUniformAirGrid& Grid = Solver.GetMutableGrid();
	const double LengthM = NumCells * CellSizeM;

	// 비발산 소용돌이 + 발산이 큰 교란을 섞어 넣는다.
	//
	// 소용돌이는 흐름함수 psi = A*sin(pi*x/L)*sin(pi*z/L)에서 온 것이라
	// 연속 수준에서 발산이 0이고 네 벽에서 법선 속도가 0이다.
	// 투영은 Helmholtz 분해에서 발산 성분만 걷어내야 하므로,
	// **발산은 사라지고 소용돌이는 남아야** 한다.
	// 둘을 함께 보지 않으면 "속도를 전부 0으로 만들어도 통과"하는 검사가 된다.
	const double SwirlMs = 1.0;
	const double DivergentMs = 2.0;

	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = 0; X <= NumCells; ++X)
			{
				const double PosXM = X * CellSizeM;
				const double PosZM = (Z + 0.5) * CellSizeM;
				const double Swirl = SwirlMs
					* FMath::Sin(UE_DOUBLE_PI * PosXM / LengthM)
					* FMath::Cos(UE_DOUBLE_PI * PosZM / LengthM);
				const double Divergent = DivergentMs * (PosXM / LengthM - 0.5);
				Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] = Swirl + Divergent;
			}
		}
	}

	for (int32 Z = 0; Z <= NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = 0; X < NumCells; ++X)
			{
				const double PosXM = (X + 0.5) * CellSizeM;
				const double PosZM = Z * CellSizeM;
				const double Swirl = -SwirlMs
					* FMath::Cos(UE_DOUBLE_PI * PosXM / LengthM)
					* FMath::Sin(UE_DOUBLE_PI * PosZM / LengthM);
				Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] = Swirl;
			}
		}
	}

	const double DivergenceBeforePerS = Grid.MaxAbsDivergencePerS();

	// 이류를 섞지 않고 투영만 본다.
	Solver.ApplyWallBoundary();
	Solver.ProjectToDivergenceFree(0.02);

	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(0.02);
	const double DivergenceRatio = Snapshot.MaxAbsDivergencePerS / DivergenceBeforePerS;

	AddInfo(FString::Printf(TEXT("투영 전 최대 발산 %.4f 1/s -> 투영 후 %.3e 1/s (비율 %.3e)"),
		DivergenceBeforePerS, Snapshot.MaxAbsDivergencePerS, DivergenceRatio));
	AddInfo(FString::Printf(TEXT("투영 후 최대 속도 %.4f m/s | h=%.2f m 기준 정규화 발산 %.3e (무차원)"),
		Snapshot.MaxSpeedMs, CellSizeM, Snapshot.NormalizedDivergence));
	AddInfo(FString::Printf(TEXT("압력 %s (반복 %d회, 정규화 잔차 %.3e)"),
		Snapshot.bPressureConverged ? TEXT("수렴") : TEXT("수렴 실패"),
		Snapshot.PressureIterations, Snapshot.PressureNormalizedResidual));

	TestTrue(TEXT("압력 풀이가 수렴해야 한다"), Snapshot.bPressureConverged);
	TestTrue(TEXT("투영 뒤 발산이 투영 전의 1e-6 미만으로 줄어야 한다"), DivergenceRatio < 1.0e-6);
	TestTrue(TEXT("투영 뒤 정규화 발산이 0에 가까워야 한다"), Snapshot.NormalizedDivergence < 1.0e-6);

	// 비발산 성분은 살아남아야 한다. 속도를 전부 0으로 만드는 것은 투영이 아니다.
	AddInfo(FString::Printf(TEXT("소용돌이 세기 %.1f m/s를 넣었고 투영 뒤 최대 속도가 %.4f m/s다"),
		SwirlMs, Snapshot.MaxSpeedMs));
	TestTrue(TEXT("비발산 소용돌이가 남아 있어야 한다"), Snapshot.MaxSpeedMs > 0.5 * SwirlMs);

	return true;
}

// 막힌 상자에서 넣은 열은 전부 공기에 쌓여 있어야 한다 (축적량 포함).
// 이류는 열을 옮길 뿐 만들거나 없애지 않는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirEnergyConservationTest,
	"VRG.Air.Energy.ClosedBoxConservation", VrgAirSolverTestFlags)

bool FAirEnergyConservationTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 12;
	const double CellSizeM = 0.25;
	const double InitialTempK = 297.15;

	FAirSolver Solver;
	Solver.Initialize(MakeGridParams(NumCells, CellSizeM), InitialTempK);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-7;
	Solver.GetMutablePressureSettings().MaxIterations = 2000;

	const FUniformAirGrid& Grid = Solver.GetGrid();

	// 바닥 가운데에 열원을 둔다. 부력으로 기둥이 올라가는 구성이다.
	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());

	const double TotalHeatW = 2000.0;
	TArray<int32> SourceCells;
	for (int32 Y = 5; Y <= 6; ++Y)
	{
		for (int32 X = 5; X <= 6; ++X)
		{
			SourceCells.Add(Grid.CellIndex(X, Y, 1));
		}
	}
	for (const int32 Index : SourceCells)
	{
		Input.CellHeatW[Index] = TotalHeatW / SourceCells.Num();
	}

	const double DeltaTimeS = 0.02;
	const int32 NumSteps = 500;
	bool bAllPressureConverged = true;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Solver.Step(Input, DeltaTimeS);
		bAllPressureConverged = bAllPressureConverged && Solver.GetLastPressureResult().bConverged;
	}

	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(DeltaTimeS * NumSteps);
	const double NormalizedResidual = FMath::Abs(Solver.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("격자 %d^3, h=%.2f m, t=%.1f s, 열원 %.0f W (셀 %d개)"),
		NumCells, CellSizeM, DeltaTimeS * NumSteps, TotalHeatW, SourceCells.Num()));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 축적 %.6e J / 잔차 %.6e J"),
		Solver.GetEnergyInJ(), Solver.GetEnergyStoredJ(), Solver.GetEnergyResidualJ()));
	AddInfo(FString::Printf(TEXT("유입 기준 정규화 잔차 %.3e (무차원)"), NormalizedResidual));
	AddInfo(FString::Printf(TEXT("온도 %.2f ~ %.2f C / 최대 속도 %.4f m/s / CFL %.3f"),
		Snapshot.MinTemperatureK - 273.15, Snapshot.MaxTemperatureK - 273.15,
		Snapshot.MaxSpeedMs, Snapshot.CourantNumber));
	AddInfo(FString::Printf(TEXT("정규화 발산 %.3e / 압력 반복 %d회"),
		Snapshot.NormalizedDivergence, Snapshot.PressureIterations));

	TestTrue(TEXT("모든 스텝에서 압력이 수렴해야 한다"), bAllPressureConverged);
	TestTrue(TEXT("넣은 열이 전부 공기에 쌓여 있어야 한다"), NormalizedResidual < 1.0e-9);
	TestTrue(TEXT("투영 뒤 정규화 발산이 0에 가까워야 한다"), Snapshot.NormalizedDivergence < 1.0e-5);

	// 타당성 안전망: 열을 넣었으면 공기가 더워지고 움직여야 한다.
	TestTrue(TEXT("공기가 초기보다 더워져야 한다"), Snapshot.MaxTemperatureK > InitialTempK);
	TestTrue(TEXT("부력으로 공기가 움직여야 한다"), Snapshot.MaxSpeedMs > 0.01);

	return true;
}

// D2/규약 2: 부력은 절대온도로 계산한다. 뜨거운 공기는 위로 떠야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirBuoyancyDirectionTest,
	"VRG.Air.Buoyancy.HotAirRises", VrgAirSolverTestFlags)

bool FAirBuoyancyDirectionTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 12;
	FAirSolver Solver;
	Solver.Initialize(MakeGridParams(NumCells, 0.25), 297.15);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-7;
	Solver.GetMutablePressureSettings().MaxIterations = 2000;

	FUniformAirGrid& Grid = Solver.GetMutableGrid();

	// 아래쪽 가운데 한 덩어리를 데운다.
	const int32 HotZ = 2;
	for (int32 Y = 5; Y <= 6; ++Y)
	{
		for (int32 X = 5; X <= 6; ++X)
		{
			Grid.TemperatureK[Grid.CellIndex(X, Y, HotZ)] = 330.0;
		}
	}

	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());

	for (int32 StepIndex = 0; StepIndex < 40; ++StepIndex)
	{
		Solver.Step(Input, 0.02);
	}

	// 뜨거운 덩어리 바로 위 면의 속도가 양의 Z여야 한다.
	const double UpwardMs = Grid.VelocityZMs[Grid.FaceZIndex(5, 5, HotZ + 1)];

	// 무게중심이 위로 올라갔는지도 본다. 국소 값 하나로만 판정하지 않는다.
	double WeightedZSum = 0.0;
	double WeightSum = 0.0;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = 0; X < NumCells; ++X)
			{
				const double ExcessK = Grid.TemperatureK[Grid.CellIndex(X, Y, Z)] - 297.15;
				if (ExcessK > 0.0)
				{
					WeightedZSum += ExcessK * (Z + 0.5);
					WeightSum += ExcessK;
				}
			}
		}
	}
	const double HeatCentroidZ = WeightSum > 0.0 ? WeightedZSum / WeightSum : 0.0;

	AddInfo(FString::Printf(TEXT("뜨거운 덩어리 바로 위 면 속도 %.6f m/s (양수면 상승)"), UpwardMs));
	AddInfo(FString::Printf(TEXT("열 무게중심 Z = %.3f 셀 (처음 %.1f 셀)"),
		HeatCentroidZ, HotZ + 0.5));

	TestTrue(TEXT("뜨거운 공기 위쪽 속도가 위를 향해야 한다"), UpwardMs > 0.0);
	TestTrue(TEXT("열 무게중심이 위로 올라가야 한다"), HeatCentroidZ > HotZ + 0.5);

	return true;
}

// D2: 속도 0에서 특이해지지 않아야 한다. 유량이 분모에 오는 식이 없다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirZeroVelocityTest,
	"VRG.Air.ZeroVelocityNoSingularity", VrgAirSolverTestFlags)

bool FAirZeroVelocityTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 8;
	const double InitialTempK = 297.15;

	FAirSolver Solver;
	Solver.Initialize(MakeGridParams(NumCells, 0.25), InitialTempK);

	// 부력도 끄고 열원만 넣는다. 속도가 끝까지 0이다.
	FAirSolverInput Input;
	Input.bEnableBuoyancy = false;
	Input.CellHeatW.Init(0.0, Solver.GetGrid().GetNumCells());
	Input.CellHeatW[Solver.GetGrid().CellIndex(4, 4, 4)] = 500.0;

	const double DeltaTimeS = 0.05;
	const int32 NumSteps = 200;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Solver.Step(Input, DeltaTimeS);
	}

	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(DeltaTimeS * NumSteps);

	bool bAllFinite = true;
	for (const double TempK : Snapshot.TemperatureK)
	{
		bAllFinite = bAllFinite && FMath::IsFinite(TempK);
	}

	// 속도가 0이면 이류가 없으므로 열은 그 셀에만 쌓인다.
	const FAirGridParams& Params = Solver.GetGrid().GetParams();
	const double CellHeatCapacityJK =
		Params.DensityKgM3 * Solver.GetGrid().GetCellVolumeM3() * Params.SpecificHeatJKgK;
	const double ExpectedRiseK = 500.0 * DeltaTimeS * NumSteps / CellHeatCapacityJK;
	const double ActualRiseK =
		Solver.GetGrid().TemperatureK[Solver.GetGrid().CellIndex(4, 4, 4)] - InitialTempK;
	const double NormalizedRiseError = FMath::Abs(ActualRiseK - ExpectedRiseK) / ExpectedRiseK;

	AddInfo(FString::Printf(TEXT("최대 속도 %.3e m/s / 모든 온도 유한 %s"),
		Snapshot.MaxSpeedMs, bAllFinite ? TEXT("예") : TEXT("아니오")));
	AddInfo(FString::Printf(TEXT("열원 셀 온도 상승 %.4f K / 해석해 %.4f K / 정규화 오차 %.3e"),
		ActualRiseK, ExpectedRiseK, NormalizedRiseError));
	AddInfo(FString::Printf(TEXT("유입 기준 정규화 열수지 잔차 %.3e"),
		FMath::Abs(Solver.GetNormalizedEnergyResidual())));

	TestTrue(TEXT("모든 온도가 유한해야 한다"), bAllFinite);
	TestTrue(TEXT("속도가 0이면 이류가 없어야 한다"), Snapshot.MaxSpeedMs < 1.0e-12);
	TestTrue(TEXT("열원 셀 온도 상승이 해석해와 일치해야 한다"), NormalizedRiseError < 1.0e-12);
	TestTrue(TEXT("열수지가 맞아야 한다"), FMath::Abs(Solver.GetNormalizedEnergyResidual()) < 1.0e-12);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
