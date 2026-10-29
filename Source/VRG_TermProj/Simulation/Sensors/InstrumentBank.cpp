// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Sensors/InstrumentBank.h"

const TCHAR* InstrumentSpecs::SiteCode(EInstrumentSite Site)
{
	switch (Site)
	{
	case EInstrumentSite::RoomPipe: return TEXT("room_pipe");
	case EInstrumentSite::PlantPipe: return TEXT("plant_pipe");
	case EInstrumentSite::PlantUnit: return TEXT("plant_unit");
	case EInstrumentSite::Cdu: return TEXT("cdu");
	case EInstrumentSite::RackDripTray: return TEXT("rack_drip_tray");
	case EInstrumentSite::RackManifoldRope: return TEXT("rack_manifold_rope");
	default: return TEXT("unknown");
	}
}

FInstrumentChannelSpec InstrumentSpecs::Make(FName Name, EInstrumentQuantity Quantity, double Scale)
{
	// ESTIMATES for generic devices (no product datasheet); the alarm of FVirtualSensor is not used here.
	FInstrumentChannelSpec C;
	C.Name = Name;
	C.Quantity = Quantity;
	FVirtualSensorSpec& S = C.Sensor;
	S.AlarmThresholdK = TNumericLimits<double>::Max();
	const double M = FMath::Max(FMath::Abs(Scale), 1e-12);
	switch (Quantity)
	{
	case EInstrumentQuantity::TemperatureK:   // immersion RTD in a thermowell
		S.SamplePeriodS = 1.0; S.TimeConstantS = 5.0; S.NoiseStdK = 0.05; S.ResolutionK = 0.1; break;
	case EInstrumentQuantity::FlowM3s:        // ultrasonic / magnetic flow meter, +-0.5 % of the expected flow
		S.SamplePeriodS = 1.0; S.TimeConstantS = 1.0; S.NoiseStdK = 0.005 * M; S.ResolutionK = 1.0e-5;
		C.LowCutoff = 0.01 * M;   // low-flow cutoff 1 % of the expected flow: a shut branch reads 0, not noise
		break;
	case EInstrumentQuantity::PressurePa:     // pressure transmitter, 0.25 % of the expected value, 0.1 kPa
		S.SamplePeriodS = 1.0; S.TimeConstantS = 0.5; S.NoiseStdK = 0.0025 * M; S.ResolutionK = 100.0; break;
	case EInstrumentQuantity::Ratio:          // VFD speed feedback, 0.1 %
		S.SamplePeriodS = 1.0; S.TimeConstantS = 0.0; S.NoiseStdK = 0.0; S.ResolutionK = 0.001; break;
	case EInstrumentQuantity::PowerW:         // power meter, 0.5 % of the expected value, 10 W
		S.SamplePeriodS = 1.0; S.TimeConstantS = 1.0; S.NoiseStdK = 0.005 * M; S.ResolutionK = 10.0; break;
	case EInstrumentQuantity::Cop:            // derived from the plant's own heat and power meters
		S.SamplePeriodS = 1.0; S.TimeConstantS = 5.0; S.NoiseStdK = 0.0; S.ResolutionK = 0.01; break;
	}
	return C;
}

void FInstrumentBank::Reset(const FLeakCableSpec& InLeakSpec)
{
	LeakSpec = InLeakSpec;
	Points.Reset();
	AlarmLog.Reset();
}

int32 FInstrumentBank::AddPoint(const FInstrumentPoint& Point, const TArray<double>& InitialValues)
{
	FState& S = Points.AddDefaulted_GetRef();
	S.Def = Point;
	S.NextPollS = FMath::Max(Point.bWaterContact ? Point.Contact.SamplePeriodS : LeakSpec.SamplePeriodS, 1e-9);
	for (int32 C = 0; C < Point.Channels.Num(); ++C)
	{
		FVirtualSensor& V = S.Sensors.AddDefaulted_GetRef();
		const bool bKnown = InitialValues.IsValidIndex(C) && FMath::IsFinite(InitialValues[C]);
		// A fixed seed per channel: a rerun gives the same noise (D9).
		V.Reset(Point.Channels[C].Sensor, bKnown ? InitialValues[C] : 0.0, 7919 * (Points.Num() - 1) + C + 1);
		S.Primed.Add(bKnown);
	}
	return Points.Num() - 1;
}

TArray<FLeakAlarm> FInstrumentBank::Advance(double SimTimeS, double DtS, const TArray<TArray<double>>& TrueValues, const TArray<FInstrumentLeak>& Leaks,
	const TArray<double>& ContactCumulativeM3)
{
	TArray<FLeakAlarm> Raised;
	if (!(DtS > 0.0)) return Raised;
	for (int32 P = 0; P < Points.Num(); ++P)
	{
		FState& S = Points[P];
		if (TrueValues.IsValidIndex(P))
			for (int32 C = 0; C < S.Sensors.Num(); ++C)
				if (TrueValues[P].IsValidIndex(C) && FMath::IsFinite(TrueValues[P][C]))
				{
					if (!S.Primed[C])
					{
						// The element starts at the first model value (no response from an arbitrary 0), same seed.
						S.Sensors[C].Reset(S.Def.Channels[C].Sensor, TrueValues[P][C], 7919 * P + C + 1);
						S.Primed[C] = true;
					}
					S.Sensors[C].Advance(SimTimeS, DtS, TrueValues[P][C]);
				}
		if (S.Def.bWaterContact)
		{
			const FWaterContactSpec& Spec = S.Def.Contact;
			// Absent/invalid/regressing data cannot clear a latch or invent contact.
			if (!ContactCumulativeM3.IsValidIndex(P) || !FMath::IsFinite(ContactCumulativeM3[P]) || ContactCumulativeM3[P] < S.WaterM3
				|| !FMath::IsFinite(Spec.WettingVolumeM3) || Spec.WettingVolumeM3 < 0.0
				|| !FMath::IsFinite(Spec.ResponseDelayS) || Spec.ResponseDelayS < 0.0 || !FMath::IsFinite(Spec.SamplePeriodS) || Spec.SamplePeriodS <= 0.0) continue;
			S.WaterM3 = ContactCumulativeM3[P]; // cumulative model value, NOT integrated a second time
			if (S.WaterM3 > 0.0 && S.FirstContactS < 0.0) S.FirstContactS = SimTimeS;
			const bool bWet = S.WaterM3 > 0.0 && S.WaterM3 >= Spec.WettingVolumeM3;
			if (bWet && S.ThresholdS < 0.0) S.ThresholdS = SimTimeS;
			if (SimTimeS + 1e-9 < S.NextPollS) continue;
			while (S.NextPollS <= SimTimeS + 1e-9) S.NextPollS += Spec.SamplePeriodS;
			if (!S.bLeakAlarm && bWet && SimTimeS + 1e-9 >= S.ThresholdS + Spec.ResponseDelayS)
			{
				S.bLeakAlarm = true; S.LeakAlarmS = SimTimeS;
				Raised.Add({P, SimTimeS, 0.0, S.WaterM3, S.FirstContactS, S.ThresholdS});
			}
			continue;
		}
		if (!S.Def.bLeakCable) continue;
		// The water the covered leaks let out over the step collects at the cable (it does not dry by itself).
		double Covered = 0.0;
		for (const FInstrumentLeak& L : Leaks)
			if (L.FlowM3s > LeakSpec.DetectFlowM3s && FVector::Dist(L.PositionM, S.Def.PositionM) <= S.Def.CoverRadiusM) Covered += L.FlowM3s;
		S.WaterM3 += Covered * DtS;
		if (SimTimeS + 1e-9 < S.NextPollS) continue;
		while (S.NextPollS <= SimTimeS + 1e-9) S.NextPollS += FMath::Max(LeakSpec.SamplePeriodS, 1e-9);
		if (!S.bLeakAlarm && S.WaterM3 >= LeakSpec.WettingVolumeM3 * (1.0 - 1e-9))
		{
			S.bLeakAlarm = true;   // latched
			S.LeakAlarmS = SimTimeS;
			Raised.Add({ P, SimTimeS, Covered });
		}
	}
	AlarmLog.Append(Raised);
	return Raised;
}

bool FInstrumentBank::HasReading(int32 PointIndex, int32 Channel) const
{
	return Points.IsValidIndex(PointIndex) && Points[PointIndex].Sensors.IsValidIndex(Channel) && Points[PointIndex].Sensors[Channel].HasReading();
}

double FInstrumentBank::GetReading(int32 PointIndex, int32 Channel) const
{
	if (!HasReading(PointIndex, Channel)) return 0.0;
	const double R = Points[PointIndex].Sensors[Channel].GetLastReadingK();
	return FMath::Abs(R) < Points[PointIndex].Def.Channels[Channel].LowCutoff ? 0.0 : R;
}

int32 FInstrumentBank::GetNumLeakCables() const
{
	int32 N = 0;
	for (const FState& S : Points) N += S.Def.bLeakCable ? 1 : 0;
	return N;
}
