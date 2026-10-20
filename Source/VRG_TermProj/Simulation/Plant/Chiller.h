// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Chiller.generated.h"

// S8 F3 (Tools/설비냉각_계획.md, 2026-10-20): electric water-cooled chiller, DOE-2 form (EnergyPlus Chiller:Electric:EIR).
//
//   x = leaving chilled water temperature [C], y = entering condenser water temperature [C]
//   Capacity  = Cap_ref * CAPFT(x, y)                      (biquadratic, inputs held at the curve limits outside them)
//   Power     = Capacity * EIR_ref * EIRFT_eff(x, y) * EIRFPLR(PLR),   PLR = load / capacity
//   Q_cond    = Q_evap + Power                             (all compressor work to the condenser, computed once, rule 6)
//
// Inside the curve range EIRFT_eff is the dataset curve exactly. Outside it (this model's 18 C chilled water and dry
// cooler condensing are both outside the public curves) the temperature effect is extrapolated with the Carnot ratio
// from the nearest in-range point (decision 2026-10-20, ESTIMATE):
//   EIRFT_eff(x, y) = EIRFT(xc, yc) * COPc(xc, yc) / COPc(x, y),   COPc = (x + 273.15) / (y - x)
// and the resulting COP never exceeds the best COP the curve itself reaches inside its range (no extrapolation to
// better performance than the dataset demonstrates; low lift is where the economizer of F4 takes over).
// Below the minimum part load ratio the chiller cycles: power scales with the load at the minimum-PLR EIR.
// Plain C++ (USTRUCT only for editable settings).

USTRUCT(BlueprintType)
struct FChillerCurves
{
	GENERATED_BODY()

	// EnergyPlus datasets/Chillers.idf, "ElectricEIRChiller Carrier 19XR 1558kW/5.81COP/VSD" (centrifugal, R-134a, VSD),
	// coefficients copied from the file (2026-10-20): reference 1557.8 kW, COP 5.81 at 5.56 C leaving chilled water and
	// 21.11 C entering condenser water; chilled / condenser reference flows 0.03785 / 0.05142 m^3/s.
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefCapacityW = 1557800.0;
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefCop = 5.81;
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefLeavingChilledC = 5.56;
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefEnteringCondenserC = 21.11;
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefChilledFlowM3s = 0.03785;
	UPROPERTY(EditAnywhere, Category = "Chiller") double RefCondenserFlowM3s = 0.05142;
	UPROPERTY(EditAnywhere, Category = "Chiller") TArray<double> CapFt = { 1.034307E+00, 2.390635E-02, -1.222578E-03, -5.346220E-03, -2.183258E-04, 6.282331E-04 };
	UPROPERTY(EditAnywhere, Category = "Chiller") TArray<double> EirFt = { 1.099343E+00, -3.132676E-02, 4.108521E-04, -7.377845E-03, 5.775048E-04, -3.528472E-04 };
	UPROPERTY(EditAnywhere, Category = "Chiller") TArray<double> EirFplr = { 1.620909E-01, -2.553890E-01, 1.092232E+00 };
	UPROPERTY(EditAnywhere, Category = "Chiller") double MinX = 5.56;
	UPROPERTY(EditAnywhere, Category = "Chiller") double MaxX = 10.00;
	UPROPERTY(EditAnywhere, Category = "Chiller") double MinY = 11.94;
	UPROPERTY(EditAnywhere, Category = "Chiller") double MaxY = 23.89;
	UPROPERTY(EditAnywhere, Category = "Chiller") double MinPlr = 0.19;
	UPROPERTY(EditAnywhere, Category = "Chiller") double MaxPlr = 1.01;
};

struct FChillerOperatingPoint
{
	double LoadW = 0.0;           // evaporator heat removed from the chilled water
	double PowerW = 0.0;          // compressor electric input
	double CondenserW = 0.0;      // = LoadW + PowerW
	double CapacityW = 0.0;
	double Plr = 0.0;
	double LeavingChilledK = 0.0;
	bool bCapacityLimited = false;   // could not reach the setpoint
	bool bOutsideCurves = false;     // x or y outside the dataset range (extrapolated)
	double Cop() const { return PowerW > 0.0 ? LoadW / PowerW : 0.0; }
};

namespace Chiller
{
	FORCEINLINE double Biquadratic(const TArray<double>& C, double X, double Y)
	{
		return C.Num() < 6 ? 1.0 : C[0] + C[1] * X + C[2] * X * X + C[3] * Y + C[4] * Y * Y + C[5] * X * Y;
	}
	FORCEINLINE double Quadratic(const TArray<double>& C, double X)
	{
		return C.Num() < 3 ? 1.0 : C[0] + C[1] * X + C[2] * X * X;
	}
	FORCEINLINE double CarnotCop(double XC, double YC)
	{
		const double Lift = YC - XC;
		return Lift > 1e-6 ? (XC + 273.15) / Lift : TNumericLimits<double>::Max();
	}

	// Best full-load COP (1 / (EIR_ref * EIRFT)) the curve reaches inside its own range (grid search): the cap of the
	// temperature extrapolation. The part-load curve still applies on top (a VSD chiller is better at part load).
	inline double MaxCopInRange(const FChillerCurves& K)
	{
		double Best = 0.0;
		for (int32 I = 0; I <= 40; ++I)
			for (int32 J = 0; J <= 40; ++J)
			{
				const double X = K.MinX + (K.MaxX - K.MinX) * I / 40.0, Y = K.MinY + (K.MaxY - K.MinY) * J / 40.0;
				const double Eir = Biquadratic(K.EirFt, X, Y) / K.RefCop;   // the full-load temperature part (EIRFPLR applies on top)
				if (Eir > 0.0) Best = FMath::Max(Best, 1.0 / Eir);
			}
		return Best;
	}

	// Full-load EIR (= 1/COP at PLR 1 before EIRFPLR) at (x, y) [C], with the Carnot extrapolation outside the range.
	inline double FullLoadEir(const FChillerCurves& K, double X, double Y, double MaxCop, bool& bOutside)
	{
		const double Xc = FMath::Clamp(X, K.MinX, K.MaxX), Yc = FMath::Clamp(Y, K.MinY, K.MaxY);
		bOutside = Xc != X || Yc != Y;
		double Eir = Biquadratic(K.EirFt, Xc, Yc) / K.RefCop;
		if (bOutside)
		{
			const double CopIn = CarnotCop(Xc, Yc), CopOut = CarnotCop(X, Y);
			if (CopIn < TNumericLimits<double>::Max() && CopOut > 0.0) Eir *= CopIn / FMath::Min(CopOut, 1e6);
		}
		return MaxCop > 0.0 ? FMath::Max(Eir, 1.0 / MaxCop) : Eir;
	}

	FORCEINLINE double CapacityW(const FChillerCurves& K, double CapRefW, double X, double Y)
	{
		return CapRefW * FMath::Max(Biquadratic(K.CapFt, FMath::Clamp(X, K.MinX, K.MaxX), FMath::Clamp(Y, K.MinY, K.MaxY)), 0.0);
	}

	// One chiller: chilled water enters at InK with WaterWK = m cp; the setpoint is the leaving temperature it tries to
	// reach; condenser water enters at CondInK. CapRefW is the sized reference capacity (curves are normalized).
	inline FChillerOperatingPoint Evaluate(const FChillerCurves& K, double CapRefW, double MaxCop, double InK, double WaterWK,
		double SetpointK, double CondInK)
	{
		FChillerOperatingPoint P;
		P.LeavingChilledK = InK;
		const double Request = WaterWK > 0.0 ? WaterWK * (InK - SetpointK) : 0.0;
		if (Request <= 0.0 || CapRefW <= 0.0) return P;   // nothing to do (D2: no flow, no division)
		const double Y = CondInK - 273.15;
		double X = SetpointK - 273.15;
		double Cap = CapacityW(K, CapRefW, X, Y);
		double Load = FMath::Min(Request, Cap);
		// Capacity limited: the leaving temperature rises, which changes the capacity a little; settle by iteration.
		for (int32 I = 0; I < 8 && Load < Request; ++I)
		{
			X = (InK - Load / WaterWK) - 273.15;
			Cap = CapacityW(K, CapRefW, X, Y);
			Load = FMath::Min(Request, Cap);
		}
		P.bCapacityLimited = Load < Request;
		P.LoadW = Load;
		P.CapacityW = Cap;
		P.LeavingChilledK = InK - Load / WaterWK;
		X = P.LeavingChilledK - 273.15;
		bool bOutside = false;
		const double Eir = FullLoadEir(K, X, Y, MaxCop, bOutside);
		P.bOutsideCurves = bOutside;
		P.Plr = Cap > 0.0 ? Load / Cap : 0.0;
		const double PlrUsed = FMath::Clamp(P.Plr, K.MinPlr, K.MaxPlr);
		const double Cycling = P.Plr < K.MinPlr && K.MinPlr > 0.0 ? P.Plr / K.MinPlr : 1.0;
		P.PowerW = Cap * Eir * Quadratic(K.EirFplr, PlrUsed) * Cycling;
		P.CondenserW = P.LoadW + P.PowerW;
		return P;
	}
}
