// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// Rack coolant (technology cooling system, TCS) properties - one source for every TCS model (rule: one place).
//
// 2026-11-03 (Tools/실사양정합_계획.md P2): GB200-class direct-to-chip loops run PG25 (25 vol% inhibited propylene
// glycol), not plain water: the OCP liquid cooling guideline's flow rule (1.25-2.0 L/min/kW, 1.5 at a 10 K rise) is
// stated for PG25, and Meta's GB200 and Vertiv's CDU 121 documents name PG25. Facility water (FWS / CHW / CW) stays
// water (FacilityPlant / CDU primary side).
//
// Values at 35 C, the middle of the TCS operating range here (27-46 C between the fixed-boundary and plant modes),
// from Interstate Chemical INTERCOOL DC-300 25% technical data sheet, "Physical Properties" table (25 vol% PG):
//   35 C: density 1025.2 kg/m^3, specific heat 3.90 kJ/kg K, viscosity 1.78 mPa s.
//   https://interstatechemical.com/assets/docs/INTERCOOL-DC-300-25-Tech-Bulletin.pdf
// Held constant over temperature (the table varies by -0.5 % density / +0.5 % cp / -21 % viscosity from 30 to 40 C):
// limitation. Electrical conductivity > 2000 uS/cm (same sheet): the leaked coolant is conductive (W4 basis).
// Surface tension: no sourced value was found for 25 vol% PG at 35 C. 0.055 N/m is an ESTIMATE (propylene glycol
// lowers water's 0.072 N/m; pure PG is about 0.036 N/m).
namespace TcsCoolant
{
	constexpr double DensityKgM3 = 1025.2;
	constexpr double CpJKgK = 3900.0;
	constexpr double ViscosityPaS = 1.78e-3;
	constexpr double SurfaceTensionNm = 0.055;   // ESTIMATE (see above)
}

// Facility water (FWS / CHW / CW, the CDU primary side): water at 25 C (handbook). One source for the plant side.
namespace FacilityWater
{
	constexpr double DensityKgM3 = 997.0;
	constexpr double CpJKgK = 4180.0;
}
