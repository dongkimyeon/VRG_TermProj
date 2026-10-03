#pragma once
#include "Actors/RackThermalActor.h"

// Structural/admissibility guards for serialized configuration, not fitted physics-error thresholds.
inline bool VrgValidRackSettings(const FRackLayoutSettings& S)
{
	const auto& P=S.Params; const auto& I=S.Input;
	if (S.RackSizeM.GetMin()<=0 || S.FixedStepS<=0 || S.PanelThicknessM<0 || S.SledPullOutM<0
		|| S.SledSlideSpeed<0 || S.ColorMinCelsius>=S.ColorMaxCelsius || S.Radiator.UAWK<0 || S.Radiator.AmbientTempK<=0
		|| P.CoolantHeatCapacityJK<=0 || P.CoolantCpJKgK<=0 || P.AirCpJKgK<=0 || P.AirDensityKgM3<=0
		|| I.Utilization<0 || I.Utilization>1 || I.FanSpeedOverride>1) return false;
	int32 Nodes=0;
	for (const auto& Slot:P.Slots)
	{
		if (!P.DeviceTypes.IsValidIndex(Slot.DeviceType) || Slot.BottomZM<0 || Slot.HeightM<=0) return false;
		Nodes+=P.DeviceTypes[Slot.DeviceType].Components.Num();
	}
	for (const auto& D:P.DeviceTypes)
	{
		const auto& F=D.Fans;
		if (D.DockedResistancePaS2Kg2<0 || F.NumFans<0 || F.MaxFlowM3s<0 || F.ShutoffPressurePa<0 || F.RatedInputW<0
			|| F.MinSpeedRatio<0 || F.MinSpeedRatio>1 || F.MaxSlewPerS<0
			|| (F.HasFans() && (F.ControlStartK<=0 || F.ControlFullK<=F.ControlStartK))) return false;
		for (const auto& C:D.Components)
			if (C.HeatCapacityJK<=0 || C.ToCoolantUAWK<0 || C.ToAirUAWK<0 || C.ToAmbientUAWK<0 || C.DefaultHeatW<0
				|| C.LossFractionOfRackLoad<0 || C.IdleHeatFraction<0 || C.IdleHeatFraction>1) return false;
	}
	if ((!I.ComponentHeatW.IsEmpty() && I.ComponentHeatW.Num()!=Nodes)
		|| (!I.SledDocked.IsEmpty() && I.SledDocked.Num()!=P.Slots.Num())
		|| (!I.SlotUtilization.IsEmpty() && I.SlotUtilization.Num()!=P.Slots.Num())) return false;
	for (double W:I.ComponentHeatW) if (W<0) return false;
	for (double U:I.SlotUtilization) if (U<0 || U>1) return false;
	return true;
}
