// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/HydraulicTypes.h"

// S2: 배관 수력 네트워크.
//
// D3: 노드 압력을 미지수로 두고 Newton으로 푼다. Hardy Cross를 쓰지 않는다.
// 펌프 유량은 보조 미지수로 함께 푼다.
// 미지수 벡터 = [자유 노드 압력 (Pa) ..., 펌프 유량 (m^3/s) ...]
// 방정식 = [자유 노드 질량 보존 (m^3/s) ..., 펌프 곡선 (Pa) ...]
//
// 파이프 유량은 압력차에서 직접 나온다:
//   Flow = sign(Δp) * sqrt(|Δp| / Resistance)
// 이 식의 도함수는 Δp=0에서 발산하므로 Jacobian에만 하한을 건다.
// 잔차에는 하한을 걸지 않으므로 수렴한 해 자체는 정확하다.
//
// S2b height (2026-10-20): every node has an elevation z [m]. Pipes and pumps see the piezometric pressure
//   P* = P + rho * g * z
// so a vertical pipe at rest holds P_bottom - P_top = rho*g*dz, and around a closed loop the height terms cancel:
// circulating flows do not depend on the heights, only the static pressures do. Unknowns stay static pressures.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않는다.
class FHydraulicNetwork
{
public:
	int32 AddNode(const FHydraulicNode& Node);
	int32 AddPipe(const FHydraulicPipe& Pipe);
	int32 AddPump(const FHydraulicPump& Pump);
	void Reset();

	int32 GetNumNodes() const { return Nodes.Num(); }
	int32 GetNumPipes() const { return Pipes.Num(); }
	int32 GetNumPumps() const { return Pumps.Num(); }

	FHydraulicSolveResult Solve(const FHydraulicSolveSettings& Settings);

	double GetNodePressurePa(int32 NodeIndex) const;

	// Node height [m] and the fluid density / gravity of the height term. Water 997 kg/m^3 (about 25 C) is the
	// fallback; the integrated actor passes its CoolantDensityKgM3 (one source). Standard gravity 9.80665 m/s^2.
	void SetNodeElevationM(int32 NodeIndex, double ElevationM);
	double GetNodeElevationM(int32 NodeIndex) const { return Nodes.IsValidIndex(NodeIndex) ? Nodes[NodeIndex].ElevationM : 0.0; }
	void SetFluidDensityKgM3(double InDensityKgM3) { FluidDensityKgM3 = InDensityKgM3; }
	double GetFluidDensityKgM3() const { return FluidDensityKgM3; }
	static constexpr double GravityMS2 = 9.80665;
	// rho * g * z of a node [Pa].
	double NodeHydrostaticPa(int32 NodeIndex) const { return FluidDensityKgM3 * GravityMS2 * Nodes[NodeIndex].ElevationM; }
	// Static pressure + rho*g*z [Pa].
	double GetNodePiezometricPa(int32 NodeIndex) const
	{
		return NodePressurePa.IsValidIndex(NodeIndex) ? NodePressurePa[NodeIndex] + NodeHydrostaticPa(NodeIndex) : 0.0;
	}

	// 위상 조회. 시각화처럼 그래프를 되짚어야 하는 쪽에서 쓴다.
	int32 GetPipeFromNodeIndex(int32 PipeIndex) const;
	int32 GetPipeToNodeIndex(int32 PipeIndex) const;
	int32 GetPumpFromNodeIndex(int32 PumpIndex) const { return Pumps.IsValidIndex(PumpIndex) ? Pumps[PumpIndex].FromNodeIndex : INDEX_NONE; }
	int32 GetPumpToNodeIndex(int32 PumpIndex) const { return Pumps.IsValidIndex(PumpIndex) ? Pumps[PumpIndex].ToNodeIndex : INDEX_NONE; }

	// 파이프 유량 [m^3/s]. FromNode -> ToNode 방향이 양수다.
	double GetPipeFlowM3s(int32 PipeIndex) const;

	// 펌프 유량 [m^3/s]. FromNode -> ToNode 방향이 양수다.
	double GetPumpFlowM3s(int32 PumpIndex) const;

	// 펌프가 만들어 낸 압력 상승 [Pa] (piezometric, i.e. including any height difference between its ports).
	double GetPumpDeltaPPa(int32 PumpIndex) const;

	// D8: 운전점에서 유체에 전달된 동력 [W] = Δp * FlowM3s.
	double GetPumpHydraulicPowerW(int32 PumpIndex) const;

	// D8: 전기 입력 [W] = 유체 동력 / 효율. 정격 전력에서 유도하지 않는다.
	double GetPumpElectricalInputPowerW(int32 PumpIndex) const;

	// 이 노드의 순 유출 - 외부 유입 [m^3/s]. 보존 검사용이며 0에 가까워야 한다.
	double GetNodeImbalanceM3s(int32 NodeIndex) const;

	// 파이프 유량을 압력차에서 계산한다. 잔차용이라 하한을 걸지 않는다.
	static double PipeFlowM3s(double DeltaPPa, double ResistancePaS2M6);

private:
	TArray<FHydraulicNode> Nodes;
	TArray<FHydraulicPipe> Pipes;
	TArray<FHydraulicPump> Pumps;
	double FluidDensityKgM3 = 997.0;

	// 해. Solve가 채운다.
	TArray<double> NodePressurePa;
	TArray<double> PumpFlowM3s;

	// 자유 노드 -> 미지수 인덱스. 고정 압력 노드는 INDEX_NONE.
	TArray<int32> NodeToUnknown;

	void BuildUnknownMap(int32& OutNumFreeNodes);
	void SeedInitialGuess();

	// 잔차를 계산하고 정규화 잔차를 돌려준다.
	double EvaluateResidual(const FHydraulicSolveSettings& Settings, TArray<double>& OutResidual,
		int32 NumFreeNodes, double& OutReferenceFlowM3s, double& OutReferencePressurePa) const;
};
