// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/CoolantInventory.h"

namespace ExpansionTank
{
	static double PrechargeAbsPa(const FExpansionTankParams& Params, double FillGaugePa)
	{
		return FMath::Max(FillGaugePa - Params.PrechargeBelowFillPa + Params.AtmosphericPressurePa, 1.0);
	}

	double GasInvariantPaM3(const FExpansionTankParams& Params, double FillGaugePa)
	{
		return PrechargeAbsPa(Params, FillGaugePa) * Params.TankVolumeM3;
	}

	FExpansionTankState Initial(const FExpansionTankParams& Params, double FillGaugePa)
	{
		FExpansionTankState State;
		const double FillAbsPa = FillGaugePa + Params.AtmosphericPressurePa;
		const double GasM3 = FillAbsPa > 0.0 ? GasInvariantPaM3(Params, FillGaugePa) / FillAbsPa : Params.TankVolumeM3;
		State.WaterVolumeM3 = FMath::Clamp(Params.TankVolumeM3 - GasM3, 0.0, Params.TankVolumeM3);
		State.InitialWaterM3 = State.WaterVolumeM3;
		State.bDepleted = State.WaterVolumeM3 <= 0.0;
		return State;
	}

	double GaugePressurePa(const FExpansionTankParams& Params, double FillGaugePa, const FExpansionTankState& State)
	{
		if (State.bDepleted) return 0.0;   // gas in the loop: the cushion no longer holds the loop (see header)
		const double GasM3 = FMath::Max(Params.TankVolumeM3 - State.WaterVolumeM3, UE_DOUBLE_SMALL_NUMBER);
		return GasInvariantPaM3(Params, FillGaugePa) / GasM3 - Params.AtmosphericPressurePa;
	}

	void Advance(const FExpansionTankParams& Params, double FillGaugePa, FExpansionTankState& State,
		double LeakM3s, double DtS, double NowS)
	{
		if (State.bDepleted || DtS <= 0.0) return;
		const double LeakDemandM3 = FMath::Max(LeakM3s, 0.0) * DtS;
		// Make-up refills toward the fill state only (never above the fill pressure), within its pump limit.
		double MakeupM3 = 0.0;
		if (Params.bMakeupWater && GaugePressurePa(Params, FillGaugePa, State) < FillGaugePa)
		{
			const double NeededM3 = State.InitialWaterM3 - State.WaterVolumeM3 + LeakDemandM3;
			MakeupM3 = FMath::Clamp(NeededM3, 0.0, Params.MakeupMaxFlowM3s * DtS);
		}
		const double LeakM3 = FMath::Min(LeakDemandM3, State.WaterVolumeM3 + MakeupM3);   // cannot lose more than it holds
		State.WaterVolumeM3 = State.WaterVolumeM3 + MakeupM3 - LeakM3;
		State.LeakedM3 += LeakM3;
		State.MakeupM3 += MakeupM3;
		if (State.WaterVolumeM3 <= 0.0 && LeakDemandM3 > 0.0)
		{
			State.WaterVolumeM3 = 0.0;
			State.bDepleted = true;
			State.DepletedAtS = NowS;
		}
	}
}

int32 FCoolantInventory::AddTank(const FHydraulicNetwork& Network, int32 CduNodeIndex, int32 PumpIndex,
	const TArray<int32>& LeakIndices, const FExpansionTankParams& Params)
{
	FTank& Tank = Tanks.AddDefaulted_GetRef();
	Tank.CduNodeIndex = CduNodeIndex;
	Tank.PumpIndex = PumpIndex;
	Tank.LeakIndices = LeakIndices;
	Tank.Params = Params;
	Tank.FillGaugePa = Network.GetNodeSetPressurePa(CduNodeIndex);
	Tank.PumpSpeedRatio = Network.GetPumpSpeedRatio(PumpIndex);
	for (const int32 Leak : LeakIndices) Tank.LeakDiametersM.Add(Network.GetLeak(Leak).HoleDiameterM);
	Tank.State = ExpansionTank::Initial(Params, Tank.FillGaugePa);
	return Tanks.Num() - 1;
}

double FCoolantInventory::GetTankGaugePressurePa(int32 TankIndex) const
{
	if (!Tanks.IsValidIndex(TankIndex)) return 0.0;
	const FTank& Tank = Tanks[TankIndex];
	return ExpansionTank::GaugePressurePa(Tank.Params, Tank.FillGaugePa, Tank.State);
}

bool FCoolantInventory::IsActive(const FHydraulicNetwork& Network) const
{
	if (bDirty) return true;
	for (const FTank& Tank : Tanks)
	{
		if (Tank.State.bDepleted) continue;
		for (const int32 Leak : Tank.LeakIndices) if (Network.GetLeak(Leak).HoleDiameterM > 0.0) return true;
		if (Tank.Params.bMakeupWater && Tank.State.WaterVolumeM3 < Tank.State.InitialWaterM3) return true;
	}
	return false;
}

void FCoolantInventory::ApplyBoundary(FHydraulicNetwork& Network) const
{
	for (int32 T = 0; T < Tanks.Num(); ++T)
	{
		const FTank& Tank = Tanks[T];
		Network.SetNodeSetPressurePa(Tank.CduNodeIndex, GetTankGaugePressurePa(T));
		if (Tank.State.bDepleted)
		{
			Network.SetPumpSpeedRatio(Tank.PumpIndex, 0.0);
			for (const int32 Leak : Tank.LeakIndices) Network.SetLeakHoleDiameterM(Leak, 0.0);
		}
	}
}

bool FCoolantInventory::Restart(FHydraulicNetwork& Network, const FHydraulicSolveSettings& Settings, FHydraulicSolveResult& OutResult)
{
	bool bChanged = false;
	for (FTank& Tank : Tanks)
	{
		const FExpansionTankState Fresh = ExpansionTank::Initial(Tank.Params, Tank.FillGaugePa);
		bChanged |= Tank.State.WaterVolumeM3 != Fresh.WaterVolumeM3 || Tank.State.bDepleted != Fresh.bDepleted
			|| Network.GetNodeSetPressurePa(Tank.CduNodeIndex) != Tank.FillGaugePa
			|| Network.GetPumpSpeedRatio(Tank.PumpIndex) != Tank.PumpSpeedRatio;
		for (int32 L = 0; L < Tank.LeakIndices.Num(); ++L)
			bChanged |= Network.GetLeak(Tank.LeakIndices[L]).HoleDiameterM != Tank.LeakDiametersM[L];
		Tank.State = Fresh;
		Tank.LastLeakM3s = Tank.LastMakeupM3s = 0.0;
		Network.SetNodeSetPressurePa(Tank.CduNodeIndex, Tank.FillGaugePa);
		Network.SetPumpSpeedRatio(Tank.PumpIndex, Tank.PumpSpeedRatio);
		for (int32 L = 0; L < Tank.LeakIndices.Num(); ++L) Network.SetLeakHoleDiameterM(Tank.LeakIndices[L], Tank.LeakDiametersM[L]);
	}
	bDirty = false;
	NumSolves = 0;
	if (bChanged && Network.GetNumNodes() > 0)
	{
		OutResult = Network.Solve(Settings);
		return true;
	}
	return false;
}

bool FCoolantInventory::Step(FHydraulicNetwork& Network, const FHydraulicSolveSettings& Settings, double DtS, double NowS,
	FHydraulicSolveResult& OutResult)
{
	if (!IsActive(Network) || DtS <= 0.0) return false;
	ApplyBoundary(Network);
	OutResult = Network.Solve(Settings);
	++NumSolves;
	bDirty = false;
	for (FTank& Tank : Tanks)
	{
		const bool bWasDepleted = Tank.State.bDepleted;
		double LeakM3s = 0.0;
		for (const int32 Leak : Tank.LeakIndices) LeakM3s += Network.GetLeakFlowM3s(Leak);
		const double MakeupBeforeM3 = Tank.State.MakeupM3;
		const double LeakedBeforeM3 = Tank.State.LeakedM3;
		ExpansionTank::Advance(Tank.Params, Tank.FillGaugePa, Tank.State, LeakM3s, DtS, NowS);
		Tank.LastLeakM3s = (Tank.State.LeakedM3 - LeakedBeforeM3) / DtS;
		Tank.LastMakeupM3s = (Tank.State.MakeupM3 - MakeupBeforeM3) / DtS;
		bDirty |= Tank.State.bDepleted && !bWasDepleted;   // the pump stop / closed leaks need one more solve
	}
	return true;
}
