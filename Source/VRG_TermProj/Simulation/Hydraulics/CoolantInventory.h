// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "CoolantInventory.generated.h"

// S2b step 2 (Tools/S2b_누수_계획.md, 2026-10-20): expansion tank, coolant inventory and make-up water of a CDU loop.
//
// The CDU node (pump suction) is the loop's pressure reference. It is no longer an unlimited reservoir: an expansion
// tank with a gas cushion (diaphragm, isothermal ideal gas) sets its static gauge pressure from the water it holds.
//   P_abs * V_gas = P_precharge_abs * V_tank        (the cushion fills the whole tank at pre-charge)
//   P_gauge = P_abs - P_atm,   V_gas = V_tank - V_water
// The loop stays full of incompressible water while the tank holds water, so a leak drains the TANK:
//   dV_water/dt = -Q_leak + Q_makeup       (explicit Euler on the integrated clock, rule 4)
// and the falling tank pressure lowers the leak (the leak is never a fixed amount). Ledger, exact by construction:
//   V_water0 - V_water - sum(leak volume) + sum(make-up volume) = 0
// Tank empty = "depleted": gas would enter the loop. Two-phase behaviour is not modelled; the loop's pump is stopped
// (speed 0), its leaks are closed and the tank pressure is 0 Pa(g). Coolant flow to its racks drops to 0 (D2).
// Plain C++ with a UStruct only for the editable parameters; no actor access (rule 5).

// Editable tank parameters (on the CDU actor; the room actor keeps fallback copies). See the defaults' notes.
USTRUCT(BlueprintType)
struct FExpansionTankParams
{
	GENERATED_BODY()

	// Total tank volume [m^3]. ESTIMATE: no public CDU datasheet found states its expansion vessel volume (Vertiv
	// XDU450/1350 and Delta SuperCDU list expansion vessels without a size). 24 L is a placeholder of the right order.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory", meta = (ClampMin = "0.0001"))
	double TankVolumeM3 = 0.024;

	// Gas pre-charge below the fill pressure [Pa]. EN 12828 sizing relation (as given in expansion vessel guides,
	// e.g. Reflex / Flamco): initial (fill) pressure = pre-charge + 0.3 bar. That standard is for heating systems,
	// not data-centre CDUs, so the value is an ESTIMATE here. It sets the water reserve: V_tank * (1 - p0_abs / pa_abs).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory", meta = (ClampMin = "0.0"))
	double PrechargeBelowFillPa = 0.3e5;

	// Atmospheric pressure [Pa] (standard atmosphere) converting gauge <-> absolute for the gas law.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	double AtmosphericPressurePa = 101325.0;

	// Make-up water (auto fill). Off by default so a leak shows its consequence.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	bool bMakeupWater = false;

	// Make-up pump limit [m^3/s]. ESTIMATE (0.1 L/s = 6 L/min), not a product rating.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory", meta = (ClampMin = "0.0"))
	double MakeupMaxFlowM3s = 1.0e-4;
};

struct FExpansionTankState
{
	double WaterVolumeM3 = 0.0;       // water in the tank (the loop itself is always full until depleted)
	double LeakedM3 = 0.0;            // cumulative water lost through leaks
	double MakeupM3 = 0.0;            // cumulative make-up water added
	double InitialWaterM3 = 0.0;
	bool bDepleted = false;
	double DepletedAtS = -1.0;
};

namespace ExpansionTank
{
	// State at the fill pressure FillGaugePa (the CDU reference pressure). Pre-charge >= fill leaves an empty tank.
	FExpansionTankState Initial(const FExpansionTankParams& Params, double FillGaugePa);
	// P_precharge_abs * V_tank [Pa m^3]: the cushion's invariant.
	double GasInvariantPaM3(const FExpansionTankParams& Params, double FillGaugePa);
	double GaugePressurePa(const FExpansionTankParams& Params, double FillGaugePa, const FExpansionTankState& State);
	// One step: removes LeakM3s * Dt (never more than the tank holds plus this step's make-up), adds make-up up to its
	// limit while the pressure is below the fill pressure (it never fills past the fill pressure).
	void Advance(const FExpansionTankParams& Params, double FillGaugePa, FExpansionTankState& State,
		double LeakM3s, double DtS, double NowS);
}

// The tanks and leaks of one hydraulic network (one tank per CDU loop), stepped with that network.
class VRG_TERMPROJ_API FCoolantInventory
{
public:
	struct FTank
	{
		int32 CduNodeIndex = INDEX_NONE;
		int32 PumpIndex = INDEX_NONE;
		TArray<int32> LeakIndices;      // leaks of this loop
		FExpansionTankParams Params;
		double FillGaugePa = 0.0;       // CDU reference pressure: the tank pressure at fill
		double PumpSpeedRatio = 1.0;    // as built, restored on Restart
		TArray<double> LeakDiametersM;  // as built, restored on Restart (a depleted loop closes its leaks)
		FExpansionTankState State;
		double LastLeakM3s = 0.0, LastMakeupM3s = 0.0;
	};

	void Reset() { Tanks.Reset(); bDirty = false; }
	// Registers a loop after the network is built. Reads the CDU node pressure as the fill pressure.
	int32 AddTank(const FHydraulicNetwork& Network, int32 CduNodeIndex, int32 PumpIndex, const TArray<int32>& LeakIndices,
		const FExpansionTankParams& Params);
	// Back to the fill state: tanks full, pump speeds and leak sizes as built, CDU pressures at fill. Returns true when
	// the network had been changed and was solved again (OutResult then holds that solve).
	bool Restart(FHydraulicNetwork& Network, const FHydraulicSolveSettings& Settings, FHydraulicSolveResult& OutResult);
	// One integrated step [s] at NowS. Does nothing (returns false, no solve) while no loop has an open leak, an active
	// make-up or a pending change: without leaks every result stays bit-identical to the static network. Otherwise
	// sets each CDU node to its tank pressure, solves the network once, then advances every tank with that solve's
	// leak flows over DtS (the same flows the coupling reads for this step).
	bool Step(FHydraulicNetwork& Network, const FHydraulicSolveSettings& Settings, double DtS, double NowS, FHydraulicSolveResult& OutResult);

	bool IsActive(const FHydraulicNetwork& Network) const;
	// Opening, resizing or repairing the last hole must invalidate the previous solve,
	// including a closed loop without make-up (which otherwise becomes immediately idle).
	void MarkHydraulicsDirty() { bDirty = true; }
	const TArray<FTank>& GetTanks() const { return Tanks; }
	double GetTankGaugePressurePa(int32 Tank) const;
	int32 GetNumSolves() const { return NumSolves; }

private:
	TArray<FTank> Tanks;
	bool bDirty = false;       // a depletion changed the network: one more solve is due
	int32 NumSolves = 0;
	void ApplyBoundary(FHydraulicNetwork& Network) const;
};
