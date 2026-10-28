#pragma once

#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Thermal/RackThermalTypes.h"

// W3.5: 순서는 슬롯 높이순. 지점은 공급 티/공급 연결/플레이트/회수 연결/회수 티.
struct FInRackSpec
{
	TArray<int32> Slots;
	TArray<double> HeightM, DesignFraction;
	TArray<bool> Docked;
	double EquivalentResistancePaS2M6 = 3.4e10;
	double DesignFlowM3s = 0.0023;
};

struct FInRackHandles
{
	TArray<int32> Slots, Nodes, Leaks, Pipes, InletPipes, OutletPipes;
	TArray<int32> SupplyManifoldPipes, ReturnManifoldPipes;
	TArray<double> DesignFraction;
	int32 SupplyNode = INDEX_NONE, ReturnNode = INDEX_NONE;
	// 전체/트레이/매니폴드의 유량 잔차 [m³/s]. 회수 티 누수를 중복해서 더하지 않는다.
	double MaxBalanceErrorM3s(const FHydraulicNetwork& Network) const;
	void SetDocked(FHydraulicNetwork& Network, int32 Tray, bool bDocked) const;
};

namespace InRackNetwork
{
	FInRackSpec FromThermal(const FRackThermalParams& Params, const FRackThermalInput& Input);
	// 외부 공급/회수 분기를 제외한 랙 안 망. 기존 두 분기 합의 98%를 사용한다.
	FInRackHandles Add(FHydraulicNetwork& Network, const FInRackSpec& Spec, int32 SupplyNode, int32 ReturnNode);
	// Danfoss GB200 인증 Compact UQD04 Cv=1.37. 실제 설치 모델 선택은 추정.
	// https://assets.danfoss.com/documents/latest/398195/AD489931119420en-000102.pdf p.2
	double ConnectorResistancePaS2M6();
	// 같은 자료의 3/8 inch hose barb 선택을 호스 내경으로 적용한 추정. 유로 지름 5.5 mm와 다르다.
	constexpr double HoseDiameterM = 0.009525;
}
