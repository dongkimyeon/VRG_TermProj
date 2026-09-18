// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Thermal/RackThermalModel.h"

void FRackThermalModel::Initialize(const FRackThermalParams& InParams, const FRackThermalState& InInitialState)
{
	Params = InParams;
	State = InInitialState;
	InitialState = InInitialState;

	EnergyInJ = 0.0;
	EnergyOutJ = 0.0;
	LastHeatChipToCoolantW = 0.0;
	LastHeatChipToAirW = 0.0;
	LastHeatRemovedByCoolantW = 0.0;
	bLastStepStable = true;
}

double FRackThermalModel::EffectiveAirConductanceWK(const FRackThermalParams& InParams, double AirMassFlowKgs)
{
	// 공기가 실어 나를 수 있는 열용량률 [W/K].
	const double CapacityRateWK = AirMassFlowKgs * InParams.AirCpJKgK;

	// D2: 유량 0에서 나눗셈을 하지 않는다. 흐르는 공기가 없으면 뺏기는 열도 없다.
	if (CapacityRateWK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}

	const double NTU = InParams.ChipToAirUAWK / CapacityRateWK;
	return CapacityRateWK * (1.0 - FMath::Exp(-NTU));
}

bool FRackThermalModel::IsStepStable(const FRackThermalInput& Input, double DeltaTimeS) const
{
	const double AirConductanceWK = EffectiveAirConductanceWK(Params, Input.AirMassFlowKgs);
	const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;

	const double ChipRate = (Params.ChipToCoolantUAWK + AirConductanceWK) / Params.ChipHeatCapacityJK;
	const double CoolantRate = (Params.ChipToCoolantUAWK + CoolantCapacityRateWK) / Params.CoolantHeatCapacityJK;

	return DeltaTimeS * FMath::Max(ChipRate, CoolantRate) <= 1.0;
}

void FRackThermalModel::Step(const FRackThermalInput& Input, double DeltaTimeS)
{
	bLastStepStable = IsStepStable(Input, DeltaTimeS);

	// 열유속 3개를 여기서 한 번만 계산한다 (규약 6).
	const double HeatChipToCoolantW = Params.ChipToCoolantUAWK * (State.ChipTempK - State.CoolantTempK);

	const double AirConductanceWK = EffectiveAirConductanceWK(Params, Input.AirMassFlowKgs);
	const double HeatChipToAirW = AirConductanceWK * (State.ChipTempK - Input.AirInletTempK);

	// 냉각수가 이류로 랙 밖에 내보내는 열. 유량이 분자에 있으므로 유량 0이면 그대로 0이다.
	const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;
	const double HeatRemovedByCoolantW = CoolantCapacityRateWK * (State.CoolantTempK - Input.CoolantInletTempK);

	// 같은 HeatChipToCoolantW가 칩에서 빠지고 냉각수로 들어간다.
	const double ChipRateKs = (Input.HeatW - HeatChipToCoolantW - HeatChipToAirW) / Params.ChipHeatCapacityJK;
	const double CoolantRateKs = (HeatChipToCoolantW - HeatRemovedByCoolantW) / Params.CoolantHeatCapacityJK;

	State.ChipTempK += ChipRateKs * DeltaTimeS;
	State.CoolantTempK += CoolantRateKs * DeltaTimeS;

	EnergyInJ += Input.HeatW * DeltaTimeS;
	EnergyOutJ += (HeatChipToAirW + HeatRemovedByCoolantW) * DeltaTimeS;

	LastHeatChipToCoolantW = HeatChipToCoolantW;
	LastHeatChipToAirW = HeatChipToAirW;
	LastHeatRemovedByCoolantW = HeatRemovedByCoolantW;
}

double FRackThermalModel::GetEnergyStoredJ() const
{
	return Params.ChipHeatCapacityJK * (State.ChipTempK - InitialState.ChipTempK)
		+ Params.CoolantHeatCapacityJK * (State.CoolantTempK - InitialState.CoolantTempK);
}

double FRackThermalModel::GetEnergyResidualJ() const
{
	return EnergyInJ - EnergyOutJ - GetEnergyStoredJ();
}

double FRackThermalModel::GetNormalizedEnergyResidual() const
{
	if (FMath::Abs(EnergyInJ) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return GetEnergyResidualJ() / EnergyInJ;
}

FRackThermalSnapshot FRackThermalModel::MakeSnapshot(double SimTimeS) const
{
	FRackThermalSnapshot Snapshot;
	Snapshot.SimTimeS = SimTimeS;
	Snapshot.ChipTempK = State.ChipTempK;
	Snapshot.CoolantTempK = State.CoolantTempK;
	Snapshot.CoolantOutletTempK = State.CoolantTempK;
	Snapshot.HeatChipToCoolantW = LastHeatChipToCoolantW;
	Snapshot.HeatChipToAirW = LastHeatChipToAirW;
	Snapshot.HeatRemovedByCoolantW = LastHeatRemovedByCoolantW;
	Snapshot.EnergyResidualJ = GetEnergyResidualJ();
	Snapshot.NormalizedEnergyResidual = GetNormalizedEnergyResidual();
	Snapshot.bStepStable = bLastStepStable;
	return Snapshot;
}
