// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// CDU liquid-to-liquid heat exchanger (2026-09-27): the rack coolant (technology cooling system, TCS) of one row
// against facility water (FWS). Replaces one radiator per rack, which could not show a CDU's capacity limit.
//
// The racks of a row share the CDU: their return streams mix (flow-weighted), pass the exchanger once, and every
// rack of the row gets the same supply temperature back. Counterflow epsilon-NTU with finite capacity on both
// sides, so the duty saturates at C_min * (T_return - T_facility) however large UA gets:
//   C_hot = sum(mdot_i) * cp,  C_fac = mdot_fac * cp_fac,  NTU = UA / C_min,  Cr = C_min / C_max
//   eps = (1 - exp(-NTU (1 - Cr))) / (1 - Cr exp(-NTU (1 - Cr)))     (Cr -> 1: eps = NTU / (1 + NTU))
//   Duty = eps * C_min * (T_mix - T_fac_in),  T_supply = T_mix - Duty / C_hot
// Rule 6: the duty and the supply temperature come from the same eps in one place, so the heat the racks hand to
// the coolant, sum(mdot_i cp (T_out_i - T_supply)) = C_hot (T_mix - T_supply), equals the duty exactly.
// D2: no flow on either side -> no duty, no division by a zero capacity rate.
// Plain struct, no UObject: usable from worker threads.
struct FCduHeatExchangerParams
{
	double UAWK = 0.0;                 // exchanger conductance [W/K]
	double FacilitySupplyTempK = 295.15;
	double FacilityMassFlowKgs = 0.0;
	double FacilityCpJKgK = 4180.0;    // water
};

namespace CduHeatExchanger
{
	FORCEINLINE double CounterflowEffectiveness(double Ntu, double Cr)
	{
		if (Ntu <= 0.0) return 0.0;
		Cr = FMath::Clamp(Cr, 0.0, 1.0);
		if (Cr > 1.0 - 1e-9) return Ntu / (1.0 + Ntu);
		const double E = FMath::Exp(-Ntu * (1.0 - Cr));
		return (1.0 - E) / (1.0 - Cr * E);
	}

	// Duty [W] for a hot stream of capacity HotCapacityWK at MixTempK. Negative if the facility water is warmer.
	FORCEINLINE double DutyW(const FCduHeatExchangerParams& Hx, double HotCapacityWK, double MixTempK)
	{
		const double FacilityWK = Hx.FacilityMassFlowKgs * Hx.FacilityCpJKgK;
		const double CMin = FMath::Min(HotCapacityWK, FacilityWK), CMax = FMath::Max(HotCapacityWK, FacilityWK);
		if (CMin <= UE_DOUBLE_SMALL_NUMBER || Hx.UAWK <= 0.0) return 0.0;
		return CounterflowEffectiveness(Hx.UAWK / CMin, CMin / CMax) * CMin * (MixTempK - Hx.FacilitySupplyTempK);
	}

	// Counterflow NTU that reaches effectiveness Eps at capacity ratio Cr (inverse of the above; for sizing).
	FORCEINLINE double NtuForEffectiveness(double Eps, double Cr)
	{
		Eps = FMath::Clamp(Eps, 0.0, 1.0 - 1e-9);
		Cr = FMath::Clamp(Cr, 0.0, 1.0);
		if (Cr > 1.0 - 1e-9) return Eps / (1.0 - Eps);
		return FMath::Loge((1.0 - Eps * Cr) / (1.0 - Eps)) / (1.0 - Cr);
	}
}
