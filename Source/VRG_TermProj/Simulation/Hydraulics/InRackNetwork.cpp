#include "Simulation/Hydraulics/InRackNetwork.h"
#include "Simulation/Thermal/RackThermalModel.h"

double InRackNetwork::ConnectorResistancePaS2M6()
{
	// OCP UQDB methodology p.6: Cv = flow[GPM] sqrt(SG / dp[psi]). 물 SG=1 기준.
	// https://www.opencompute.org/documents/whitepaper-blind-mate-universal-quick-disconnect-methodology-rev-1-0-pdf
	return 6894.757293168 / FMath::Square(1.37 * 0.003785411784 / 60.0);
}

FInRackSpec InRackNetwork::FromThermal(const FRackThermalParams& Params, const FRackThermalInput& Input)
{
	FInRackSpec Spec;
	TArray<int32> Order;
	for (int32 S = 0; S < Params.Slots.Num(); ++S)
		if (Params.DeviceTypes[Params.Slots[S].DeviceType].bLiquidCooled) Order.Add(S);
	Order.Sort([&](int32 A, int32 B) { return Params.Slots[A].BottomZM < Params.Slots[B].BottomZM; });
	double SumW = 0.0;
	for (const int32 S : Order)
	{
		FRackThermalInput Design = Input;
		Design.SledDocked.Init(false, Params.Slots.Num()); Design.SledDocked[S] = true;
		const double HeatW = FRackThermalModel::DesignHeatToCoolantW(Params, Design);
		Spec.Slots.Add(S); Spec.HeightM.Add(Params.Slots[S].BottomZM + 0.5 * Params.Slots[S].HeightM);
		Spec.DesignFraction.Add(HeatW); SumW += HeatW;
		Spec.Docked.Add(Input.SledDocked.Num() != Params.Slots.Num() || Input.SledDocked[S]);
	}
	if (SumW > 0.0) for (double& F : Spec.DesignFraction) F /= SumW;
	return Spec;
}

FInRackHandles InRackNetwork::Add(FHydraulicNetwork& Network, const FInRackSpec& Spec, int32 SupplyNode, int32 ReturnNode)
{
	FInRackHandles H;
	H.Slots = Spec.Slots; H.DesignFraction = Spec.DesignFraction;
	H.SupplyNode = SupplyNode; H.ReturnNode = ReturnNode;
	const int32 Count = Spec.Slots.Num();
	if (Count == 0) return H;
	auto Pipe = [&](int32 A, int32 B, double Resistance)
	{
		FHydraulicPipe P; P.FromNodeIndex = A; P.ToNodeIndex = B; P.ResistancePaS2M6 = Resistance; P.bRackInternal = true;
		const int32 I = Network.AddPipe(P); H.Pipes.Add(I); return I;
	};
	// 추정: 세로 매니폴드 왕복 손실을 전체 랙 손실의 최대 1%로 제한한다.
	// 각 구간의 설계 유량으로 손실을 계산하고 트레이 저항을 보정: 병렬 비율을 강제하는 런타임 모델이 아니다.
	const double SegmentR = Spec.EquivalentResistancePaS2M6 * 0.005 / Count;
	double Remaining = 1.0, ManifoldDropCoefficient = 0.0;
	int32 PreviousSupply = SupplyNode, PreviousReturn = ReturnNode;
	for (int32 T = 0; T < Count; ++T)
	{
		for (int32 K = 0; K < 5; ++K)
		{
			FHydraulicNode Node; Node.ElevationM = Spec.HeightM[T];
			const int32 N = Network.AddNode(Node); H.Nodes.Add(N);
			FHydraulicLeak Leak; Leak.NodeIndex = N; H.Leaks.Add(Network.AddLeak(Leak));
		}
		const int32 B = T * 5;
		H.SupplyManifoldPipes.Add(Pipe(PreviousSupply, H.Nodes[B], SegmentR));
		H.ReturnManifoldPipes.Add(Pipe(H.Nodes[B + 4], PreviousReturn, SegmentR));
		ManifoldDropCoefficient += 2.0 * SegmentR * Remaining * Remaining;
		const double Fraction = Spec.DesignFraction[T];
		checkf(Fraction > 0.0, TEXT("트레이 설계 열은 양수여야 한다"));
		const double TrayR = (0.98 * Spec.EquivalentResistancePaS2M6 - ManifoldDropCoefficient) / FMath::Square(Fraction);
		const double ConnectorR = ConnectorResistancePaS2M6();
		checkf(TrayR > 2.0 * ConnectorR, TEXT("공개 커넥터 저항이 기존 랙 압력 예산을 초과한다"));
		H.InletPipes.Add(Pipe(H.Nodes[B], H.Nodes[B + 1], ConnectorR));
		// 플레이트·호스 합의 나머지 저항을 중간 누수 지점 양쪽에 반씩 배분(추정).
		Pipe(H.Nodes[B + 1], H.Nodes[B + 2], 0.5 * (TrayR - 2.0 * ConnectorR));
		Pipe(H.Nodes[B + 2], H.Nodes[B + 3], 0.5 * (TrayR - 2.0 * ConnectorR));
		H.OutletPipes.Add(Pipe(H.Nodes[B + 3], H.Nodes[B + 4], ConnectorR));
		H.SetDocked(Network, T, Spec.Docked[T]);
		Remaining -= Fraction; PreviousSupply = H.Nodes[B]; PreviousReturn = H.Nodes[B + 4];
	}
	return H;
}

void FInRackHandles::SetDocked(FHydraulicNetwork& Network, int32 Tray, bool bDocked) const
{
	// 드라이브레이크 양끝 차단은 추정. 트레이를 빼도 매니폴드/지점/번호는 남는다.
	if (!InletPipes.IsValidIndex(Tray)) return;
	Network.SetPipeClosed(InletPipes[Tray], !bDocked);
	Network.SetPipeClosed(OutletPipes[Tray], !bDocked);
}

double FInRackHandles::MaxBalanceErrorM3s(const FHydraulicNetwork& Network) const
{
	double Worst = 0.0;
	for (const int32 N : Nodes) Worst = FMath::Max(Worst, FMath::Abs(Network.GetNodeImbalanceM3s(N)));
	return Worst;
}
