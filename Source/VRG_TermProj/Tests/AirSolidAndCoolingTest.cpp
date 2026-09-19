// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgAirSolidTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	FAirGridParams MakeSolidTestGridParams(int32 NumCells, double CellSizeM)
	{
		FAirGridParams Params;
		Params.NumCellsX = NumCells;
		Params.NumCellsY = NumCells;
		Params.NumCellsZ = NumCells;
		Params.CellSizeM = CellSizeM;
		return Params;
	}
}

// 고체가 막은 면으로는 공기가 지나가지 못해야 한다.
// 막지 않으면 기류가 랙을 그냥 통과해서 결합이 무의미해진다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirSolidBlocksFlowTest,
	"VRG.Air.Solid.BlocksFlow", VrgAirSolidTestFlags)

bool FAirSolidBlocksFlowTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 12;
	FAirSolver Solver;
	Solver.Initialize(MakeSolidTestGridParams(NumCells, 0.25), 297.15);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-7;
	Solver.GetMutablePressureSettings().MaxIterations = 3000;

	FUniformAirGrid& Grid = Solver.GetMutableGrid();

	// 도메인을 가로지르는 벽을 세운다. X = 6 한 층 전체가 고체다.
	const int32 WallX = 6;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			Grid.bSolidCell[Grid.CellIndex(WallX, Y, Z)] = true;
		}
	}
	Solver.OnSolidCellsChanged();

	// 벽 왼쪽만 데운다. 부력으로 기류가 생긴다.
	for (int32 Y = 4; Y <= 7; ++Y)
	{
		for (int32 X = 2; X <= 4; ++X)
		{
			Grid.TemperatureK[Grid.CellIndex(X, Y, 1)] = 340.0;
		}
	}

	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());

	for (int32 StepIndex = 0; StepIndex < 120; ++StepIndex)
	{
		Solver.Step(Input, 0.02);
	}

	// 벽면을 지나는 법선 속도가 정확히 0이어야 한다.
	double MaxWallFaceSpeedMs = 0.0;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			MaxWallFaceSpeedMs = FMath::Max(MaxWallFaceSpeedMs,
				FMath::Abs(Grid.VelocityXMs[Grid.FaceXIndex(WallX, Y, Z)]));
			MaxWallFaceSpeedMs = FMath::Max(MaxWallFaceSpeedMs,
				FMath::Abs(Grid.VelocityXMs[Grid.FaceXIndex(WallX + 1, Y, Z)]));
		}
	}

	// 벽 오른쪽은 열이 못 넘어가 초기 온도 그대로여야 한다.
	double MaxRightTempK = 0.0;
	for (int32 Z = 0; Z < NumCells; ++Z)
	{
		for (int32 Y = 0; Y < NumCells; ++Y)
		{
			for (int32 X = WallX + 1; X < NumCells; ++X)
			{
				MaxRightTempK = FMath::Max(MaxRightTempK, Grid.TemperatureK[Grid.CellIndex(X, Y, Z)]);
			}
		}
	}

	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(2.4);

	AddInfo(FString::Printf(TEXT("고체 셀 %d개 / 공기 셀 %d개 (전체 %d개)"),
		Grid.GetNumCells() - Snapshot.NumFluidCells, Snapshot.NumFluidCells, Grid.GetNumCells()));
	AddInfo(FString::Printf(TEXT("벽면 법선 속도 최대 %.3e m/s (도메인 최대 속도 %.4f m/s)"),
		MaxWallFaceSpeedMs, Snapshot.MaxSpeedMs));
	AddInfo(FString::Printf(TEXT("벽 오른쪽 최고 온도 %.6f K (초기 297.15 K)"), MaxRightTempK));
	AddInfo(FString::Printf(TEXT("정규화 발산 %.3e / 압력 %s (반복 %d회)"),
		Snapshot.NormalizedDivergence,
		Snapshot.bPressureConverged ? TEXT("수렴") : TEXT("수렴 실패"), Snapshot.PressureIterations));

	TestTrue(TEXT("압력 풀이가 수렴해야 한다"), Snapshot.bPressureConverged);
	TestTrue(TEXT("고체 면 법선 속도가 정확히 0이어야 한다"), MaxWallFaceSpeedMs == 0.0);
	TestTrue(TEXT("기류가 실제로 생겨야 한다 (안 생기면 막혔는지 알 수 없다)"),
		Snapshot.MaxSpeedMs > 0.01);
	TestTrue(TEXT("벽 너머로 열이 넘어가면 안 된다"),
		FMath::Abs(MaxRightTempK - 297.15) < 1.0e-9);
	TestTrue(TEXT("고체가 있어도 투영 뒤 발산이 0에 가까워야 한다"),
		Snapshot.NormalizedDivergence < 1.0e-5);

	return true;
}

// 고체가 있어도 열수지가 맞아야 한다. 고체 셀은 공기가 아니므로 축적에 넣지 않는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirSolidEnergyConservationTest,
	"VRG.Air.Solid.EnergyConservation", VrgAirSolidTestFlags)

bool FAirSolidEnergyConservationTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 10;
	FAirSolver Solver;
	Solver.Initialize(MakeSolidTestGridParams(NumCells, 0.25), 297.15);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-7;
	Solver.GetMutablePressureSettings().MaxIterations = 2000;

	FUniformAirGrid& Grid = Solver.GetMutableGrid();

	// 가운데에 랙 모양 덩어리를 놓는다.
	for (int32 Z = 0; Z < 7; ++Z)
	{
		for (int32 Y = 4; Y <= 5; ++Y)
		{
			for (int32 X = 4; X <= 6; ++X)
			{
				Grid.bSolidCell[Grid.CellIndex(X, Y, Z)] = true;
			}
		}
	}
	Solver.OnSolidCellsChanged();

	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());

	// 덩어리 바로 옆(공기 셀)에 열을 넣는다.
	const double TotalHeatW = 1500.0;
	TArray<int32> SourceCells;
	for (int32 Z = 1; Z <= 3; ++Z)
	{
		SourceCells.Add(Grid.CellIndex(7, 4, Z));
		SourceCells.Add(Grid.CellIndex(7, 5, Z));
	}
	for (const int32 Index : SourceCells)
	{
		Input.CellHeatW[Index] = TotalHeatW / SourceCells.Num();
	}

	const double DeltaTimeS = 0.02;
	const int32 NumSteps = 400;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Solver.Step(Input, DeltaTimeS);
	}

	const double NormalizedResidual = FMath::Abs(Solver.GetNormalizedEnergyResidual());
	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(DeltaTimeS * NumSteps);

	AddInfo(FString::Printf(TEXT("고체 셀 %d개 / 공기 셀 %d개"),
		Grid.GetNumCells() - Snapshot.NumFluidCells, Snapshot.NumFluidCells));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J / 잔차 %.6e J"),
		Solver.GetEnergyInJ(), Solver.GetEnergyOutJ(), Solver.GetEnergyStoredJ(),
		Solver.GetEnergyResidualJ()));
	AddInfo(FString::Printf(TEXT("유입 기준 정규화 잔차 %.3e (무차원)"), NormalizedResidual));

	TestTrue(TEXT("고체가 있어도 열수지가 맞아야 한다"), NormalizedResidual < 1.0e-9);

	return true;
}

// 냉각 코일이 열을 빼 가면 정상 상태가 생겨야 한다.
// 코일이 없으면 막힌 방이 무한정 데워져 정상 상태가 없다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirCoolingSteadyStateTest,
	"VRG.Air.Cooling.ReachesSteadyState", VrgAirSolidTestFlags)

bool FAirCoolingSteadyStateTest::RunTest(const FString& Parameters)
{
	// 격자를 작게 잡는다. 정상 상태에 닿는 시간이 방 열용량 / 코일 UA로 정해지는데,
	// 10^3이면 시정수가 약 56초라 120초로는 77%까지밖에 못 간다.
	// 8^3이면 약 29초라 120초면 98% 넘게 붙는다.
	const int32 NumCells = 8;
	const double InitialTempK = 297.15;

	FAirSolver Solver;
	Solver.Initialize(MakeSolidTestGridParams(NumCells, 0.25), InitialTempK);
	Solver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-7;
	Solver.GetMutablePressureSettings().MaxIterations = 2000;

	const FUniformAirGrid& Grid = Solver.GetGrid();

	FAirSolverInput Input;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());
	Input.CellCoolingUAWK.Init(0.0, Grid.GetNumCells());
	Input.CoolingSupplyTempK = 291.15;

	// 한쪽 벽에 열원, 반대쪽 벽에 냉각 코일.
	const double TotalHeatW = 2000.0;
	TArray<int32> SourceCells;
	for (int32 Z = 1; Z <= 4; ++Z)
	{
		for (int32 Y = 3; Y <= 6; ++Y)
		{
			SourceCells.Add(Grid.CellIndex(1, Y, Z));
		}
	}
	for (const int32 Index : SourceCells)
	{
		Input.CellHeatW[Index] = TotalHeatW / SourceCells.Num();
	}

	// 코일 UA는 정상 상태에서 2000 W를 빼도록 역산한다.
	// 코일 셀이 공급 온도보다 약 6 K 높다고 보면 UA_total = 2000/6 = 333 W/K다.
	// 방 열용량은 셀 512개 x 18.6 J/K = 약 9.5 kJ/K이므로 시정수가 약 29초다.
	const double CoilUATotalWK = 333.0;
	TArray<int32> CoilCells;
	for (int32 Z = 1; Z <= 4; ++Z)
	{
		for (int32 Y = 3; Y <= 6; ++Y)
		{
			CoilCells.Add(Grid.CellIndex(NumCells - 2, Y, Z));
		}
	}
	for (const int32 Index : CoilCells)
	{
		Input.CellCoolingUAWK[Index] = CoilUATotalWK / CoilCells.Num();
	}

	const double DeltaTimeS = 0.02;
	const int32 NumSteps = 6000;

	double PreviousMaxTempK = 0.0;
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Solver.Step(Input, DeltaTimeS);
		if (StepIndex == NumSteps - 500)
		{
			PreviousMaxTempK = Solver.MakeSnapshot(0.0).MaxTemperatureK;
		}
	}

	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(DeltaTimeS * NumSteps);
	const double NormalizedResidual = FMath::Abs(Solver.GetNormalizedEnergyResidual());

	// 정상 상태에 가까우면 냉각량이 발열량에 근접한다.
	const double CoolingBalance = Snapshot.CoolingRemovedW / TotalHeatW;

	// 마지막 500스텝(10초) 동안의 온도 변화가 작아야 한다.
	const double RecentDriftK = FMath::Abs(Snapshot.MaxTemperatureK - PreviousMaxTempK);

	AddInfo(FString::Printf(TEXT("t=%.0f s | 공기 %.2f ~ %.2f C | 최대 속도 %.4f m/s"),
		Snapshot.SimTimeS, Snapshot.MinTemperatureK - 273.15, Snapshot.MaxTemperatureK - 273.15,
		Snapshot.MaxSpeedMs));
	AddInfo(FString::Printf(TEXT("발열 %.0f W / 냉각 %.1f W (비율 %.4f) | 최근 10초 최고온도 변화 %.4f K"),
		TotalHeatW, Snapshot.CoolingRemovedW, CoolingBalance, RecentDriftK));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J / 정규화 잔차 %.3e"),
		Solver.GetEnergyInJ(), Solver.GetEnergyOutJ(), Solver.GetEnergyStoredJ(), NormalizedResidual));

	TestTrue(TEXT("열수지가 맞아야 한다 (유입 - 유출 - 축적 = 0)"), NormalizedResidual < 1.0e-9);
	TestTrue(TEXT("정상 상태에서 냉각량이 발열량에 근접해야 한다"),
		CoolingBalance > 0.9 && CoolingBalance < 1.1);
	TestTrue(TEXT("온도가 더 이상 오르지 않아야 한다"), RecentDriftK < 0.5);
	TestTrue(TEXT("공기가 공급 온도 아래로 내려가면 안 된다"),
		Snapshot.MinTemperatureK >= Input.CoolingSupplyTempK - 1.0e-9);

	return true;
}

// 코일 UA를 아무리 키워도 공급 온도 아래로 내려가거나 진동하면 안 된다.
// 명시적 갱신이라 한 스텝에 공급 온도를 지나칠 수 있어 잘라 두었다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirCoolingNoOvershootTest,
	"VRG.Air.Cooling.NoOvershoot", VrgAirSolidTestFlags)

bool FAirCoolingNoOvershootTest::RunTest(const FString& Parameters)
{
	const int32 NumCells = 6;
	FAirSolver Solver;
	Solver.Initialize(MakeSolidTestGridParams(NumCells, 0.25), 320.0);

	const FUniformAirGrid& Grid = Solver.GetGrid();

	FAirSolverInput Input;
	Input.bEnableBuoyancy = false;
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());
	Input.CoolingSupplyTempK = 291.15;

	// 일부러 터무니없이 큰 UA를 준다. 명시적 갱신이면 한 스텝에 지나쳐 발산한다.
	Input.CellCoolingUAWK.Init(1.0e6, Grid.GetNumCells());

	const double DeltaTimeS = 0.05;
	double MinSeenTempK = TNumericLimits<double>::Max();
	bool bAllFinite = true;

	for (int32 StepIndex = 0; StepIndex < 200; ++StepIndex)
	{
		Solver.Step(Input, DeltaTimeS);
		for (const double TempK : Grid.TemperatureK)
		{
			MinSeenTempK = FMath::Min(MinSeenTempK, TempK);
			bAllFinite = bAllFinite && FMath::IsFinite(TempK);
		}
	}

	const double NormalizedResidual = FMath::Abs(Solver.GetNormalizedEnergyResidual());
	const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(DeltaTimeS * 200);

	AddInfo(FString::Printf(TEXT("UA 1e6 W/K, dt=%.2f s | 최저 온도 %.6f K (공급 %.2f K)"),
		DeltaTimeS, MinSeenTempK, Input.CoolingSupplyTempK));
	AddInfo(FString::Printf(TEXT("최종 %.4f ~ %.4f C | 유출 %.6e J"),
		Snapshot.MinTemperatureK - 273.15, Snapshot.MaxTemperatureK - 273.15,
		Solver.GetEnergyOutJ()));

	TestTrue(TEXT("모든 온도가 유한해야 한다"), bAllFinite);
	TestTrue(TEXT("공급 온도 아래로 내려가면 안 된다"), MinSeenTempK >= Input.CoolingSupplyTempK - 1.0e-9);

	// 유입이 0이라 정규화 잔차는 0을 돌려준다. 절대 잔차로 본다.
	const double AbsResidualJ = FMath::Abs(Solver.GetEnergyResidualJ());
	const double NormalizedByOut = AbsResidualJ / FMath::Max(FMath::Abs(Solver.GetEnergyOutJ()), 1.0);
	AddInfo(FString::Printf(TEXT("절대 잔차 %.3e J / 유출 기준 정규화 %.3e"),
		AbsResidualJ, NormalizedByOut));
	TestTrue(TEXT("빼 간 열이 장부와 맞아야 한다"), NormalizedByOut < 1.0e-12);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
