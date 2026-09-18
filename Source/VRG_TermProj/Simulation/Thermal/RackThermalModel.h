// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Thermal/RackThermalTypes.h"

// S1: 랙 1대 과도 열모델.
//
// 노드 2개. 둘 다 열용량을 가진 과도 노드다 (D2).
//   칩(IT 장비):  C_chip * dT/dt = HeatW - Heat_to_coolant - Heat_to_air
//   냉각수:       C_cool * dT/dt = Heat_to_coolant - Heat_removed_by_coolant
//
// 룸 공기는 준정상(D2)이라 열용량 노드를 두지 않고 유효 전도도로만 열을 가져간다.
//
// D2 준수: 어디에서도 HeatW/(MassFlowKgs*Cp)를 계산하지 않는다. 유량이 분모에
// 오는 식이 없으므로 유량 0에서 특이해지지 않는다. 유량이 0이면 냉각량이 0이 되고
// 열이 축적되어 온도가 올라간다.
//
// 규약 6 준수: 열유속은 스텝당 한 번만 계산해서 주는 쪽과 받는 쪽에 부호만 바꿔
// 같은 값으로 적용한다. 각 노드가 같은 열유속을 따로 다시 계산하지 않는다.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않으므로 워커 스레드에서 돌릴 수 있다.
class FRackThermalModel
{
public:
	void Initialize(const FRackThermalParams& InParams, const FRackThermalState& InitialState);

	// 고정 스텝 한 번을 명시적 오일러로 적분한다.
	void Step(const FRackThermalInput& Input, double DeltaTimeS);

	FRackThermalSnapshot MakeSnapshot(double SimTimeS) const;

	const FRackThermalState& GetState() const { return State; }
	const FRackThermalParams& GetParams() const { return Params; }

	// 누적 에너지 [J]. 보존 검사용.
	double GetEnergyInJ() const { return EnergyInJ; }
	double GetEnergyOutJ() const { return EnergyOutJ; }

	// 초기 상태 대비 두 노드에 쌓인 에너지 [J].
	double GetEnergyStoredJ() const;

	// 유입 - 유출 - 축적 [J]. 0에 가까워야 한다.
	double GetEnergyResidualJ() const;

	// 잔차를 총 유입 에너지로 나눈 무차원 값. 유입이 0이면 0을 돌려준다.
	double GetNormalizedEnergyResidual() const;

	// 명시적 오일러 단조 조건: dt * (노드 전도도 합 / 열용량) <= 1.
	// 위반하면 진동하거나 발산한다.
	bool IsStepStable(const FRackThermalInput& Input, double DeltaTimeS) const;

	// 공기 쪽 유효 전도도 [W/K]. epsilon-NTU 기반이라 유량 0에서 0으로 수렴하고,
	// 유량이 커져도 UA를 넘지 않는다. 유량으로 나누는 연산이 없다.
	static double EffectiveAirConductanceWK(const FRackThermalParams& InParams, double AirMassFlowKgs);

private:
	FRackThermalParams Params;
	FRackThermalState State;
	FRackThermalState InitialState;

	double EnergyInJ = 0.0;
	double EnergyOutJ = 0.0;

	// 마지막 스텝의 열유속. 스냅샷 표시용이며 다시 계산하지 않는다.
	double LastHeatChipToCoolantW = 0.0;
	double LastHeatChipToAirW = 0.0;
	double LastHeatRemovedByCoolantW = 0.0;
	bool bLastStepStable = true;
};
