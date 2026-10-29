// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/InRowLoopBuilder.h"

int32 InRowLoopBuilder::DockedSledsAt(const FInRowLoopSpec& Spec, int32 RackIndex)
{
	const int32 SledsPerRack = FMath::Max(1, Spec.SledsPerRack);

	// 길이가 맞지 않으면 구성이 아직 안 들어온 것으로 보고 만재로 다룬다.
	if (!Spec.DockedSledsPerRack.IsValidIndex(RackIndex) || Spec.DockedSledsPerRack.Num() != Spec.NumRacks)
	{
		return SledsPerRack;
	}
	return FMath::Clamp(Spec.DockedSledsPerRack[RackIndex], 0, SledsPerRack);
}

double InRowLoopBuilder::BranchResistancePaS2M6(const FInRowLoopSpec& Spec, int32 DockedSleds)
{
	if (DockedSleds <= 0)
	{
		return 0.0;
	}

	// 2차 손실에서 동일 저항 n개를 병렬로 묶으면 합성 저항은 R/n^2이다.
	// 기준값이 "슬롯이 전부 찼을 때"이므로 (SledsPerRack / n)^2를 곱한다.
	const double ParallelScale = static_cast<double>(FMath::Max(1, Spec.SledsPerRack))
		/ static_cast<double>(DockedSleds);
	return Spec.RackBranchResistancePaS2M6 * ParallelScale * ParallelScale;
}

FInRowLoopHandles InRowLoopBuilder::AddLoop(FHydraulicNetwork& Network, const FInRowLoopSpec& Spec, const TArray<FInRackSpec>* InRacks)
{
	FInRowLoopHandles Handles;

	const int32 RackCount = FMath::Max(1, Spec.NumRacks);
	Handles.InRacks.SetNum(RackCount);
	auto HasInternal = [&](int32 R) { return InRacks && InRacks->IsValidIndex(R) && !(*InRacks)[R].Slots.IsEmpty(); };

	FHydraulicNode CduNode;
	CduNode.bFixedPressure = true;
	CduNode.PressurePa = Spec.CduReferencePressurePa;
	Handles.CduNodeIndex = Network.AddNode(CduNode);

	Handles.SupplyNodeIndices.Reserve(RackCount);
	Handles.ReturnNodeIndices.Reserve(RackCount);
	Handles.RackNodeIndices.Reserve(RackCount);
	Handles.RackBranchPipeIndices.Reserve(RackCount);

	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		Handles.SupplyNodeIndices.Add(Network.AddNode(FHydraulicNode()));
		Handles.ReturnNodeIndices.Add(Network.AddNode(FHydraulicNode()));

		// 유닛이 한 대도 없는 랙은 연결될 곳이 없으므로 노드를 만들지 않는다.
		// 만들어 두면 어떤 배관에도 닿지 않는 자유 노드가 생겨 압력이 결정되지 않고
		// Jacobian이 특이해진다.
		const bool bHasDockedSleds = HasInternal(RackIndex) || DockedSledsAt(Spec, RackIndex) > 0;
		Handles.RackNodeIndices.Add(bHasDockedSleds ? Network.AddNode(FHydraulicNode()) : INDEX_NONE);
	}

	FHydraulicPump Pump;
	Pump.FromNodeIndex = Handles.CduNodeIndex;
	Pump.ToNodeIndex = Handles.SupplyNodeIndices[0];
	Pump.ShutoffHeadPa = Spec.PumpShutoffHeadPa;
	Pump.CurveCoeffPaS2M6 = Spec.PumpCurveCoeffPaS2M6;
	Pump.Efficiency = Spec.PumpEfficiency;
	Pump.SpeedRatio = Spec.PumpSpeedRatio;
	Handles.PumpIndex = Network.AddPump(Pump);

	auto AddPipe = [&Network](int32 FromNodeIndex, int32 ToNodeIndex, double ResistancePaS2M6) -> int32
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = FromNodeIndex;
		Pipe.ToNodeIndex = ToNodeIndex;
		Pipe.ResistancePaS2M6 = ResistancePaS2M6;
		return Network.AddPipe(Pipe);
	};

	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		// 공급 헤더 구간
		if (RackIndex + 1 < RackCount)
		{
			Handles.SupplyHeaderPipeIndices.Add(
				AddPipe(Handles.SupplyNodeIndices[RackIndex], Handles.SupplyNodeIndices[RackIndex + 1],
					Spec.HeaderSegmentResistancePaS2M6));
		}

		// 랙 분기 (공급 -> 랙 -> 회수)
		const int32 RackNodeIndex = Handles.RackNodeIndices[RackIndex];
		if (RackNodeIndex == INDEX_NONE)
		{
			Handles.RackBranchPipeIndices.Add(INDEX_NONE);
			Handles.RackReturnPipeIndices.Add(INDEX_NONE);
		}
		else
		{
			// Existing budget is TWO branch losses. Each external hose keeps 1% of that
			// total, InRackNetwork consumes the remaining 98%; no dock-count scaling here.
			const bool bInternal = HasInternal(RackIndex);
			const double BranchPaS2M6 = bInternal ? 0.02 * Spec.RackBranchResistancePaS2M6
				: BranchResistancePaS2M6(Spec, DockedSledsAt(Spec, RackIndex));
			const int32 ReturnNode = bInternal ? Network.AddNode(FHydraulicNode()) : RackNodeIndex;
			Handles.RackBranchPipeIndices.Add(
				AddPipe(Handles.SupplyNodeIndices[RackIndex], RackNodeIndex, BranchPaS2M6));
			Handles.RackReturnPipeIndices.Add(
				AddPipe(ReturnNode, Handles.ReturnNodeIndices[RackIndex], BranchPaS2M6));
			if (bInternal)
			{
				FInRackSpec Internal = (*InRacks)[RackIndex];
				Internal.EquivalentResistancePaS2M6 = 2.0 * Spec.RackBranchResistancePaS2M6;
				Handles.InRacks[RackIndex] = InRackNetwork::Add(Network, Internal, RackNodeIndex, ReturnNode);
			}
		}

		// 회수 헤더 구간. CDU 쪽으로 되돌아간다.
		if (RackIndex + 1 < RackCount)
		{
			Handles.ReturnHeaderPipeIndices.Add(
				AddPipe(Handles.ReturnNodeIndices[RackIndex + 1], Handles.ReturnNodeIndices[RackIndex],
					Spec.HeaderSegmentResistancePaS2M6));
		}
	}

	Handles.ReturnToCduPipeIndex =
		AddPipe(Handles.ReturnNodeIndices[0], Handles.CduNodeIndex, Spec.ReturnToCduResistancePaS2M6);

	return Handles;
}
