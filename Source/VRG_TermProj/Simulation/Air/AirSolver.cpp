// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/AirSolver.h"

void FAirSolver::Initialize(const FAirGridParams& InGridParams, double InitialTempK)
{
	Grid.Initialize(InGridParams);
	Grid.ResetToUniform(InitialTempK);

	InitialTemperatureK = InitialTempK;
	EnergyInJ = 0.0;
	EnergyOutJ = 0.0;
	LastCoolingRemovedW = 0.0;
	LastCourantNumber = 0.0;

	DivergencePerS.Init(0.0, Grid.GetNumCells());
	PressureScratch.Init(0.0, Grid.GetNumCells());
	TemperatureFluxX.Init(0.0, Grid.GetNumFacesX());
	TemperatureFluxY.Init(0.0, Grid.GetNumFacesY());
	TemperatureFluxZ.Init(0.0, Grid.GetNumFacesZ());
	LastPressureResult = FPoissonSolveResult();
}

FVector FAirSolver::CellCenterVelocityMs(int32 X, int32 Y, int32 Z) const
{
	return FVector(
		0.5 * (Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] + Grid.VelocityXMs[Grid.FaceXIndex(X + 1, Y, Z)]),
		0.5 * (Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)] + Grid.VelocityYMs[Grid.FaceYIndex(X, Y + 1, Z)]),
		0.5 * (Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] + Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z + 1)]));
}

void FAirSolver::AdvectVelocity(double DeltaTimeS)
{
	const double CellSizeM = Grid.GetCellSizeM();

	// 반라그랑주: 각 면에서 시간을 거슬러 올라가 그 자리의 속도를 가져온다.
	// 읽기는 현재 버퍼, 쓰기는 다음 버퍼다 (규약 7: in-place 갱신 금지).
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X <= Grid.GetNumCellsX(); ++X)
			{
				const FVector FacePositionM(X * CellSizeM, (Y + 0.5) * CellSizeM, (Z + 0.5) * CellSizeM);
				const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
				Grid.NextVelocityXMs[Grid.FaceXIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).X;
			}
		}
	}

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y <= Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const FVector FacePositionM((X + 0.5) * CellSizeM, Y * CellSizeM, (Z + 0.5) * CellSizeM);
				const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
				Grid.NextVelocityYMs[Grid.FaceYIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).Y;
			}
		}
	}

	for (int32 Z = 0; Z <= Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const FVector FacePositionM((X + 0.5) * CellSizeM, (Y + 0.5) * CellSizeM, Z * CellSizeM);
				const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
				Grid.NextVelocityZMs[Grid.FaceZIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).Z;
			}
		}
	}

	Grid.SwapVelocityBuffers();
}

void FAirSolver::ApplyBuoyancy(const FAirSolverInput& Input, double DeltaTimeS)
{
	if (!Input.bEnableBuoyancy)
	{
		return;
	}

	const FAirGridParams& Params = Grid.GetParams();
	const double ReferenceTempK = Params.ReferenceTempK;
	if (ReferenceTempK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return;
	}

	// D2/규약 2: Boussinesq에서 이상기체의 열팽창계수는 1/T_ref다.
	// 절대온도가 아니면 부호도 크기도 틀린다.
	const double ExpansionPerK = 1.0 / ReferenceTempK;

	// 부력은 연직 성분에만 붙는다. Z면은 위아래 두 셀 사이에 있으므로 둘을 평균한다.
	// 바닥과 천장 면은 한쪽 셀밖에 없다 - 어차피 벽이라 다음 단계에서 0이 된다.
	for (int32 Z = 1; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				// 고체가 막은 면에는 부력을 붙이지 않는다. 어차피 다음 단계에서 0이 되지만,
				// 고체 셀의 온도는 공기 온도가 아니므로 읽어 쓰면 안 된다.
				if (Grid.IsSolidCell(X, Y, Z - 1) || Grid.IsSolidCell(X, Y, Z))
				{
					continue;
				}

				const double BelowTempK = Grid.TemperatureK[Grid.CellIndex(X, Y, Z - 1)];
				const double AboveTempK = Grid.TemperatureK[Grid.CellIndex(X, Y, Z)];
				const double FaceTempK = 0.5 * (BelowTempK + AboveTempK);

				// 기준보다 뜨거우면 밀도가 낮아 위로 뜬다. GravityZMs2가 음수이므로
				// (T - T_ref)가 양수일 때 가속도가 양의 Z가 되도록 부호를 뒤집는다.
				const double AccelerationMs2 =
					-Input.GravityZMs2 * ExpansionPerK * (FaceTempK - ReferenceTempK);

				Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] += AccelerationMs2 * DeltaTimeS;
			}
		}
	}
}

void FAirSolver::OnSolidCellsChanged()
{
	// 고체 셀 안에 남아 있던 속도와 압력을 지운다.
	ApplyWallBoundary();

	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		if (Grid.bSolidCell.IsValidIndex(Index) && Grid.bSolidCell[Index])
		{
			Grid.PressurePa[Index] = 0.0;
		}
	}
}

void FAirSolver::ApplyWallBoundary()
{
	// 막힌 상자: 벽면을 지나는 법선 속도는 0이다.
	// 이렇게 둬야 총 질량과 총 에너지가 정확히 보존되고 보존 검사가 의미를 갖는다.
	//
	// 고체 셀에 닿는 면도 같다. 한쪽이라도 고체면 그 면으로 공기가 못 지나간다.
	// 이걸 안 하면 기류가 랙을 그냥 통과한다.
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			Grid.VelocityXMs[Grid.FaceXIndex(0, Y, Z)] = 0.0;
			Grid.VelocityXMs[Grid.FaceXIndex(Grid.GetNumCellsX(), Y, Z)] = 0.0;
		}
	}

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			Grid.VelocityYMs[Grid.FaceYIndex(X, 0, Z)] = 0.0;
			Grid.VelocityYMs[Grid.FaceYIndex(X, Grid.GetNumCellsY(), Z)] = 0.0;
		}
	}

	for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			Grid.VelocityZMs[Grid.FaceZIndex(X, Y, 0)] = 0.0;
			Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Grid.GetNumCellsZ())] = 0.0;
		}
	}

	// 고체가 막은 면. 마주 보는 두 셀 중 하나라도 고체면 법선 속도를 0으로 둔다.
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 1; X < Grid.GetNumCellsX(); ++X)
			{
				if (Grid.IsSolidCell(X - 1, Y, Z) || Grid.IsSolidCell(X, Y, Z))
				{
					Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] = 0.0;
				}
			}
		}
	}

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 1; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (Grid.IsSolidCell(X, Y - 1, Z) || Grid.IsSolidCell(X, Y, Z))
				{
					Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)] = 0.0;
				}
			}
		}
	}

	for (int32 Z = 1; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (Grid.IsSolidCell(X, Y, Z - 1) || Grid.IsSolidCell(X, Y, Z))
				{
					Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] = 0.0;
				}
			}
		}
	}
}

void FAirSolver::ProjectToDivergenceFree(double DeltaTimeS)
{
	if (DeltaTimeS <= UE_DOUBLE_SMALL_NUMBER)
	{
		return;
	}

	const FAirGridParams& Params = Grid.GetParams();
	const double CellSizeM = Grid.GetCellSizeM();

	// laplacian(p) = rho/dt * div(u) 를 푼다.
	// 여기서 p는 압력 [Pa]이고 우변의 단위는 Pa/m^2다.
	const double RhoOverDt = Params.DensityKgM3 / DeltaTimeS;

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				// 고체 셀에는 풀 방정식이 없다. 우변을 0으로 둬야 적합성이 깨지지 않는다.
				DivergencePerS[Grid.CellIndex(X, Y, Z)] = Grid.IsFluidCell(X, Y, Z)
					? RhoOverDt * Grid.CellDivergencePerS(X, Y, Z)
					: 0.0;
			}
		}
	}

	// 막힌 상자에서는 총 발산이 0이라 우변이 적합하다. 반올림으로 남는 평균만 덜어 낸다.
	// 이걸 안 하면 Neumann Poisson이 풀리지 않아 수렴 실패로 보고된다.
	// 고체 셀은 방정식에 없으므로 평균에서 뺀다.
	JacobiPoissonSolver::RemoveFluidMean(Grid, DivergencePerS);

	LastPressureResult = JacobiPoissonSolver::Solve(Grid, DivergencePerS, Grid.PressurePa,
		PressureScratch, PressureSettings);

	// u -= dt/rho * grad(p). 압력은 셀 중심, 속도는 면이라 한 칸 차분으로 떨어진다.
	const double DtOverRho = DeltaTimeS / Params.DensityKgM3;
	const double InvCellSize = 1.0 / CellSizeM;

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 1; X < Grid.GetNumCellsX(); ++X)
			{
				// 고체가 막은 면은 손대지 않는다. 고체 셀의 압력은 해가 아니다.
				if (Grid.IsSolidCell(X - 1, Y, Z) || Grid.IsSolidCell(X, Y, Z))
				{
					continue;
				}
				const double GradPa = (Grid.PressurePa[Grid.CellIndex(X, Y, Z)]
					- Grid.PressurePa[Grid.CellIndex(X - 1, Y, Z)]) * InvCellSize;
				Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] -= DtOverRho * GradPa;
			}
		}
	}

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 1; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (Grid.IsSolidCell(X, Y - 1, Z) || Grid.IsSolidCell(X, Y, Z))
				{
					continue;
				}
				const double GradPa = (Grid.PressurePa[Grid.CellIndex(X, Y, Z)]
					- Grid.PressurePa[Grid.CellIndex(X, Y - 1, Z)]) * InvCellSize;
				Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)] -= DtOverRho * GradPa;
			}
		}
	}

	for (int32 Z = 1; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (Grid.IsSolidCell(X, Y, Z - 1) || Grid.IsSolidCell(X, Y, Z))
				{
					continue;
				}
				const double GradPa = (Grid.PressurePa[Grid.CellIndex(X, Y, Z)]
					- Grid.PressurePa[Grid.CellIndex(X, Y, Z - 1)]) * InvCellSize;
				Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] -= DtOverRho * GradPa;
			}
		}
	}

	// 벽면은 손대지 않았으므로 0 그대로다. 그래도 한 번 더 확실히 해 둔다.
	ApplyWallBoundary();
}

void FAirSolver::AdvectTemperature(double DeltaTimeS)
{
	const double CellSizeM = Grid.GetCellSizeM();

	// 1) 면마다 플럭스를 **한 번만** 계산한다 (규약 6).
	//    풍상차분: 바람이 불어오는 쪽 셀의 값을 쓴다.
	//
	//    경계면은 바깥 셀이 없으므로 안쪽 셀 값을 그대로 쓴다 (영기울기 외삽).
	//    플럭스를 0으로 박지 않는 이유: 막힌 상자에서는 벽면 속도가 0이라 어차피
	//    플럭스가 0이 되어 결과가 같지만, 0으로 박아 두면 경계에 속도가 있는 구성
	//    (S4의 급배기)에서 그 셀의 열이 통째로 사라진다.
	//    실제로 겪었다 - 관통류 테스트에서 첫 셀 온도가 320 K에서 0으로 떨어졌다.
	//
	//    그래서 닫힌 상자에서는 지금도 보존이 정확하고, 경계에 흐름이 생기면
	//    그때는 경계 플럭스를 열수지 장부에 유출입으로 더해야 한다 (S4에서 한다).
	auto UpwindFlux = [](double FaceVelocityMs, double UpstreamTempK, double DownstreamTempK) -> double
	{
		return FaceVelocityMs >= 0.0
			? FaceVelocityMs * UpstreamTempK
			: FaceVelocityMs * DownstreamTempK;
	};

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X <= Grid.GetNumCellsX(); ++X)
			{
				const int32 FaceIndex = Grid.FaceXIndex(X, Y, Z);
				const int32 UpstreamX = FMath::Max(X - 1, 0);
				const int32 DownstreamX = FMath::Min(X, Grid.GetNumCellsX() - 1);

				// 고체가 막은 면은 플럭스가 0이다. 속도도 0이라 값은 같지만,
				// 고체 셀의 온도를 읽어 쓰지 않도록 여기서 끊는다.
				if (Grid.IsSolidCell(UpstreamX, Y, Z) || Grid.IsSolidCell(DownstreamX, Y, Z))
				{
					TemperatureFluxX[FaceIndex] = 0.0;
					continue;
				}

				TemperatureFluxX[FaceIndex] = UpwindFlux(Grid.VelocityXMs[FaceIndex],
					Grid.TemperatureK[Grid.CellIndex(UpstreamX, Y, Z)],
					Grid.TemperatureK[Grid.CellIndex(DownstreamX, Y, Z)]);
			}
		}
	}

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y <= Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 FaceIndex = Grid.FaceYIndex(X, Y, Z);
				const int32 UpstreamY = FMath::Max(Y - 1, 0);
				const int32 DownstreamY = FMath::Min(Y, Grid.GetNumCellsY() - 1);

				if (Grid.IsSolidCell(X, UpstreamY, Z) || Grid.IsSolidCell(X, DownstreamY, Z))
				{
					TemperatureFluxY[FaceIndex] = 0.0;
					continue;
				}

				TemperatureFluxY[FaceIndex] = UpwindFlux(Grid.VelocityYMs[FaceIndex],
					Grid.TemperatureK[Grid.CellIndex(X, UpstreamY, Z)],
					Grid.TemperatureK[Grid.CellIndex(X, DownstreamY, Z)]);
			}
		}
	}

	for (int32 Z = 0; Z <= Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 FaceIndex = Grid.FaceZIndex(X, Y, Z);
				const int32 UpstreamZ = FMath::Max(Z - 1, 0);
				const int32 DownstreamZ = FMath::Min(Z, Grid.GetNumCellsZ() - 1);

				if (Grid.IsSolidCell(X, Y, UpstreamZ) || Grid.IsSolidCell(X, Y, DownstreamZ))
				{
					TemperatureFluxZ[FaceIndex] = 0.0;
					continue;
				}

				TemperatureFluxZ[FaceIndex] = UpwindFlux(Grid.VelocityZMs[FaceIndex],
					Grid.TemperatureK[Grid.CellIndex(X, Y, UpstreamZ)],
					Grid.TemperatureK[Grid.CellIndex(X, Y, DownstreamZ)]);
			}
		}
	}

	// 2) 같은 플럭스를 양쪽 셀에 부호만 바꿔 적용한다.
	//    내부 면은 들어가는 쪽과 나오는 쪽이 정확히 상쇄되므로 총 열량이 보존된다.
	//
	//    비압축 조건에서 div(u*T) = u.grad(T)이므로 플럭스 형태가 곧 이류다.
	//    투영 뒤에도 발산이 반올림만큼은 남는데, 그때 T*div(u)를 빼서 보정하면
	//    이류에는 더 충실해지지만 **총 열량 보존이 깨진다**.
	//    여기서는 장부가 어긋나지 않는 쪽을 택했다. 발산 잔차는 별도로 감시한다.
	const double DtOverH = DeltaTimeS / CellSizeM;

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 Index = Grid.CellIndex(X, Y, Z);

				const double NetFlux =
					(TemperatureFluxX[Grid.FaceXIndex(X + 1, Y, Z)] - TemperatureFluxX[Grid.FaceXIndex(X, Y, Z)])
					+ (TemperatureFluxY[Grid.FaceYIndex(X, Y + 1, Z)] - TemperatureFluxY[Grid.FaceYIndex(X, Y, Z)])
					+ (TemperatureFluxZ[Grid.FaceZIndex(X, Y, Z + 1)] - TemperatureFluxZ[Grid.FaceZIndex(X, Y, Z)]);

				Grid.NextTemperatureK[Index] = Grid.TemperatureK[Index] - DtOverH * NetFlux;
			}
		}
	}

	Grid.SwapTemperatureBuffers();
}

void FAirSolver::InjectHeat(const FAirSolverInput& Input, double DeltaTimeS)
{
	if (Input.CellHeatW.Num() != Grid.GetNumCells())
	{
		return;
	}

	const FAirGridParams& Params = Grid.GetParams();

	// D1: 체적 열원으로만 넣는다. 배기 온도를 따로 덮어쓰지 않는다.
	// 유량이 분모에 오는 식이 없으므로 유량 0에서도 특이해지지 않는다 (D2).
	const double CellHeatCapacityJK = Params.DensityKgM3 * Grid.GetCellVolumeM3() * Params.SpecificHeatJKgK;
	if (CellHeatCapacityJK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return;
	}

	double InjectedJ = 0.0;
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		const double HeatW = Input.CellHeatW[Index];
		if (HeatW == 0.0)
		{
			continue;
		}
		// 고체 셀에는 공기가 없다. 넣으면 장부에는 잡히는데 공기는 안 데워진다.
		if (Grid.bSolidCell.IsValidIndex(Index) && Grid.bSolidCell[Index])
		{
			continue;
		}

		Grid.TemperatureK[Index] += HeatW * DeltaTimeS / CellHeatCapacityJK;
		InjectedJ += HeatW * DeltaTimeS;
	}

	EnergyInJ += InjectedJ;
}

void FAirSolver::Step(const FAirSolverInput& Input, double DeltaTimeS)
{
	if (DeltaTimeS <= 0.0)
	{
		return;
	}

	// CFL을 먼저 재 둔다. 1을 넘으면 반라그랑주가 한 스텝에 한 셀을 건너뛴다.
	// 반라그랑주는 무조건 안정이라 터지지는 않지만 결과가 뭉개진다.
	LastCourantNumber = Grid.MaxAbsVelocityMs() * DeltaTimeS / Grid.GetCellSizeM();

	AdvectVelocity(DeltaTimeS);
	ApplyBuoyancy(Input, DeltaTimeS);
	ApplyWallBoundary();
	ProjectToDivergenceFree(DeltaTimeS);
	AdvectTemperature(DeltaTimeS);
	InjectHeat(Input, DeltaTimeS);
	ApplyCooling(Input, DeltaTimeS);
}

void FAirSolver::ApplyCooling(const FAirSolverInput& Input, double DeltaTimeS)
{
	LastCoolingRemovedW = 0.0;

	if (Input.CellCoolingUAWK.Num() != Grid.GetNumCells())
	{
		return;
	}

	const FAirGridParams& Params = Grid.GetParams();
	const double CellHeatCapacityJK = Params.DensityKgM3 * Grid.GetCellVolumeM3() * Params.SpecificHeatJKgK;
	if (CellHeatCapacityJK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return;
	}

	double RemovedW = 0.0;
	for (int32 Index = 0; Index < Grid.GetNumCells(); ++Index)
	{
		const double CoolingUAWK = Input.CellCoolingUAWK[Index];
		if (CoolingUAWK <= 0.0)
		{
			continue;
		}
		if (Grid.bSolidCell.IsValidIndex(Index) && Grid.bSolidCell[Index])
		{
			continue;
		}

		// 코일이 빼 가는 열을 한 번만 계산해 공기에서 빼고 장부에 적는다 (규약 6).
		const double HeatW = CoolingUAWK * (Grid.TemperatureK[Index] - Input.CoolingSupplyTempK);

		// 명시적 갱신이라 한 스텝에 공급 온도를 지나치면 진동한다.
		// 빼는 양을 "공급 온도까지 내려가는 만큼"으로 잘라 둔다.
		const double MaxRemovableW =
			(Grid.TemperatureK[Index] - Input.CoolingSupplyTempK) * CellHeatCapacityJK / DeltaTimeS;
		const double AppliedW = (HeatW >= 0.0)
			? FMath::Min(HeatW, FMath::Max(MaxRemovableW, 0.0))
			: FMath::Max(HeatW, FMath::Min(MaxRemovableW, 0.0));

		Grid.TemperatureK[Index] -= AppliedW * DeltaTimeS / CellHeatCapacityJK;
		RemovedW += AppliedW;
	}

	LastCoolingRemovedW = RemovedW;
	EnergyOutJ += RemovedW * DeltaTimeS;
}

double FAirSolver::GetEnergyStoredJ() const
{
	return Grid.TotalThermalEnergyJ(InitialTemperatureK);
}

double FAirSolver::GetEnergyResidualJ() const
{
	// 유입(체적 열원) - 유출(냉각 코일) - 축적(공기 온도 상승).
	// 벽을 통한 질량 출입이 없으므로 이 셋이 전부다.
	return EnergyInJ - EnergyOutJ - GetEnergyStoredJ();
}

double FAirSolver::GetNormalizedEnergyResidual() const
{
	if (FMath::Abs(EnergyInJ) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return GetEnergyResidualJ() / EnergyInJ;
}

FAirSolverSnapshot FAirSolver::MakeSnapshot(double SimTimeS) const
{
	FAirSolverSnapshot Snapshot;
	Snapshot.SimTimeS = SimTimeS;
	Snapshot.TemperatureK = Grid.TemperatureK;

	Snapshot.SpeedMs.SetNum(Grid.GetNumCells());
	Snapshot.MinTemperatureK = TNumericLimits<double>::Max();
	Snapshot.MaxTemperatureK = TNumericLimits<double>::Lowest();

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 Index = Grid.CellIndex(X, Y, Z);
				const double SpeedMs = CellCenterVelocityMs(X, Y, Z).Length();
				Snapshot.SpeedMs[Index] = SpeedMs;

				// 고체 셀은 공기가 아니다. 통계에 넣으면 최저/최고 온도가 엉뚱해진다.
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				Snapshot.MaxSpeedMs = FMath::Max(Snapshot.MaxSpeedMs, SpeedMs);

				const double TempK = Grid.TemperatureK[Index];
				Snapshot.MinTemperatureK = FMath::Min(Snapshot.MinTemperatureK, TempK);
				Snapshot.MaxTemperatureK = FMath::Max(Snapshot.MaxTemperatureK, TempK);
			}
		}
	}

	Snapshot.MaxAbsDivergencePerS = Grid.MaxAbsDivergencePerS();

	// 발산의 자연스러운 기준은 (속도 / 길이)다. 속도가 0이면 발산도 0이라 0을 쓴다.
	const double DivergenceReferencePerS = Grid.MaxAbsVelocityMs() / Grid.GetCellSizeM();
	Snapshot.NormalizedDivergence = DivergenceReferencePerS > UE_DOUBLE_SMALL_NUMBER
		? Snapshot.MaxAbsDivergencePerS / DivergenceReferencePerS
		: 0.0;

	Snapshot.NormalizedEnergyResidual = GetNormalizedEnergyResidual();
	Snapshot.CoolingRemovedW = LastCoolingRemovedW;
	Snapshot.NumFluidCells = Grid.GetNumFluidCells();
	Snapshot.bPressureConverged = LastPressureResult.bConverged;
	Snapshot.PressureIterations = LastPressureResult.IterationsUsed;
	Snapshot.PressureNormalizedResidual = LastPressureResult.NormalizedResidual;
	Snapshot.CourantNumber = LastCourantNumber;

	return Snapshot;
}
