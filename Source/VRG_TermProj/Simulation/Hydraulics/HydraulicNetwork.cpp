// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/HydraulicNetwork.h"
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

void FHydraulicNetwork::Reset()
{
	Nodes.Reset();
	Pipes.Reset();
	Pumps.Reset();
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

double FHydraulicNetwork::GetPipeFlowM3s(int32 PipeIndex) const
{
	if (!Pipes.IsValidIndex(PipeIndex))
	{
		return 0.0;
	}
	const FHydraulicPipe& Pipe = Pipes[PipeIndex];
	const double DeltaPPa = NodePressurePa[Pipe.FromNodeIndex] - NodePressurePa[Pipe.ToNodeIndex];
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
	return NodePressurePa[Pump.ToNodeIndex] - NodePressurePa[Pump.FromNodeIndex];
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

	return SignedOutflowM3s - Nodes[NodeIndex].ExternalInflowM3s;
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
		else
		{
			NodeToUnknown[NodeIndex] = OutNumFreeNodes++;
		}
	}
}

void FHydraulicNetwork::SeedInitialGuess()
{
	double FixedSumPa = 0.0;
	int32 FixedCount = 0;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].bFixedPressure)
		{
			FixedSumPa += Nodes[NodeIndex].PressurePa;
			++FixedCount;
		}
	}

	const double SeedPressurePa = FixedCount > 0 ? FixedSumPa / FixedCount : 1.0e5;
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (!Nodes[NodeIndex].bFixedPressure)
		{
			NodePressurePa[NodeIndex] = SeedPressurePa;
		}
	}

	// 자유 토출 유량의 절반을 펌프 유량 초기값으로 쓴다.
	for (int32 PumpIndex = 0; PumpIndex < Pumps.Num(); ++PumpIndex)
	{
		const FHydraulicPump& Pump = Pumps[PumpIndex];
		if (Pump.CurveCoeffPaS2M6 > UE_DOUBLE_SMALL_NUMBER && Pump.ShutoffHeadPa > 0.0)
		{
			PumpFlowM3s[PumpIndex] = 0.5 * FMath::Sqrt(Pump.ShutoffHeadPa / Pump.CurveCoeffPaS2M6);
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
		const double DeltaPPa = NodePressurePa[Pipe.FromNodeIndex] - NodePressurePa[Pipe.ToNodeIndex];
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
		const double CurveDeltaPPa = Pump.ShutoffHeadPa - Pump.CurveCoeffPaS2M6 * FlowM3s * FMath::Abs(FlowM3s);
		const double ActualDeltaPPa = NodePressurePa[Pump.ToNodeIndex] - NodePressurePa[Pump.FromNodeIndex];
		OutResidual[NumFreeNodes + PumpIndex] = ActualDeltaPPa - CurveDeltaPPa;

		MaxShutoffHeadPa = FMath::Max(MaxShutoffHeadPa, FMath::Abs(Pump.ShutoffHeadPa));
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

	if (Nodes.Num() == 0 || !bHasFixedPressure || !bIndicesValid)
	{
		Result.bIllPosed = true;
		return Result;
	}

	int32 NumFreeNodes = 0;
	BuildUnknownMap(NumFreeNodes);
	SeedInitialGuess();

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

	TArray<double> Jacobian;
	TArray<double> Delta;
	TArray<double> SavedPressurePa;
	TArray<double> SavedPumpFlowM3s;

	int32 Iteration = 0;
	while (Iteration < Settings.MaxIterations && CurrentResidual > Settings.NormalizedResidualTarget)
	{
		Jacobian.SetNumZeroed(NumUnknowns * NumUnknowns, EAllowShrinking::No);
		for (int32 Index = 0; Index < NumUnknowns * NumUnknowns; ++Index)
		{
			Jacobian[Index] = 0.0;
		}

		// 파이프: Δp에 대한 도함수가 Δp=0에서 발산하므로 여기에만 하한을 건다.
		for (const FHydraulicPipe& Pipe : Pipes)
		{
			const double DeltaPPa = NodePressurePa[Pipe.FromNodeIndex] - NodePressurePa[Pipe.ToNodeIndex];
			const double ClampedDeltaPPa = FMath::Max(FMath::Abs(DeltaPPa), Settings.JacobianDeltaPFloorPa);
			const double ConductanceM3sPa = 1.0 / (2.0 * FMath::Sqrt(Pipe.ResistancePaS2M6 * ClampedDeltaPPa));

			const int32 FromUnknown = NodeToUnknown[Pipe.FromNodeIndex];
			const int32 ToUnknown = NodeToUnknown[Pipe.ToNodeIndex];

			if (FromUnknown != INDEX_NONE)
			{
				Jacobian[FromUnknown * NumUnknowns + FromUnknown] += ConductanceM3sPa;
				if (ToUnknown != INDEX_NONE)
				{
					Jacobian[FromUnknown * NumUnknowns + ToUnknown] -= ConductanceM3sPa;
				}
			}
			if (ToUnknown != INDEX_NONE)
			{
				Jacobian[ToUnknown * NumUnknowns + ToUnknown] += ConductanceM3sPa;
				if (FromUnknown != INDEX_NONE)
				{
					Jacobian[ToUnknown * NumUnknowns + FromUnknown] -= ConductanceM3sPa;
				}
			}
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
				Jacobian[FromUnknown * NumUnknowns + FlowUnknown] += 1.0;
				Jacobian[FlowUnknown * NumUnknowns + FromUnknown] -= 1.0;
			}
			if (ToUnknown != INDEX_NONE)
			{
				Jacobian[ToUnknown * NumUnknowns + FlowUnknown] -= 1.0;
				Jacobian[FlowUnknown * NumUnknowns + ToUnknown] += 1.0;
			}

			const double ClampedFlowM3s = FMath::Max(FMath::Abs(PumpFlowM3s[PumpIndex]), Settings.JacobianFlowFloorM3s);
			Jacobian[FlowUnknown * NumUnknowns + FlowUnknown] += 2.0 * Pump.CurveCoeffPaS2M6 * ClampedFlowM3s;
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
			for (int32 Col = 0; Col < NumUnknowns; ++Col)
			{
				Jacobian[Row * NumUnknowns + Col] *= UnknownScale(Col) / RowScale;
			}
			Delta[Row] /= RowScale;
		}

		if (!SolveDenseLinearSystem(Jacobian, Delta, NumUnknowns))
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
