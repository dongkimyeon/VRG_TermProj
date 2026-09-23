// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Thermal/RackThermalModel.h"

TArray<FRackComponentParams> FRackThermalModel::DefaultComponents()
{
	// 배열 순서 = 공랭 직렬 체인 순서. 허용 온도가 낮은 부품이 앞이다.
	// Cpu/Gpu는 ToAirUAWK가 0이라 체인에서 공기 온도를 바꾸지 않는다.
	TArray<FRackComponentParams> Components;
	Components.SetNum(NumRackComponents);

	// ToAmbientUAWK는 케이스를 통한 자연대류·복사다. 합치면 유닛당 0.6 W/K,
	// 랙 12대 기준 7.2 W/K로 정상 상태 반출의 1% 정도다. 밀폐 캐비닛에서
	// 이보다 크게 잡으면 랙이 저절로 수 백 W를 버리는 셈이 돼 타당하지 않다.
	// 대신 뽑은 유닛은 이 경로로만 식으므로 시정수가 C/UA = 2000/0.6 약 55분이다.
	// 느리게 식는 것이 정상이며 데모를 위해 부풀린 값이 아니다.

	// CPU: 서버용 1소켓. 콜드플레이트 UA는 350 W를 약 9 K 접근온도로 뽑는 값이다.
	// 열용량은 다이 + IHS + 콜드플레이트 구리 약 1 kg (c ~ 385 J/kgK).
	FRackComponentParams& Cpu = Components[static_cast<int32>(ERackComponent::Cpu)];
	Cpu.Name = TEXT("CPU");
	Cpu.HeatCapacityJK = 400.0;
	Cpu.ToCoolantUAWK = 40.0;
	Cpu.ToAirUAWK = 0.0;
	Cpu.ToAmbientUAWK = 0.2;
	Cpu.Kind = ERackPartKind::Cpu;

	// GPU: 700 W급 가속기 1장. 콜드플레이트 UA는 700 W를 약 10 K로 뽑는 값이다.
	// 열용량은 보드 + 콜드플레이트 약 2 kg.
	FRackComponentParams& Gpu = Components[static_cast<int32>(ERackComponent::Gpu)];
	Gpu.Name = TEXT("GPU");
	Gpu.HeatCapacityJK = 800.0;
	Gpu.ToCoolantUAWK = 70.0;
	Gpu.ToAirUAWK = 0.0;
	Gpu.ToAmbientUAWK = 0.2;
	Gpu.Kind = ERackPartKind::Gpu;

	// RAM: DDR5 RDIMM 16장. 공랭 UA는 120 W를 약 24 K 상승으로 버티는 값이다.
	// 허용 온도 약 85 C로 공랭 부품 중 가장 낮아 체인 앞에 둔다.
	FRackComponentParams& Ram = Components[static_cast<int32>(ERackComponent::Ram)];
	Ram.Name = TEXT("RAM");
	Ram.HeatCapacityJK = 500.0;
	Ram.ToCoolantUAWK = 0.0;
	Ram.ToAirUAWK = 5.0;
	Ram.ToAmbientUAWK = 0.1;
	Ram.Kind = ERackPartKind::Memory;

	// VRM: 마더보드 전원부 파워스테이지. 허용 온도 약 125 C로 RAM보다 높아 체인 뒤다.
	// 공랭 UA는 80 W를 약 27 K 상승으로 버티는 값이다.
	FRackComponentParams& Vrm = Components[static_cast<int32>(ERackComponent::Vrm)];
	Vrm.Name = TEXT("VRM");
	Vrm.HeatCapacityJK = 300.0;
	Vrm.ToCoolantUAWK = 0.0;
	Vrm.ToAirUAWK = 3.0;
	Vrm.ToAmbientUAWK = 0.1;
	Vrm.Kind = ERackPartKind::Power;

	return Components;
}

TArray<double> FRackThermalModel::DefaultSledHeatW()
{
	TArray<double> HeatW;
	HeatW.SetNum(NumRackComponents);
	// 유닛 1대 합계 1250 W. 슬롯 12개면 랙 15.0 kW로 CLAUDE.md의 S1 참고 발열과 같다.
	HeatW[static_cast<int32>(ERackComponent::Cpu)] = 350.0;  // 상위 서버 CPU TDP 350 W급 1소켓
	HeatW[static_cast<int32>(ERackComponent::Gpu)] = 700.0;  // 700 W급 가속기 1장
	HeatW[static_cast<int32>(ERackComponent::Ram)] = 120.0;  // RDIMM 16장 x 약 7.5 W
	HeatW[static_cast<int32>(ERackComponent::Vrm)] = 80.0;   // 위 1170 W를 효율 약 93%로 변환할 때의 손실
	return HeatW;
}

TArray<double> FRackThermalModel::MakeUniformHeatW(int32 NumSleds)
{
	const TArray<double> SledHeatW = DefaultSledHeatW();
	TArray<double> HeatW;
	HeatW.Reserve(FMath::Max(0, NumSleds) * NumRackComponents);
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		HeatW.Append(SledHeatW);
	}
	return HeatW;
}

int32 FRackThermalModel::NumSlotsOf(const FRackThermalParams& InParams)
{
	return InParams.Slots.Num() > 0 ? InParams.Slots.Num() : FMath::Max(1, InParams.NumSleds);
}

void FRackThermalModel::ResolveLayout(const FRackThermalParams& InParams, TArray<FRackComponentParams>& OutNodeParams,
	TArray<int32>& OutSlotNodeStart)
{
	OutNodeParams.Reset(); OutSlotNodeStart.Reset();
	const int32 NumSlots = NumSlotsOf(InParams);
	OutSlotNodeStart.Reserve(NumSlots + 1);
	if (InParams.Slots.Num() == 0)
	{
		// Legacy uniform rack: node = Sled * 4 + Component, exactly as in S1.
		const TArray<FRackComponentParams> Parts =
			InParams.Components.Num() == NumRackComponents ? InParams.Components : DefaultComponents();
		for (int32 S = 0; S < NumSlots; ++S) { OutSlotNodeStart.Add(OutNodeParams.Num()); OutNodeParams.Append(Parts); }
	}
	else
	{
		for (const FRackSlot& Slot : InParams.Slots)
		{
			OutSlotNodeStart.Add(OutNodeParams.Num());
			if (InParams.DeviceTypes.IsValidIndex(Slot.DeviceType)) OutNodeParams.Append(InParams.DeviceTypes[Slot.DeviceType].Components);
		}
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

void FRackThermalModel::Initialize(const FRackThermalParams& InParams, const FRackThermalState& InInitialState)
{
	Params = InParams;
	Params.NumSleds = NumSlotsOf(Params);
	if (Params.Slots.Num() == 0 && Params.Components.Num() != NumRackComponents)
	{
		Params.Components = DefaultComponents();
	}
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
	LastHeatRemovedByCoolantW = 0.0;
	bLastStepStable = true;

	LastHeatToCoolantW.Init(0.0, NumNodes);
	LastHeatToAirW.Init(0.0, NumNodes);
	LastHeatToAmbientW.Init(0.0, NumNodes);
	LastSlotAirMassFlowKgs.Init(0.0, Params.NumSleds);
	LastSlotHeatToAirW.Init(0.0, Params.NumSleds);
	LastSledAirOutletTempK.Init(State.CoolantTempK, Params.NumSleds);
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

double FRackThermalModel::GetComponentTempK(int32 SledIndex, ERackComponent Component) const
{
	const int32 Index = NodeIndex(SledIndex, Component);
	return State.ComponentTempK.IsValidIndex(Index) ? State.ComponentTempK[Index] : 0.0;
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

void FRackThermalModel::DistributeSlotAirMassFlowKgs(const FRackThermalParams& InParams,
	const TArray<bool>& SledDocked, double TotalAirMassFlowKgs, TArray<double>& OutSlotFlowKgs)
{
	const int32 NumSleds = NumSlotsOf(InParams);
	OutSlotFlowKgs.SetNum(NumSleds);

	// 2차 손실 병렬 유로: 같은 dP에서 MassFlow_i = sqrt(dP / R_i).
	// 총 유량이 주어지면 dP는 소거되고 배분 가중치는 1/sqrt(R_i)만 남는다.
	double TotalWeight = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		double DockedResistance = InParams.DockedSlotResistancePaS2Kg2;
		if (InParams.Slots.IsValidIndex(SledIndex) && InParams.DeviceTypes.IsValidIndex(InParams.Slots[SledIndex].DeviceType)
			&& InParams.DeviceTypes[InParams.Slots[SledIndex].DeviceType].DockedResistancePaS2Kg2 > 0.0)
		{
			DockedResistance = InParams.DeviceTypes[InParams.Slots[SledIndex].DeviceType].DockedResistancePaS2Kg2;
		}
		const double ResistancePaS2Kg2 = IsDocked(SledDocked, SledIndex, NumSleds)
			? DockedResistance
			: InParams.EmptySlotResistancePaS2Kg2;

		const double Weight = ResistancePaS2Kg2 > UE_DOUBLE_SMALL_NUMBER
			? 1.0 / FMath::Sqrt(ResistancePaS2Kg2)
			: 0.0;

		OutSlotFlowKgs[SledIndex] = Weight;
		TotalWeight += Weight;
	}

	if (TotalWeight <= UE_DOUBLE_SMALL_NUMBER)
	{
		for (double& FlowKgs : OutSlotFlowKgs)
		{
			FlowKgs = 0.0;
		}
		return;
	}

	for (double& FlowKgs : OutSlotFlowKgs)
	{
		FlowKgs = TotalAirMassFlowKgs * FlowKgs / TotalWeight;
	}
}

bool FRackThermalModel::IsStepStable(const FRackThermalInput& Input, double DeltaTimeS) const
{
	const int32 NumSleds = Params.NumSleds;
	if (SlotNodeStart.Num() != NumSleds + 1) return true;  // not initialized yet
	DistributeSlotAirMassFlowKgs(Params, Input.SledDocked, Input.TotalAirMassFlowKgs, ScratchSlotFlowKgs);

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

	const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;
	MaxRatePerS = FMath::Max(MaxRatePerS,
		(TotalCoolantUAWK + CoolantCapacityRateWK) / Params.CoolantHeatCapacityJK);

	return DeltaTimeS * MaxRatePerS <= 1.0;
}

void FRackThermalModel::Step(const FRackThermalInput& Input, double DeltaTimeS)
{
	if (SlotNodeStart.Num() != Params.NumSleds + 1) return;  // not initialized yet
	bLastStepStable = IsStepStable(Input, DeltaTimeS);

	const int32 NumSleds = Params.NumSleds;
	DistributeSlotAirMassFlowKgs(Params, Input.SledDocked, Input.TotalAirMassFlowKgs, LastSlotAirMassFlowKgs);

	double TotalHeatW = 0.0;
	double TotalHeatToCoolantW = 0.0;
	double TotalHeatToAirW = 0.0;
	double TotalHeatToAmbientW = 0.0;

	// DC load for conversion-loss parts (power shelves): heat of every other docked part this step.
	double DcLoadW = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		if (!IsDocked(Input.SledDocked, SledIndex, NumSleds)) continue;
		for (int32 Index = SlotNodeStart[SledIndex]; Index < SlotNodeStart[SledIndex + 1]; ++Index)
			if (NodeParams[Index].LossFractionOfRackLoad <= 0.0) DcLoadW += HeatAt(Input.ComponentHeatW, Index);
	}
	const bool bSlotInlets = Input.SlotAirInletTempK.Num() == NumSleds;

	// 부품 노드를 먼저 전부 갱신하고, 냉각수 노드에는 합계만 넘긴다.
	// 냉각수가 같은 열유속을 다시 계산하지 않게 하려는 것이다 (규약 6).
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const bool bDocked = IsDocked(Input.SledDocked, SledIndex, NumSleds);

		// 빈 슬롯에도 공기는 흐른다. 오히려 저항이 낮아 더 많이 흐른다.
		// 다만 지나갈 하드웨어가 없으므로 아래에서 부품의 공기 UA를 0으로 둔다.
		// 그래서 빈 슬롯 배기는 입구 온도 그대로 나오고(= 바이패스), 열은 오가지 않는다.
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

			// 뽑힌 유닛은 전원이 끊긴다. 변환 손실 부품은 입력이 아니라 이번 스텝 DC 부하의 비율이다.
			const double HeatW = !bDocked ? 0.0
				: Component.LossFractionOfRackLoad > 0.0 ? Component.LossFractionOfRackLoad * DcLoadW
				: HeatAt(Input.ComponentHeatW, Index);

			// 열유속 3개를 여기서 한 번만 계산한다 (규약 6).
			// 뽑힌 유닛은 콜드플레이트와 강제 공랭이 모두 끊긴다. 케이스 자연대류만 남는다.
			const double ToCoolantUAWK = bDocked ? Component.ToCoolantUAWK : 0.0;
			const double ToAirUAWK = bDocked ? Component.ToAirUAWK : 0.0;
			const double HeatToCoolantW = ToCoolantUAWK * (ComponentTempK - State.CoolantTempK);

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
	const double CoolantCapacityRateWK = Input.CoolantMassFlowKgs * Params.CoolantCpJKgK;
	const double HeatRemovedByCoolantW = CoolantCapacityRateWK * (State.CoolantTempK - Input.CoolantInletTempK);

	// 부품이 내놓은 것과 같은 TotalHeatToCoolantW가 냉각수로 들어간다 (규약 6).
	const double CoolantRateKs = (TotalHeatToCoolantW - HeatRemovedByCoolantW) / Params.CoolantHeatCapacityJK;
	State.CoolantTempK += CoolantRateKs * DeltaTimeS;

	EnergyInJ += TotalHeatW * DeltaTimeS;
	EnergyOutJ += (TotalHeatToAirW + TotalHeatToAmbientW + HeatRemovedByCoolantW) * DeltaTimeS;

	LastTotalHeatW = TotalHeatW;
	LastHeatRemovedByCoolantW = HeatRemovedByCoolantW;
}

double FRackThermalModel::GetEnergyStoredJ() const
{
	double StoredJ = Params.CoolantHeatCapacityJK * (State.CoolantTempK - InitialState.CoolantTempK);

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
	Snapshot.SlotAirMassFlowKgs = LastSlotAirMassFlowKgs;
	Snapshot.SledAirOutletTempK = LastSledAirOutletTempK;
	Snapshot.SlotHeatToAirW = LastSlotHeatToAirW;
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
// 결과 (MakeGb200Nvl72Params 기본값): DC 부하 117.0 kW + PSU 손실 3.0 kW = 랙 120.0 kW,
// 공기 약 17.2 kW(14%) / 액체 약 102.8 kW(86%). 테스트가 이 범위를 확인한다.

namespace
{
	FRackComponentParams Part(const TCHAR* Name, ERackPartKind Kind, double HeatW, double CapacityJK,
		double ToCoolantUAWK, double ToAirUAWK, double ToAmbientUAWK, int32 AirStage = -1)
	{
		FRackComponentParams P;
		P.Name = Name; P.Kind = Kind; P.DefaultHeatW = HeatW; P.HeatCapacityJK = CapacityJK;
		P.ToCoolantUAWK = ToCoolantUAWK; P.ToAirUAWK = ToAirUAWK; P.ToAmbientUAWK = ToAmbientUAWK; P.AirStage = AirStage;
		return P;
	}

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
	D.DockedResistancePaS2Kg2 = 1.0e4;  // same as the S1 docked slot (dP ~100 Pa at 0.1 kg/s)
	// Air path front -> rear. Stage 0: E1.S bay, 1: front IO cards side by side, 2: cold-plated boards
	// (no air coupling, pass-through), 3: power distribution board, 4: fan wall.
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("E1.S NVMe %d"), I), ERackPartKind::Storage, 20.0, 60.0, 0.0, 1.0, .02, 0));
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("ConnectX-7 %d"), I), ERackPartKind::Nic, 25.0, 80.0, 0.0, 1.2, .02, 1));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("BlueField-3 %d"), I), ERackPartKind::Dpu, 75.0, 150.0, 0.0, 2.5, .03, 1));
	D.Components.Add(Part(TEXT("M.2 boot"), ERackPartKind::Storage, 8.0, 20.0, 0.0, .4, .01, 1));
	D.Components.Add(Part(TEXT("BMC / mgmt"), ERackPartKind::Management, 25.0, 60.0, 0.0, 1.0, .02, 1));
	// Two Bianca boards: each Grace + its LPDDR5X + two Blackwell on one cold-plate loop.
	// UA: Grace 280 W at ~8 K, LPDDR5X 20 W at ~10 K through the plate, Blackwell 1200 W at ~12 K.
	for (int32 B = 0; B < 2; ++B)
	{
		D.Components.Add(Part(*FString::Printf(TEXT("Grace CPU %d"), B), ERackPartKind::Cpu, 280.0, 400.0, 35.0, 0.0, .05, 2));
		D.Components.Add(Part(*FString::Printf(TEXT("LPDDR5X %d"), B), ERackPartKind::Memory, 20.0, 80.0, 2.0, 0.0, .02, 2));
		for (int32 G = 0; G < 2; ++G)
			D.Components.Add(Part(*FString::Printf(TEXT("Blackwell GPU %d"), B * 2 + G), ERackPartKind::Gpu, 1200.0, 600.0, 100.0, 0.0, .05, 2));
	}
	// 48 V -> 12 V power distribution + board VRM share that is not under the plates (~96% of 5.9 kW).
	D.Components.Add(Part(TEXT("PDB / VRM loss"), ERackPartKind::Power, 250.0, 500.0, 0.0, 8.3, .1, 3));
	for (int32 I = 0; I < 8; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 20.0, 40.0, 0.0, 3.0, .01, 4));
	return D;
}

FRackDeviceType FRackThermalModel::Gb200NvLinkSwitchTray()
{
	FRackDeviceType D;
	D.Name = TEXT("NVLink Switch Tray");
	D.bLiquidCooled = true;
	D.DockedResistancePaS2Kg2 = 1.0e6;  // fully liquid-cooled: almost no air through the tray
	// UA: each ASIC 230 W at ~9 K; board (retimers, CPLD, mgmt) 85 W on the plate at ~10 K.
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("NVSwitch %d"), I), ERackPartKind::Switch, 230.0, 300.0, 25.0, 0.0, .05));
	D.Components.Add(Part(TEXT("Switch board"), ERackPartKind::Other, 85.0, 300.0, 8.0, 0.0, .05));
	return D;
}

FRackDeviceType FRackThermalModel::Gb200PowerShelf(int32 NumShelves)
{
	FRackDeviceType D;
	D.Name = TEXT("Power Shelf 33 kW");
	D.DockedResistancePaS2Kg2 = 1.5e4;
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
	D.DockedResistancePaS2Kg2 = 2.0e4;
	// 1 GbE-class management switch, ~140 W total. Estimated split.
	D.Components.Add(Part(TEXT("Switch ASIC"), ERackPartKind::Switch, 70.0, 150.0, 0.0, 4.0, .03, 0));
	D.Components.Add(Part(TEXT("Control CPU"), ERackPartKind::Cpu, 25.0, 60.0, 0.0, 1.5, .02, 0));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("PSU %d"), I), ERackPartKind::Power, 20.0, 120.0, 0.0, 1.5, .02, 1));
	for (int32 I = 0; I < 4; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 3.75, 20.0, 0.0, 1.0, .01, 2));
	return D;
}

FRackDeviceType FRackThermalModel::QuantumSpineSwitch()
{
	FRackDeviceType D;
	D.Name = TEXT("Quantum-2 Spine Switch");
	D.DockedResistancePaS2Kg2 = 1.0e4;
	// ~620 W with passive cables (estimated split; datasheet totals vary with optics).
	D.Components.Add(Part(TEXT("Quantum-2 ASIC"), ERackPartKind::Switch, 400.0, 800.0, 0.0, 15.0, .05, 0));
	D.Components.Add(Part(TEXT("Control CPU"), ERackPartKind::Cpu, 30.0, 60.0, 0.0, 1.5, .02, 0));
	for (int32 I = 0; I < 2; ++I) D.Components.Add(Part(*FString::Printf(TEXT("PSU %d"), I), ERackPartKind::Power, 20.0, 200.0, 0.0, 1.5, .02, 1));
	for (int32 I = 0; I < 6; ++I) D.Components.Add(Part(*FString::Printf(TEXT("Fan %d"), I), ERackPartKind::Fan, 25.0, 60.0, 0.0, 2.0, .01, 2));
	return D;
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
	if (InParams.Slots.Num() == 0)
	{
		const int32 N = FMath::Max(1, InParams.NumSleds);
		if (!Docked || Docked->Num() != N) return N;
		int32 C = 0; for (const bool b : *Docked) C += b; return C;
	}
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

double FRackThermalModel::DesignHeatToAirW(const FRackThermalParams& InParams, const FRackThermalInput& Input)
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
			HeatW += Heat * (Total > 0.0 ? P.ToAirUAWK / Total : 1.0);
		}
	}
	return HeatW;
}
