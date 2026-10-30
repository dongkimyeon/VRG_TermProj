// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/SparseLinearSystem.h"
#include "Simulation/Hydraulics/DenseLinearSystem.h"

int32 FHydraulicNetwork::AddNode(const FHydraulicNode& Node)
{
	const int32 NodeIndex = Nodes.Add(Node);
	NodePressurePa.Add(Node.PressurePa);
	return NodeIndex;
}

int32 FHydraulicNetwork::AddPipe(const FHydraulicPipe& Pipe)
{
	return Pipes.Add(Pipe);
}

int32 FHydraulicNetwork::AddPump(const FHydraulicPump& Pump)
{
	const int32 PumpIndex = Pumps.Add(Pump);
	PumpFlowM3s.Add(0.0);
	return PumpIndex;
}

int32 FHydraulicNetwork::AddLeak(const FHydraulicLeak& Leak)
{
	return Leaks.Add(Leak);
}

void FHydraulicNetwork::SetLeakHoleDiameterM(int32 LeakIndex, double HoleDiameterM)
{
	if (Leaks.IsValidIndex(LeakIndex))
	{
		Leaks[LeakIndex].HoleDiameterM = FMath::Max(0.0, HoleDiameterM);
	}
}

int32 FHydraulicNetwork::SplitPipe(int32 PipeIndex, double Fraction, double ElevationM)
{
	if (!Pipes.IsValidIndex(PipeIndex) || !(Fraction > 0.0 && Fraction < 1.0))
	{
		return INDEX_NONE;
	}
	FHydraulicNode Mid;
	Mid.ElevationM = ElevationM;
	const FHydraulicPipe Original = Pipes[PipeIndex];
	Mid.PressurePa = 0.5 * (GetNodePressurePa(Original.FromNodeIndex) + GetNodePressurePa(Original.ToNodeIndex));
	const int32 MidIndex = AddNode(Mid);
	Pipes[PipeIndex].ToNodeIndex = MidIndex;
	Pipes[PipeIndex].ResistancePaS2M6 = Original.ResistancePaS2M6 * Fraction;
	FHydraulicPipe Rest = Original;
	Rest.FromNodeIndex = MidIndex;
	Rest.ResistancePaS2M6 = Original.ResistancePaS2M6 * (1.0 - Fraction);
	AddPipe(Rest);
	return MidIndex;
}

double FHydraulicNetwork::LeakResistancePaS2M6(const FHydraulicLeak& Leak, double DensityKgM3)
{
	const double AreaM2 = 0.25 * UE_DOUBLE_PI * Leak.HoleDiameterM * Leak.HoleDiameterM;
	const double CdA = Leak.DischargeCoefficient * AreaM2;
	return CdA > 0.0 ? DensityKgM3 / (2.0 * CdA * CdA) : 0.0;
}

double FHydraulicNetwork::GetLeakFlowM3s(int32 LeakIndex) const
{
	if (!Leaks.IsValidIndex(LeakIndex) || !NodePressurePa.IsValidIndex(Leaks[LeakIndex].NodeIndex))
	{
		return 0.0;
	}
	const FHydraulicLeak& Leak = Leaks[LeakIndex];
	if (IsNodeIsolated(Leak.NodeIndex)) return 0.0;  // S2b step 4: valved off, nothing feeds the hole
	const double R = LeakResistancePaS2M6(Leak, FluidDensityKgM3);
	const double DeltaPPa = NodePressurePa[Leak.NodeIndex] - Leak.AmbientPressurePa;
	return R > 0.0 && DeltaPPa > 0.0 ? PipeFlowM3s(DeltaPPa, R) : 0.0;   // no inflow from the ambient
}

double FHydraulicNetwork::GetTotalLeakFlowM3s() const
{
	double SumM3s = 0.0;
	for (int32 L = 0; L < Leaks.Num(); ++L) SumM3s += GetLeakFlowM3s(L);
	return SumM3s;
}

void FHydraulicNetwork::Reset()
{
	Nodes.Reset();
	Pipes.Reset();
	Pumps.Reset();
	Leaks.Reset();
	NodePressurePa.Reset();
	PumpFlowM3s.Reset();
	NodeToUnknown.Reset();
}

double FHydraulicNetwork::PipeFlowM3s(double DeltaPPa, double ResistancePaS2M6)
{
	if (ResistancePaS2M6 <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	const double Magnitude = FMath::Sqrt(FMath::Abs(DeltaPPa) / ResistancePaS2M6);
	return DeltaPPa >= 0.0 ? Magnitude : -Magnitude;
}

void FHydraulicNetwork::SetNodeElevationM(int32 NodeIndex, double ElevationM)
{
	if (Nodes.IsValidIndex(NodeIndex))
	{
		Nodes[NodeIndex].ElevationM = ElevationM;
	}
}

double FHydraulicNetwork::GetNodePressurePa(int32 NodeIndex) const
{
	return NodePressurePa.IsValidIndex(NodeIndex) ? NodePressurePa[NodeIndex] : 0.0;
}

int32 FHydraulicNetwork::GetPipeFromNodeIndex(int32 PipeIndex) const
{
	return Pipes.IsValidIndex(PipeIndex) ? Pipes[PipeIndex].FromNodeIndex : INDEX_NONE;
}

int32 FHydraulicNetwork::GetPipeToNodeIndex(int32 PipeIndex) const
{
	return Pipes.IsValidIndex(PipeIndex) ? Pipes[PipeIndex].ToNodeIndex : INDEX_NONE;
}

// S2b step 5: the piezometric drop of a pipe, with a difference below the rounding of its two operands taken as 0.
// Such a difference carries no information; turned into a flow it kept a stopped (depleted) loop from ever meeting
// the normalized residual target. Above that level the value is the plain difference (bit-identical results).
double FHydraulicNetwork::PiezometricDropPa(int32 FromNode, int32 ToNode) const
{
	const double A = GetNodePiezometricPa(FromNode), B = GetNodePiezometricPa(ToNode);
	const double Drop = A - B;
	return FMath::Abs(Drop) <= 8.0 * DBL_EPSILON * FMath::Max(FMath::Abs(A), FMath::Abs(B)) ? 0.0 : Drop;
}
double FHydraulicNetwork::GetPipeFlowM3s(int32 PipeIndex) const
{
	if (!Pipes.IsValidIndex(PipeIndex))
	{
		return 0.0;
	}
	const FHydraulicPipe& Pipe = Pipes[PipeIndex];
	if (Pipe.bClosed) return 0.0;  // S2b step 4: closed valve
	const double DeltaPPa = PiezometricDropPa(Pipe.FromNodeIndex, Pipe.ToNodeIndex);
	return PipeFlowM3s(DeltaPPa, Pipe.ResistancePaS2M6);
}

double FHydraulicNetwork::GetPumpFlowM3s(int32 PumpIndex) const
{
	return PumpFlowM3s.IsValidIndex(PumpIndex) ? PumpFlowM3s[PumpIndex] : 0.0;
}

double FHydraulicNetwork::GetPumpDeltaPPa(int32 PumpIndex) const
{
	if (!Pumps.IsValidIndex(PumpIndex))
	{
		return 0.0;
	}
	// 실제로 실현된 압력 상승을 쓴다. 수렴하면 펌프 곡선값과 일치하고,
	// 수렴하지 않았다면 어긋난 상태가 그대로 드러난다.
	const FHydraulicPump& Pump = Pumps[PumpIndex];
	return GetNodePiezometricPa(Pump.ToNodeIndex) - GetNodePiezometricPa(Pump.FromNodeIndex);
}

double FHydraulicNetwork::GetPumpHydraulicPowerW(int32 PumpIndex) const
{
	return GetPumpDeltaPPa(PumpIndex) * GetPumpFlowM3s(PumpIndex);
}

double FHydraulicNetwork::GetPumpElectricalInputPowerW(int32 PumpIndex) const
{
	if (!Pumps.IsValidIndex(PumpIndex))
	{
		return 0.0;
	}
	const double Efficiency = Pumps[PumpIndex].Efficiency;
	if (Efficiency <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return GetPumpHydraulicPowerW(PumpIndex) / Efficiency;
}

double FHydraulicNetwork::GetNodeImbalanceM3s(int32 NodeIndex) const
{
	if (!Nodes.IsValidIndex(NodeIndex))
	{
		return 0.0;
	}

	double SignedOutflowM3s = 0.0;
	for (int32 PipeIndex = 0; PipeIndex < Pipes.Num(); ++PipeIndex)
	{
		const FHydraulicPipe& Pipe = Pipes[PipeIndex];
		const double FlowM3s = GetPipeFlowM3s(PipeIndex);
		if (Pipe.FromNodeIndex == NodeIndex)
		{
			SignedOutflowM3s += FlowM3s;
		}
		if (Pipe.ToNodeIndex == NodeIndex)
		{
			SignedOutflowM3s -= FlowM3s;
		}
	}
	for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
	{
		const FHydraulicPump& Pump = Pumps[PumpIndex];
		const double FlowM3s = PumpFlowM3s[PumpIndex];
		if (Pump.FromNodeIndex == NodeIndex)
		{
			SignedOutflowM3s += FlowM3s;
		}
		if (Pump.ToNodeIndex == NodeIndex)
		{
			SignedOutflowM3s -= FlowM3s;
		}
	}

	for (int32 LeakIndex = 0; LeakIndex < Leaks.Num(); ++LeakIndex)
	{
		if (Leaks[LeakIndex].NodeIndex == NodeIndex)
		{
			SignedOutflowM3s += GetLeakFlowM3s(LeakIndex);
		}
	}

	return SignedOutflowM3s - Nodes[NodeIndex].ExternalInflowM3s;
}

void FHydraulicNetwork::GetNodeImbalancesM3s(TArray<double>& Out) const
{
	Out.Init(0.0, Nodes.Num());
	for (int32 I = 0; I < Pipes.Num(); ++I)
	{
		const double FlowM3s = GetPipeFlowM3s(I);
		Out[Pipes[I].FromNodeIndex] += FlowM3s;
		Out[Pipes[I].ToNodeIndex] -= FlowM3s;
	}
	for (int32 I = 0; I < Pumps.Num(); ++I)
	{
		Out[Pumps[I].FromNodeIndex] += PumpFlowM3s[I];
		Out[Pumps[I].ToNodeIndex] -= PumpFlowM3s[I];
	}
	for (int32 I = 0; I < Leaks.Num(); ++I) Out[Leaks[I].NodeIndex] += GetLeakFlowM3s(I);
	for (int32 I = 0; I < Nodes.Num(); ++I) Out[I] -= Nodes[I].ExternalInflowM3s;
}

void FHydraulicNetwork::BuildUnknownMap(int32& OutNumFreeNodes)
{
	NodeToUnknown.SetNum(Nodes.Num());
	OutNumFreeNodes = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].bFixedPressure)
		{
			NodeToUnknown[NodeIndex] = INDEX_NONE;
			NodePressurePa[NodeIndex] = Nodes[NodeIndex].PressurePa;
		}
		else if (NodeIsolated.IsValidIndex(NodeIndex) && NodeIsolated[NodeIndex])
		{
			NodeToUnknown[NodeIndex] = INDEX_NONE;  // S2b step 4: cut off by closed valves, not an unknown
		}
		else
		{
			NodeToUnknown[NodeIndex] = OutNumFreeNodes++;
		}
	}
}

void FHydraulicNetwork::SeedInitialGuess()
{
	// Seed every free node at the mean piezometric pressure of the fixed nodes (static = that - rho*g*z), so a
	// network at rest starts at its hydrostatic solution. With all heights 0 this is the former mean static seed.
	double FixedSumPa = 0.0;
	int32 FixedCount = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].bFixedPressure)
		{
			FixedSumPa += Nodes[NodeIndex].PressurePa + NodeHydrostaticPa(NodeIndex);
			++FixedCount;
		}
	}

	const double SeedPressurePa = FixedCount > 0 ? FixedSumPa / FixedCount : 1.0e5;

	// S2b step 2: seed each connected loop (pipes and pumps; leaks open to ambient and do not connect) from its own
	// fixed nodes. A drained loop's CDU sits at 0 Pa(g) while the other rows stay at their fill pressure; seeding it
	// from the room-wide mean starts that stopped loop far from rest, and nodal Newton then stalls at zero flow (D2).
	// With one fixed node per loop at equal pressure (all rows filled alike) the seed is the same value as before.
	TArray<int32> Parent;
	Parent.SetNum(Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex) Parent[NodeIndex] = NodeIndex;
	auto Find = [&Parent](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
	auto Unite = [&](int32 A, int32 B) { if (Parent.IsValidIndex(A) && Parent.IsValidIndex(B)) Parent[Find(A)] = Find(B); };
	for (const FHydraulicPipe& Pipe : Pipes) if (!Pipe.bClosed) Unite(Pipe.FromNodeIndex, Pipe.ToNodeIndex);
	for (const FHydraulicPump& Pump : Pumps) Unite(Pump.FromNodeIndex, Pump.ToNodeIndex);
	TArray<double> LoopSumPa;
	TArray<int32> LoopCount;
	LoopSumPa.Init(0.0, Nodes.Num());
	LoopCount.Init(0, Nodes.Num());
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].bFixedPressure)
		{
			const int32 Root = Find(NodeIndex);
			LoopSumPa[Root] += Nodes[NodeIndex].PressurePa + NodeHydrostaticPa(NodeIndex);
			++LoopCount[Root];
		}
	}
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (!Nodes[NodeIndex].bFixedPressure)
		{
			const int32 Root = Find(NodeIndex);
			const double LoopSeedPa = LoopCount[Root] > 0 ? LoopSumPa[Root] / LoopCount[Root] : SeedPressurePa;
			NodePressurePa[NodeIndex] = LoopSeedPa - NodeHydrostaticPa(NodeIndex);
		}
	}

	// 자유 토출 유량의 절반을 펌프 유량 초기값으로 쓴다.
	for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
	{
		const FHydraulicPump& Pump = Pumps[PumpIndex];
		const double ScaledShutoffHeadPa = Pump.ShutoffHeadPa * Pump.SpeedRatio * Pump.SpeedRatio;
		if (Pump.CurveCoeffPaS2M6 > UE_DOUBLE_SMALL_NUMBER && ScaledShutoffHeadPa > 0.0)
		{
			PumpFlowM3s[PumpIndex] = 0.5 * FMath::Sqrt(ScaledShutoffHeadPa / Pump.CurveCoeffPaS2M6);
		}
		else
		{
			PumpFlowM3s[PumpIndex] = 0.0;
		}
	}
}

double FHydraulicNetwork::EvaluateResidual(const FHydraulicSolveSettings& Settings, TArray<double>& OutResidual,
	int32 NumFreeNodes, double& OutReferenceFlowM3s, double& OutReferencePressurePa) const
{
	const int32 NumUnknowns = NumFreeNodes + Pumps.Num();
	OutResidual.SetNumZeroed(NumUnknowns, EAllowShrinking::No);
	for (int32 Index = 0; Index < NumUnknowns; ++Index)
	{
		OutResidual[Index] = 0.0;
	}

	double MaxFlowMagnitudeM3s = 0.0;

	// 자유 노드 질량 보존 [m^3/s]
	for (int32 PipeIndex = 0; PipeIndex < Pipes.Num(); ++PipeIndex)
	{
		const FHydraulicPipe& Pipe = Pipes[PipeIndex];
		if (Pipe.bClosed) continue;
		const double DeltaPPa = PiezometricDropPa(Pipe.FromNodeIndex, Pipe.ToNodeIndex);
		const double FlowM3s = PipeFlowM3s(DeltaPPa, Pipe.ResistancePaS2M6);
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(FlowM3s));

		const int32 FromUnknown = NodeToUnknown[Pipe.FromNodeIndex];
		const int32 ToUnknown = NodeToUnknown[Pipe.ToNodeIndex];
		if (FromUnknown != INDEX_NONE)
		{
			OutResidual[FromUnknown] += FlowM3s;
		}
		if (ToUnknown != INDEX_NONE)
		{
			OutResidual[ToUnknown] -= FlowM3s;
		}
	}

	for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
	{
		const FHydraulicPump& Pump = Pumps[PumpIndex];
		const double FlowM3s = PumpFlowM3s[PumpIndex];
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(FlowM3s));

		const int32 FromUnknown = NodeToUnknown[Pump.FromNodeIndex];
		const int32 ToUnknown = NodeToUnknown[Pump.ToNodeIndex];
		if (FromUnknown != INDEX_NONE)
		{
			OutResidual[FromUnknown] += FlowM3s;
		}
		if (ToUnknown != INDEX_NONE)
		{
			OutResidual[ToUnknown] -= FlowM3s;
		}
	}

	// Leak orifices: outflow of their node (S2b step 1).
	for (int32 LeakIndex = 0; LeakIndex < Leaks.Num(); ++LeakIndex)
	{
		const double FlowM3s = GetLeakFlowM3s(LeakIndex);
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FlowM3s);
		const int32 Unknown = NodeToUnknown[Leaks[LeakIndex].NodeIndex];
		if (Unknown != INDEX_NONE)
		{
			OutResidual[Unknown] += FlowM3s;
		}
	}

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const int32 Unknown = NodeToUnknown[NodeIndex];
		if (Unknown != INDEX_NONE)
		{
			OutResidual[Unknown] -= Nodes[NodeIndex].ExternalInflowM3s;
		}
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(Nodes[NodeIndex].ExternalInflowM3s));
	}

	// 펌프 곡선 방정식 [Pa]
	double MaxShutoffHeadPa = 0.0;
	for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
	{
		const FHydraulicPump& Pump = Pumps[PumpIndex];
		const double FlowM3s = PumpFlowM3s[PumpIndex];
		// 상사식: 계수는 그대로 두고 체절양정만 회전수비의 제곱으로 스케일한다.
		const double ScaledShutoffHeadPa = Pump.ShutoffHeadPa * Pump.SpeedRatio * Pump.SpeedRatio;
		const double CurveDeltaPPa = ScaledShutoffHeadPa - Pump.CurveCoeffPaS2M6 * FlowM3s * FMath::Abs(FlowM3s);
		const double ActualDeltaPPa = GetNodePiezometricPa(Pump.ToNodeIndex) - GetNodePiezometricPa(Pump.FromNodeIndex);
		OutResidual[NumFreeNodes + PumpIndex] = ActualDeltaPPa - CurveDeltaPPa;

		MaxShutoffHeadPa = FMath::Max(MaxShutoffHeadPa, FMath::Abs(ScaledShutoffHeadPa));
	}

	// 정규화 기준. 질량 블록은 유량 스케일로, 펌프 블록은 압력 스케일로 나눈다.
	double MinPressurePa = TNumericLimits<double>::Max();
	double MaxPressurePa = TNumericLimits<double>::Lowest();
	for (const double PressurePa : NodePressurePa)
	{
		MinPressurePa = FMath::Min(MinPressurePa, PressurePa);
		MaxPressurePa = FMath::Max(MaxPressurePa, PressurePa);
	}
	const double PressureSpreadPa = Nodes.Num() > 0 ? (MaxPressurePa - MinPressurePa) : 0.0;

	OutReferenceFlowM3s = FMath::Max(MaxFlowMagnitudeM3s, 1.0e-12);
	OutReferencePressurePa = FMath::Max3(MaxShutoffHeadPa, PressureSpreadPa, 1.0);

	double NormalizedResidual = 0.0;
	for (int32 Index = 0; Index < NumFreeNodes; ++Index)
	{
		NormalizedResidual = FMath::Max(NormalizedResidual, FMath::Abs(OutResidual[Index]) / OutReferenceFlowM3s);
	}
	for (int32 Index = NumFreeNodes; Index < NumUnknowns; ++Index)
	{
		NormalizedResidual = FMath::Max(NormalizedResidual, FMath::Abs(OutResidual[Index]) / OutReferencePressurePa);
	}

	return NormalizedResidual;
}

FHydraulicSolveResult FHydraulicNetwork::Solve(const FHydraulicSolveSettings& Settings)
{
	FHydraulicSolveResult Result;

	// 구성 검증. 고정 압력 노드가 없으면 압력이 상수만큼 불정이다.
	bool bHasFixedPressure = false;
	for (const FHydraulicNode& Node : Nodes)
	{
		bHasFixedPressure |= Node.bFixedPressure;
	}

	bool bIndicesValid = true;
	for (const FHydraulicPipe& Pipe : Pipes)
	{
		bIndicesValid &= Nodes.IsValidIndex(Pipe.FromNodeIndex) && Nodes.IsValidIndex(Pipe.ToNodeIndex);
	}
	for (const FHydraulicPump& Pump : Pumps)
	{
		bIndicesValid &= Nodes.IsValidIndex(Pump.FromNodeIndex) && Nodes.IsValidIndex(Pump.ToNodeIndex);
	}
	for (const FHydraulicLeak& Leak : Leaks)
	{
		bIndicesValid &= Nodes.IsValidIndex(Leak.NodeIndex);
	}

	if (Nodes.Num() == 0 || !bHasFixedPressure || !bIndicesValid)
	{
		Result.bIllPosed = true;
		return Result;
	}

	// S2b step 4: closed valves. A node joined to no fixed-pressure node through open pipes and pumps is isolated;
	// a pump in such a part has no defined operating point, so that case is reported as ill-posed.
	{
		TArray<int32> Parent;
		Parent.SetNum(Nodes.Num());
		for (int32 N = 0; N < Nodes.Num(); ++N) Parent[N] = N;
		auto Find = [&Parent](int32 X) { while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; } return X; };
		for (const FHydraulicPipe& Pipe : Pipes) if (!Pipe.bClosed) Parent[Find(Pipe.FromNodeIndex)] = Find(Pipe.ToNodeIndex);
		for (const FHydraulicPump& Pump : Pumps) Parent[Find(Pump.FromNodeIndex)] = Find(Pump.ToNodeIndex);
		TArray<bool> RootHasFixed;
		RootHasFixed.Init(false, Nodes.Num());
		for (int32 N = 0; N < Nodes.Num(); ++N) if (Nodes[N].bFixedPressure) RootHasFixed[Find(N)] = true;
		NodeIsolated.Init(false, Nodes.Num());
		for (int32 N = 0; N < Nodes.Num(); ++N) NodeIsolated[N] = !RootHasFixed[Find(N)];
		for (const FHydraulicPump& Pump : Pumps)
		{
			if (NodeIsolated[Pump.FromNodeIndex])
			{
				Result.bIllPosed = true;
				return Result;
			}
		}
	}

	int32 NumFreeNodes = 0;
	BuildUnknownMap(NumFreeNodes);
	SeedInitialGuess();
	for (int32 N = 0; N < Nodes.Num(); ++N)
		if (NodeIsolated[N]) NodePressurePa[N] = -NodeHydrostaticPa(N);  // vented: piezometric 0 Pa(g), no flow

	const int32 NumUnknowns = NumFreeNodes + Pumps.Num();

	TArray<double> Residual;
	double CurrentResidual = EvaluateResidual(Settings, Residual, NumFreeNodes,
		Result.ReferenceFlowM3s, Result.ReferencePressurePa);

	if (NumUnknowns == 0)
	{
		Result.bConverged = true;
		Result.NormalizedResidual = CurrentResidual;
		return Result;
	}

	Result.bUsedSparse = Nodes.Num() > 200;
	TArray<FHydraulicMatrixEntry> SparseJacobian;
	TArray<double> Jacobian;
	TArray<double> Delta;
	TArray<double> SavedPressurePa;
	TArray<double> SavedPumpFlowM3s;

	int32 Iteration = 0;
	while (Iteration < Settings.MaxIterations && CurrentResidual > Settings.NormalizedResidualTarget)
	{
		SparseJacobian.Reset();
		if (!Result.bUsedSparse) Jacobian.Init(0.0, NumUnknowns * NumUnknowns);
		auto AddJacobian = [&](int32 Row, int32 Col, double Value)
		{
			if (Result.bUsedSparse) SparseJacobian.Add({Row, Col, Value});
			else Jacobian[Row * NumUnknowns + Col] += Value;
		};

		// 파이프: Δp에 대한 도함수가 Δp=0에서 발산하므로 여기에만 하한을 건다.
		for (const FHydraulicPipe& Pipe : Pipes)
		{
			if (Pipe.bClosed) continue;
			const double DeltaPPa = GetNodePiezometricPa(Pipe.FromNodeIndex) - GetNodePiezometricPa(Pipe.ToNodeIndex);
			const double ClampedDeltaPPa = FMath::Max(FMath::Abs(DeltaPPa), Settings.JacobianDeltaPFloorPa);
			const double ConductanceM3sPa = 1.0 / (2.0 * FMath::Sqrt(Pipe.ResistancePaS2M6 * ClampedDeltaPPa));

			const int32 FromUnknown = NodeToUnknown[Pipe.FromNodeIndex];
			const int32 ToUnknown = NodeToUnknown[Pipe.ToNodeIndex];

			if (FromUnknown != INDEX_NONE)
			{
				AddJacobian(FromUnknown, FromUnknown, ConductanceM3sPa);
				if (ToUnknown != INDEX_NONE)
				{
					AddJacobian(FromUnknown, ToUnknown, -ConductanceM3sPa);
				}
			}
			if (ToUnknown != INDEX_NONE)
			{
				AddJacobian(ToUnknown, ToUnknown, ConductanceM3sPa);
				if (FromUnknown != INDEX_NONE)
				{
					AddJacobian(ToUnknown, FromUnknown, -ConductanceM3sPa);
				}
			}
		}

		// Leaks: d(Flow)/dP like a pipe to a fixed ambient node, with the same Jacobian-only floor. Below the ambient
		// the residual flow is 0 (one-way); the floored derivative still gives Newton a direction back.
		for (const FHydraulicLeak& Leak : Leaks)
		{
			const double R = LeakResistancePaS2M6(Leak, FluidDensityKgM3);
			const int32 Unknown = NodeToUnknown[Leak.NodeIndex];
			if (R <= 0.0 || Unknown == INDEX_NONE) continue;
			const double DeltaPPa = NodePressurePa[Leak.NodeIndex] - Leak.AmbientPressurePa;
			const double ClampedDeltaPPa = FMath::Max(DeltaPPa, Settings.JacobianDeltaPFloorPa);
			AddJacobian(Unknown, Unknown, 1.0 / (2.0 * FMath::Sqrt(R * ClampedDeltaPPa)));
		}

		// 펌프: 유량이 보조 미지수다.
		for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
		{
			const FHydraulicPump& Pump = Pumps[PumpIndex];
			const int32 FlowUnknown = NumFreeNodes + PumpIndex;
			const int32 FromUnknown = NodeToUnknown[Pump.FromNodeIndex];
			const int32 ToUnknown = NodeToUnknown[Pump.ToNodeIndex];

			if (FromUnknown != INDEX_NONE)
			{
				AddJacobian(FromUnknown, FlowUnknown, 1.0);
				AddJacobian(FlowUnknown, FromUnknown, -1.0);
			}
			if (ToUnknown != INDEX_NONE)
			{
				AddJacobian(ToUnknown, FlowUnknown, -1.0);
				AddJacobian(FlowUnknown, ToUnknown, 1.0);
			}

			const double ClampedFlowM3s = FMath::Max(FMath::Abs(PumpFlowM3s[PumpIndex]), Settings.JacobianFlowFloorM3s);
			AddJacobian(FlowUnknown, FlowUnknown, 2.0 * Pump.CurveCoeffPaS2M6 * ClampedFlowM3s);
		}

		Delta.SetNumUninitialized(NumUnknowns, EAllowShrinking::No);
		for (int32 Index = 0; Index < NumUnknowns; ++Index)
		{
			Delta[Index] = -Residual[Index];
		}

		// 미지수는 압력(~1e5 Pa)과 유량(~1e-3 m^3/s)이 섞여 있어 8자릿수쯤 차이가 난다.
		// 그대로 풀면 Jacobian 성분이 1e-9 ~ 1e8까지 퍼져 가우스 소거가 정밀도를 잃고
		// Newton 방향이 망가진다. 열(미지수)과 행(방정식)을 각각의 기준량으로 나눠
		// 성분을 O(1)로 맞춘 뒤 푼다. 푼 결과는 다시 원래 스케일로 되돌린다.
		// 스케일링은 같은 해를 다른 좌표로 푸는 것이라 수렴한 해는 바뀌지 않는다.
		const double PressureScalePa = FMath::Max(Result.ReferencePressurePa, 1.0);
		const double FlowScaleM3s = FMath::Max(Result.ReferenceFlowM3s, 1.0e-9);

		auto UnknownScale = [&](int32 UnknownIndex)
		{
			return UnknownIndex < NumFreeNodes ? PressureScalePa : FlowScaleM3s;
		};
		auto EquationScale = [&](int32 EquationIndex)
		{
			return EquationIndex < NumFreeNodes ? FlowScaleM3s : PressureScalePa;
		};

		for (int32 Row = 0; Row < NumUnknowns; ++Row)
		{
			const double RowScale = EquationScale(Row);
			for (int32 Col = 0; !Result.bUsedSparse && Col < NumUnknowns; ++Col)
			{
				Jacobian[Row * NumUnknowns + Col] *= UnknownScale(Col) / RowScale;
			}
			Delta[Row] /= RowScale;
		}

		for (FHydraulicMatrixEntry& Entry : SparseJacobian)
			Entry.Value *= UnknownScale(Entry.Col) / EquationScale(Entry.Row);

		if (!(Result.bUsedSparse ? SolveSparseLinearSystem(SparseJacobian, Delta, NumUnknowns)
			: SolveDenseLinearSystem(Jacobian, Delta, NumUnknowns)))
		{
			Result.bLinearSolveFailed = true;
			break;
		}

		for (int32 Index = 0; Index < NumUnknowns; ++Index)
		{
			Delta[Index] *= UnknownScale(Index);
		}

		SavedPressurePa = NodePressurePa;
		SavedPumpFlowM3s = PumpFlowM3s;

		// 잔차가 줄어들 때까지 스텝을 절반씩 줄인다.
		double StepScale = 1.0;
		double BestResidual = CurrentResidual;
		bool bAccepted = false;
		for (int32 Backtrack = 0; Backtrack <= Settings.MaxBacktrackSteps; ++Backtrack)
		{
			NodePressurePa = SavedPressurePa;
			PumpFlowM3s = SavedPumpFlowM3s;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const int32 Unknown = NodeToUnknown[NodeIndex];
				if (Unknown != INDEX_NONE)
				{
					NodePressurePa[NodeIndex] += StepScale * Delta[Unknown];
				}
			}
			for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
			{
				PumpFlowM3s[PumpIndex] += StepScale * Delta[NumFreeNodes + PumpIndex];
			}

			const double TrialResidual = EvaluateResidual(Settings, Residual, NumFreeNodes,
				Result.ReferenceFlowM3s, Result.ReferencePressurePa);
			if (TrialResidual < BestResidual)
			{
				BestResidual = TrialResidual;
				bAccepted = true;
				break;
			}
			StepScale *= 0.5;
		}

		if (!bAccepted)
		{
			// 어떤 스텝으로도 잔차를 못 줄였다. 원래 상태로 되돌리고 멈춘다.
			NodePressurePa = SavedPressurePa;
			PumpFlowM3s = SavedPumpFlowM3s;
			CurrentResidual = EvaluateResidual(Settings, Residual, NumFreeNodes,
				Result.ReferenceFlowM3s, Result.ReferencePressurePa);
			++Iteration;
			break;
		}

		CurrentResidual = BestResidual;
		++Iteration;
	}

	Result.IterationsUsed = Iteration;
	Result.NormalizedResidual = CurrentResidual;
	Result.bConverged = !Result.bLinearSolveFailed && CurrentResidual <= Settings.NormalizedResidualTarget;
	return Result;
}
