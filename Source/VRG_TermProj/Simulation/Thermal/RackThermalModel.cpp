// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Thermal/RackThermalModel.h"

int32 FRackThermalModel::NumSlotsOf(const FRackThermalParams& InParams)
{
	return InParams.Slots.Num();
}

void FRackThermalModel::ResolveLayout(const FRackThermalParams& InParams, TArray<FRackComponentParams>& OutNodeParams,
	TArray<int32>& OutSlotNodeStart)
{
	OutNodeParams.Reset(); OutSlotNodeStart.Reset();
	const int32 NumSlots = NumSlotsOf(InParams);
	OutSlotNodeStart.Reserve(NumSlots + 1);
	for (const FRackSlot& Slot : InParams.Slots)
	{
		OutSlotNodeStart.Add(OutNodeParams.Num());
		if (InParams.DeviceTypes.IsValidIndex(Slot.DeviceType)) OutNodeParams.Append(InParams.DeviceTypes[Slot.DeviceType].Components);
	}
	OutSlotNodeStart.Add(OutNodeParams.Num());
}

int32 FRackThermalModel::CountNodes(const FRackThermalParams& InParams)
{
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	ResolveLayout(InParams, Nodes, Starts);
	return Nodes.Num();
}

TArray<double> FRackThermalModel::MakeDefaultHeatW(const FRackThermalParams& InParams)
{
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	ResolveLayout(InParams, Nodes, Starts);
	TArray<double> HeatW; HeatW.Reserve(Nodes.Num());
	for (const FRackComponentParams& N : Nodes) HeatW.Add(N.DefaultHeatW);
	return HeatW;
}

bool FRackThermalModel::ApplyKindHeatScale(const FRackThermalParams& InParams, TArray<double>& InOutHeatW, ERackPartKind Kind, double FirstNodeHeatW)
{
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	ResolveLayout(InParams, Nodes, Starts);
	const int32 First = Nodes.IndexOfByPredicate([Kind](const FRackComponentParams& P) { return P.Kind == Kind; });
	if (InOutHeatW.Num() != Nodes.Num() || First == INDEX_NONE || !(Nodes[First].DefaultHeatW > 0.0)) return false;
	const double Multiple = FirstNodeHeatW / Nodes[First].DefaultHeatW;
	for (int32 N = 0; N < Nodes.Num(); ++N)
	{
		if (Nodes[N].Kind == Kind) InOutHeatW[N] = N == First ? FirstNodeHeatW : Nodes[N].DefaultHeatW * Multiple;
	}
	return true;
}

int32 FRackThermalModel::RepairKindHeatSpread(const FRackThermalParams& InParams, TArray<double>& InOutHeatW)
{
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	ResolveLayout(InParams, Nodes, Starts);
	if (InOutHeatW.Num() != Nodes.Num()) return 0;
	int32 Repaired = 0;
	for (const ERackPartKind Kind : { ERackPartKind::Cpu, ERackPartKind::Gpu })
	{
		const int32 First = Nodes.IndexOfByPredicate([Kind](const FRackComponentParams& P) { return P.Kind == Kind; });
		if (First == INDEX_NONE || !(Nodes[First].DefaultHeatW > 0.0)) continue;
		const double FirstW = InOutHeatW[First];
		const double Multiple = FirstW / Nodes[First].DefaultHeatW;
		for (int32 N = 0; N < Nodes.Num(); ++N)
		{
			if (Nodes[N].Kind != Kind || Nodes[N].DefaultHeatW == Nodes[First].DefaultHeatW) continue;
			if (InOutHeatW[N] == FirstW && InOutHeatW[N] != Nodes[N].DefaultHeatW)
			{
				InOutHeatW[N] = Nodes[N].DefaultHeatW * Multiple;
				++Repaired;
			}
		}
	}
	return Repaired;
}

void FRackThermalModel::Initialize(const FRackThermalParams& InParams, const FRackThermalState& InInitialState)
{
	bTrayCoolant = false; TrayCoolant = FTrayCoolantModel(); SlotToTray.Reset();
	LastTraySubsteps = 0; LastTrayIntegratedTimeS = LastTrayLeakHeatW = 0.0;
	Params = InParams;
	Params.NumSleds = NumSlotsOf(Params);
	ResolveLayout(Params, NodeParams, SlotNodeStart);

	const int32 NumNodes = GetNumNodes();

	State = InInitialState;
	if (State.ComponentTempK.Num() != NumNodes)
	{
		State.ComponentTempK.Init(State.CoolantTempK, NumNodes);
	}
	InitialState = State;

	EnergyInJ = 0.0;
	EnergyOutJ = 0.0;
	LastTotalHeatW = 0.0;
	LastConversionLossW = 0.0;
	LastHeatRemovedByCoolantW = 0.0;
	bLastStepStable = true;

	LastHeatToCoolantW.Init(0.0, NumNodes);
	LastHeatToAirW.Init(0.0, NumNodes);
	LastHeatToAmbientW.Init(0.0, NumNodes);
	LastSlotAirMassFlowKgs.Init(0.0, Params.NumSleds);
	LastSlotHeatToAirW.Init(0.0, Params.NumSleds);
	LastSledAirOutletTempK.Init(State.CoolantTempK, Params.NumSleds);
	LastSlotPressureRisePa.Init(0.0, Params.NumSleds);
	LastSlotFanPowerW.Init(0.0, Params.NumSleds);
	LastTotalHeatToRoomAirW = 0.0;
	ScratchNodeHeatW.Init(0.0, NumNodes);

	// Fan mode and where each slot's fan power turns into heat.
	bFanMode = IsFanMode(Params);
	NodeGetsFanHeat.Init(0, NumNodes);
	SlotFanHeatNodes.Init(0, Params.NumSleds);
	for (int32 Slot = 0; Slot < Params.NumSleds; ++Slot)
	{
		if (!bFanMode || !FansOf(Params, Slot)) continue;
		const int32 Begin = SlotNodeStart[Slot], End = SlotNodeStart[Slot + 1];
		auto Mark = [&](TFunctionRef<bool(const FRackComponentParams&)> Pred)
		{
			for (int32 I = Begin; I < End; ++I) if (Pred(NodeParams[I])) { NodeGetsFanHeat[I] = 1; ++SlotFanHeatNodes[Slot]; }
		};
		Mark([](const FRackComponentParams& P) { return P.Kind == ERackPartKind::Fan; });
		if (SlotFanHeatNodes[Slot] == 0) Mark([](const FRackComponentParams& P) { return P.ToAirUAWK > 0.0; });
		if (SlotFanHeatNodes[Slot] == 0) Mark([](const FRackComponentParams&) { return true; });
	}
	// Fan parts in fan mode take their heat from the fan operating point, not from ComponentHeatW.
	// Only non-Fan receivers add the share on top of their own heat (see Step).
	for (int32 I = 0; I < NumNodes; ++I) if (NodeParams[I].Kind == ERackPartKind::Fan) NodeGetsFanHeat[I] = 0;

	if (State.SlotFanSpeedRatio.Num() != Params.NumSleds)
	{
		State.SlotFanSpeedRatio.Init(0.0, Params.NumSleds);
		for (int32 Slot = 0; Slot < Params.NumSleds; ++Slot)
			if (const FRackFanSpec* Fans = bFanMode ? FansOf(Params, Slot) : nullptr)
				State.SlotFanSpeedRatio[Slot] = FanTargetSpeedRatio(*Fans, SlotControlTempK(Slot));
	}
	InitialState.SlotFanSpeedRatio = State.SlotFanSpeedRatio;
}

namespace
{
	// [Index, return) is one air stage: a run of parts sharing AirStage >= 0, or a single part.
	int32 AirStageEnd(const TArray<FRackComponentParams>& Nodes, int32 Index, int32 End)
	{
		int32 G = Index + 1;
		if (Nodes[Index].AirStage >= 0)
			while (G < End && Nodes[G].AirStage == Nodes[Index].AirStage) ++G;
		return G;
	}
	int32 CountAirParts(const TArray<FRackComponentParams>& Nodes, int32 Begin, int32 End, bool bDocked)
	{
		int32 N = 0;
		for (int32 I = Begin; I < End; ++I) N += bDocked && Nodes[I].ToAirUAWK > 0.0;
		return N;
	}
}

double FRackThermalModel::HeatAt(const TArray<double>& HeatW, int32 Index)
{
	return HeatW.IsValidIndex(Index) ? HeatW[Index] : 0.0;
}

bool FRackThermalModel::IsDocked(const TArray<bool>& SledDocked, int32 SledIndex, int32 NumSleds)
{
	// 길이가 맞지 않으면 구성이 아직 안 들어온 것으로 보고 전부 도킹으로 다룬다.
	return SledDocked.Num() != NumSleds || SledDocked[SledIndex];
}

double FRackThermalModel::AirEffectiveness(double ToAirUAWK, double AirCapacityRateWK)
{
	if (ToAirUAWK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	// D2: 유량 0에서 나눗셈을 하지 않는다. 정체 공기는 부품 온도까지 데워지지만
	// 열용량률이 0이라 실어 나가는 열은 0이다.
	if (AirCapacityRateWK <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 1.0;
	}
	const double NTU = ToAirUAWK / AirCapacityRateWK;
	return 1.0 - FMath::Exp(-NTU);
}

bool FRackThermalModel::IsStepStable(const FRackThermalInput& Input, double DeltaTimeS) const
{
	const int32 NumSleds = Params.NumSleds;
	if (SlotNodeStart.Num() != NumSleds + 1) return true;  // not initialized yet
	ComputeSlotAirFlows(Input, ScratchSlotFlowKgs);

	double MaxRatePerS = 0.0;
	double TotalCoolantUAWK = 0.0;

	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const bool bDocked = IsDocked(Input.SledDocked, SledIndex, NumSleds);
		const double AirCapacityRateWK = ScratchSlotFlowKgs[SledIndex] * Params.AirCpJKgK;

		for (int32 Index = SlotNodeStart[SledIndex]; Index < SlotNodeStart[SledIndex + 1]; ++Index)
		{
			const FRackComponentParams& Component = NodeParams[Index];
			const double ToCoolantUAWK = bDocked ? Component.ToCoolantUAWK : 0.0;
			const double ToAirUAWK = bDocked ? Component.ToAirUAWK : 0.0;
			// Parallel stage: this part only gets its share of the slot flow.
			int32 Begin = SlotNodeStart[SledIndex];
			for (int32 G = Begin; G <= Index; G = AirStageEnd(NodeParams, G, SlotNodeStart[SledIndex + 1])) Begin = G;
			const int32 StageEnd = AirStageEnd(NodeParams, Begin, SlotNodeStart[SledIndex + 1]);
			const int32 Parts = StageEnd - Begin > 1 ? FMath::Max(1, CountAirParts(NodeParams, Begin, StageEnd, bDocked)) : 1;
			const double PartRateWK = AirCapacityRateWK / Parts;
			const double AirConductanceWK = PartRateWK * AirEffectiveness(ToAirUAWK, PartRateWK);

			const double RatePerS = (ToCoolantUAWK + AirConductanceWK + Component.ToAmbientUAWK)
				/ Component.HeatCapacityJK;
			MaxRatePerS = FMath::Max(MaxRatePerS, RatePerS);
			TotalCoolantUAWK += ToCoolantUAWK;
		}
	}

	if (!bTrayCoolant)
	{
		const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;
		MaxRatePerS = FMath::Max(MaxRatePerS,
			(TotalCoolantUAWK + CoolantCapacityRateWK) / Params.CoolantHeatCapacityJK);
	}

	return DeltaTimeS * MaxRatePerS <= 1.0;
}

void FRackThermalModel::Step(const FRackThermalInput& Input, double DeltaTimeS)
{
	if (!ensureMsgf(!bTrayCoolant, TEXT("Tray coolant requires StepWithTrayCoolant and explicit boundary flows"))) return;
	StepInternal(Input, DeltaTimeS, nullptr);
}

bool FRackThermalModel::StepInternal(const FRackThermalInput& Input, double DeltaTimeS, FTrayCoolantInput* WaterInput)
{
	if (SlotNodeStart.Num() != Params.NumSleds + 1) return false;
	bLastStepStable = IsStepStable(Input, DeltaTimeS);

	const int32 NumSleds = Params.NumSleds;
	ComputeSlotAirFlows(Input, LastSlotAirMassFlowKgs);

	double TotalHeatW = 0.0;
	double TotalHeatToCoolantW = 0.0;
	double TotalHeatToAirW = 0.0;
	double TotalHeatToAmbientW = 0.0;

	// Heat of every node this step [W], computed once (rule 6) and read by the loop below.
	//   - ordinary part: full-load input x load factor (utilization, IdleHeatFraction)
	//   - fan mode: the slot's fan electric input (affinity law, D8) becomes heat of its fan parts
	//     (or of its air-cooled parts when the device lists no separate fan part, e.g. PSU fans)
	//   - conversion-loss part (power shelf): fraction of the rack's DC load, added after the DC sum
	ScratchNodeHeatW.SetNumZeroed(NodeParams.Num());
	double DcLoadW = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const bool bDocked = IsDocked(Input.SledDocked, SledIndex, NumSleds);
		LastSlotFanPowerW[SledIndex] = SlotFanPowerW(SledIndex, bDocked, Input);
		const FRackFanSpec* Fans = FansOf(Params, SledIndex);
		LastSlotPressureRisePa[SledIndex] = bFanMode && Fans && bDocked
			? DockedResistanceOf(Params, SledIndex) * FMath::Square(LastSlotAirMassFlowKgs[SledIndex]) : 0.0;
		if (!bDocked) continue;
		const double Utilization = SlotUtilizationOf(Input, SledIndex, NumSleds);
		const double FanShareW = SlotFanHeatNodes[SledIndex] > 0 ? LastSlotFanPowerW[SledIndex] / SlotFanHeatNodes[SledIndex] : 0.0;
		for (int32 Index = SlotNodeStart[SledIndex]; Index < SlotNodeStart[SledIndex + 1]; ++Index)
		{
			const FRackComponentParams& Part = NodeParams[Index];
			double HeatW = 0.0;
			if (bFanMode && Fans && Part.Kind == ERackPartKind::Fan) HeatW = FanShareW;
			else if (Part.LossFractionOfRackLoad <= 0.0)
				HeatW = HeatAt(Input.ComponentHeatW, Index) * LoadFactor(Part, Utilization) + (NodeGetsFanHeat[Index] ? FanShareW : 0.0);
			ScratchNodeHeatW[Index] = HeatW;
			if (Part.LossFractionOfRackLoad <= 0.0) DcLoadW += HeatW;
		}
	}
	LastConversionLossW = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		if (!IsDocked(Input.SledDocked, SledIndex, NumSleds)) continue;
		const double FanShareW = SlotFanHeatNodes[SledIndex] > 0 ? LastSlotFanPowerW[SledIndex] / SlotFanHeatNodes[SledIndex] : 0.0;
		for (int32 Index = SlotNodeStart[SledIndex]; Index < SlotNodeStart[SledIndex + 1]; ++Index)
			if (NodeParams[Index].LossFractionOfRackLoad > 0.0)
			{
				const double LossW = NodeParams[Index].LossFractionOfRackLoad * DcLoadW;
				ScratchNodeHeatW[Index] = LossW + (NodeGetsFanHeat[Index] ? FanShareW : 0.0);
				LastConversionLossW += LossW;
			}
	}
	const bool bSlotInlets = Input.SlotAirInletTempK.Num() == NumSleds;

	// 부품 노드를 먼저 전부 갱신하고, 냉각수 노드에는 합계만 넘긴다.
	// 냉각수가 같은 열유속을 다시 계산하지 않게 하려는 것이다 (규약 6).
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const bool bDocked = IsDocked(Input.SledDocked, SledIndex, NumSleds);

		// 뽑힌 슬롯과 팬 없는 장비는 강제 공기가 없다 (ComputeSlotAirFlows).
		const double AirCapacityRateWK = LastSlotAirMassFlowKgs[SledIndex] * Params.AirCpJKgK;

		// 공랭 직렬 체인. 이 변수가 부품을 지나며 데워진다.
		const double SlotInletK = bSlotInlets ? Input.SlotAirInletTempK[SledIndex] : Input.AirInletTempK;
		double AirTempK = SlotInletK;
		double SlotAirW = 0.0;

		const int32 SlotEnd = SlotNodeStart[SledIndex + 1];
		int32 StageBegin = SlotNodeStart[SledIndex];
		int32 StageEnd = StageBegin;
		bool bParallelStage = false;
		double StageInletK = AirTempK, StageAirW = 0.0, PartRateWK = AirCapacityRateWK;
		for (int32 Index = SlotNodeStart[SledIndex]; Index < SlotEnd; ++Index)
		{
			if (Index == StageEnd)
			{
				// Close the previous parallel stage: mixed outlet from the energy it picked up.
				if (bParallelStage && AirCapacityRateWK > UE_DOUBLE_SMALL_NUMBER) AirTempK = StageInletK + StageAirW / AirCapacityRateWK;
				StageBegin = Index;
				StageEnd = AirStageEnd(NodeParams, StageBegin, SlotEnd);
				bParallelStage = StageEnd - StageBegin > 1;
				StageInletK = AirTempK; StageAirW = 0.0;
				PartRateWK = bParallelStage ? AirCapacityRateWK / FMath::Max(1, CountAirParts(NodeParams, StageBegin, StageEnd, bDocked))
					: AirCapacityRateWK;
			}
			const FRackComponentParams& Component = NodeParams[Index];
			const double ComponentTempK = State.ComponentTempK[Index];

			// 뽑힌 유닛은 전원이 끊긴다 (위에서 0으로 남겨 둔다).
			const double HeatW = ScratchNodeHeatW[Index];

			// 열유속 3개를 여기서 한 번만 계산한다 (규약 6).
			// 뽑힌 유닛은 콜드플레이트와 강제 공랭이 모두 끊긴다. 케이스 자연대류만 남는다.
			const double ToCoolantUAWK = bDocked ? Component.ToCoolantUAWK : 0.0;
			const double ToAirUAWK = bDocked ? Component.ToAirUAWK : 0.0;
			const int32 Tray = WaterInput ? SlotToTray[SledIndex] : INDEX_NONE;
			const double WaterTempK = Tray != INDEX_NONE ? TrayCoolant.GetTrayTempK()[Tray] : State.CoolantTempK;
			const double HeatToCoolantW = ToCoolantUAWK * (ComponentTempK - WaterTempK);
			if (Tray != INDEX_NONE) WaterInput->HeatFromPartsW[Tray] += HeatToCoolantW;

			const double Effectiveness = AirEffectiveness(ToAirUAWK, PartRateWK);
			const double HeatToAirW = PartRateWK * Effectiveness * (ComponentTempK - (bParallelStage ? StageInletK : AirTempK));

			// 같은 Effectiveness로 출구 공기를 잇는다. 그래서 구간 열량의 합과
			// 공기 엔탈피 상승이 정확히 같아진다. 나눗셈이 없다.
			// A parallel stage instead mixes its parts' outlets when the stage closes (same energy).
			if (bParallelStage) StageAirW += HeatToAirW;
			else AirTempK += Effectiveness * (ComponentTempK - AirTempK);

			const double HeatToAmbientW = Component.ToAmbientUAWK * (ComponentTempK - SlotInletK);
			SlotAirW += HeatToAirW + HeatToAmbientW;

			const double RateKs = (HeatW - HeatToCoolantW - HeatToAirW - HeatToAmbientW)
				/ Component.HeatCapacityJK;
			State.ComponentTempK[Index] += RateKs * DeltaTimeS;

			LastHeatToCoolantW[Index] = HeatToCoolantW;
			LastHeatToAirW[Index] = HeatToAirW;
			LastHeatToAmbientW[Index] = HeatToAmbientW;

			TotalHeatW += HeatW;
			TotalHeatToCoolantW += HeatToCoolantW;
			TotalHeatToAirW += HeatToAirW;
			TotalHeatToAmbientW += HeatToAmbientW;
		}

		if (bParallelStage && AirCapacityRateWK > UE_DOUBLE_SMALL_NUMBER) AirTempK = StageInletK + StageAirW / AirCapacityRateWK;
		LastSledAirOutletTempK[SledIndex] = AirTempK;
		LastSlotHeatToAirW[SledIndex] = SlotAirW;
	}

	// 냉각수가 이류로 랙 밖에 내보내는 열. 유량이 분자에 있으므로 유량 0이면 그대로 0이다.
	double HeatRemovedByCoolantW, LeakHeatW = 0.0;
	if (WaterInput)
	{
		if (!TrayCoolant.Step(*WaterInput, DeltaTimeS)) return false; // outer transaction owns rollback
		// Compatibility view of return mixing, never another storage node.
		State.CoolantTempK = TrayCoolant.GetReturnTempK();
		HeatRemovedByCoolantW = TrayCoolant.GetLastBoundaryHeatOutW();
		LeakHeatW = TrayCoolant.GetLastLeakHeatW();
	}
	else
	{
		const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;
		HeatRemovedByCoolantW = CoolantCapacityRateWK * (State.CoolantTempK - Input.CoolantInletTempK);
		const double CoolantRateKs = (TotalHeatToCoolantW - HeatRemovedByCoolantW) / Params.CoolantHeatCapacityJK;
		State.CoolantTempK += CoolantRateKs * DeltaTimeS;
	}

	EnergyInJ += TotalHeatW * DeltaTimeS;
	EnergyOutJ += (TotalHeatToAirW + TotalHeatToAmbientW + HeatRemovedByCoolantW + LeakHeatW) * DeltaTimeS;

	LastTotalHeatW = TotalHeatW;
	LastHeatRemovedByCoolantW = HeatRemovedByCoolantW;
	LastTotalHeatToRoomAirW = TotalHeatToAirW + TotalHeatToAmbientW;

	// Fan controller: move each fan-equipped slot toward its target at the slew limit, from the temperatures
	// just integrated. The new speed acts from the next step (explicit, like every other exchange here).
	if (bFanMode)
	{
		for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
		{
			const FRackFanSpec* Fans = FansOf(Params, SledIndex);
			if (!Fans) continue;
			double& Ratio = State.SlotFanSpeedRatio[SledIndex];
			if (Input.FanSpeedOverride >= 0.0) { Ratio = FMath::Clamp(Input.FanSpeedOverride, 0.0, 1.0); continue; }
			if (!IsDocked(Input.SledDocked, SledIndex, NumSleds)) continue;
			const double Target = FanTargetSpeedRatio(*Fans, SlotControlTempK(SledIndex));
			const double MaxChange = FMath::Max(0.0, Fans->MaxSlewPerS) * DeltaTimeS;
			Ratio += FMath::Clamp(Target - Ratio, -MaxChange, MaxChange);
		}
	}
	return true;
}

bool FRackThermalModel::IsFanMode(const FRackThermalParams& InParams)
{
	for (const FRackDeviceType& D : InParams.DeviceTypes) if (D.Fans.HasFans()) return true;
	return false;
}

double FRackThermalModel::FanOperatingMassFlowKgs(const FRackFanSpec& Fans, double ResistancePaS2Kg2,
	double AirDensityKgM3, double SpeedRatio)
{
	if (!Fans.HasFans() || SpeedRatio <= 0.0 || AirDensityKgM3 <= 0.0) return 0.0;
	const double TotalFreeFlowM3s = Fans.NumFans * Fans.MaxFlowM3s;
	const double Denominator = FMath::Max(ResistancePaS2Kg2, 0.0) * AirDensityKgM3 * AirDensityKgM3
		+ Fans.ShutoffPressurePa / (TotalFreeFlowM3s * TotalFreeFlowM3s);
	return AirDensityKgM3 * SpeedRatio * FMath::Sqrt(Fans.ShutoffPressurePa / Denominator);
}

double FRackThermalModel::FanTargetSpeedRatio(const FRackFanSpec& Fans, double ControlTempK)
{
	const double MinRatio = FMath::Clamp(Fans.MinSpeedRatio, 0.0, 1.0);
	const double Span = Fans.ControlFullK - Fans.ControlStartK;
	const double Alpha = Span > UE_DOUBLE_SMALL_NUMBER
		? FMath::Clamp((ControlTempK - Fans.ControlStartK) / Span, 0.0, 1.0)
		: (ControlTempK >= Fans.ControlStartK ? 1.0 : 0.0);
	return MinRatio + (1.0 - MinRatio) * Alpha;
}

double FRackThermalModel::DockedResistanceOf(const FRackThermalParams& InParams, int32 Slot)
{
	if (InParams.Slots.IsValidIndex(Slot) && InParams.DeviceTypes.IsValidIndex(InParams.Slots[Slot].DeviceType)
		&& InParams.DeviceTypes[InParams.Slots[Slot].DeviceType].DockedResistancePaS2Kg2 > 0.0)
	{
		return InParams.DeviceTypes[InParams.Slots[Slot].DeviceType].DockedResistancePaS2Kg2;
	}
	return 0.0;
}

const FRackFanSpec* FRackThermalModel::FansOf(const FRackThermalParams& InParams, int32 Slot)
{
	if (!InParams.Slots.IsValidIndex(Slot) || !InParams.DeviceTypes.IsValidIndex(InParams.Slots[Slot].DeviceType)) return nullptr;
	const FRackFanSpec& Fans = InParams.DeviceTypes[InParams.Slots[Slot].DeviceType].Fans;
	return Fans.HasFans() ? &Fans : nullptr;
}

double FRackThermalModel::LoadFactor(const FRackComponentParams& Part, double Utilization)
{
	// f >= 1 returns exactly 1 (load-independent part).
	const double F = Part.IdleHeatFraction;
	if (F >= 1.0) return 1.0;
	return FMath::Max(F, 0.0) + (1.0 - FMath::Max(F, 0.0)) * FMath::Clamp(Utilization, 0.0, 1.0);
}

double FRackThermalModel::SlotUtilizationOf(const FRackThermalInput& Input, int32 Slot, int32 NumSlots)
{
	const double U = Input.SlotUtilization.Num() == NumSlots && Input.SlotUtilization.IsValidIndex(Slot)
		? Input.SlotUtilization[Slot] : Input.Utilization;
	return FMath::Clamp(U, 0.0, 1.0);
}

void FRackThermalModel::ComputeSlotAirFlows(const FRackThermalInput& Input, TArray<double>& OutSlotFlowKgs) const
{
	// Each device pulls its own air. An empty slot or a fanless device gets no forced flow.
	// (Recirculation through an open slot driven by a front/rear aisle pressure difference is not modelled:
	// the rack front and rear are taken at the same static pressure.)
	const int32 NumSleds = Params.NumSleds;
	OutSlotFlowKgs.SetNum(NumSleds);
	for (int32 Slot = 0; Slot < NumSleds; ++Slot)
	{
		const FRackFanSpec* Fans = FansOf(Params, Slot);
		if (!Fans || !IsDocked(Input.SledDocked, Slot, NumSleds)) { OutSlotFlowKgs[Slot] = 0.0; continue; }
		const double Ratio = Input.FanSpeedOverride >= 0.0 ? FMath::Clamp(Input.FanSpeedOverride, 0.0, 1.0)
			: State.SlotFanSpeedRatio.IsValidIndex(Slot) ? State.SlotFanSpeedRatio[Slot] : 1.0;
		OutSlotFlowKgs[Slot] = FanOperatingMassFlowKgs(*Fans, DockedResistanceOf(Params, Slot), Params.AirDensityKgM3, Ratio);
	}
}

double FRackThermalModel::SlotFanPowerW(int32 Slot, bool bDocked, const FRackThermalInput& Input) const
{
	const FRackFanSpec* Fans = bFanMode ? FansOf(Params, Slot) : nullptr;
	if (!Fans || !bDocked) return 0.0;
	const double Ratio = Input.FanSpeedOverride >= 0.0 ? FMath::Clamp(Input.FanSpeedOverride, 0.0, 1.0)
		: State.SlotFanSpeedRatio.IsValidIndex(Slot) ? State.SlotFanSpeedRatio[Slot] : 1.0;
	return Fans->NumFans * Fans->RatedInputW * Ratio * Ratio * Ratio;
}

double FRackThermalModel::SlotControlTempK(int32 Slot) const
{
	double MaxAirK = -1.0, MaxAnyK = -1.0;
	for (int32 Index = SlotNodeStart[Slot]; Index < SlotNodeStart[Slot + 1]; ++Index)
	{
		if (NodeParams[Index].Kind == ERackPartKind::Fan) continue;
		MaxAnyK = FMath::Max(MaxAnyK, State.ComponentTempK[Index]);
		if (NodeParams[Index].ToAirUAWK > 0.0) MaxAirK = FMath::Max(MaxAirK, State.ComponentTempK[Index]);
	}
	return MaxAirK > 0.0 ? MaxAirK : MaxAnyK;
}

double FRackThermalModel::GetEnergyStoredJ() const
{
	double StoredJ = bTrayCoolant ? TrayCoolant.GetStoredJ()
		: Params.CoolantHeatCapacityJK * (State.CoolantTempK - InitialState.CoolantTempK);

	// Initialize 전에 불릴 수 있다 (에디터 틱은 BeginPlay 없이도 돌아간다).
	// 그럴 때는 배열이 비어 있으므로 상태 배열 길이를 기준으로 돌고 비었으면 0을 돌려준다.
	const int32 NumNodes = FMath::Min3(State.ComponentTempK.Num(), InitialState.ComponentTempK.Num(), NodeParams.Num());
	for (int32 Index = 0; Index < NumNodes; ++Index)
	{
		StoredJ += NodeParams[Index].HeatCapacityJK
			* (State.ComponentTempK[Index] - InitialState.ComponentTempK[Index]);
	}

	return StoredJ;
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
	Snapshot.ComponentTempK = State.ComponentTempK;
	Snapshot.ComponentHeatToCoolantW = LastHeatToCoolantW;
	Snapshot.ComponentHeatToAirW = LastHeatToAirW;
	Snapshot.ComponentHeatW = ScratchNodeHeatW;
	Snapshot.SlotAirMassFlowKgs = LastSlotAirMassFlowKgs;
	Snapshot.SledAirOutletTempK = LastSledAirOutletTempK;
	Snapshot.SlotHeatToAirW = LastSlotHeatToAirW;
	Snapshot.bFanMode = bFanMode;
	Snapshot.SlotFanSpeedRatio = State.SlotFanSpeedRatio;
	Snapshot.SlotPressureRisePa = LastSlotPressureRisePa;
	Snapshot.SlotFanPowerW = LastSlotFanPowerW;
	for (const double W : LastSlotFanPowerW) Snapshot.TotalFanPowerW += W;
	Snapshot.CoolantTempK = State.CoolantTempK;
	Snapshot.CoolantOutletTempK = State.CoolantTempK;
	Snapshot.TotalHeatW = LastTotalHeatW;
	Snapshot.HeatRemovedByCoolantW = LastHeatRemovedByCoolantW;

	double TotalHeatToCoolantW = 0.0;
	double TotalHeatToAirW = 0.0;
	double TotalHeatToAmbientW = 0.0;
	for (int32 Index = 0; Index < LastHeatToCoolantW.Num(); ++Index)
	{
		TotalHeatToCoolantW += LastHeatToCoolantW[Index];
		TotalHeatToAirW += LastHeatToAirW[Index];
		TotalHeatToAmbientW += LastHeatToAmbientW[Index];
	}
	Snapshot.TotalHeatToCoolantW = TotalHeatToCoolantW;
	Snapshot.TotalHeatToAirW = TotalHeatToAirW;
	Snapshot.TotalHeatToAmbientW = TotalHeatToAmbientW;

	// 슬롯 배기를 질량유량으로 가중 혼합한다. 빈 슬롯은 입구 온도 그대로 섞인다.
	double MassFlowSumKgs = 0.0;
	double WeightedTempSum = 0.0;
	for (int32 SledIndex = 0; SledIndex < LastSlotAirMassFlowKgs.Num(); ++SledIndex)
	{
		MassFlowSumKgs += LastSlotAirMassFlowKgs[SledIndex];
		WeightedTempSum += LastSlotAirMassFlowKgs[SledIndex] * LastSledAirOutletTempK[SledIndex];
	}
	Snapshot.MixedAirOutletTempK = MassFlowSumKgs > UE_DOUBLE_SMALL_NUMBER
		? WeightedTempSum / MassFlowSumKgs
		: 0.0;

	Snapshot.EnergyResidualJ = GetEnergyResidualJ();
	Snapshot.NormalizedEnergyResidual = GetNormalizedEnergyResidual();
	Snapshot.bStepStable = bLastStepStable;
	return Snapshot;
}

// ---- NVIDIA GB200 NVL72 프리셋 (장비·칩 단위, 2026-09-23) ----
//
// 목표는 "실제 랙과 같은 곳에서 같은 만큼 열이 난다"이다. 값마다 근거와 추정 여부를 적는다.
//
// 공개 자료로 확인한 것:
//   - 랙 약 120 kW (NVIDIA GB200 NVL72 페이지, DGX GB200 사용자 가이드).
//   - 1U 컴퓨트 트레이 18장(Grace 2 + Blackwell 4), 1U NVLink 스위치 트레이 9장(NVSwitch 2개),
//     1U 전원 셸프 6~8개(셸프당 5.5 kW PSU 6개, 33 kW), 관리 스위치 2대 (DGX GB200 가이드, Lenovo GB300 NVL72 가이드).
//   - 컴퓨트 트레이 CPU·GPU는 콜드플레이트, 네트워크·스토리지는 공랭 (DGX 가이드).
//   - NVLink 스위치 트레이는 "fully liquid-cooled" (Lenovo GB300 NVL72 가이드).
//   - 랙 전체 공기:액체 약 10:90 (Lenovo), 컴퓨트 트레이 약 15:85 (fibermall 분석).
//   - 컴퓨트 트레이 최대 TDP 6.3 kW (SemiAnalysis), Blackwell 약 1200 W, GB200 슈퍼칩 약 2700 W.
//   - PSU 효율 97.5% 이상 (ORv3 전원 셸프, LITEON / Lenovo).
//   - 트레이 구성: E1.S 4개, M.2 1개, ConnectX-7 4개, BlueField-3 2개, 팬 8개 (DGX 가이드, SemiAnalysis).
//
// 추정한 것 (공개된 부품별 전력이 없다):
//   - 부품별 전력 분할(Grace 280 W + LPDDR5X 20 W 등), 모든 UA·열용량.
//   - NVLink 스위치 트레이 545 W: 공개 수치가 3.6 kW/9장 ~ 14 kW/9장으로 엇갈린다.
//     랙 120 kW에서 역산했다 (컴퓨트 111.8 + 관리 0.3 + 스위치 S) * (1/0.975) = 120 -> S = 4.9 kW.
//   - U 배치(아래부터 빈칸 2, 전원 4, 컴퓨트 8, 스위치 9, 컴퓨트 10, 전원 4, 관리 스위치 2, 빈칸 8):
//     공개 사진의 순서(위 관리 스위치 - 전원 - 트레이 묶음 - 전원)를 따른 배치이며 U 번호는 추정이다.
//
// 측정 결과 (MakeGb200Nvl72Params): DC 116.32 kW + PSU 손실 2.983 kW = 119.30 kW,
// 정상 공기 14.6% / 액체 85.4%. SteadyStatePlausible 테스트가 별도 타당성 범위를 확인한다.

// ---- Load dependence and fans (2026-09-25) ----
// Heat at utilization u = full-load heat x (f + (1 - f) u), f = IdleHeatFraction (Fan et al., ISCA 2007).
// Published anchors:
//   - Blackwell: NVIDIA DGX B200 User Guide, "Managing Power Capping": B200 can be capped down to 200 W and
//     "will report a power draw exceeding 200 W, including at idle". Idle = 200 W is taken as the floor
//     -> f = 200/1200. https://docs.nvidia.com/dgx/dgxb200-user-guide/power-capping.html
//   - E1.S NVMe: Samsung PM9A3 datasheet idle 3.5 W -> f = 3.5/20 (the 20 W full load is the preset's
//     Gen5 E1.S estimate, not PM9A3).
// Estimates (no public idle figure): Grace 60 W idle (f 0.21), LPDDR5X 0.3, NVSwitch 0.5 (links stay up),
// ConnectX-7 0.7 and BlueField-3 0.5 (SerDes / Arm cores idle), M.2 0.5, BMC 1.0 (always on),
// PDB/VRM 0.2 (conversion loss follows the power it converts), switches 0.6-0.8.
// PSU loss is a fraction of the live DC load, so it follows utilization by construction. PSU efficiency is
// held at 97.5% at every load; real PSUs lose efficiency at light load (not modelled).
//
// Fans (representative parts; the actual SKUs fitted in GB200 / QM9700 are not published):
//   - Compute tray: 8 x San Ace 9CRJ0412P5J001 (40x40x56 mm counter-rotating, 1U server fan):
//     1.06 m^3/min, 2400 Pa, 37.2 W rated input.
//     https://products.sanyodenki.com/en/sanace/dc/counter-rotating-fan/9CRJ0412P5J001/
//   - Switches / PSUs: San Ace 9HVA0412P3J001 (40x40x28 mm): 1.05 m^3/min, 2300 Pa, 31.2 W.
//     https://products.sanyodenki.com/en/sanace/dc/dc-fan/9HVA0412P3J001/
//   The fan count of the compute tray (8) is public (DGX GB200 guide / SemiAnalysis); the others are estimates.
// Device impedance R [Pa s^2/kg^2]: a typical dense 1U chassis sits at 8-15 mmH2O at its operating flow
// (Sanyo Denki / server fan selection guides). R is set so that ~12 mmH2O (118 Pa) is reached at the flow
// that carries the device's design air heat with a ~15 K air rise. Estimate, not measured.
// Fan speed control: proportional on the hottest air-cooled part, 30% minimum duty (PWM fans run reliably
// down to ~20-33% duty), full speed at 75 C. BMC control laws are vendor specific; this one is an estimate.
namespace
{
	FRackComponentParams Part(const TCHAR* Name, ERackPartKind Kind, double HeatW, double CapacityJK,
		double ToCoolantUAWK, double ToAirUAWK, double ToAmbientUAWK, int32 AirStage = -1, double IdleFraction = 1.0)
	{
		FRackComponentParams P;
		P.Name = Name; P.Kind = Kind; P.DefaultHeatW = HeatW; P.HeatCapacityJK = CapacityJK;
		P.ToCoolantUAWK = ToCoolantUAWK; P.ToAirUAWK = ToAirUAWK; P.ToAmbientUAWK = ToAmbientUAWK; P.AirStage = AirStage;
		P.IdleHeatFraction = IdleFraction;
		return P;
	}

	FRackFanSpec MakeFans(int32 Num, double MaxFlowM3min, double ShutoffPa, double RatedInputW)
	{
		FRackFanSpec F;
		F.NumFans = Num; F.MaxFlowM3s = MaxFlowM3min / 60.0; F.ShutoffPressurePa = ShutoffPa; F.RatedInputW = RatedInputW;
		F.MinSpeedRatio = .3; F.ControlStartK = 328.15; F.ControlFullK = 348.15; F.MaxSlewPerS = .2;
		return F;
	}
	FRackFanSpec San9Crj(int32 Num) { return MakeFans(Num, 1.06, 2400.0, 37.2); }
	FRackFanSpec San9Hva(int32 Num) { return MakeFans(Num, 1.05, 2300.0, 31.2); }

	// R that gives 118 Pa (12 mmH2O) at the mass flow carrying DesignAirW with a 15 K rise (cp 1005).
	double ImpedanceFor(double DesignAirW) { const double M = DesignAirW / (1005.0 * 15.0); return 118.0 / (M * M); }

	// PSU conversion loss shared by every PSU module of every shelf: (1/eta - 1) / (shelves * PSUs).
	constexpr double PsuEfficiency = .975;
	constexpr int32 PsusPerShelf = 6;

	// Bottom-up U map (U0 at Gb200SlotZ0M). -1 = blank.
	constexpr int32 LayoutU[] = {
		-1, -1,                                   // U0-1 floor clearance
		2, 2, 2, 2,                               // U2-5 power shelves
		0, 0, 0, 0, 0, 0, 0, 0,                   // U6-13 compute trays 0..7
		1, 1, 1, 1, 1, 1, 1, 1, 1,                // U14-22 NVLink switch trays
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,             // U23-32 compute trays 8..17
		2, 2, 2, 2,                               // U33-36 power shelves
		3, 3,                                     // U37-38 management switches
	};
}

FRackDeviceType FRackThermalModel::Gb200ComputeTray()
{
	FRackDeviceType D;
	D.Name = TEXT("GB200 Compute Tray");
	D.bLiquidCooled = true;
	// Air-side design heat ~640 W (drives, IO cards, PDB, fans) -> R ~ 6.7e4 (118 Pa at 0.042 kg/s).
	D.DockedResistancePaS2Kg2 = ImpedanceFor(640.0);
	D.Fans = San9Crj(8);
	// Air path front -> rear. Stage 0: E1.S bay, 1: front IO cards side by side, 2: cold-plated boards
	// (no air coupling, pass-through), 3: power distribution board, 4: fan wall.
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("E1.S NVMe %d"), I), ERackPartKind::Storage, 20.0, 60.0, 0.0, 1.0, .02, 0, 3.5 / 20.0));
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("ConnectX-7 %d"), I), ERackPartKind::Nic, 25.0, 80.0, 0.0, 1.2, .02, 1, .7));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("BlueField-3 %d"), I), ERackPartKind::Dpu, 75.0, 150.0, 0.0, 2.5, .03, 1, .5));
	D.Components.Add(Part(TEXT("M.2 boot"), ERackPartKind::Storage, 8.0, 20.0, 0.0, .4, .01, 1, .5));
	D.Components.Add(Part(TEXT("BMC / mgmt"), ERackPartKind::Management, 25.0, 60.0, 0.0, 1.0, .02, 1, 1.0));
	// Two Bianca boards: each Grace + its LPDDR5X + two Blackwell on one cold-plate loop.
	// UA: Grace 280 W at ~8 K, LPDDR5X 20 W at ~10 K through the plate, Blackwell 1200 W at ~12 K.
	for (int32 B = 0; B < 2; ++B)
	{
		D.Components.Add(Part(*FString::Printf(TEXT("Grace CPU %d"), B), ERackPartKind::Cpu, 280.0, 400.0, 35.0, 0.0, .05, 2, 60.0 / 280.0));
		D.Components.Add(Part(*FString::Printf(TEXT("LPDDR5X %d"), B), ERackPartKind::Memory, 20.0, 80.0, 2.0, 0.0, .02, 2, .3));
		for (int32 G = 0; G < 2; ++G)
			D.Components.Add(Part(*FString::Printf(TEXT("Blackwell GPU %d"), B * 2 + G), ERackPartKind::Gpu, 1200.0, 600.0, 100.0, 0.0, .05, 2, 200.0 / 1200.0));
	}
	// 48 V -> 12 V power distribution + board VRM share that is not under the plates (~96% of 5.9 kW).
	D.Components.Add(Part(TEXT("PDB / VRM loss"), ERackPartKind::Power, 250.0, 500.0, 0.0, 8.3, .1, 3, .2));
	// Fan parts: in fan mode their heat is the fans' electric input at the current speed (the 20 W here is
	// only the design value used for CRAH sizing, DesignHeatToAirW).
	for (int32 I = 0; I < 8; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 20.0, 40.0, 0.0, 3.0, .01, 4));
	return D;
}

FRackDeviceType FRackThermalModel::Gb200NvLinkSwitchTray()
{
	FRackDeviceType D;
	D.Name = TEXT("NVLink Switch Tray");
	D.bLiquidCooled = true;
	D.DockedResistancePaS2Kg2 = 1.0e6;  // fully liquid-cooled: no fans, no forced air (Lenovo GB300 NVL72 guide)
	// UA: each ASIC 230 W at ~9 K; board (retimers, CPLD, mgmt) 85 W on the plate at ~10 K.
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("NVSwitch %d"), I), ERackPartKind::Switch, 230.0, 300.0, 25.0, 0.0, .05, -1, .5));
	D.Components.Add(Part(TEXT("Switch board"), ERackPartKind::Other, 85.0, 300.0, 8.0, 0.0, .05, -1, .6));
	return D;
}

FRackDeviceType FRackThermalModel::Gb200PowerShelf(int32 NumShelves)
{
	FRackDeviceType D;
	D.Name = TEXT("Power Shelf 33 kW");
	// Shelf loss at full rack load ~2.98 kW / 8 shelves = 373 W -> R ~ 1.9e5.
	D.DockedResistancePaS2Kg2 = ImpedanceFor(373.0);
	D.Fans = San9Hva(PsusPerShelf);  // one fan per PSU module; its power heats the PSUs (no separate fan part)
	// Six 5.5 kW PSUs side by side (one parallel stage). Their heat is the conversion loss of the whole
	// rack's DC load, recomputed every step, shared by all shelves (N+N load sharing).
	const double Loss = (1.0 / PsuEfficiency - 1.0) / (FMath::Max(1, NumShelves) * PsusPerShelf);
	for (int32 I = 0; I < PsusPerShelf; ++I)
	{
		FRackComponentParams P = Part(*FString::Printf(TEXT("PSU %d"), I), ERackPartKind::Power, 0.0, 400.0, 0.0, 3.0, .05, 0);
		P.LossFractionOfRackLoad = Loss;
		D.Components.Add(P);
	}
	return D;
}

FRackDeviceType FRackThermalModel::ManagementSwitch()
{
	FRackDeviceType D;
	D.Name = TEXT("Management Switch 1U");
	D.DockedResistancePaS2Kg2 = ImpedanceFor(140.0);
	D.Fans = San9Hva(4);
	// 1 GbE-class management switch, ~140 W total. Estimated split.
	D.Components.Add(Part(TEXT("Switch ASIC"), ERackPartKind::Switch, 70.0, 150.0, 0.0, 4.0, .03, 0, .7));
	D.Components.Add(Part(TEXT("Control CPU"), ERackPartKind::Cpu, 25.0, 60.0, 0.0, 1.5, .02, 0, .6));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("PSU %d"), I), ERackPartKind::Power, 20.0, 120.0, 0.0, 1.5, .02, 1, .8));
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 3.75, 20.0, 0.0, 1.0, .01, 2));
	return D;
}

FRackDeviceType FRackThermalModel::QuantumSpineSwitch()
{
	FRackDeviceType D;
	D.Name = TEXT("Quantum-2 Spine Switch");
	D.DockedResistancePaS2Kg2 = ImpedanceFor(747.0);
	D.Fans = San9Hva(6);
	// QM9700 typical passive-cable total = 747 W (ATIS); active-cable maximum = 1720 W.
	// Source checked 2026-09-23: https://networking-docs.nvidia.com/qm97x0hw/specifications
	// All component splits, capacities and UA are estimates; photo geometry is not this SKU.
	// Since 2026-09-25 the fan parts' heat is the fans' computed electric input at their current speed. The 747 W
	// reference includes the fans, so the non-fan parts are 597 W and 747 W is an upper reference at typical
	// fan speed (the earlier 6 x 25 W fan share was itself an estimate).
	D.Components.Add(Part(TEXT("Quantum-2 ASIC"), ERackPartKind::Switch, 527.0, 800.0, 0.0, 15.0, .05, 0, .6));
	D.Components.Add(Part(TEXT("Control CPU"), ERackPartKind::Cpu, 30.0, 60.0, 0.0, 1.5, .02, 0, .6));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("PSU %d"), I), ERackPartKind::Power, 20.0, 200.0, 0.0, 1.5, .02, 1, .8));
	for (int32 I = 0; I < 6; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 25.0, 60.0, 0.0, 2.0, .01, 2));
	return D;
}

bool FRackThermalModel::RefreshKnownDeviceTypes(FRackThermalParams& InOutParams)
{
	bool bChanged = false;
	for (int32 T = 0; T < InOutParams.DeviceTypes.Num(); ++T)
	{
		const FName Name = InOutParams.DeviceTypes[T].Name;
		int32 Count = 0;
		for (const FRackSlot& Slot : InOutParams.Slots) Count += Slot.DeviceType == T;
		FRackDeviceType Fresh;
		if (Name == Gb200ComputeTray().Name) Fresh = Gb200ComputeTray();
		else if (Name == Gb200NvLinkSwitchTray().Name) Fresh = Gb200NvLinkSwitchTray();
		else if (Name == Gb200PowerShelf(1).Name) Fresh = Gb200PowerShelf(FMath::Max(1, Count));
		else if (Name == ManagementSwitch().Name) Fresh = ManagementSwitch();
		else if (Name == QuantumSpineSwitch().Name) Fresh = QuantumSpineSwitch();
		else continue;
		// A different part list would silently remap every node: keep the saved definition then.
		if (Fresh.Components.Num() != InOutParams.DeviceTypes[T].Components.Num()) continue;
		InOutParams.DeviceTypes[T] = Fresh;
		bChanged = true;
	}
	return bChanged;
}

FRackThermalParams FRackThermalModel::MakeGb200Nvl72Params()
{
	FRackThermalParams Params;
	Params.DeviceTypes = {Gb200ComputeTray(), Gb200NvLinkSwitchTray(), Gb200PowerShelf(Gb200Nvl72PowerShelves), ManagementSwitch()};
	for (int32 U = 0; U < UE_ARRAY_COUNT(LayoutU); ++U)
		if (LayoutU[U] >= 0) Params.Slots.Add({LayoutU[U], Gb200SlotZ0M + U * Gb200UnitM, Gb200UnitM});
	Params.NumSleds = Params.Slots.Num();

	// 랙 매니폴드 + 콜드플레이트 27개 트레이분의 물 약 22 kg.
	Params.CoolantHeatCapacityJK = 9.2e4;
	return Params;
}

TArray<double> FRackThermalModel::MakeGb200Nvl72HeatW()
{
	return MakeDefaultHeatW(MakeGb200Nvl72Params());
}

FRackThermalParams FRackThermalModel::MakeQuantumSpineParams(const TArray<double>& SlotBottomZM)
{
	FRackThermalParams Params;
	Params.DeviceTypes = {QuantumSpineSwitch()};
	for (const double Z : SlotBottomZM) Params.Slots.Add({0, Z, Gb200UnitM});
	Params.NumSleds = FMath::Max(1, Params.Slots.Num());
	Params.CoolantHeatCapacityJK = 1.0e3;  // no liquid loop; tiny inert node so the model stays well posed
	return Params;
}

int32 FRackThermalModel::CountLiquidSlots(const FRackThermalParams& InParams, const TArray<bool>* Docked)
{
	int32 C = 0;
	for (int32 S = 0; S < InParams.Slots.Num(); ++S)
	{
		const int32 T = InParams.Slots[S].DeviceType;
		if (!InParams.DeviceTypes.IsValidIndex(T) || !InParams.DeviceTypes[T].bLiquidCooled) continue;
		if (Docked && Docked->Num() == InParams.Slots.Num() && !(*Docked)[S]) continue;
		++C;
	}
	return C;
}

double FRackThermalModel::DesignHeatToCoolantW(const FRackThermalParams& InParams, const FRackThermalInput& Input)
{
	return DesignHeatShareW(InParams, Input, false);
}

double FRackThermalModel::DesignHeatToAirW(const FRackThermalParams& InParams, const FRackThermalInput& Input)
{
	return DesignHeatShareW(InParams, Input, true);
}

// Docked design heat split by each part's UA share; bAir picks the air share, else the coolant share.
// A part with no UA to either goes to air (it can only leave through the case).
double FRackThermalModel::DesignHeatShareW(const FRackThermalParams& InParams, const FRackThermalInput& Input, bool bAir)
{
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	ResolveLayout(InParams, Nodes, Starts);
	const int32 NumSlots = Starts.Num() - 1;
	double DcLoadW = 0.0;
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		if (IsDocked(Input.SledDocked, Slot, NumSlots))
			for (int32 I = Starts[Slot]; I < Starts[Slot + 1]; ++I)
				if (Nodes[I].LossFractionOfRackLoad <= 0.0) DcLoadW += HeatAt(Input.ComponentHeatW, I);
	double HeatW = 0.0;
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		if (!IsDocked(Input.SledDocked, Slot, NumSlots)) continue;
		for (int32 I = Starts[Slot]; I < Starts[Slot + 1]; ++I)
		{
			const FRackComponentParams& P = Nodes[I];
			const double Heat = P.LossFractionOfRackLoad > 0.0 ? P.LossFractionOfRackLoad * DcLoadW : HeatAt(Input.ComponentHeatW, I);
			const double Total = P.ToAirUAWK + P.ToCoolantUAWK;
			const double AirShare = Total > 0.0 ? P.ToAirUAWK / Total : 1.0;
			HeatW += Heat * (bAir ? AirShare : 1.0 - AirShare);
		}
	}
	return HeatW;
}
