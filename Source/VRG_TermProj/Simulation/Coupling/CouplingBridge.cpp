// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Coupling/CouplingBridge.h"

#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Thermal/RackThermalModel.h"

void FCouplingBridge::Initialize(const TArray<FRackCouplingTarget>& InTargets,
	const TArray<FRackCouplingLink>& InLinks)
{
	Targets = InTargets;
	Links = InLinks;

	// 짝이 안 맞으면 인덱스로 서로를 가리킬 수 없다. 짧은 쪽에 맞춘다.
	const int32 NumRacks = FMath::Min(Targets.Num(), Links.Num());
	Targets.SetNum(NumRacks);
	Links.SetNum(NumRacks);

	EnergyInJ = 0.0;
	EnergyOutJ = 0.0;
	RadiatorRejectedJ = 0.0;
	LastItHeatW = 0.0;
	LastRadiatorRejectedW = 0.0;
	LastAirCoolingRemovedW = 0.0;
	bLastHydraulicsResolved = false;
	bLastHydraulicsConverged = true;

	LastCoolantMassFlowKgs.Init(0.0, NumRacks);
	LastAirInletTempK.Init(0.0, NumRacks);
	LastHeatToAirW.Init(0.0, NumRacks);
}

void FCouplingBridge::PullCoolantFlow()
{
	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		FRackThermalInput* Input = Targets[RackIndex].Input;
		if (Input == nullptr)
		{
			continue;
		}

		const int32 PipeIndex = Links[RackIndex].BranchPipeIndex;

		double MassFlowKgs = Links[RackIndex].FallbackCoolantMassFlowKgs;
		if (HydraulicNetwork != nullptr && PipeIndex != INDEX_NONE)
		{
			// 수력은 체적유량 [m^3/s]으로 푼다. 열모델은 질량유량 [kg/s]을 받는다.
			// 바꾸는 곳을 여기 하나로 둬야 밀도가 두 군데로 갈라지지 않는다 (규약 3·6).
			const double FlowM3s = FMath::Abs(HydraulicNetwork->GetPipeFlowM3s(PipeIndex));
			MassFlowKgs = FlowM3s * CoolantDensityKgM3;
		}

		Input->CoolantMassFlowKgs = MassFlowKgs;
		LastCoolantMassFlowKgs[RackIndex] = MassFlowKgs;
	}
}

void FCouplingBridge::PullAirInletTemperature()
{
	if (AirSolver == nullptr)
	{
		for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
		{
			const FRackThermalInput* Input = Targets[RackIndex].Input;
			LastAirInletTempK[RackIndex] = Input != nullptr ? Input->AirInletTempK : 0.0;
		}
		return;
	}

	const FUniformAirGrid& Grid = AirSolver->GetGrid();

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		FRackThermalInput* Input = Targets[RackIndex].Input;
		if (Input == nullptr)
		{
			continue;
		}

		// 흡기 셀들의 평균 온도를 랙 흡기 온도로 준다.
		// 셀이 하나도 안 잡히면 랙이 들고 있던 값을 그대로 둔다 (격자 밖에 있는 랙).
		double SumTempK = 0.0;
		int32 Count = 0;
		for (const int32 CellIndex : Links[RackIndex].InletCellIndices)
		{
			if (!Grid.TemperatureK.IsValidIndex(CellIndex))
			{
				continue;
			}
			SumTempK += Grid.TemperatureK[CellIndex];
			++Count;
		}

		if (Count > 0)
		{
			Input->AirInletTempK = SumTempK / Count;
		}
		LastAirInletTempK[RackIndex] = Input->AirInletTempK;
	}
}

void FCouplingBridge::CloseCoolantLoop(double DeltaTimeS)
{
	double RejectedW = 0.0;

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const FRackCouplingTarget& Target = Targets[RackIndex];
		if (Target.Model == nullptr || Target.Input == nullptr || Target.Radiator == nullptr)
		{
			continue;
		}

		const double CoolantCpJKgK = Target.Model->GetParams().CoolantCpJKgK;
		const double MassFlowKgs = Target.Input->CoolantMassFlowKgs;

		// 랙 출구 = 지금 냉각수 노드 온도. 랙 모델도 스텝을 시작할 때 이 값을 읽는다.
		const double OutletTempK = Target.Model->GetState().CoolantTempK;

		// 버린 열과 되돌아오는 온도를 **같은 effectiveness로 한 번에** 구한다 (규약 6).
		// 그래서 랙이 냉각수로 내보낸 열과 라디에이터가 버린 열이 정확히 같아진다.
		//   랙이 내보낸 열 = m*cp*(T_out - T_in)
		//   라디에이터가 버린 열 = m*cp*eps*(T_out - T_amb)
		//   T_in = T_out - eps*(T_out - T_amb)  이므로 둘이 같다.
		// 둘을 따로 계산하면 전역 장부가 그 차이만큼 어긋난다.
		Target.Input->CoolantInletTempK =
			RadiatorModel::OutletTempK(*Target.Radiator, OutletTempK, MassFlowKgs, CoolantCpJKgK);

		RejectedW +=
			RadiatorModel::HeatRejectedW(*Target.Radiator, OutletTempK, MassFlowKgs, CoolantCpJKgK);
	}

	LastRadiatorRejectedW = RejectedW;
	RadiatorRejectedJ += RejectedW * DeltaTimeS;
}

void FCouplingBridge::PushHeatToAir()
{
	double TotalHeatToAirW = 0.0;

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const FRackThermalModel* Model = Targets[RackIndex].Model;
		if (Model == nullptr)
		{
			LastHeatToAirW[RackIndex] = 0.0;
			continue;
		}

		// D1: 부품별로 쪼개 넣지 않는다. 강제 공랭과 케이스 자연대류를 **합산해 하나**로 준다.
		// 자연대류를 빼면 그만큼 전역 장부에서 새어 나간다 (그 열도 방 공기를 데운다).
		const FRackThermalSnapshot Snapshot = Model->MakeSnapshot(0.0);
		const double HeatW = Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW;

		LastHeatToAirW[RackIndex] = HeatW;
		TotalHeatToAirW += HeatW;
	}

	if (AirSolver == nullptr)
	{
		return;
	}

	const FUniformAirGrid& Grid = AirSolver->GetGrid();
	ScratchCellHeatW.Init(0.0, Grid.GetNumCells());

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const TArray<int32>& ExhaustCells = Links[RackIndex].ExhaustCellIndices;
		if (ExhaustCells.Num() == 0)
		{
			continue;
		}

		// 배기 셀에 고르게 나눈다. 겹치면 더한다 - 덮어쓰면 열이 사라진다.
		const double PerCellW = LastHeatToAirW[RackIndex] / ExhaustCells.Num();
		for (const int32 CellIndex : ExhaustCells)
		{
			if (ScratchCellHeatW.IsValidIndex(CellIndex))
			{
				ScratchCellHeatW[CellIndex] += PerCellW;
			}
		}
	}
}

void FCouplingBridge::Step(double DeltaTimeS, bool bHydraulicsResolved, bool bHydraulicsConverged)
{
	if (DeltaTimeS <= 0.0)
	{
		return;
	}

	bLastHydraulicsResolved = bHydraulicsResolved;
	bLastHydraulicsConverged = bHydraulicsConverged;

	// 1) 수력 -> 열: 랙 분기 유량을 냉각수 질량유량으로 넘긴다.
	PullCoolantFlow();

	// 2) 공기 -> 열: 랙 앞 공기 온도를 흡기 온도로 넘긴다.
	PullAirInletTemperature();

	// 3) 냉각수 루프를 라디에이터로 닫는다. 버린 열을 한 번만 계산해 장부에 적는다.
	CloseCoolantLoop(DeltaTimeS);

	// 4) 랙 열모델을 돌린다. 규약 4대로 모든 도메인이 같은 dt를 쓴다.
	double ItHeatW = 0.0;
	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const FRackCouplingTarget& Target = Targets[RackIndex];
		if (Target.Model == nullptr || Target.Input == nullptr)
		{
			continue;
		}

		Target.Model->Step(*Target.Input, DeltaTimeS);
		ItHeatW += Target.Model->MakeSnapshot(0.0).TotalHeatW;
	}
	LastItHeatW = ItHeatW;

	// 5) 열 -> 공기: 랙이 공기로 내보낸 열을 체적 열원으로 민다 (D1: 합산해 하나로).
	PushHeatToAir();

	// 6) 공기 격자를 같은 dt로 돌린다.
	if (AirSolver != nullptr)
	{
		FAirSolverInput& AirInput = AirSolverInput;
		AirInput.CellHeatW = ScratchCellHeatW;

		const double CoolingBeforeJ = AirSolver->GetEnergyOutJ();
		{
			// S7-0 계측. 통합 경로에서도 공기 솔버 몫을 따로 뗀다.
			// SCOPE_CYCLE_COUNTER만 스레드 안전하므로 누산기 갱신은 아래에서 한 번에 한다.
			SCOPE_CYCLE_COUNTER(STAT_VrgAirSolverStep);
			const uint64 StartCycles = FPlatformTime::Cycles64();
			AirSolver->Step(AirInput, DeltaTimeS);
			LastAirSolverMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
		}
		LastAirCoolingRemovedW = (AirSolver->GetEnergyOutJ() - CoolingBeforeJ) / DeltaTimeS;
	}
	else
	{
		LastAirCoolingRemovedW = 0.0;
	}

	// 7) 전역 장부. 유입은 랙 모델이 이미 세고 있으므로 합치기만 한다.
	EnergyInJ = 0.0;
	for (const FRackCouplingTarget& Target : Targets)
	{
		if (Target.Model != nullptr)
		{
			EnergyInJ += Target.Model->GetEnergyInJ();
		}
	}

	// 유출은 계 밖으로 나간 것만이다: 라디에이터(냉각수)와 CRAH 코일(공기).
	// 랙에서 공기로 넘어간 열은 계 안에서의 이동이라 여기 넣지 않는다.
	EnergyOutJ = RadiatorRejectedJ + (AirSolver != nullptr ? AirSolver->GetEnergyOutJ() : 0.0);
}

double FCouplingBridge::GetEnergyStoredJ() const
{
	double StoredJ = 0.0;

	for (const FRackCouplingTarget& Target : Targets)
	{
		if (Target.Model != nullptr)
		{
			StoredJ += Target.Model->GetEnergyStoredJ();
		}
	}

	if (AirSolver != nullptr)
	{
		StoredJ += AirSolver->GetEnergyStoredJ();
	}

	return StoredJ;
}

double FCouplingBridge::GetEnergyResidualJ() const
{
	return EnergyInJ - EnergyOutJ - GetEnergyStoredJ();
}

double FCouplingBridge::GetNormalizedEnergyResidual() const
{
	if (FMath::Abs(EnergyInJ) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return GetEnergyResidualJ() / EnergyInJ;
}

FCouplingSnapshot FCouplingBridge::MakeSnapshot(double SimTimeS) const
{
	FCouplingSnapshot Snapshot;
	Snapshot.SimTimeS = SimTimeS;
	Snapshot.RackCoolantMassFlowKgs = LastCoolantMassFlowKgs;
	Snapshot.RackAirInletTempK = LastAirInletTempK;
	Snapshot.RackHeatToAirW = LastHeatToAirW;

	Snapshot.TotalItHeatW = LastItHeatW;
	Snapshot.TotalRadiatorRejectedW = LastRadiatorRejectedW;
	Snapshot.TotalAirCoolingRemovedW = LastAirCoolingRemovedW;

	Snapshot.EnergyInJ = EnergyInJ;
	Snapshot.EnergyOutJ = EnergyOutJ;
	Snapshot.EnergyStoredJ = GetEnergyStoredJ();
	Snapshot.NormalizedEnergyResidual = GetNormalizedEnergyResidual();

	Snapshot.bHydraulicsResolvedThisStep = bLastHydraulicsResolved;
	Snapshot.bHydraulicsConverged = bLastHydraulicsConverged;
	Snapshot.bAirPressureConverged =
		(AirSolver == nullptr) || AirSolver->GetLastPressureResult().bConverged;

	return Snapshot;
}
