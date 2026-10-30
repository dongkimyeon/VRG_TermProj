#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Thermal/RackThermalModel.h"

bool FCouplingBridge::PrepareTrayTransport()
{
	bool bHasInternal = false;
	for (const auto& Link : Links) bHasInternal |= !Link.Internal.Slots.IsEmpty();
	if (!bHasInternal) return true;
	if (!HydraulicNetwork) return false;
	// W3.5 measured hotspot: per-node queries repeatedly scanned every pipe in the room.
	// Recompute once for this solved state/step, never cache across a hydraulic change.
	TArray<double> NodeImbalancesM3s;
	HydraulicNetwork->GetNodeImbalancesM3s(NodeImbalancesM3s);
	for (int32 R = 0; R < Targets.Num(); ++R)
	{
		if (Links[R].Internal.Slots.IsEmpty()) continue;
		if (!HydraulicNetwork || !Targets[R].Model || !Targets[R].Input || !Targets[R].Model->UsesTrayCoolant()
			|| !InRackTransport::Build(*HydraulicNetwork, Links[R].Internal, Links[R].ReturnPipeIndex,
				CoolantDensityKgM3, Targets[R].Model->GetParams().CoolantCpJKgK, TrayInputs[R], &NodeImbalancesM3s)) return false;
	}
	return true;
}

void FCouplingBridge::Step(double DtS, bool bResolved, bool bConverged)
{
	LastCoupledSubsteps = 0;
	if (!(DtS > 0.0) || !FMath::IsFinite(DtS)) return;
	if (!ensureMsgf(PrepareTrayTransport(), TEXT("Invalid solved rack transport; coupled step rejected"))) return;
	bool bHasInternal = false;
	for (const auto& L : Links) bHasInternal |= !L.Internal.Slots.IsEmpty();
	if (!bHasInternal) { StepOnce(DtS, bResolved, bConverged); LastCoupledSubsteps = 1; return; }

	// A common explicit step lets HX rejection, local leaks and tray exchanges all read
	// the same old temperatures. Letting individual racks subcycle under a frozen HX
	// would silently create a global energy residual. Air advances over this same interval.
	double Remaining = DtS, ItJ = 0.0, LossJ = 0.0, FanJ = 0.0, LeakJ = 0.0, PumpJ = 0.0, RejectedJ = 0.0, AirJ = 0.0;
	double AirMs = 0.0, RackMs = 0.0;
	TArray<double> DutyJ; DutyJ.Init(0.0, CduGroups.Num());
	TArray<double> InternalLeakJ; InternalLeakJ.Init(0.0, LastInternalLeakHeatW.Num());
	while (Remaining > 0.0)
	{
		double StepS = Remaining;
		for (int32 R = 0; R < Targets.Num(); ++R)
			if (!Links[R].Internal.Slots.IsEmpty()) StepS = FMath::Min(StepS, Targets[R].Model->LimitTrayStepS(*Targets[R].Input, TrayInputs[R], Remaining));
		if (!ensureMsgf(StepS > 0.0 && Remaining - StepS != Remaining, TEXT("No stable coupled interval"))) return;
		StepOnce(StepS, bResolved && LastCoupledSubsteps == 0, bConverged);
		ItJ += LastItHeatW * StepS; LossJ += LastConversionLossW * StepS; FanJ += LastFanPowerW * StepS;
		LeakJ += LastLeakHeatW * StepS; PumpJ += LastCduPumpHeatW * StepS;
		RejectedJ += LastRadiatorRejectedW * StepS; AirJ += LastAirCoolingRemovedW * StepS;
		AirMs += LastAirSolverMs; RackMs += LastRackModelsMs;
		for (int32 G = 0; G < DutyJ.Num(); ++G) DutyJ[G] += LastCduDutyW[G] * StepS;
		for (int32 K = 0; K < InternalLeakJ.Num(); ++K) InternalLeakJ[K] += LastInternalLeakHeatW[K] * StepS;
		Remaining -= StepS; ++LastCoupledSubsteps;
	}
	LastItHeatW = ItJ / DtS; LastConversionLossW = LossJ / DtS; LastFanPowerW = FanJ / DtS;
	LastLeakHeatW = LeakJ / DtS; LastCduPumpHeatW = PumpJ / DtS;
	LastRadiatorRejectedW = RejectedJ / DtS; LastAirCoolingRemovedW = AirJ / DtS;
	LastAirSolverMs = AirMs; LastRackModelsMs = RackMs;
	for (int32 G = 0; G < DutyJ.Num(); ++G) LastCduDutyW[G] = DutyJ[G] / DtS;
	for (int32 K = 0; K < InternalLeakJ.Num(); ++K) LastInternalLeakHeatW[K] = InternalLeakJ[K] / DtS;
}
