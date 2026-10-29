#include "Simulation/Thermal/RackThermalModel.h"

bool FRackThermalModel::EnableTrayCoolant(const TArray<int32>& Slots, const TArray<double>& Fractions)
{
	if (bTrayCoolant || SlotNodeStart.Num() != Params.NumSleds + 1 || Slots.IsEmpty()
		|| Slots.Num() != Fractions.Num() || EnergyInJ != 0.0 || EnergyOutJ != 0.0
		|| State.ComponentTempK != InitialState.ComponentTempK || State.CoolantTempK != InitialState.CoolantTempK) return false;
	TArray<int32> Mapping; Mapping.Init(INDEX_NONE, Params.NumSleds);
	for (int32 T = 0; T < Slots.Num(); ++T)
	{
		const int32 S = Slots[T];
		if (!Mapping.IsValidIndex(S) || Mapping[S] != INDEX_NONE) return false;
		bool bLiquid = false;
		for (int32 N = SlotNodeStart[S]; N < SlotNodeStart[S + 1]; ++N) bLiquid |= NodeParams[N].ToCoolantUAWK > 0.0;
		if (!bLiquid) return false;
		Mapping[S] = T;
	}
	for (int32 S = 0; S < Params.NumSleds; ++S)
		for (int32 N = SlotNodeStart[S]; N < SlotNodeStart[S + 1]; ++N)
		{
			const auto& P = NodeParams[N];
			if (!FMath::IsFinite(P.HeatCapacityJK) || P.HeatCapacityJK <= 0.0
				|| !FMath::IsFinite(P.ToCoolantUAWK) || P.ToCoolantUAWK < 0.0
				|| !FMath::IsFinite(P.ToAirUAWK) || P.ToAirUAWK < 0.0
				|| !FMath::IsFinite(P.ToAmbientUAWK) || P.ToAmbientUAWK < 0.0
				|| (P.ToCoolantUAWK > 0.0 && Mapping[S] == INDEX_NONE)) return false;
		}
	FTrayCoolantModel Water;
	if (!Water.Initialize(Fractions, Params.CoolantHeatCapacityJK, State.CoolantTempK, Params.CoolantCpJKgK)) return false;
	TrayCoolant = MoveTemp(Water); SlotToTray = MoveTemp(Mapping); bTrayCoolant = true;
	return true;
}

bool FRackThermalModel::StepWithTrayCoolant(const FRackThermalInput& Input, const FTrayCoolantInput& Boundary,
	double DeltaTimeS, int32 MaxSubsteps)
{
	if (!bTrayCoolant || !FMath::IsFinite(DeltaTimeS) || DeltaTimeS <= 0.0 || MaxSubsteps <= 0
		|| Input.ComponentHeatW.Num() != NodeParams.Num() || !FMath::IsFinite(Input.AirInletTempK) || Input.AirInletTempK <= 0.0
		|| !FMath::IsFinite(Input.Utilization) || !FMath::IsFinite(Input.FanSpeedOverride)) return false;
	for (double W : Input.ComponentHeatW) if (!FMath::IsFinite(W) || W < 0.0) return false;
	for (double U : Input.SlotUtilization) if (!FMath::IsFinite(U)) return false;
	for (double K : Input.SlotAirInletTempK) if (!FMath::IsFinite(K) || K <= 0.0) return false;

	FTrayCoolantInput Water = Boundary;
	const int32 NumTrays = TrayCoolant.GetTrayTempK().Num();
	Water.HeatFromPartsW.Init(0.0, NumTrays); Water.PartsConductanceWK.Init(0.0, NumTrays);
	for (int32 S = 0; S < Params.NumSleds; ++S)
		if (SlotToTray[S] != INDEX_NONE && IsDocked(Input.SledDocked, S, Params.NumSleds))
			for (int32 N = SlotNodeStart[S]; N < SlotNodeStart[S + 1]; ++N)
				Water.PartsConductanceWK[SlotToTray[S]] += NodeParams[N].ToCoolantUAWK;
	const double WaterLimitS = TrayCoolant.GetStableStepLimitS(Water);
	if (!(WaterLimitS > 0.0)) return false;

	// Transaction covers temperatures, fan state, every diagnostic and both ledgers, even if a later
	// substep overflows or the caller's work cap is reached. No UObject/room/clock access here.
	FRackThermalModel Candidate = *this;
	TArray<double> PartHeat, ToWater, ToAir, ToAmbient, SlotAir, SlotFlow, SlotMassTemp, FanPower, Pressure;
	PartHeat.Init(0.0, NodeParams.Num()); ToWater = ToAir = ToAmbient = PartHeat;
	SlotAir.Init(0.0, Params.NumSleds); SlotFlow = SlotMassTemp = FanPower = Pressure = SlotAir;
	auto Accumulate = [](TArray<double>& Sum, const TArray<double>& Values, double Dt)
	{
		for (int32 I = 0; I < Sum.Num(); ++I) Sum[I] += Dt * Values[I];
	};
	double RemainingS = DeltaTimeS, TotalHeatJ = 0.0, LossJ = 0.0, BoundaryJ = 0.0, LeakJ = 0.0, AirJ = 0.0;
	int32 Substeps = 0;
	while (RemainingS > 0.0)
	{
		if (Substeps >= MaxSubsteps) return false;
		// Half of the explicit monotonicity limit avoids roundoff at dt*rate=1. Numerical
		// integration margin only; capacitances, conductances and physical time are unchanged.
		double Dt = FMath::Min(RemainingS, 0.5 * WaterLimitS);
		while (Dt > 0.0 && !Candidate.IsStepStable(Input, Dt)) Dt *= 0.5;
		if (!(Dt > 0.0) || RemainingS - Dt == RemainingS) return false;
		Water.HeatFromPartsW.Init(0.0, NumTrays);
		if (!Candidate.StepInternal(Input, Dt, &Water)) return false;
		if (!FMath::IsFinite(Candidate.EnergyInJ) || !FMath::IsFinite(Candidate.EnergyOutJ)
			|| !FMath::IsFinite(Candidate.State.CoolantTempK) || Candidate.State.CoolantTempK <= 0.0) return false;
		for (double K : Candidate.State.ComponentTempK) if (!FMath::IsFinite(K) || K <= 0.0) return false;
		for (double K : Candidate.TrayCoolant.GetTrayTempK()) if (!FMath::IsFinite(K) || K <= 0.0) return false;
		Accumulate(PartHeat, Candidate.ScratchNodeHeatW, Dt); Accumulate(ToWater, Candidate.LastHeatToCoolantW, Dt);
		Accumulate(ToAir, Candidate.LastHeatToAirW, Dt); Accumulate(ToAmbient, Candidate.LastHeatToAmbientW, Dt);
		Accumulate(SlotAir, Candidate.LastSlotHeatToAirW, Dt); Accumulate(SlotFlow, Candidate.LastSlotAirMassFlowKgs, Dt);
		Accumulate(FanPower, Candidate.LastSlotFanPowerW, Dt); Accumulate(Pressure, Candidate.LastSlotPressureRisePa, Dt);
		for (int32 S = 0; S < Params.NumSleds; ++S)
			SlotMassTemp[S] += Dt * Candidate.LastSlotAirMassFlowKgs[S] * Candidate.LastSledAirOutletTempK[S];
		TotalHeatJ += Dt * Candidate.LastTotalHeatW; LossJ += Dt * Candidate.LastConversionLossW;
		BoundaryJ += Dt * Candidate.LastHeatRemovedByCoolantW; LeakJ += Dt * Candidate.TrayCoolant.GetLastLeakHeatW();
		AirJ += Dt * Candidate.LastTotalHeatToRoomAirW;
		RemainingS -= Dt; ++Substeps;
	}
	// Coupling consumers multiply snapshot W/kg/s by the full requested dt, so return interval
	// means, not just the final substep's flux. Outlet air temperature is mass-time weighted.
	auto Mean = [DeltaTimeS](TArray<double>& Values) { for (double& V : Values) V /= DeltaTimeS; };
	for (int32 S = 0; S < Params.NumSleds; ++S)
		if (SlotFlow[S] > 0.0) Candidate.LastSledAirOutletTempK[S] = SlotMassTemp[S] / SlotFlow[S];
	Mean(PartHeat); Mean(ToWater); Mean(ToAir); Mean(ToAmbient); Mean(SlotAir); Mean(SlotFlow); Mean(FanPower); Mean(Pressure);
	Candidate.ScratchNodeHeatW = MoveTemp(PartHeat); Candidate.LastHeatToCoolantW = MoveTemp(ToWater);
	Candidate.LastHeatToAirW = MoveTemp(ToAir); Candidate.LastHeatToAmbientW = MoveTemp(ToAmbient);
	Candidate.LastSlotHeatToAirW = MoveTemp(SlotAir); Candidate.LastSlotAirMassFlowKgs = MoveTemp(SlotFlow);
	Candidate.LastSlotFanPowerW = MoveTemp(FanPower); Candidate.LastSlotPressureRisePa = MoveTemp(Pressure);
	Candidate.LastTotalHeatW = TotalHeatJ / DeltaTimeS; Candidate.LastConversionLossW = LossJ / DeltaTimeS;
	Candidate.LastHeatRemovedByCoolantW = BoundaryJ / DeltaTimeS; Candidate.LastTrayLeakHeatW = LeakJ / DeltaTimeS;
	Candidate.LastTotalHeatToRoomAirW = AirJ / DeltaTimeS;
	Candidate.LastTraySubsteps = Substeps; Candidate.LastTrayIntegratedTimeS = DeltaTimeS;
	*this = MoveTemp(Candidate);
	return true;
}
