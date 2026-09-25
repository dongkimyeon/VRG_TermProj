// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Coupling/CouplingBridge.h"

#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Thermal/RackThermalModel.h"

void FCouplingBridge::Initialize(const TArray<FRackCouplingTarget>& InTargets,
	const TArray<FRackCouplingLink>& InLinks)
{
	Targets = InTargets;
	Links = InLinks;

	// 짝이 안 맞으면 인덱스로 서로를 가리킬 수 없다. 짧은 쪽에 맞춘다.
	const int32 NumRacks = FMath::Min(Targets.Num(), Links.Num());
	Targets.SetNum(NumRacks);
	Links.SetNum(NumRacks);

	EnergyInJ = 0.0;
	EnergyOutJ = 0.0;
	RadiatorRejectedJ = 0.0;
	RackCoolantOutJ = RackAirOutJ = DroppedAirHeatJ = 0.0;
	bLedgerBaselineTaken = false;
	LastItHeatW = 0.0;
	LastRadiatorRejectedW = 0.0;
	LastAirCoolingRemovedW = 0.0;
	bLastHydraulicsResolved = false;
	bLastHydraulicsConverged = true;

	LastCoolantMassFlowKgs.Init(0.0, NumRacks);
	LastAirInletTempK.Init(0.0, NumRacks);
	LastHeatToAirW.Init(0.0, NumRacks);
	LastRackAirMassFlowKgs.Init(0.0, NumRacks);
	LastRackDuctMassFlowKgs.Init(0.0, NumRacks);
	LastFanPowerW = 0.0;
	BuildDuctFaces();
}

void FCouplingBridge::BuildDuctFaces()
{
	// Static duct structure: one duct per (rack, layer). Only velocities change per step (UpdateDuctVelocities).
	FAirSolverInput& In = AirSolverInput;
	In.DuctFaceAxis.Reset(); In.DuctFaceIndex.Reset(); In.DuctFaceFluidCell.Reset();
	In.DuctFaceDuct.Reset(); In.DuctFaceIsExhaust.Reset(); In.DuctFaceVelocityMs.Reset();
	DuctBase.Init(0, Links.Num());
	int32 NumDucts = 0;
	for (int32 R = 0; R < Links.Num(); ++R)
	{
		const FRackCouplingLink& L = Links[R];
		DuctBase[R] = NumDucts;
		if (!L.HasFanFaces()) continue;
		for (int32 F = 0; F < L.FanFaceIndex.Num(); ++F)
		{
			In.DuctFaceAxis.Add(L.FanFaceAxis[F]);
			In.DuctFaceIndex.Add(L.FanFaceIndex[F]);
			In.DuctFaceFluidCell.Add(L.FanFaceFluidCell[F]);
			In.DuctFaceDuct.Add(NumDucts + L.FanFaceLayer[F]);
			In.DuctFaceIsExhaust.Add(L.FanFaceIsExhaust[F]);
			In.DuctFaceVelocityMs.Add(0.0);
		}
		NumDucts += L.NumFanLayers();
	}
	In.NumDucts = NumDucts;
	ScratchLayerFlowKgs.Init(0.0, NumDucts);
	ScratchLayerIntakeK.Init(0.0, NumDucts);
	ScratchLayerIntakeCount.Init(0, NumDucts);
	ScratchLayerExhaustCount.Init(0, NumDucts);
	for (int32 F = 0; F < In.DuctFaceDuct.Num(); ++F)
		++(In.DuctFaceIsExhaust[F] ? ScratchLayerExhaustCount : ScratchLayerIntakeCount)[In.DuctFaceDuct[F]];
}

void FCouplingBridge::AssignSlotsToFanLayers(FRackCouplingLink& Link, const TArray<double>& LayerCenterZM, double LayerHeightM,
	const TArray<double>& SlotBottomZM, const TArray<double>& SlotHeightM)
{
	Link.LayerSlotStart.Reset(); Link.LayerSlot.Reset(); Link.LayerSlotWeight.Reset();
	for (int32 Layer = 0; Layer < LayerCenterZM.Num(); ++Layer)
	{
		Link.LayerSlotStart.Add(Link.LayerSlot.Num());
		for (int32 Slot = 0; Slot < FMath::Min(SlotBottomZM.Num(), SlotHeightM.Num()); ++Slot)
		{
			if (SlotHeightM[Slot] <= 0.0) continue;
			const double Overlap = FMath::Min(SlotBottomZM[Slot] + SlotHeightM[Slot], LayerCenterZM[Layer] + .5 * LayerHeightM)
				- FMath::Max(SlotBottomZM[Slot], LayerCenterZM[Layer] - .5 * LayerHeightM);
			if (Overlap > 0.0) { Link.LayerSlot.Add(Slot); Link.LayerSlotWeight.Add(Overlap / SlotHeightM[Slot]); }
		}
	}
	Link.LayerSlotStart.Add(Link.LayerSlot.Num());
}

bool FCouplingBridge::IsLayerActive(int32 RackIndex, int32 Layer) const
{
	const int32 D = DuctBase[RackIndex] + Layer;
	return ScratchLayerIntakeCount[D] > 0 && ScratchLayerExhaustCount[D] > 0;
}

void FCouplingBridge::MeasureLayers(int32 RackIndex)
{
	// Mixed intake temperature of every layer of one rack: the mean over its intake faces' fluid cells,
	// which is also what the air solver's duct takes in (equal face velocities within a layer).
	const FRackCouplingLink& L = Links[RackIndex];
	const FUniformAirGrid& Grid = AirSolver->GetGrid();
	for (int32 Layer = 0; Layer < L.NumFanLayers(); ++Layer) ScratchLayerIntakeK[DuctBase[RackIndex] + Layer] = 0.0;
	for (int32 F = 0; F < L.FanFaceIndex.Num(); ++F)
		if (!L.FanFaceIsExhaust[F] && Grid.TemperatureK.IsValidIndex(L.FanFaceFluidCell[F]))
			ScratchLayerIntakeK[DuctBase[RackIndex] + L.FanFaceLayer[F]] += Grid.TemperatureK[L.FanFaceFluidCell[F]];
	for (int32 Layer = 0; Layer < L.NumFanLayers(); ++Layer)
	{
		const int32 D = DuctBase[RackIndex] + Layer;
		if (ScratchLayerIntakeCount[D] > 0) ScratchLayerIntakeK[D] /= ScratchLayerIntakeCount[D];
	}
}

void FCouplingBridge::UpdateDuctVelocities()
{
	LastFanPowerW = 0.0;
	for (int32 R = 0; R < Targets.Num(); ++R)
	{
		LastRackAirMassFlowKgs[R] = LastRackDuctMassFlowKgs[R] = 0.0;
		const FRackThermalModel* Model = Targets[R].Model;
		if (!Model) continue;
		const TArray<double>& SlotFlow = Model->GetLastSlotAirMassFlowKgs();
		for (const double M : SlotFlow) LastRackAirMassFlowKgs[R] += M;
		LastFanPowerW += Model->GetLastTotalFanPowerW();
		const FRackCouplingLink& L = Links[R];
		for (int32 Layer = 0; Layer < L.NumFanLayers(); ++Layer)
		{
			double FlowKgs = 0.0;
			for (int32 K = L.LayerSlotStart[Layer]; K < L.LayerSlotStart[Layer + 1]; ++K)
				if (SlotFlow.IsValidIndex(L.LayerSlot[K])) FlowKgs += L.LayerSlotWeight[K] * SlotFlow[L.LayerSlot[K]];
			const int32 D = DuctBase[R] + Layer;
			// A layer without faces on both sides cannot carry mass (the pressure problem would be incompatible).
			ScratchLayerFlowKgs[D] = IsLayerActive(R, Layer) ? FlowKgs : 0.0;
			LastRackDuctMassFlowKgs[R] += ScratchLayerFlowKgs[D];
		}
	}
	if (!AirSolver || AirSolverInput.DuctFaceIndex.Num() == 0) return;
	const FAirGridParams& GridParams = AirSolver->GetGrid().GetParams();
	const double FaceAreaM2 = GridParams.CellSizeM * GridParams.CellSizeM;
	int32 FaceCursor = 0;
	for (int32 R = 0; R < Links.Num(); ++R)
	{
		const FRackCouplingLink& L = Links[R];
		if (!L.HasFanFaces()) continue;
		for (int32 F = 0; F < L.FanFaceIndex.Num(); ++F, ++FaceCursor)
		{
			const int32 D = DuctBase[R] + L.FanFaceLayer[F];
			const int32 Count = L.FanFaceIsExhaust[F] ? ScratchLayerExhaustCount[D] : ScratchLayerIntakeCount[D];
			const double VolumeFlowM3s = ScratchLayerFlowKgs[D] / GridParams.DensityKgM3;
			AirSolverInput.DuctFaceVelocityMs[FaceCursor] = Count > 0 ? L.FanFaceSign[F] * VolumeFlowM3s / (Count * FaceAreaM2) : 0.0;
		}
	}
}

void FCouplingBridge::PullCoolantFlow()
{
	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		FRackThermalInput* Input = Targets[RackIndex].Input;
		if (Input == nullptr)
		{
			continue;
		}

		const int32 PipeIndex = Links[RackIndex].BranchPipeIndex;

		double MassFlowKgs = Links[RackIndex].FallbackCoolantMassFlowKgs;
		if (HydraulicNetwork != nullptr && PipeIndex != INDEX_NONE)
		{
			// 수력은 체적유량 [m^3/s]으로 푼다. 열모델은 질량유량 [kg/s]을 받는다.
			// 바꾸는 곳을 여기 하나로 둬야 밀도가 두 군데로 갈라지지 않는다 (규약 3·6).
			const double FlowM3s = FMath::Abs(HydraulicNetwork->GetPipeFlowM3s(PipeIndex));
			MassFlowKgs = FlowM3s * CoolantDensityKgM3;
		}

		Input->CoolantMassFlowKgs = MassFlowKgs;
		LastCoolantMassFlowKgs[RackIndex] = MassFlowKgs;
	}
}

void FCouplingBridge::PullAirInletTemperature()
{
	if (AirSolver == nullptr)
	{
		for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
		{
			const FRackThermalInput* Input = Targets[RackIndex].Input;
			LastAirInletTempK[RackIndex] = Input != nullptr ? Input->AirInletTempK : 0.0;
		}
		return;
	}

	const FUniformAirGrid& Grid = AirSolver->GetGrid();

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		FRackThermalInput* Input = Targets[RackIndex].Input;
		if (Input == nullptr)
		{
			continue;
		}

		// 흡기 셀들의 평균 온도를 랙 흡기 온도로 준다.
		// 셀이 하나도 안 잡히면 랙이 들고 있던 값을 그대로 둔다 (격자 밖에 있는 랙).
		double SumTempK = 0.0;
		int32 Count = 0;
		for (const int32 CellIndex : Links[RackIndex].InletCellIndices)
		{
			if (!Grid.TemperatureK.IsValidIndex(CellIndex))
			{
				continue;
			}
			SumTempK += Grid.TemperatureK[CellIndex];
			++Count;
		}

		if (Count > 0)
		{
			Input->AirInletTempK = SumTempK / Count;
		}
		LastAirInletTempK[RackIndex] = Input->AirInletTempK;

		// Per-slot inlet: the room air at that device's height, not the rack average.
		const FRackCouplingLink& Link = Links[RackIndex];
		const int32 NumSlots = Link.SlotInletStart.Num() - 1;
		if (NumSlots > 0 && Targets[RackIndex].Model && NumSlots == Targets[RackIndex].Model->GetNumSleds())
		{
			Input->SlotAirInletTempK.SetNum(NumSlots);
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				double Sum = 0.0; int32 N = 0;
				for (int32 K = Link.SlotInletStart[Slot]; K < Link.SlotInletStart[Slot + 1]; ++K)
					if (Grid.TemperatureK.IsValidIndex(Link.SlotInletCells[K])) { Sum += Grid.TemperatureK[Link.SlotInletCells[K]]; ++N; }
				Input->SlotAirInletTempK[Slot] = N > 0 ? Sum / N : Input->AirInletTempK;
			}
		}
		else
		{
			Input->SlotAirInletTempK.Reset();
		}

		// Fan through-flow: a slot breathes exactly the air its duct layers take in (the flow-weighted intake
		// temperature), so the rack model and the air grid see the same inlet enthalpy.
		if (Link.HasFanFaces() && Targets[RackIndex].Model)
		{
			const int32 NumModelSlots = Targets[RackIndex].Model->GetNumSleds();
			MeasureLayers(RackIndex);
			TArray<double> Sum, Weight;
			Sum.Init(0.0, NumModelSlots); Weight.Init(0.0, NumModelSlots);
			for (int32 Layer = 0; Layer < Link.NumFanLayers(); ++Layer)
			{
				if (!IsLayerActive(RackIndex, Layer)) continue;
				const double LayerK = ScratchLayerIntakeK[DuctBase[RackIndex] + Layer];
				for (int32 K = Link.LayerSlotStart[Layer]; K < Link.LayerSlotStart[Layer + 1]; ++K)
					if (Sum.IsValidIndex(Link.LayerSlot[K])) { Sum[Link.LayerSlot[K]] += Link.LayerSlotWeight[K] * LayerK; Weight[Link.LayerSlot[K]] += Link.LayerSlotWeight[K]; }
			}
			if (Input->SlotAirInletTempK.Num() != NumModelSlots) Input->SlotAirInletTempK.Init(Input->AirInletTempK, NumModelSlots);
			for (int32 Slot = 0; Slot < NumModelSlots; ++Slot)
				if (Weight[Slot] > 0.0) Input->SlotAirInletTempK[Slot] = Sum[Slot] / Weight[Slot];
		}
	}
}

void FCouplingBridge::CloseCoolantLoop(double DeltaTimeS)
{
	double RejectedW = 0.0;

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const FRackCouplingTarget& Target = Targets[RackIndex];
		if (Target.Model == nullptr || Target.Input == nullptr || Target.Radiator == nullptr)
		{
			continue;
		}

		const double CoolantCpJKgK = Target.Model->GetParams().CoolantCpJKgK;
		const double MassFlowKgs = Target.Input->CoolantMassFlowKgs;

		// 랙 출구 = 지금 냉각수 노드 온도. 랙 모델도 스텝을 시작할 때 이 값을 읽는다.
		const double OutletTempK = Target.Model->GetState().CoolantTempK;

		// 버린 열과 되돌아오는 온도를 **같은 effectiveness로 한 번에** 구한다 (규약 6).
		// 그래서 랙이 냉각수로 내보낸 열과 라디에이터가 버린 열이 정확히 같아진다.
		//   랙이 내보낸 열 = m*cp*(T_out - T_in)
		//   라디에이터가 버린 열 = m*cp*eps*(T_out - T_amb)
		//   T_in = T_out - eps*(T_out - T_amb)  이므로 둘이 같다.
		// 둘을 따로 계산하면 전역 장부가 그 차이만큼 어긋난다.
		Target.Input->CoolantInletTempK =
			RadiatorModel::OutletTempK(*Target.Radiator, OutletTempK, MassFlowKgs, CoolantCpJKgK);

		RejectedW +=
			RadiatorModel::HeatRejectedW(*Target.Radiator, OutletTempK, MassFlowKgs, CoolantCpJKgK);
	}

	LastRadiatorRejectedW = RejectedW;
	RadiatorRejectedJ += RejectedW * DeltaTimeS;
}

void FCouplingBridge::PushHeatToAir()
{
	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		// D1: 부품별로 쪼개 넣지 않는다. 강제 공랭과 케이스 자연대류를 합산한다.
		// 자연대류를 빼면 그만큼 전역 장부에서 새어 나간다 (그 열도 방 공기를 데운다).
		const FRackThermalModel* Model = Targets[RackIndex].Model;
		LastHeatToAirW[RackIndex] = Model ? Model->GetLastTotalHeatToRoomAirW() : 0.0;
	}

	if (AirSolver == nullptr)
	{
		return;
	}

	const FUniformAirGrid& Grid = AirSolver->GetGrid();
	ScratchCellHeatW.Init(0.0, Grid.GetNumCells());

	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const TArray<int32>& ExhaustCells = Links[RackIndex].ExhaustCellIndices;
		auto Spread = [&](double HeatW, const int32* Cells, int32 Num)
		{
			// 배기 셀에 고르게 나눈다. 겹치면 더한다 - 덮어쓰면 열이 사라진다.
			for (int32 K = 0; K < Num; ++K)
				if (ScratchCellHeatW.IsValidIndex(Cells[K])) ScratchCellHeatW[Cells[K]] += HeatW / Num;
		};

		const FRackCouplingLink& Link = Links[RackIndex];
		const FRackThermalModel* Model = Targets[RackIndex].Model;
		if (Model == nullptr) continue;
		const TArray<double>& SlotHeatW = Model->GetLastSlotHeatToAirW();
		const int32 NumSlots = Link.SlotExhaustStart.Num() - 1;

		// Fan through-flow: a slot's air-side heat enters the fluid cells its duct layers exhaust into, in
		// proportion to the slot flow each layer carries (D1: still one volumetric source per cell).
		TArray<double> SlotRemainingW = SlotHeatW;
		if (Link.HasFanFaces())
		{
			for (int32 Slot = 0; Slot < SlotHeatW.Num(); ++Slot)
			{
				double WeightSum = 0.0;
				for (int32 Layer = 0; Layer < Link.NumFanLayers(); ++Layer)
				{
					if (!IsLayerActive(RackIndex, Layer)) continue;
					for (int32 K = Link.LayerSlotStart[Layer]; K < Link.LayerSlotStart[Layer + 1]; ++K)
						if (Link.LayerSlot[K] == Slot) WeightSum += Link.LayerSlotWeight[K];
				}
				if (WeightSum <= 0.0) continue;
				for (int32 Layer = 0; Layer < Link.NumFanLayers(); ++Layer)
				{
					if (!IsLayerActive(RackIndex, Layer)) continue;
					double W = 0.0;
					for (int32 K = Link.LayerSlotStart[Layer]; K < Link.LayerSlotStart[Layer + 1]; ++K)
						if (Link.LayerSlot[K] == Slot) W += Link.LayerSlotWeight[K];
					if (W <= 0.0) continue;
					const int32 D = DuctBase[RackIndex] + Layer;
					const double FaceHeatW = SlotHeatW[Slot] * W / WeightSum / ScratchLayerExhaustCount[D];
					for (int32 F = 0; F < Link.FanFaceIndex.Num(); ++F)
						if (Link.FanFaceIsExhaust[F] && Link.FanFaceLayer[F] == Layer && ScratchCellHeatW.IsValidIndex(Link.FanFaceFluidCell[F]))
							ScratchCellHeatW[Link.FanFaceFluidCell[F]] += FaceHeatW;
				}
				SlotRemainingW[Slot] = 0.0;
			}
		}

		if (ExhaustCells.Num() == 0)
		{
			continue;
		}

		// Per-slot injection: every device's air-side heat enters behind it at its own height.
		if (NumSlots > 0 && SlotHeatW.Num() == NumSlots)
		{
			for (int32 Slot = 0; Slot < NumSlots; ++Slot)
			{
				if (SlotRemainingW[Slot] == 0.0) continue;
				const int32 Num = Link.SlotExhaustStart[Slot + 1] - Link.SlotExhaustStart[Slot];
				if (Num > 0) Spread(SlotRemainingW[Slot], Link.SlotExhaustCells.GetData() + Link.SlotExhaustStart[Slot], Num);
				else Spread(SlotRemainingW[Slot], ExhaustCells.GetData(), ExhaustCells.Num());
			}
			continue;
		}
		if (!Link.HasFanFaces()) Spread(LastHeatToAirW[RackIndex], ExhaustCells.GetData(), ExhaustCells.Num());
	}
}

void FCouplingBridge::Step(double DeltaTimeS, bool bHydraulicsResolved, bool bHydraulicsConverged)
{
	if (DeltaTimeS <= 0.0)
	{
		return;
	}

	bLastHydraulicsResolved = bHydraulicsResolved;
	bLastHydraulicsConverged = bHydraulicsConverged;

	if (!bLedgerBaselineTaken)
	{
		BaselineModelInJ = 0.0;
		for (const FRackCouplingTarget& Target : Targets) if (Target.Model) BaselineModelInJ += Target.Model->GetEnergyInJ();
		BaselineStoredJ = GetRawEnergyStoredJ();
		BaselineAirOutJ = AirSolver ? AirSolver->GetEnergyOutJ() : 0.0;
		RadiatorRejectedJ = RackCoolantOutJ = RackAirOutJ = DroppedAirHeatJ = 0.0;
		bLedgerBaselineTaken = true;
	}

	// 1) 수력 -> 열: 랙 분기 유량을 냉각수 질량유량으로 넘긴다.
	PullCoolantFlow();

	// 2) 공기 -> 열: 랙 앞 공기 온도를 흡기 온도로 넘긴다.
	PullAirInletTemperature();

	// 3) 냉각수 루프를 라디에이터로 닫는다. 버린 열을 한 번만 계산해 장부에 적는다.
	CloseCoolantLoop(DeltaTimeS);

	// 4) 랙 열모델을 돌린다. 규약 4대로 모든 도메인이 같은 dt를 쓴다.
	double ItHeatW = 0.0;
	for (int32 RackIndex = 0; RackIndex < Targets.Num(); ++RackIndex)
	{
		const FRackCouplingTarget& Target = Targets[RackIndex];
		if (Target.Model == nullptr || Target.Input == nullptr)
		{
			continue;
		}

		Target.Model->Step(*Target.Input, DeltaTimeS);
		ItHeatW += Target.Model->GetLastTotalHeatW();
		RackCoolantOutJ += Target.Model->GetLastHeatRemovedByCoolantW() * DeltaTimeS;
		RackAirOutJ += Target.Model->GetLastTotalHeatToRoomAirW() * DeltaTimeS;
	}
	LastItHeatW = ItHeatW;

	// 5) 열 -> 공기: 랙이 공기로 내보낸 열을 체적 열원으로 민다 (D1: 합산해 하나로).
	//    Fans: the slot flows the rack models just used become the duct face velocities (one exchange, rule 6).
	UpdateDuctVelocities();
	PushHeatToAir();

	// 6) 공기 격자를 같은 dt로 돌린다.
	if (AirSolver != nullptr)
	{
		FAirSolverInput& AirInput = AirSolverInput;
		AirInput.CellHeatW = ScratchCellHeatW;

		const double CoolingBeforeJ = AirSolver->GetEnergyOutJ();
		{
			// S7-0 계측. 통합 경로에서도 공기 솔버 몫을 따로 뗀다.
			// SCOPE_CYCLE_COUNTER만 스레드 안전하므로 누산기 갱신은 아래에서 한 번에 한다.
			SCOPE_CYCLE_COUNTER(STAT_VrgAirSolverStep);
			const uint64 StartCycles = FPlatformTime::Cycles64();
			AirSolver->Step(AirInput, DeltaTimeS);
			for (int32 I = 0; I < AirInput.CellHeatW.Num(); ++I)
				if (AirInput.CellHeatW[I] != 0.0 && AirSolver->GetGrid().bSolidCell.IsValidIndex(I) && AirSolver->GetGrid().bSolidCell[I])
					DroppedAirHeatJ += AirInput.CellHeatW[I] * DeltaTimeS;
			LastAirSolverMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
		}
		LastAirCoolingRemovedW = (AirSolver->GetEnergyOutJ() - CoolingBeforeJ) / DeltaTimeS;
	}
	else
	{
		LastAirCoolingRemovedW = 0.0;
	}

	// 7) 전역 장부. 유입은 랙 모델이 이미 세고 있으므로 합치기만 한다.
	EnergyInJ = -BaselineModelInJ;
	for (const FRackCouplingTarget& Target : Targets)
	{
		if (Target.Model != nullptr)
		{
			EnergyInJ += Target.Model->GetEnergyInJ();
		}
	}

	// 유출은 계 밖으로 나간 것만이다: 라디에이터(냉각수)와 CRAH 코일(공기).
	// 랙에서 공기로 넘어간 열은 계 안에서의 이동이라 여기 넣지 않는다.
	EnergyOutJ = RadiatorRejectedJ + (AirSolver != nullptr ? AirSolver->GetEnergyOutJ() - BaselineAirOutJ : 0.0);
}

double FCouplingBridge::GetEnergyStoredJ() const
{
	return GetRawEnergyStoredJ() - (bLedgerBaselineTaken ? BaselineStoredJ : 0.0);
}

double FCouplingBridge::GetRawEnergyStoredJ() const
{
	double StoredJ = 0.0;

	for (const FRackCouplingTarget& Target : Targets)
	{
		if (Target.Model != nullptr)
		{
			StoredJ += Target.Model->GetEnergyStoredJ();
		}
	}

	if (AirSolver != nullptr)
	{
		StoredJ += AirSolver->GetEnergyStoredJ();
	}

	return StoredJ;
}

double FCouplingBridge::GetEnergyResidualJ() const
{
	return EnergyInJ - EnergyOutJ - GetEnergyStoredJ();
}

double FCouplingBridge::GetNormalizedEnergyResidual() const
{
	if (FMath::Abs(EnergyInJ) <= UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	return GetEnergyResidualJ() / EnergyInJ;
}

FCouplingSnapshot FCouplingBridge::MakeSnapshot(double SimTimeS) const
{
	FCouplingSnapshot Snapshot;
	Snapshot.SimTimeS = SimTimeS;
	Snapshot.RackCoolantMassFlowKgs = LastCoolantMassFlowKgs;
	Snapshot.RackAirInletTempK = LastAirInletTempK;
	Snapshot.RackHeatToAirW = LastHeatToAirW;
	Snapshot.RackAirMassFlowKgs = LastRackAirMassFlowKgs;
	Snapshot.RackDuctMassFlowKgs = LastRackDuctMassFlowKgs;
	Snapshot.TotalFanPowerW = LastFanPowerW;
	Snapshot.bAirDuctsRejected = AirSolver != nullptr && AirSolver->WereDuctsRejected();

	Snapshot.TotalItHeatW = LastItHeatW;
	Snapshot.TotalRadiatorRejectedW = LastRadiatorRejectedW;
	Snapshot.TotalAirCoolingRemovedW = LastAirCoolingRemovedW;

	Snapshot.EnergyInJ = EnergyInJ;
	Snapshot.EnergyOutJ = EnergyOutJ;
	Snapshot.EnergyStoredJ = GetEnergyStoredJ();
	Snapshot.NormalizedEnergyResidual = GetNormalizedEnergyResidual();

	Snapshot.bHydraulicsResolvedThisStep = bLastHydraulicsResolved;
	Snapshot.bHydraulicsConverged = bLastHydraulicsConverged;
	Snapshot.bAirPressureConverged =
		(AirSolver == nullptr) || AirSolver->GetLastPressureResult().bConverged;

	return Snapshot;
}

FString FCouplingBridge::DescribeEnergyLedger() const
{
	double RackResidualJ = 0.0;
	for (const FRackCouplingTarget& Target : Targets) if (Target.Model) RackResidualJ += Target.Model->GetEnergyResidualJ();
	const double AirResidualJ = AirSolver ? AirSolver->GetEnergyResidualJ() : 0.0;
	const double AirInJ = AirSolver ? AirSolver->GetEnergyInJ() : 0.0;
	return FString::Printf(TEXT("전역 잔차 %.3e J = 랙 %.3e + 공기 %.3e + 결합 %.3e | 결합: 냉각수(랙 반출 %.6e - 라디에이터 %.6e = %.3e), ")
		TEXT("공기(랙 반출 %.6e - 공기 주입 %.6e = %.3e, 고체 셀 버림 %.3e)"),
		GetEnergyResidualJ(), RackResidualJ, AirResidualJ, GetEnergyResidualJ() - RackResidualJ - AirResidualJ,
		RackCoolantOutJ, RadiatorRejectedJ, RackCoolantOutJ - RadiatorRejectedJ, RackAirOutJ, AirInJ, RackAirOutJ - AirInJ, DroppedAirHeatJ);
}

