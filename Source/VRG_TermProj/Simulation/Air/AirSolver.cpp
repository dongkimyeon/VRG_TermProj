// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/WallDistance.h"
#include "Simulation/Core/VrgParallel.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "VrgPressureGpuSolver.h"

void FAirSolver::Initialize(const FAirGridParams& InGridParams, double InitialTempK)
{
	bUseOctree = false;
	Grid.Initialize(InGridParams);
	Grid.ResetToUniform(InitialTempK);

	InitialTemperatureK = InitialTempK;
	NumPressureSolves = NumPressureFailures = 0;
	EnergyInJ = 0.0;
	DuctFanHeatTotalJ = 0.0;
	EnergyOutJ = 0.0;
	LastCoolingRemovedW = 0.0;
	LastCourantNumber = 0.0;
	bWallDistanceDirty = true;
	TurbulentViscosityM2s.Reset();
	LastMaxTurbulentViscosityM2s = LastMeanTurbulentViscosityM2s = 0.0;
	LastMomentumDiffusionSubsteps = 0;

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
	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X <= Grid.GetNumCellsX(); ++X)
		{
			const FVector FacePositionM(X * CellSizeM, (Y + 0.5) * CellSizeM, (Z + 0.5) * CellSizeM);
			const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
			Grid.NextVelocityXMs[Grid.FaceXIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).X;
		}
	});

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY() + 1, 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			const FVector FacePositionM((X + 0.5) * CellSizeM, Y * CellSizeM, (Z + 0.5) * CellSizeM);
			const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
			Grid.NextVelocityYMs[Grid.FaceYIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).Y;
		}
	});

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ() + 1, [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			const FVector FacePositionM((X + 0.5) * CellSizeM, (Y + 0.5) * CellSizeM, Z * CellSizeM);
			const FVector BackPositionM = FacePositionM - Grid.SampleVelocityMs(FacePositionM) * DeltaTimeS;
			Grid.NextVelocityZMs[Grid.FaceZIndex(X, Y, Z)] = Grid.SampleVelocityMs(BackPositionM).Z;
		}
	});

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
	if (bUseOctree) bOctreeNeedsImport = true;
	bWallDistanceDirty = true;
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

	// Fan duct faces: the same solid boundary, but with the fans' prescribed normal velocity instead of 0.
	// The projection never touches a face with a solid neighbour, so this flux survives it unchanged.
	if (ActiveDucts != nullptr)
	{
		for (int32 F = 0; F < ActiveDucts->DuctFaceIndex.Num(); ++F)
		{
			VelocityArray(ActiveDucts->DuctFaceAxis[F])[ActiveDucts->DuctFaceIndex[F]] = ActiveDucts->DuctFaceVelocityMs[F];
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

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			// 고체 셀에는 풀 방정식이 없다. 우변을 0으로 둬야 적합성이 깨지지 않는다.
			DivergencePerS[Grid.CellIndex(X, Y, Z)] = Grid.IsFluidCell(X, Y, Z)
				? RhoOverDt * Grid.CellDivergencePerS(X, Y, Z)
				: 0.0;
		}
	});

	// 막힌 상자에서는 총 발산이 0이라 우변이 적합하다. 반올림으로 남는 평균만 덜어 낸다.
	// 이걸 안 하면 Neumann Poisson이 풀리지 않아 수렴 실패로 보고된다.
	// 고체 셀은 방정식에 없으므로 평균에서 뺀다.
	JacobiPoissonSolver::RemoveFluidMean(Grid, DivergencePerS);

	{
		// SCOPE_CYCLE_COUNTER는 스레드 안전하므로 솔버 안에 둬도 규약 5를 어기지 않는다.
		// 지역 타이머도 전역을 건드리지 않는다.
		SCOPE_CYCLE_COUNTER(STAT_VrgPressureSolve);
		const uint64 StartCycles = FPlatformTime::Cycles64();
		SolvePressure();
		LastPressureSolveMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
	}
	// D5: a failed solve is also written to the log (the HUD alone is invisible in unattended runs).
	// Rate-limited: first failure, then every 100th. UE_LOG is thread safe (rule 5).
	++NumPressureSolves;
	if (!LastPressureResult.bConverged && (++NumPressureFailures == 1 || NumPressureFailures % 100 == 0))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Air] 압력 수렴 실패 %lld/%lld 스텝 | 반복 %d | 정규화 잔차 %.3e"),
			NumPressureFailures, NumPressureSolves, LastPressureResult.IterationsUsed, LastPressureResult.NormalizedResidual);
	}

	// u -= dt/rho * grad(p). 압력은 셀 중심, 속도는 면이라 한 칸 차분으로 떨어진다.
	const double DtOverRho = DeltaTimeS / Params.DensityKgM3;
	const double InvCellSize = 1.0 / CellSizeM;

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
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
	});

	VrgParallel::ForEachRow(1, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
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
	});

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 1, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
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
	});

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

	// Turbulent + molecular heat diffusion as part of the same face flux (rule 6): -alpha * dT/dn on fluid-fluid faces,
	// alpha at the face = alpha_mol + mean nu_t of the two cells / Pr_t. Boundary faces have the same cell on both sides
	// (no flux), and solid and duct faces never get here, so walls and bodies stay adiabatic.
	const bool bDiffuse = Turbulence.bEnabled && TurbulentViscosityM2s.Num() == Grid.GetNumCells();
	const double InvTurbulentPrandtl = 1.0 / Turbulence.TurbulentPrandtl;
	auto DiffusiveFlux = [&](int32 Lower, int32 Upper) -> double
	{
		const double Alpha = Turbulence.MolecularDiffusivityM2s
			+ 0.5 * (TurbulentViscosityM2s[Lower] + TurbulentViscosityM2s[Upper]) * InvTurbulentPrandtl;
		return -Alpha * (Grid.TemperatureK[Upper] - Grid.TemperatureK[Lower]) / CellSizeM;
	};

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
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
			if (bDiffuse) TemperatureFluxX[FaceIndex] += DiffusiveFlux(Grid.CellIndex(UpstreamX, Y, Z), Grid.CellIndex(DownstreamX, Y, Z));
		}
	});

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY() + 1, 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
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
			if (bDiffuse) TemperatureFluxY[FaceIndex] += DiffusiveFlux(Grid.CellIndex(X, UpstreamY, Z), Grid.CellIndex(X, DownstreamY, Z));
		}
	});

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ() + 1, [&](int32 Y, int32 Z)
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
			if (bDiffuse) TemperatureFluxZ[FaceIndex] += DiffusiveFlux(Grid.CellIndex(X, Y, UpstreamZ), Grid.CellIndex(X, Y, DownstreamZ));
		}
	});

	// 2) 같은 플럭스를 양쪽 셀에 부호만 바꿔 적용한다.
	//    내부 면은 들어가는 쪽과 나오는 쪽이 정확히 상쇄되므로 총 열량이 보존된다.
	//
	//    비압축 조건에서 div(u*T) = u.grad(T)이므로 플럭스 형태가 곧 이류다.
	//    투영 뒤에도 발산이 반올림만큼은 남는데, 그때 T*div(u)를 빼서 보정하면
	//    이류에는 더 충실해지지만 **총 열량 보존이 깨진다**.
	//    여기서는 장부가 어긋나지 않는 쪽을 택했다. 발산 잔차는 별도로 감시한다.
	// Fan ducts (rule 6): the enthalpy leaving through intake faces is summed once per duct and the exhaust
	// faces carry exactly that back, at the flow-weighted intake temperature. The solid body in between
	// stores nothing, so the fluid's total heat is unchanged by the duct itself.
	if (ActiveDucts != nullptr)
	{
		DuctIntakeEnthalpy.Init(0.0, ActiveDucts->NumDucts);
		DuctExhaustSpeedSum.Init(0.0, ActiveDucts->NumDucts);
		for (int32 F = 0; F < ActiveDucts->DuctFaceIndex.Num(); ++F)
		{
			const double Speed = FMath::Abs(ActiveDucts->DuctFaceVelocityMs[F]);
			if (ActiveDucts->DuctFaceIsExhaust[F]) DuctExhaustSpeedSum[ActiveDucts->DuctFaceDuct[F]] += Speed;
			else DuctIntakeEnthalpy[ActiveDucts->DuctFaceDuct[F]] += Speed * Grid.TemperatureK[ActiveDucts->DuctFaceFluidCell[F]];
		}
		// Duct coils: DuctIntakeEnthalpy becomes the exhaust temperature of each duct (mixed intake, then the coil).
		// The heat removed is booked from that same temperature, so fluid change + removed = 0 exactly.
		const bool bCoils = ActiveDucts->DuctCoilUAWK.Num() == ActiveDucts->NumDucts;
		const bool bFans = ActiveDucts->DuctFanHeatW.Num() == ActiveDucts->NumDucts;
		const FAirGridParams& Params = Grid.GetParams();
		const double FaceAreaM2 = CellSizeM * CellSizeM;
		for (int32 D = 0; D < ActiveDucts->NumDucts; ++D)
		{
			const double SpeedSum = DuctExhaustSpeedSum[D];
			const double MixK = SpeedSum > 0.0 ? DuctIntakeEnthalpy[D] / SpeedSum : 0.0;
			double ExhaustK = MixK;
			if (bCoils && SpeedSum > 0.0 && ActiveDucts->DuctCoilUAWK[D] > 0.0)
			{
				const double CapacityWK = Params.DensityKgM3 * SpeedSum * FaceAreaM2 * Params.SpecificHeatJKgK;
				const double Effectiveness = 1.0 - FMath::Exp(-ActiveDucts->DuctCoilUAWK[D] / CapacityWK);
				ExhaustK = MixK - Effectiveness * (MixK - ActiveDucts->DuctCoilWaterTempK[D]);
				DuctCoilRemovedJ += CapacityWK * (MixK - ExhaustK) * DeltaTimeS;
			}
			if (bFans && SpeedSum > 0.0 && ActiveDucts->DuctFanHeatW[D] > 0.0)
			{
				const double CapacityWK = Params.DensityKgM3 * SpeedSum * FaceAreaM2 * Params.SpecificHeatJKgK;
				ExhaustK += ActiveDucts->DuctFanHeatW[D] / CapacityWK;
				DuctFanHeatStepJ += ActiveDucts->DuctFanHeatW[D] * DeltaTimeS;
			}
			DuctIntakeEnthalpy[D] = ExhaustK;
		}
		for (int32 F = 0; F < ActiveDucts->DuctFaceIndex.Num(); ++F)
		{
			const double CarriedK = !ActiveDucts->DuctFaceIsExhaust[F] ? Grid.TemperatureK[ActiveDucts->DuctFaceFluidCell[F]]
				: DuctIntakeEnthalpy[ActiveDucts->DuctFaceDuct[F]];
			FluxArray(ActiveDucts->DuctFaceAxis[F])[ActiveDucts->DuctFaceIndex[F]] = ActiveDucts->DuctFaceVelocityMs[F] * CarriedK;
		}
	}

	const double DtOverH = DeltaTimeS / CellSizeM;

	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			const int32 Index = Grid.CellIndex(X, Y, Z);

			// A solid cell holds no air; with ducts its faces carry flux, but that enthalpy belongs to
			// the duct transfer above, not to the body.
			if (Grid.bSolidCell.IsValidIndex(Index) && Grid.bSolidCell[Index])
			{
				Grid.NextTemperatureK[Index] = Grid.TemperatureK[Index];
				continue;
			}

			const double NetFlux =
				(TemperatureFluxX[Grid.FaceXIndex(X + 1, Y, Z)] - TemperatureFluxX[Grid.FaceXIndex(X, Y, Z)])
				+ (TemperatureFluxY[Grid.FaceYIndex(X, Y + 1, Z)] - TemperatureFluxY[Grid.FaceYIndex(X, Y, Z)])
				+ (TemperatureFluxZ[Grid.FaceZIndex(X, Y, Z + 1)] - TemperatureFluxZ[Grid.FaceZIndex(X, Y, Z)]);

			Grid.NextTemperatureK[Index] = Grid.TemperatureK[Index] - DtOverH * NetFlux;
		}
	});

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
	bLastDuctsRejected = false;
	LastDuctFlowM3s = 0.0;
	LastNumDuctFaces = 0;
	DuctCoilRemovedJ = LastDuctCoilRemovedW = 0.0;
	DuctFanHeatStepJ = LastDuctFanHeatW = 0.0;
	if (bUseOctree)
	{
		if (Input.HasDucts())
		{
			// Not silent (D5 spirit): the octree path has no duct faces yet, so rack fans are not driving air.
			bLastDuctsRejected = true;
			if (!bOctreeDuctWarningLogged)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Air] 옥트리 경로는 팬 덕트(랙 관통 기류)를 아직 지원하지 않는다. 팬 기류 없이 적분한다."));
				bOctreeDuctWarningLogged = true;
			}
		}
		if (bOctreeNeedsImport)
		{
			FString Error;
			if (!OctreeSolver.Initialize(Grid, OctreeSettings, OctreeBoxesM, Error))
			{
				LastPressureResult.bConverged = false;
				UE_LOG(LogTemp, Error, TEXT("Octree import failed: %s"), *Error);
				return;
			}
			bOctreeNeedsImport = false;
		}
		OctreeSolver.PressureSettings = PressureSettings;
		const double BeforeIn = OctreeSolver.EnergyInJ, BeforeOut = OctreeSolver.EnergyOutJ;
		const uint64 StepStartCycles = FPlatformTime::Cycles64();
		const int64 BeforeSolves = OctreeSolver.PressureSolves, BeforeFailures = OctreeSolver.PressureFailures;
		OctreeSolver.Step(Input, DeltaTimeS);
		OctreeSolver.ExportUniform(Grid);
		// S5-E: the measured cost of this step (kept in the controller, rule 5: no game-thread globals).
		if (bAutoAdapt) AdaptController.ReportStepCost(FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StepStartCycles),
			OctreeSolver.GetGrid().GetNumLeaves());
		EnergyInJ += OctreeSolver.EnergyInJ - BeforeIn; EnergyOutJ += OctreeSolver.EnergyOutJ - BeforeOut;
		LastCoolingRemovedW = OctreeSolver.LastCoolingRemovedW; LastCourantNumber = OctreeSolver.LastCourant;
		LastPressureResult = OctreeSolver.PressureResult; LastPressureSolveMs = OctreeSolver.LastPressureMs;
		NumPressureSolves += OctreeSolver.PressureSolves - BeforeSolves;
		NumPressureFailures += OctreeSolver.PressureFailures - BeforeFailures;
		bLastPressureRanOnGpu = false;
		if (OctreeSolver.PressureFailures != BeforeFailures)
			UE_LOG(LogTemp, Warning, TEXT("[OctreeAir] Pressure failure: residual %.3e"), LastPressureResult.NormalizedResidual);

		// S5-D: after the step (same dt, the clock is untouched), let the indicators pick the refined roots. The
		// split / merge itself is S5-C's conservative transfer and re-projection.
		if (bAutoAdapt)
		{
			AdaptElapsedS += DeltaTimeS;
			TArray<FBox> AutoBoxes;
			if (AdaptController.Update(Grid, AdaptSettings, AdaptElapsedS, AutoBoxes))
			{
				TArray<FBox> Combined = OctreeUserBoxesM;
				Combined.Append(AutoBoxes);
				FString Error;
				if (AdaptToBoxes(Combined, DeltaTimeS, Error))
				{
					AdaptController.Commit();
				}
				else
				{
					// Not silent (D5 spirit), and the old topology stays in force.
					AdaptController.Discard();
					if (NumAutoAdaptFailures++ == 0 || NumAutoAdaptFailures % 100 == 0)
						UE_LOG(LogTemp, Warning, TEXT("[OctreeAir] 자동 적응 실패 %lld회: %s (기존 격자 유지)"), NumAutoAdaptFailures, *Error);
					LastAutoAdaptError = Error;
				}
			}
		}
		return;
	}

	// Fan ducts are checked before anything moves: a bad duct would make the pressure problem incompatible.
	if (Input.HasDucts())
	{
		const FString DuctError = ValidateDucts(Input);
		if (DuctError.IsEmpty())
		{
			ActiveDucts = &Input;
			LastNumDuctFaces = Input.DuctFaceIndex.Num();
			const double FaceAreaM2 = Grid.GetCellSizeM() * Grid.GetCellSizeM();
			for (int32 F = 0; F < Input.DuctFaceIndex.Num(); ++F)
				if (Input.DuctFaceIsExhaust[F]) LastDuctFlowM3s += FMath::Abs(Input.DuctFaceVelocityMs[F]) * FaceAreaM2;
		}
		else
		{
			bLastDuctsRejected = true;
			UE_LOG(LogTemp, Error, TEXT("[Air] 팬 덕트 입력 거부: %s"), *DuctError);
		}
	}

	// CFL을 먼저 재 둔다. 1을 넘으면 반라그랑주가 한 스텝에 한 셀을 건너뛴다.
	// 반라그랑주는 무조건 안정이라 터지지는 않지만 결과가 뭉개진다.
	LastCourantNumber = Grid.MaxAbsVelocityMs() * DeltaTimeS / Grid.GetCellSizeM();

	uint64 Mark = FPlatformTime::Cycles64();
	auto Lap = [&](int32 Stage) { const uint64 Now = FPlatformTime::Cycles64(); StageMs[Stage] += FPlatformTime::ToMilliseconds64(Now - Mark); Mark = Now; };
	const bool bTurbulence = Turbulence.bEnabled;
	if (bTurbulence) { UpdateTurbulentViscosity(); Lap(6); }
	AdvectVelocity(DeltaTimeS); Lap(0);
	ApplyBuoyancy(Input, DeltaTimeS); Lap(1);
	if (bTurbulence)
	{
		ApplyWallBoundary();  // walls and duct faces hold their Dirichlet values while momentum diffuses
		DiffuseMomentum(DeltaTimeS); Lap(6);
	}
	ApplyWallBoundary(); Lap(2);
	ProjectToDivergenceFree(DeltaTimeS); Lap(3);

	// Explicit upwind flux transport is stable while dt * (sum of outflow speeds) / h <= 1 in every cell.
	// Three axes bound that sum by 3 * max|u|, so sub-step when 3 * Courant would exceed 0.9 (same velocity
	// field, same total dt - rule 4; conservation is unchanged because every sub-step is conservative).
	const double TransportCourant = Grid.MaxAbsVelocityMs() * DeltaTimeS / Grid.GetCellSizeM();
	// With turbulence the explicit diffusion adds 6 * alpha_max * dt / h^2 to the same positivity bound.
	const double TransportLoad = bTurbulence
		? 3.0 * TransportCourant + 6.0 * (Turbulence.MolecularDiffusivityM2s + LastMaxTurbulentViscosityM2s / Turbulence.TurbulentPrandtl)
			* DeltaTimeS / (Grid.GetCellSizeM() * Grid.GetCellSizeM())
		: 3.0 * TransportCourant;
	LastTemperatureSubsteps = FMath::Clamp(FMath::CeilToInt32(TransportLoad / 0.9), 1, 64);
	for (int32 Sub = 0; Sub < LastTemperatureSubsteps; ++Sub)
	{
		AdvectTemperature(DeltaTimeS / LastTemperatureSubsteps);
	}
	Lap(4);
	InjectHeat(Input, DeltaTimeS);
	ApplyCooling(Input, DeltaTimeS);
	Lap(5);
	++StageSteps;
	// Duct coils removed their heat during transport (AdvectTemperature); book it with the cell coils.
	LastDuctCoilRemovedW = DuctCoilRemovedJ / DeltaTimeS;
	LastCoolingRemovedW += LastDuctCoilRemovedW;
	EnergyOutJ += DuctCoilRemovedJ;
	LastDuctFanHeatW = DuctFanHeatStepJ / DeltaTimeS;
	EnergyInJ += DuctFanHeatStepJ;
	DuctFanHeatTotalJ += DuctFanHeatStepJ;
	ActiveDucts = nullptr;
}

FString FAirSolver::DescribeStageTimes() const
{
	const double N = FMath::Max<double>(StageSteps, 1.0);
	double Sum = 0.0; for (const double T : StageMs) Sum += T;
	FString Text = FString::Printf(TEXT("공기 스텝 평균 %.2f ms (%lld스텝) = 속도 이류 %.2f + 부력 %.2f + 벽·덕트 %.2f + 투영(압력 포함) %.2f + 온도 수송 %.2f + 열원·냉각 %.2f + 난류 %.2f | 셀 %d"),
		Sum / N, StageSteps, StageMs[0] / N, StageMs[1] / N, StageMs[2] / N, StageMs[3] / N, StageMs[4] / N, StageMs[5] / N, StageMs[6] / N, Grid.GetNumCells());
	if (Turbulence.bEnabled)
		Text += FString::Printf(TEXT(" | 난류 nu_t 최대 %.3e / 평균 %.3e m2/s, 운동량 확산 부분 스텝 %d, 온도 부분 스텝 %d"),
			LastMaxTurbulentViscosityM2s, LastMeanTurbulentViscosityM2s, LastMomentumDiffusionSubsteps, LastTemperatureSubsteps);
	return Text;
}

void FAirSolver::UpdateTurbulentViscosity()
{
	const int32 N = Grid.GetNumCells();
	if (bWallDistanceDirty || WallDistanceM.Num() != N)
	{
		AirWallDistance::Compute(Grid, WallDistanceM);
		bWallDistanceDirty = false;
	}
	TurbulentViscosityM2s.SetNumUninitialized(N);
	const double C = Turbulence.ChenXuCoefficient;
	VrgParallel::ForEachRow(0, Grid.GetNumCellsY(), 0, Grid.GetNumCellsZ(), [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
		{
			const int32 Index = Grid.CellIndex(X, Y, Z);
			TurbulentViscosityM2s[Index] = Grid.bSolidCell[Index] ? 0.0 : C * CellCenterVelocityMs(X, Y, Z).Size() * WallDistanceM[Index];
		}
	});
	LastMaxTurbulentViscosityM2s = VrgParallel::Max(N, 4096, [&](int32 B, int32 E)
	{
		double M = 0.0;
		for (int32 I = B; I < E; ++I) M = FMath::Max(M, TurbulentViscosityM2s[I]);
		return M;
	});
	const int32 NumFluid = Grid.GetNumFluidCells();
	LastMeanTurbulentViscosityM2s = NumFluid > 0 ? VrgParallel::Sum(N, 4096, [&](int32 B, int32 E)
	{
		double S = 0.0;
		for (int32 I = B; I < E; ++I) S += TurbulentViscosityM2s[I];
		return S;
	}) / NumFluid : 0.0;
}

void FAirSolver::DiffuseMomentum(double DeltaTimeS)
{
	if (TurbulentViscosityM2s.Num() != Grid.GetNumCells()) UpdateTurbulentViscosity();
	// Explicit: the diagonal of the operator is at most 10 nu_max / h^2 (two cells along the component, four tangential
	// neighbours that may each be a no-slip ghost at twice the weight), so sub-step to keep dt * that below 0.9.
	const double H = Grid.GetCellSizeM();
	const double NuMax = Turbulence.MolecularViscosityM2s + LastMaxTurbulentViscosityM2s;
	LastMomentumDiffusionSubsteps = FMath::Clamp(FMath::CeilToInt32(10.0 * NuMax * DeltaTimeS / (H * H) / 0.9), 1, 64);
	const double SubDt = DeltaTimeS / LastMomentumDiffusionSubsteps;
	for (int32 Sub = 0; Sub < LastMomentumDiffusionSubsteps; ++Sub)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis) DiffuseFaceComponent(Axis, SubDt);
		Grid.SwapVelocityBuffers();
	}
}

void FAirSolver::DiffuseFaceComponent(int32 Axis, double DeltaTimeS)
{
	const int32 N[3] = { Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ() };
	int32 NF[3] = { N[0], N[1], N[2] };
	++NF[Axis];
	const TArray<double>& U = VelocityArray(static_cast<uint8>(Axis));
	TArray<double>& Out = Axis == 0 ? Grid.NextVelocityXMs : Axis == 1 ? Grid.NextVelocityYMs : Grid.NextVelocityZMs;
	const double InvH2 = 1.0 / (Grid.GetCellSizeM() * Grid.GetCellSizeM());
	const double NuMol = Turbulence.MolecularViscosityM2s;

	auto FaceIndex = [&](const int32* C)
	{
		return Axis == 0 ? Grid.FaceXIndex(C[0], C[1], C[2]) : Axis == 1 ? Grid.FaceYIndex(C[0], C[1], C[2]) : Grid.FaceZIndex(C[0], C[1], C[2]);
	};
	auto CellNu = [&](const int32* C) { return NuMol + TurbulentViscosityM2s[Grid.CellIndex(C[0], C[1], C[2])]; };
	// A face between two fluid cells (domain boundary, solid and duct faces are fixed by ApplyWallBoundary).
	auto IsOpen = [&](const int32* C)
	{
		if (C[Axis] <= 0 || C[Axis] >= N[Axis]) return false;
		const int32 L[3] = { C[0] - (Axis == 0), C[1] - (Axis == 1), C[2] - (Axis == 2) };
		return Grid.IsFluidCell(L[0], L[1], L[2]) && Grid.IsFluidCell(C[0], C[1], C[2]);
	};
	auto FaceNu = [&](const int32* C)
	{
		const int32 L[3] = { C[0] - (Axis == 0), C[1] - (Axis == 1), C[2] - (Axis == 2) };
		return 0.5 * (CellNu(L) + CellNu(C));
	};

	VrgParallel::ForEachRow(0, NF[1], 0, NF[2], [&](int32 Y, int32 Z)
	{
		for (int32 X = 0; X < NF[0]; ++X)
		{
			const int32 C[3] = { X, Y, Z };
			const int32 F = FaceIndex(C);
			const double UF = U[F];
			if (!IsOpen(C))
			{
				Out[F] = UF;
				continue;
			}
			const double NuF = FaceNu(C);
			double Sum = 0.0;
			for (int32 D = 0; D < 3; ++D)
			{
				for (int32 S = -1; S <= 1; S += 2)
				{
					int32 G[3] = { X, Y, Z };
					G[D] += S;
					if (D == Axis)
					{
						// Along the component: the two faces share a fluid cell, whose viscosity is the coefficient.
						// The neighbour may be a wall or duct face; its value is the exact Dirichlet value there.
						int32 Cell[3] = { X, Y, Z };
						if (S < 0) --Cell[Axis];
						Sum += CellNu(Cell) * (U[FaceIndex(G)] - UF);
					}
					else if (G[D] < 0 || G[D] >= N[D] || !IsOpen(G))
					{
						Sum += NuF * (-2.0 * UF);  // no-slip wall half a cell away: ghost value -u
					}
					else
					{
						Sum += 0.5 * (NuF + FaceNu(G)) * (U[FaceIndex(G)] - UF);
					}
				}
			}
			Out[F] = UF + DeltaTimeS * InvH2 * Sum;
		}
	});
}


FString FAirSolver::ValidateDucts(const FAirSolverInput& Input) const
{
	const int32 N = Input.DuctFaceIndex.Num();
	if (Input.DuctFaceAxis.Num() != N || Input.DuctFaceFluidCell.Num() != N || Input.DuctFaceDuct.Num() != N
		|| Input.DuctFaceIsExhaust.Num() != N || Input.DuctFaceVelocityMs.Num() != N)
	{
		return TEXT("배열 길이가 서로 다르다");
	}
	TArray<double> IntakeFlow, ExhaustFlow;
	IntakeFlow.Init(0.0, FMath::Max(Input.NumDucts, 0));
	ExhaustFlow.Init(0.0, FMath::Max(Input.NumDucts, 0));
	for (int32 F = 0; F < N; ++F)
	{
		const uint8 Axis = Input.DuctFaceAxis[F];
		const int32 NumFaces = Axis == 0 ? Grid.GetNumFacesX() : Axis == 1 ? Grid.GetNumFacesY() : Axis == 2 ? Grid.GetNumFacesZ() : -1;
		if (NumFaces < 0 || Input.DuctFaceIndex[F] < 0 || Input.DuctFaceIndex[F] >= NumFaces) return FString::Printf(TEXT("면 %d 인덱스가 격자 밖"), F);
		const int32 Cell = Input.DuctFaceFluidCell[F];
		if (!Grid.TemperatureK.IsValidIndex(Cell) || (Grid.bSolidCell.IsValidIndex(Cell) && Grid.bSolidCell[Cell]))
			return FString::Printf(TEXT("면 %d의 공기 셀이 격자 밖이거나 고체"), F);
		if (!Input.DuctFaceDuct.IsValidIndex(F) || Input.DuctFaceDuct[F] < 0 || Input.DuctFaceDuct[F] >= Input.NumDucts)
			return FString::Printf(TEXT("면 %d의 덕트 번호가 범위 밖"), F);
		if (!FMath::IsFinite(Input.DuctFaceVelocityMs[F])) return FString::Printf(TEXT("면 %d 속도가 유한하지 않다"), F);
		(Input.DuctFaceIsExhaust[F] ? ExhaustFlow : IntakeFlow)[Input.DuctFaceDuct[F]] += FMath::Abs(Input.DuctFaceVelocityMs[F]);
	}
	if (Input.DuctCoilUAWK.Num() > 0 || Input.DuctCoilWaterTempK.Num() > 0)
	{
		if (Input.DuctCoilUAWK.Num() != Input.NumDucts || Input.DuctCoilWaterTempK.Num() != Input.NumDucts)
			return TEXT("덕트 코일 배열 길이가 덕트 수와 다르다");
		for (int32 D = 0; D < Input.NumDucts; ++D)
			if (!FMath::IsFinite(Input.DuctCoilUAWK[D]) || Input.DuctCoilUAWK[D] < 0.0
				|| !FMath::IsFinite(Input.DuctCoilWaterTempK[D]) || Input.DuctCoilWaterTempK[D] <= 0.0)
				return FString::Printf(TEXT("덕트 %d 코일 UA 또는 냉수 온도가 잘못됐다"), D);
	}
	if (Input.DuctFanHeatW.Num() > 0)
	{
		if (Input.DuctFanHeatW.Num() != Input.NumDucts) return TEXT("덕트 팬 발열 배열 길이가 덕트 수와 다르다");
		for (int32 D = 0; D < Input.NumDucts; ++D)
			if (!FMath::IsFinite(Input.DuctFanHeatW[D]) || Input.DuctFanHeatW[D] < 0.0) return FString::Printf(TEXT("덕트 %d 팬 발열이 잘못됐다"), D);
	}
	for (int32 D = 0; D < Input.NumDucts; ++D)
	{
		// Unequal volume flow would make the Neumann pressure problem incompatible (net source in a closed box).
		const double Scale = FMath::Max(IntakeFlow[D], ExhaustFlow[D]);
		if (Scale > 0.0 && FMath::Abs(IntakeFlow[D] - ExhaustFlow[D]) > 1e-9 * Scale)
			return FString::Printf(TEXT("덕트 %d 흡기/배기 체적유량 불일치 (%.6e / %.6e, 면 속도 합)"), D, IntakeFlow[D], ExhaustFlow[D]);
	}
	return FString();
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
	Snapshot.DuctFlowM3s = LastDuctFlowM3s;
	Snapshot.DuctCoilRemovedW = LastDuctCoilRemovedW;
	Snapshot.NumDuctFaces = LastNumDuctFaces;
	Snapshot.bDuctsRejected = bLastDuctsRejected;
	if (bUseOctree && !bOctreeNeedsImport)
	{
		Snapshot.MaxAbsDivergencePerS = OctreeSolver.MaxDivergencePerS();
		Snapshot.NormalizedDivergence = OctreeSolver.NormalizedDivergence();
	}

	return Snapshot;
}

bool FAirSolver::ConfigureOctree(const FFixedOctreeSettings& Settings, const TArray<FBox>& BoxesM, FString& Error)
{
	if (!OctreeSolver.Initialize(Grid, Settings, BoxesM, Error)) return false;
	OctreeSettings = Settings; OctreeBoxesM = OctreeUserBoxesM = BoxesM;
	AdaptController.Reset(Grid.GetNumCells());
	AdaptElapsedS = 0.0;
	NumAutoAdaptFailures = 0;
	bUseOctree = true; bOctreeNeedsImport = false; return true;
}

bool FAirSolver::SetOctreeAutoAdapt(bool bEnable, const FOctreeAdaptSettings& Settings, FString& Error)
{
	Error = bEnable ? Settings.Validate() : FString();
	if (!Error.IsEmpty()) { bAutoAdapt = false; return false; }
	bAutoAdapt = bEnable;
	AdaptSettings = Settings;
	return true;
}

bool FAirSolver::AdaptOctree(const TArray<FBox>& BoxesM, double DtS, FString& Error)
{
	// Manual boxes replace the previous manual ones; the automatic ones currently in force stay.
	TArray<FBox> Combined = BoxesM, AutoBoxes;
	TArray<uint8> Flags; Flags.SetNum(Grid.GetNumCells());
	for (int32 I = 0; I < Flags.Num(); ++I) Flags[I] = AdaptController.IsRefined(I) ? 1 : 0;
	FOctreeAdaptController::MakeBoxes(Grid, Flags, AutoBoxes);
	Combined.Append(AutoBoxes);
	if (!AdaptToBoxes(Combined, DtS, Error)) return false;
	OctreeUserBoxesM = BoxesM;
	return true;
}

bool FAirSolver::AdaptToBoxes(const TArray<FBox>& BoxesM, double DtS, FString& Error)
{
	if (!bUseOctree || bOctreeNeedsImport) { Error = TEXT("Octree must be initialized before adaptation"); return false; }
	OctreeSolver.PressureSettings = PressureSettings;
	if (!OctreeSolver.Adapt(BoxesM, DtS, Error)) return false;
	OctreeBoxesM = BoxesM; OctreeSolver.ExportUniform(Grid);
	LastPressureResult = OctreeSolver.PressureResult;
	return true;
}

void FAirSolver::SolvePressure()
{
	bLastPressureRanOnGpu = false;
	if (bUseMultigridPressure)
	{
		if (bUseGpuMultigrid && IsInGameThread() && VrgAirGpu::IsAvailable())
		{
			// The hierarchy is built on the CPU (topology changes only) and uploaded when it was rebuilt.
			if (!GpuMultigrid.IsValid()) GpuMultigrid = MakeUnique<FVrgAirGpuPressure>();
			const bool bRebuilt = MultigridPoissonSolver::UpdateCache(Grid, MultigridCache);
			bool bReady = GpuMultigrid->IsConfigured() && !bRebuilt;
			if (!bReady)
			{
				FVrgMgHierarchy Hierarchy;
				MultigridPoissonSolver::ExportHierarchy(Grid, MultigridCache, Hierarchy);
				bReady = GpuMultigrid->Configure(Hierarchy);
			}
			if (bReady)
			{
				FVrgGpuPoissonSettings Settings;
				Settings.NormalizedResidualTarget = PressureSettings.NormalizedResidualTarget;
				Settings.MaxIterations = PressureSettings.MaxIterations;
				Settings.Omega = PressureSettings.Relaxation;
				const FVrgGpuPoissonResult R = GpuMultigrid->Solve(DivergencePerS, Grid.PressurePa, Settings);
				if (R.bRan)
				{
					LastPressureResult = FPoissonSolveResult();
					LastPressureResult.bConverged = R.bConverged;
					LastPressureResult.IterationsUsed = R.IterationsUsed;
					LastPressureResult.NormalizedResidual = R.NormalizedResidual;
					LastPressureResult.ReferenceScale = R.ReferenceScale;
					LastPressureResult.bIncompatibleRightHandSide = R.bIncompatibleRightHandSide;
					LastPressureResult.MeanRightHandSide = R.MeanRightHandSide;
					bLastPressureRanOnGpu = true;
					return;
				}
			}
			// Not silent (D5 spirit): say once that the CPU is solving instead.
			if (!bGpuMultigridFallbackLogged)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Air] GPU 압력(MG-PCG)을 쓰지 못해 CPU로 푼다."));
				bGpuMultigridFallbackLogged = true;
			}
		}
		LastPressureResult = MultigridPoissonSolver::Solve(Grid, DivergencePerS, Grid.PressurePa, MultigridCache,
			PressureSettings);
		return;
	}
	if (bUseGpuPressure && SolvePressureOnGpu())
	{
		bLastPressureRanOnGpu = true;
		return;
	}
	// GPU를 껐거나 실패했다. 기준이자 폴백인 CPU FP64 경로다.
	LastPressureResult = JacobiPoissonSolver::Solve(Grid, DivergencePerS, Grid.PressurePa,
		PressureScratch, PressureSettings);
}

bool FAirSolver::SolvePressureOnGpu()
{
	const int32 NumCells = Grid.GetNumCells();
	if (NumCells <= 0 || DivergencePerS.Num() != NumCells || !VrgPressureGpu::IsAvailable())
	{
		return false;
	}

	FPoissonSolveResult Result;

	// 정규화 기준과 적합성은 CPU 경로와 같은 방식으로 잰다 (D5).
	double ReferenceScale = 0.0;
	double SumRightHandSide = 0.0;
	int32 NumFluidCells = 0;
	GpuRightHandSide.SetNumUninitialized(NumCells);
	GpuFluidMask.SetNumUninitialized(NumCells);
	GpuField.SetNumUninitialized(NumCells);
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 Index = Grid.CellIndex(X, Y, Z);
				const bool bFluid = Grid.IsFluidCell(X, Y, Z);
				GpuFluidMask[Index] = bFluid ? 1u : 0u;
				GpuRightHandSide[Index] = float(DivergencePerS[Index]);
				GpuField[Index] = float(Grid.PressurePa[Index]);
				if (bFluid)
				{
					ReferenceScale = FMath::Max(ReferenceScale, FMath::Abs(DivergencePerS[Index]));
					SumRightHandSide += DivergencePerS[Index];
					++NumFluidCells;
				}
			}
		}
	}
	Result.ReferenceScale = ReferenceScale;
	Result.MeanRightHandSide = NumFluidCells > 0 ? SumRightHandSide / NumFluidCells : 0.0;

	if (ReferenceScale <= UE_DOUBLE_SMALL_NUMBER)
	{
		for (double& Value : Grid.PressurePa) { Value = 0.0; }
		Result.bConverged = true;
		LastPressureResult = Result;
		return true;
	}
	Result.bIncompatibleRightHandSide =
		FMath::Abs(Result.MeanRightHandSide) > 1.0e-10 * ReferenceScale;

	FVrgPressureGpuRequest Request;
	Request.GridSize = FIntVector(Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ());
	Request.CellSizeM = float(Grid.GetCellSizeM());
	Request.Omega = float(FMath::Clamp(PressureSettings.Relaxation, 0.05, 1.0));

	// 한 번에 다 돌리지 않고 덩어리로 나눈다. 매 반복 내려받으면 왕복 비용이 계산을 넘고,
	// 한 번에 상한까지 돌리면 이미 수렴한 뒤에도 계속 돈다.
	// 덩어리마다 CPU에서 평균을 빼고 잔차를 재서 D5대로 판정한다.
	const int32 ChunkSize = FMath::Max(8, PressureSettings.ResidualCheckInterval * 4);
	const int32 MaxIterations = FMath::Max(1, PressureSettings.MaxIterations);

	for (int32 Done = 0; Done < MaxIterations; Done += ChunkSize)
	{
		Request.Iterations = FMath::Min(ChunkSize, MaxIterations - Done);
		if (!VrgPressureGpu::RunJacobiSweeps(Request, GpuRightHandSide, GpuFluidMask, GpuField))
		{
			// 여기서 실패하면 압력장을 건드리지 않은 상태다. 호출자가 CPU로 간다.
			return false;
		}
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			Grid.PressurePa[Index] = double(GpuField[Index]);
		}
		// CPU 경로는 매 반복 뺐지만 여기서는 덩어리마다 뺀다.
		// 상수를 더해도 Laplacian이 0이라 잔차가 변하지 않으므로 수렴에는 영향이 없다.
		JacobiPoissonSolver::RemoveFluidMean(Grid, Grid.PressurePa);

		Result.IterationsUsed = Done + Request.Iterations;
		Result.NormalizedResidual =
			JacobiPoissonSolver::MaxAbsResidual(Grid, Grid.PressurePa, DivergencePerS) / ReferenceScale;
		if (Result.NormalizedResidual <= PressureSettings.NormalizedResidualTarget)
		{
			Result.bConverged = true;
			LastPressureResult = Result;
			return true;
		}
		for (int32 Index = 0; Index < NumCells; ++Index)
		{
			GpuField[Index] = float(Grid.PressurePa[Index]);
		}
	}

	// D5: 목표에 못 미친 채 상한에 걸렸다. 숨기지 않는다.
	Result.bConverged = false;
	LastPressureResult = Result;
	return true;
}

bool FAirSolver::CompareGpuAndCpuPressure(double& OutNormalizedDifference, FString& OutSummary)
{
	OutNormalizedDifference = 0.0;
	const int32 NumCells = Grid.GetNumCells();
	if (NumCells <= 0 || DivergencePerS.Num() != NumCells)
	{
		OutSummary = TEXT("발산 우변이 없다. 스텝을 한 번 돌린 뒤에 비교해야 한다.");
		return false;
	}
	if (!VrgPressureGpu::IsAvailable())
	{
		OutSummary = TEXT("이 실행에서는 컴퓨트 경로를 쓸 수 없다.");
		return false;
	}

	// 원래 압력장을 건드리지 않는다.
	const TArray<double> SavedPressure = Grid.PressurePa;
	const bool bSavedUseGpu = bUseGpuPressure;

	// 같은 초기 추정값(0)에서 출발해야 둘을 비교할 수 있다.
	for (double& Value : Grid.PressurePa) { Value = 0.0; }
	bUseGpuPressure = false;
	SolvePressure();
	const FPoissonSolveResult CpuResult = LastPressureResult;
	const TArray<double> CpuSolution = Grid.PressurePa;

	for (double& Value : Grid.PressurePa) { Value = 0.0; }
	bUseGpuPressure = true;
	SolvePressure();
	const FPoissonSolveResult GpuResult = LastPressureResult;
	const bool bRanOnGpu = bLastPressureRanOnGpu;
	const TArray<double> GpuSolution = Grid.PressurePa;

	Grid.PressurePa = SavedPressure;
	bUseGpuPressure = bSavedUseGpu;

	if (!bRanOnGpu)
	{
		OutSummary = TEXT("GPU 경로가 실패해 CPU로 떨어졌다. 비교할 수 없다.");
		return false;
	}

	double MaxAbsDifference = 0.0;
	double MaxAbsCpu = 0.0;
	for (int32 Index = 0; Index < NumCells; ++Index)
	{
		MaxAbsDifference = FMath::Max(MaxAbsDifference, FMath::Abs(GpuSolution[Index] - CpuSolution[Index]));
		MaxAbsCpu = FMath::Max(MaxAbsCpu, FMath::Abs(CpuSolution[Index]));
	}
	OutNormalizedDifference = MaxAbsCpu > 0.0 ? MaxAbsDifference / MaxAbsCpu : 0.0;

	// 검증 규칙: 비트 동일성이 아니라 허용 오차로 본다. 정규화 기준을 함께 적는다.
	OutSummary = FString::Printf(
		TEXT("셀 %d | CPU FP64 %s %d회 잔차 %.3e | GPU FP32 %s %d회 잔차 %.3e | ")
		TEXT("최대 차이 %.3e / CPU 해 최대 크기 %.6f 기준 정규화 %.3e"),
		NumCells,
		CpuResult.bConverged ? TEXT("수렴") : TEXT("수렴실패"), CpuResult.IterationsUsed, CpuResult.NormalizedResidual,
		GpuResult.bConverged ? TEXT("수렴") : TEXT("수렴실패"), GpuResult.IterationsUsed, GpuResult.NormalizedResidual,
		MaxAbsDifference, MaxAbsCpu, OutNormalizedDifference);
	return true;
}
