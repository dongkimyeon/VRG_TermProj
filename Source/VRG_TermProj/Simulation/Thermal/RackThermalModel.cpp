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

	// GPU: 700 W급 가속기 1장. 콜드플레이트 UA는 700 W를 약 10 K로 뽑는 값이다.
	// 열용량은 보드 + 콜드플레이트 약 2 kg.
	FRackComponentParams& Gpu = Components[static_cast<int32>(ERackComponent::Gpu)];
	Gpu.Name = TEXT("GPU");
	Gpu.HeatCapacityJK = 800.0;
	Gpu.ToCoolantUAWK = 70.0;
	Gpu.ToAirUAWK = 0.0;
	Gpu.ToAmbientUAWK = 0.2;

	// RAM: DDR5 RDIMM 16장. 공랭 UA는 120 W를 약 24 K 상승으로 버티는 값이다.
	// 허용 온도 약 85 C로 공랭 부품 중 가장 낮아 체인 앞에 둔다.
	FRackComponentParams& Ram = Components[static_cast<int32>(ERackComponent::Ram)];
	Ram.Name = TEXT("RAM");
	Ram.HeatCapacityJK = 500.0;
	Ram.ToCoolantUAWK = 0.0;
	Ram.ToAirUAWK = 5.0;
	Ram.ToAmbientUAWK = 0.1;

	// VRM: 마더보드 전원부 파워스테이지. 허용 온도 약 125 C로 RAM보다 높아 체인 뒤다.
	// 공랭 UA는 80 W를 약 27 K 상승으로 버티는 값이다.
	FRackComponentParams& Vrm = Components[static_cast<int32>(ERackComponent::Vrm)];
	Vrm.Name = TEXT("VRM");
	Vrm.HeatCapacityJK = 300.0;
	Vrm.ToCoolantUAWK = 0.0;
	Vrm.ToAirUAWK = 3.0;
	Vrm.ToAmbientUAWK = 0.1;

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

void FRackThermalModel::Initialize(const FRackThermalParams& InParams, const FRackThermalState& InInitialState)
{
	Params = InParams;
	Params.NumSleds = FMath::Max(1, Params.NumSleds);
	if (Params.Components.Num() != NumRackComponents)
	{
		Params.Components = DefaultComponents();
	}

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
	LastSledAirOutletTempK.Init(State.CoolantTempK, Params.NumSleds);
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
	const int32 NumSleds = FMath::Max(1, InParams.NumSleds);
	OutSlotFlowKgs.SetNum(NumSleds);

	// 2차 손실 병렬 유로: 같은 dP에서 MassFlow_i = sqrt(dP / R_i).
	// 총 유량이 주어지면 dP는 소거되고 배분 가중치는 1/sqrt(R_i)만 남는다.
	double TotalWeight = 0.0;
	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const double ResistancePaS2Kg2 = IsDocked(SledDocked, SledIndex, NumSleds)
			? InParams.DockedSlotResistancePaS2Kg2
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
	DistributeSlotAirMassFlowKgs(Params, Input.SledDocked, Input.TotalAirMassFlowKgs, ScratchSlotFlowKgs);

	double MaxRatePerS = 0.0;
	double TotalCoolantUAWK = 0.0;

	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const bool bDocked = IsDocked(Input.SledDocked, SledIndex, NumSleds);
		const double AirCapacityRateWK = ScratchSlotFlowKgs[SledIndex] * Params.AirCpJKgK;

		for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
		{
			const FRackComponentParams& Component = Params.Components[ComponentIndex];
			const double ToCoolantUAWK = bDocked ? Component.ToCoolantUAWK : 0.0;
			const double ToAirUAWK = bDocked ? Component.ToAirUAWK : 0.0;
			const double AirConductanceWK = AirCapacityRateWK * AirEffectiveness(ToAirUAWK, AirCapacityRateWK);

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
	bLastStepStable = IsStepStable(Input, DeltaTimeS);

	const int32 NumSleds = Params.NumSleds;
	DistributeSlotAirMassFlowKgs(Params, Input.SledDocked, Input.TotalAirMassFlowKgs, LastSlotAirMassFlowKgs);

	double TotalHeatW = 0.0;
	double TotalHeatToCoolantW = 0.0;
	double TotalHeatToAirW = 0.0;
	double TotalHeatToAmbientW = 0.0;

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
		double AirTempK = Input.AirInletTempK;

		for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
		{
			const FRackComponentParams& Component = Params.Components[ComponentIndex];
			const int32 Index = SledIndex * NumRackComponents + ComponentIndex;
			const double ComponentTempK = State.ComponentTempK[Index];

			// 뽑힌 유닛은 전원이 끊긴다.
			const double HeatW = bDocked ? HeatAt(Input.ComponentHeatW, Index) : 0.0;

			// 열유속 3개를 여기서 한 번만 계산한다 (규약 6).
			// 뽑힌 유닛은 콜드플레이트와 강제 공랭이 모두 끊긴다. 케이스 자연대류만 남는다.
			const double ToCoolantUAWK = bDocked ? Component.ToCoolantUAWK : 0.0;
			const double ToAirUAWK = bDocked ? Component.ToAirUAWK : 0.0;
			const double HeatToCoolantW = ToCoolantUAWK * (ComponentTempK - State.CoolantTempK);

			const double Effectiveness = AirEffectiveness(ToAirUAWK, AirCapacityRateWK);
			const double HeatToAirW = AirCapacityRateWK * Effectiveness * (ComponentTempK - AirTempK);

			// 같은 Effectiveness로 출구 공기를 잇는다. 그래서 구간 열량의 합과
			// 공기 엔탈피 상승이 정확히 같아진다. 나눗셈이 없다.
			AirTempK += Effectiveness * (ComponentTempK - AirTempK);

			const double HeatToAmbientW = Component.ToAmbientUAWK * (ComponentTempK - Input.AirInletTempK);

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

		LastSledAirOutletTempK[SledIndex] = AirTempK;
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
	const int32 NumNodes = FMath::Min(State.ComponentTempK.Num(), InitialState.ComponentTempK.Num());
	if (Params.Components.Num() != NumRackComponents)
	{
		return StoredJ;
	}

	for (int32 Index = 0; Index < NumNodes; ++Index)
	{
		const int32 ComponentIndex = Index % NumRackComponents;
		StoredJ += Params.Components[ComponentIndex].HeatCapacityJK
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
