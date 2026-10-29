// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Sensors/VirtualSensor.h"

// 2026-10-22 (user request): instruments on the pipes and the equipment, and leak detection along the pipes.
//
// D9: every value here is a *simulated measurement* - the model's value seen through a sensor's response time,
// sampling period, noise and resolution (FVirtualSensor, one per channel). All instrument specifications are
// ESTIMATES for generic devices (immersion RTD, ultrasonic flow meter, VFD speed feedback, power meter, pressure
// transmitter, spot leak detector), not a product's datasheet.
//
// Leak detection (D4: from the solved leak flows, never from a topology table): a pipe point carries a leak-detection
// cable section of CoverRadiusM around it. The water a covered leak lets out (its solved flow integrated over time; a
// flow under the detection flow is taken to evaporate before it spreads) collects there; once WettingVolumeM3 has come
// out the cable is wet - a large leak is found sooner than a weep - and the alarm latches (water on the floor does not
// dry by itself, and a real detector stays in alarm until reset). The radius is a game abstraction of where the water
// goes, not a physical range (as the D9 sensing radius).
//
// Plain C++ in SI units (K, m^3/s, Pa, W, ratio); positions in metres (rule 1). Game thread only (rule 5).

enum class EInstrumentQuantity : uint8 { TemperatureK, FlowM3s, PressurePa, Ratio, PowerW, Cop };

struct FInstrumentChannelSpec
{
	FName Name;                      // what the channel reads ("in", "out", "flow", "fan", ...)
	EInstrumentQuantity Quantity = EInstrumentQuantity::TemperatureK;
	FVirtualSensorSpec Sensor;       // the alarm threshold is unused (+inf)
	double LowCutoff = 0.0;          // |reading| below it reads 0 (a flow meter's low-flow cutoff)
};

// Instrument specs per quantity (ESTIMATES). Scale: the channel's expected magnitude, for relative noise / resolution.
namespace InstrumentSpecs
{
	VRG_TERMPROJ_API FInstrumentChannelSpec Make(FName Name, EInstrumentQuantity Quantity, double Scale);
}

struct FLeakCableSpec
{
	double DetectFlowM3s = 1.0e-6;   // 1 mL/s: below it the water evaporates before it spreads (ESTIMATE)
	double WettingVolumeM3 = 0.5e-3; // 0.5 L out before it has spread from the pipe to the cable below (ESTIMATE)
	double SamplePeriodS = 1.0;      // detector polling (ESTIMATE)
};

enum class EInstrumentSite : uint8 { RoomPipe, PlantPipe, PlantUnit, Cdu, RackDripTray, RackManifoldRope };

struct FWaterContactSpec
{
	// ESTIMATES for this virtual installation, NOT a TT1000 product rating:
	// 30 mL accumulated across the bottom plane; rope: any positive local rear contact.
	// Conceptual distributed water-contact basis (no volume/time claim in this source):
	// https://www.nvent.com/sites/default/files/acquiadam_assets/2021-05/RAYCHEMTraceTek-DS-H53870-TT1000-EN.pdf
	double WettingVolumeM3 = 30.0e-6;
	double SamplePeriodS = 1.0; // reuse the existing generic detector polling assumption
	double ResponseDelayS = 1.0; // ESTIMATE: fixed wet-contact debounce, then first poll
};

namespace InstrumentSpecs
{
	VRG_TERMPROJ_API const TCHAR* SiteCode(EInstrumentSite Site);
}

struct FInstrumentPoint
{
	EInstrumentSite Site = EInstrumentSite::PlantPipe;
	FString Label;                   // display name (e.g. "FWS 공급 메인", "냉수 칠러 2")
	FString Id, RackId;              // stable rack identity + sensor suffix, no physical state
	bool bWaterContact = false;      // reads cumulative model contact, never the Leaks/radius path
	FWaterContactSpec Contact;
	FVector CoverageStartM = FVector::ZeroVector, CoverageEndM = FVector::ZeroVector;
	FVector PositionM = FVector::ZeroVector;
	int32 Key = INDEX_NONE;          // the owner's index (pipe edge, unit, CDU group) to fetch its true values
	int32 Loop = INDEX_NONE;         // owner's network (room = -1, plant loops 0..2)
	TArray<FInstrumentChannelSpec> Channels;
	bool bLeakCable = false;
	double CoverRadiusM = 0.0;
};

// A leak as the solve has it now: position and outflow.
struct FInstrumentLeak { FVector PositionM = FVector::ZeroVector; double FlowM3s = 0.0; };

struct FLeakAlarm
{
	int32 Point = INDEX_NONE;
	double TimeS = 0.0, FlowM3s = 0.0;
	double WaterM3 = 0.0, FirstContactS = -1.0, ThresholdS = -1.0;
};

class VRG_TERMPROJ_API FInstrumentBank
{
public:
	void Reset(const FLeakCableSpec& InLeakSpec = FLeakCableSpec());
	// Adds a point; its channels start at InitialValues (one per channel, model values).
	int32 AddPoint(const FInstrumentPoint& Point, const TArray<double>& InitialValues);
	int32 Num() const { return Points.Num(); }
	const FInstrumentPoint& GetPoint(int32 I) const { return Points[I].Def; }

	// One coupled step ending at SimTimeS: TrueValues[point][channel] are the model's values over the step (NaN keeps a
	// channel unread this step), Leaks the solved leaks. Returns the leak alarms raised in this step.
	TArray<FLeakAlarm> Advance(double SimTimeS, double DtS, const TArray<TArray<double>>& TrueValues, const TArray<FInstrumentLeak>& Leaks,
		const TArray<double>& ContactCumulativeM3 = {});

	bool HasReading(int32 PointIndex, int32 Channel) const;
	double GetReading(int32 PointIndex, int32 Channel) const;
	bool IsLeakAlarm(int32 PointIndex) const { return Points.IsValidIndex(PointIndex) && Points[PointIndex].bLeakAlarm; }
	double GetLeakAlarmTimeS(int32 PointIndex) const { return Points.IsValidIndex(PointIndex) ? Points[PointIndex].LeakAlarmS : -1.0; }
	double GetContactWaterM3(int32 PointIndex) const { return Points.IsValidIndex(PointIndex) ? Points[PointIndex].WaterM3 : 0.0; }
	const TArray<FLeakAlarm>& GetAlarmLog() const { return AlarmLog; }
	int32 GetNumLeakCables() const;

private:
	struct FState
	{
		FInstrumentPoint Def;
		TArray<FVirtualSensor> Sensors;
		TArray<bool> Primed;   // a channel with no model value at the build starts from its first one
		double WaterM3 = 0.0, NextPollS = 0.0, LeakAlarmS = -1.0;
		double FirstContactS = -1.0, ThresholdS = -1.0;
		bool bLeakAlarm = false;
	};
	FLeakCableSpec LeakSpec;
	TArray<FState> Points;
	TArray<FLeakAlarm> AlarmLog;
};
