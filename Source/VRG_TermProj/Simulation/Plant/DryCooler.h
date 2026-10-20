// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DryCooler.generated.h"

// S8 F2 (Tools/설비냉각_계획.md, 2026-10-20): air-cooled dry cooler, water coil in outdoor air.
//
// Crossflow, both fluids unmixed (fin-tube coil), effectiveness approximation of Incropera & DeWitt table 11.3:
//   eps = 1 - exp[(1 / Cr) NTU^0.22 (exp(-Cr NTU^0.78) - 1)],   NTU = UA / C_min,  Cr = C_min / C_max
//   HeatW = eps * C_min * (T_water_in - T_air_in)       (negative when the outdoor air is warmer: the coil heats the water)
// The conductance is derived once from a catalog rating point (the inverse of the formula above), and scales with
// the fan speed ratio r as UA(r) = UA_rated * r^n (air-side film coefficient ~ Re^n; n = 0.6 is the usual tube-bank
// exponent, ESTIMATE). Airflow ~ r. Fan electric input at the operating point of the same fan on the same coil
// (a fixed quadratic system curve): P(r) = P_rated * r^3 (D8: same equipment speed change, not a design comparison).
// Plain C++ (USTRUCT only for editable settings).

USTRUCT(BlueprintType)
struct FDryCoolerCatalog
{
	GENERATED_BODY()

	// Vertiv Liebert direct-drive drycooler, model 940, 60 Hz (Technical Design Manual 2023, table 2.1):
	// 938 kBtuh (275 kW) total heat rejection at 25 F ITD (EAT 35 C, EFT 48.9 C, 40 % ethylene glycol),
	// 240 gpm (910 L/min), 8 fans, rated motor 3/4 hp each, 43,400 CFM.
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedHeatW = 275.0e3;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedItdK = 25.0 * 5.0 / 9.0;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedAirInK = 308.15;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedAirFlowM3s = 43400.0 * 0.0004719474;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedFluidFlowM3s = 910.0 / 60000.0;
	// 40 % ethylene glycol near 40 C: ESTIMATE (not taken from a table here). The fluid side is C_max at the rating
	// point, so these only enter through Cr.
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedFluidDensityKgM3 = 1050.0;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double RatedFluidCpJKgK = 3580.0;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") int32 Fans = 8;
	// "Rated motor hp" is the motor's rated output, not its electric input (D8); input = output / motor efficiency.
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double FanRatedMotorOutputW = 0.75 * 745.7;
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double FanMotorEfficiency = 0.85;   // ESTIMATE
	UPROPERTY(EditAnywhere, Category = "Dry Cooler") double UaAirFlowExponent = 0.6;     // ESTIMATE (see above)
};

struct FDryCoolerParams
{
	double RatedUAWK = 0.0;
	double RatedAirFlowM3s = 0.0;
	double UaAirFlowExponent = 0.6;
	double RatedFanElectricW = 0.0;
};

namespace DryCooler
{
	// Outdoor air as an ideal gas at standard pressure (rho = p / (R T)); cp of dry air near 300-320 K.
	constexpr double AirPressurePa = 101325.0;
	constexpr double AirGasConstantJKgK = 287.05;
	constexpr double AirCpJKgK = 1006.0;
	FORCEINLINE double AirDensityKgM3(double AirK) { return AirPressurePa / (AirGasConstantJKgK * FMath::Max(AirK, 150.0)); }

	FORCEINLINE double CrossflowUnmixedEffectiveness(double Ntu, double Cr)
	{
		if (Ntu <= 0.0) return 0.0;
		Cr = FMath::Clamp(Cr, 0.0, 1.0);
		if (Cr < 1e-12) return 1.0 - FMath::Exp(-Ntu);
		return 1.0 - FMath::Exp((1.0 / Cr) * FMath::Pow(Ntu, 0.22) * (FMath::Exp(-Cr * FMath::Pow(Ntu, 0.78)) - 1.0));
	}

	// NTU reaching Eps at Cr (bisection; the effectiveness rises with NTU). Eps must be below the Cr limit.
	inline double NtuForEffectiveness(double Eps, double Cr)
	{
		double Lo = 0.0, Hi = 1.0;
		while (CrossflowUnmixedEffectiveness(Hi, Cr) < Eps && Hi < 1e3) Hi *= 2.0;
		for (int32 I = 0; I < 200; ++I)
		{
			const double Mid = 0.5 * (Lo + Hi);
			(CrossflowUnmixedEffectiveness(Mid, Cr) < Eps ? Lo : Hi) = Mid;
		}
		return 0.5 * (Lo + Hi);
	}

	// UA from the catalog rating point; false if the rating is not reachable by a crossflow coil.
	inline bool FromCatalog(const FDryCoolerCatalog& C, FDryCoolerParams& Out)
	{
		const double AirWK = C.RatedAirFlowM3s * AirDensityKgM3(C.RatedAirInK) * AirCpJKgK;
		const double FluidWK = C.RatedFluidFlowM3s * C.RatedFluidDensityKgM3 * C.RatedFluidCpJKgK;
		const double CMin = FMath::Min(AirWK, FluidWK), CMax = FMath::Max(AirWK, FluidWK);
		if (CMin <= 0.0 || C.RatedItdK <= 0.0) return false;
		const double Eps = C.RatedHeatW / (CMin * C.RatedItdK);
		const double Cr = CMin / CMax;
		if (!(Eps > 0.0) || Eps >= CrossflowUnmixedEffectiveness(1e3, Cr)) return false;
		Out.RatedUAWK = NtuForEffectiveness(Eps, Cr) * CMin;
		Out.RatedAirFlowM3s = C.RatedAirFlowM3s;
		Out.UaAirFlowExponent = C.UaAirFlowExponent;
		Out.RatedFanElectricW = C.Fans * C.FanRatedMotorOutputW / FMath::Max(C.FanMotorEfficiency, 0.05);
		return true;
	}

	// Heat from the water to the air of one unit [W] at fan speed ratio r.
	FORCEINLINE double HeatW(const FDryCoolerParams& P, double FanRatio, double WaterKgs, double WaterCpJKgK, double WaterInK, double AirInK)
	{
		const double R = FMath::Max(FanRatio, 0.0);
		const double AirWK = P.RatedAirFlowM3s * R * AirDensityKgM3(AirInK) * AirCpJKgK;
		const double WaterWK = FMath::Max(WaterKgs, 0.0) * WaterCpJKgK;
		const double CMin = FMath::Min(AirWK, WaterWK), CMax = FMath::Max(AirWK, WaterWK);
		if (CMin <= UE_DOUBLE_SMALL_NUMBER) return 0.0;   // D2: no flow on a side, nothing exchanged
		const double UA = P.RatedUAWK * FMath::Pow(R, P.UaAirFlowExponent);
		return CrossflowUnmixedEffectiveness(UA / CMin, CMin / CMax) * CMin * (WaterInK - AirInK);
	}

	FORCEINLINE double FanElectricW(const FDryCoolerParams& P, double FanRatio)
	{
		const double R = FMath::Max(FanRatio, 0.0);
		return P.RatedFanElectricW * R * R * R;
	}
}
