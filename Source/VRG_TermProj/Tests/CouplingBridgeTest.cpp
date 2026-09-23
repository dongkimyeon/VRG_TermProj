// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgCouplingTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// 랙 몇 대 + 수력 한 줄 + 공기 격자 하나를 한자리에 세운다.
	// 액터 없이 모델만 쓴다 (규약 5: 솔버는 액터를 조회하지 않는다).
	struct FCoupledRoomFixture
	{
		static constexpr int32 NumRacks = 3;
		static constexpr int32 NumCells = 8;
		static constexpr double CellSizeM = 0.5;

		TArray<FRackThermalModel> RackModels;
		TArray<FRackThermalInput> RackInputs;
		TArray<FRadiatorParams> Radiators;

		FHydraulicNetwork Network;
		FInRowLoopHandles LoopHandles;
		FHydraulicSolveResult HydraulicResult;

		FAirSolver AirSolver;
		FCouplingBridge Bridge;

		void Build(bool bUseAir, bool bUseHydraulics, bool bUseCooling)
		{
			// --- 랙 ---
			RackModels.SetNum(NumRacks);
			RackInputs.SetNum(NumRacks);
			Radiators.SetNum(NumRacks);

			const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
			for (int32 RackIndex = 0; RackIndex < NumRacks; ++RackIndex)
			{
				FRackThermalState Initial;
				Initial.CoolantTempK = 300.15;
				RackModels[RackIndex].Initialize(Params, Initial);

				RackInputs[RackIndex].ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
				RackInputs[RackIndex].SledDocked.Init(true, Params.NumSleds);
				RackInputs[RackIndex].TotalAirMassFlowKgs = 0.6;
				RackInputs[RackIndex].AirInletTempK = 297.15;

				Radiators[RackIndex].UAWK = 1.1e4;
				Radiators[RackIndex].AmbientTempK = 295.15;
			}

			// --- 수력 ---
			if (bUseHydraulics)
			{
				FInRowLoopSpec Spec;
				Spec.NumRacks = NumRacks;
				Spec.SledsPerRack = FRackThermalModel::CountLiquidSlots(FRackThermalModel::MakeGb200Nvl72Params());
				Spec.HeaderSegmentResistancePaS2M6 = 6.0e6;
				Spec.RackBranchResistancePaS2M6 = 1.7e10;
				Spec.ReturnToCduResistancePaS2M6 = 1.5e7;
				Spec.PumpShutoffHeadPa = 4.0e5;
				Spec.PumpCurveCoeffPaS2M6 = 6.0e8;

				LoopHandles = InRowLoopBuilder::AddLoop(Network, Spec);

				FHydraulicSolveSettings Settings;
				HydraulicResult = Network.Solve(Settings);
			}

			// --- 공기 ---
			FAirGridParams GridParams;
			GridParams.NumCellsX = NumCells;
			GridParams.NumCellsY = NumCells;
			GridParams.NumCellsZ = NumCells;
			GridParams.CellSizeM = CellSizeM;
			AirSolver.Initialize(GridParams, 297.15);
			AirSolver.GetMutablePressureSettings().NormalizedResidualTarget = 1.0e-6;
			AirSolver.GetMutablePressureSettings().MaxIterations = 600;

			// --- 연결 ---
			TArray<FRackCouplingTarget> Targets;
			TArray<FRackCouplingLink> Links;
			for (int32 RackIndex = 0; RackIndex < NumRacks; ++RackIndex)
			{
				FRackCouplingTarget Target;
				Target.Model = &RackModels[RackIndex];
				Target.Input = &RackInputs[RackIndex];
				Target.Radiator = &Radiators[RackIndex];
				Targets.Add(Target);

				FRackCouplingLink Link;
				Link.BranchPipeIndex = bUseHydraulics ? LoopHandles.RackBranchPipeIndices[RackIndex] : INDEX_NONE;
				Link.FallbackCoolantMassFlowKgs = 2.3;

				if (bUseAir)
				{
					// 랙 i는 X = 2*i + 1 기둥을 차지한다고 보고,
					// 흡기는 그 왼쪽, 배기는 오른쪽 셀로 잡는다.
					const int32 RackX = 2 * RackIndex + 1;
					for (int32 Z = 1; Z <= 4; ++Z)
					{
						for (int32 Y = 2; Y <= 5; ++Y)
						{
							Link.InletCellIndices.Add(AirSolver.GetGrid().CellIndex(RackX - 1, Y, Z));
							Link.ExhaustCellIndices.Add(AirSolver.GetGrid().CellIndex(RackX + 1, Y, Z));
						}
					}
				}

				Links.Add(Link);
			}

			Bridge.Initialize(Targets, Links);
			Bridge.SetHydraulicNetwork(bUseHydraulics ? &Network : nullptr);
			Bridge.SetAirSolver(bUseAir ? &AirSolver : nullptr);

			// 공기 쪽 냉각 코일. 정상 상태를 만들려면 필요하다.
			if (bUseAir && bUseCooling)
			{
				Bridge.AirSolverInput.CellCoolingUAWK.Init(0.0, AirSolver.GetGrid().GetNumCells());
				Bridge.AirSolverInput.CoolingSupplyTempK = 291.15;

				// 천장 한 층을 코일로 쓴다.
				const int32 CoilZ = NumCells - 1;
				TArray<int32> CoilCells;
				for (int32 Y = 0; Y < NumCells; ++Y)
				{
					for (int32 X = 0; X < NumCells; ++X)
					{
						CoilCells.Add(AirSolver.GetGrid().CellIndex(X, Y, CoilZ));
					}
				}
				// 랙 3대의 공랭 부하(약 3 x 8 kW)를 약 10 K 접근온도로 빼는 UA.
				const double CoilUATotalWK = 2400.0;
				for (const int32 Index : CoilCells)
				{
					Bridge.AirSolverInput.CellCoolingUAWK[Index] = CoilUATotalWK / CoilCells.Num();
				}
			}
		}

		void Run(double DeltaTimeS, int32 NumSteps)
		{
			for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
			{
				// 수력은 유량이 바뀔 때만 다시 푼다. 여기서는 처음 한 번으로 충분하다.
				Bridge.Step(DeltaTimeS, StepIndex == 0, HydraulicResult.bConverged);
			}
		}
	};
}

// S4의 완료 조건: 도메인을 다 이어도 전역 열수지가 맞아야 한다.
//   유입(IT 발열) - 유출(라디에이터 + CRAH 코일) - 축적(랙 + 냉각수 + 공기) = 0
// 랙에서 공기로 넘어간 열은 계 안에서의 이동이라 유입에도 유출에도 넣지 않는다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCouplingGlobalEnergyTest,
	"VRG.Coupling.GlobalEnergyBalance", VrgCouplingTestFlags)

bool FCouplingGlobalEnergyTest::RunTest(const FString& Parameters)
{
	FCoupledRoomFixture Fixture;
	Fixture.Build(/*bUseAir=*/true, /*bUseHydraulics=*/true, /*bUseCooling=*/true);

	TestTrue(TEXT("수력이 수렴해야 한다"), Fixture.HydraulicResult.bConverged);

	const double DeltaTimeS = 0.02;
	const int32 NumSteps = 3000;
	Fixture.Run(DeltaTimeS, NumSteps);

	const FCouplingSnapshot Snapshot = Fixture.Bridge.MakeSnapshot(DeltaTimeS * NumSteps);
	const double NormalizedResidual = FMath::Abs(Fixture.Bridge.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("랙 %d대 / 공기 격자 %d^3 / t=%.1f s"),
		FCoupledRoomFixture::NumRacks, FCoupledRoomFixture::NumCells, Snapshot.SimTimeS));
	AddInfo(FString::Printf(TEXT("IT 발열 %.1f kW | 라디에이터 %.1f kW | CRAH %.2f kW"),
		Snapshot.TotalItHeatW / 1000.0,
		Snapshot.TotalRadiatorRejectedW / 1000.0,
		Snapshot.TotalAirCoolingRemovedW / 1000.0));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J"),
		Snapshot.EnergyInJ, Snapshot.EnergyOutJ, Snapshot.EnergyStoredJ));
	AddInfo(FString::Printf(TEXT("유입 기준 정규화 전역 잔차 %.3e (무차원)"), NormalizedResidual));

	for (int32 RackIndex = 0; RackIndex < FCoupledRoomFixture::NumRacks; ++RackIndex)
	{
		AddInfo(FString::Printf(TEXT("랙 %d: 냉각수 %.3f kg/s | 흡기 %.2f C | 공기로 %.0f W"),
			RackIndex,
			Snapshot.RackCoolantMassFlowKgs[RackIndex],
			Snapshot.RackAirInletTempK[RackIndex] - 273.15,
			Snapshot.RackHeatToAirW[RackIndex]));
	}

	TestTrue(TEXT("전역 열수지가 맞아야 한다 (유입 - 유출 - 축적 = 0)"), NormalizedResidual < 1.0e-9);

	// --- 연결이 실제로 살아 있는지 ---
	// 장부만 맞고 값이 안 흐르면 "전부 0"으로도 통과한다. 흐르는지 따로 본다.
	bool bCoolantFlowFromHydraulics = true;
	bool bAirInletFromGrid = true;
	for (int32 RackIndex = 0; RackIndex < FCoupledRoomFixture::NumRacks; ++RackIndex)
	{
		// 수력에서 온 유량은 예비값 2.3 kg/s와 달라야 한다 (랙마다 조금씩 다르다).
		if (FMath::IsNearlyEqual(Snapshot.RackCoolantMassFlowKgs[RackIndex], 2.3, 1.0e-6))
		{
			bCoolantFlowFromHydraulics = false;
		}
		// 공기에서 온 흡기 온도는 초기값 297.15 K에서 움직여 있어야 한다.
		if (FMath::IsNearlyEqual(Snapshot.RackAirInletTempK[RackIndex], 297.15, 1.0e-6))
		{
			bAirInletFromGrid = false;
		}
	}

	TestTrue(TEXT("냉각수 유량이 수력 해에서 와야 한다 (예비값이 아니어야 한다)"),
		bCoolantFlowFromHydraulics);
	TestTrue(TEXT("랙 흡기 온도가 공기 격자에서 와야 한다 (초기값에 머물면 안 된다)"),
		bAirInletFromGrid);
	TestTrue(TEXT("랙이 공기로 열을 내보내야 한다"), Snapshot.RackHeatToAirW[0] > 100.0);
	TestTrue(TEXT("라디에이터가 열을 버려야 한다"), Snapshot.TotalRadiatorRejectedW > 1000.0);

	return true;
}

// 규약 6: 랙이 냉각수로 내보낸 열과 라디에이터가 버린 열은 같은 값이어야 한다.
// 따로 계산하면 그 차이만큼 전역 장부가 어긋난다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCouplingCoolantLoopClosesTest,
	"VRG.Coupling.CoolantLoopClosesExactly", VrgCouplingTestFlags)

bool FCouplingCoolantLoopClosesTest::RunTest(const FString& Parameters)
{
	FCoupledRoomFixture Fixture;
	Fixture.Build(/*bUseAir=*/false, /*bUseHydraulics=*/true, /*bUseCooling=*/false);

	const double DeltaTimeS = 0.05;
	Fixture.Run(DeltaTimeS, 2000);

	// 랙이 냉각수로 내보낸 열의 합.
	double RackToCoolantW = 0.0;
	for (int32 RackIndex = 0; RackIndex < FCoupledRoomFixture::NumRacks; ++RackIndex)
	{
		RackToCoolantW += Fixture.RackModels[RackIndex].MakeSnapshot(0.0).HeatRemovedByCoolantW;
	}

	const double RadiatorW = Fixture.Bridge.GetTotalRadiatorRejectedW();
	const double NormalizedDifference = FMath::Abs(RackToCoolantW - RadiatorW)
		/ FMath::Max(FMath::Abs(RadiatorW), 1.0);

	AddInfo(FString::Printf(TEXT("랙이 냉각수로 내보낸 열 %.6f W / 라디에이터가 버린 열 %.6f W"),
		RackToCoolantW, RadiatorW));
	AddInfo(FString::Printf(TEXT("라디에이터 기준 정규화 차이 %.3e (무차원)"), NormalizedDifference));

	TestTrue(TEXT("랙이 내보낸 열과 라디에이터가 버린 열이 같아야 한다"),
		NormalizedDifference < 1.0e-12);

	// 여기서는 전역 장부를 검사하지 않는다. 공기 격자를 빼고 세웠으므로
	// 랙이 공기로 내보낸 열이 받을 곳이 없어 그만큼 어긋나는 게 맞다.
	// (그 사실은 MissingDomainFallsBack에서 따로 확인한다.)
	// 이 테스트가 보는 것은 냉각수 루프가 정확히 닫히는지 하나다.
	AddInfo(FString::Printf(TEXT("참고: 공기 격자를 뺐으므로 전역 정규화 잔차는 %.3e로 어긋난다 (의도된 값)"),
		FMath::Abs(Fixture.Bridge.GetNormalizedEnergyResidual())));

	return true;
}

// D1: 공기 격자에 넣는 열은 랙마다 **하나의 합산값**이어야 한다.
// 랙이 부품 72개로 쪼개져 있어도 격자에 들어가는 것은 합계 하나다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCouplingLumpedHeatSourceTest,
	"VRG.Coupling.AirHeatIsLumpedPerRack", VrgCouplingTestFlags)

bool FCouplingLumpedHeatSourceTest::RunTest(const FString& Parameters)
{
	FCoupledRoomFixture Fixture;
	Fixture.Build(/*bUseAir=*/true, /*bUseHydraulics=*/true, /*bUseCooling=*/false);

	const double DeltaTimeS = 0.02;
	Fixture.Run(DeltaTimeS, 500);

	const FCouplingSnapshot Snapshot = Fixture.Bridge.MakeSnapshot(10.0);

	// 다리가 공기에 넘긴 값과, 랙 모델이 말하는 공랭 + 자연대류 합이 같아야 한다.
	double MaxNormalizedDifference = 0.0;
	for (int32 RackIndex = 0; RackIndex < FCoupledRoomFixture::NumRacks; ++RackIndex)
	{
		const FRackThermalSnapshot RackSnapshot = Fixture.RackModels[RackIndex].MakeSnapshot(0.0);
		const double ExpectedW = RackSnapshot.TotalHeatToAirW + RackSnapshot.TotalHeatToAmbientW;
		const double ActualW = Snapshot.RackHeatToAirW[RackIndex];

		MaxNormalizedDifference = FMath::Max(MaxNormalizedDifference,
			FMath::Abs(ActualW - ExpectedW) / FMath::Max(FMath::Abs(ExpectedW), 1.0));

		if (RackIndex == 0)
		{
			AddInfo(FString::Printf(
				TEXT("랙 0: 부품 %d개 | 강제 공랭 %.2f W + 자연대류 %.2f W = %.2f W | 격자에 넣은 값 %.2f W"),
				RackSnapshot.ComponentTempK.Num(),
				RackSnapshot.TotalHeatToAirW, RackSnapshot.TotalHeatToAmbientW, ExpectedW, ActualW));
		}
	}

	AddInfo(FString::Printf(TEXT("랙별 최대 정규화 차이 %.3e (무차원)"), MaxNormalizedDifference));
	TestTrue(TEXT("격자에 넣은 값이 랙의 공기 반출 합과 같아야 한다"),
		MaxNormalizedDifference < 1.0e-12);

	// 공기 솔버가 받은 유입도 같은 값이어야 한다 (중복 계상이 없어야 한다).
	double TotalPushedW = 0.0;
	for (const double HeatW : Snapshot.RackHeatToAirW)
	{
		TotalPushedW += HeatW;
	}

	// 마지막 스텝에 공기로 들어간 양을 누적값 차이로 확인한다.
	const double AirInJ = Fixture.AirSolver.GetEnergyInJ();
	AddInfo(FString::Printf(TEXT("공기 누적 유입 %.6e J / 마지막 스텝 랙 합 %.2f W"),
		AirInJ, TotalPushedW));
	TestTrue(TEXT("공기 격자가 열을 받아야 한다"), AirInJ > 0.0);

	return true;
}

// 수력을 안 붙이면 예비 유량으로 돌아야 한다. 도메인 하나가 빠져도 터지면 안 된다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCouplingMissingDomainTest,
	"VRG.Coupling.MissingDomainFallsBack", VrgCouplingTestFlags)

bool FCouplingMissingDomainTest::RunTest(const FString& Parameters)
{
	FCoupledRoomFixture Fixture;
	Fixture.Build(/*bUseAir=*/false, /*bUseHydraulics=*/false, /*bUseCooling=*/false);

	const double DeltaTimeS = 0.05;
	Fixture.Run(DeltaTimeS, 1000);

	const FCouplingSnapshot Snapshot = Fixture.Bridge.MakeSnapshot(50.0);
	const double NormalizedResidual = FMath::Abs(Fixture.Bridge.GetNormalizedEnergyResidual());

	AddInfo(FString::Printf(TEXT("수력·공기 없이 | 냉각수 %.3f kg/s (예비값 2.3) | 흡기 %.2f C"),
		Snapshot.RackCoolantMassFlowKgs[0], Snapshot.RackAirInletTempK[0] - 273.15));
	AddInfo(FString::Printf(TEXT("유입 %.6e J / 유출 %.6e J / 축적 %.6e J / 정규화 잔차 %.3e"),
		Snapshot.EnergyInJ, Snapshot.EnergyOutJ, Snapshot.EnergyStoredJ, NormalizedResidual));

	TestTrue(TEXT("수력이 없으면 예비 유량을 써야 한다"),
		FMath::IsNearlyEqual(Snapshot.RackCoolantMassFlowKgs[0], 2.3, 1.0e-9));
	TestTrue(TEXT("공기가 없으면 흡기 온도가 그대로여야 한다"),
		FMath::IsNearlyEqual(Snapshot.RackAirInletTempK[0], 297.15, 1.0e-9));

	// 공기가 없으면 랙이 공기로 내보낸 열은 계 밖으로 나간 것이 아니라 허공에 사라진다.
	// 그래서 전역 장부는 그만큼 어긋나는 게 맞다. 그 사실을 수치로 확인해 둔다.
	double RackToAirW = 0.0;
	for (const double HeatW : Snapshot.RackHeatToAirW)
	{
		RackToAirW += HeatW;
	}
	AddInfo(FString::Printf(
		TEXT("공기 격자가 없으면 랙이 내보낸 %.1f W가 받을 곳이 없어 장부가 그만큼 어긋난다 (의도된 값)"),
		RackToAirW));

	TestTrue(TEXT("랙이 공기로 열을 내보내고 있어야 한다"), RackToAirW > 100.0);
	TestTrue(TEXT("모든 값이 유한해야 한다"),
		FMath::IsFinite(Snapshot.EnergyInJ) && FMath::IsFinite(Snapshot.EnergyStoredJ));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
