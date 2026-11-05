// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// S2b step 5 (Tools/S2b_누수_계획.md, 2026-10-20): fault judgement from computed results only (D4).
// No topology table, no hard-coded overheating order or time: a rack is flagged when its own coolant flow from the
// hydraulic solution falls below a share of its design flow, when its hottest computed component passes a limit,
// when its loop's coolant inventory is depleted, or when the hydraulic solve failed. The first time of every
// (rack, kind) pair is recorded on the integrated clock (rule 4). The monitor also keeps the leak / valve record
// and tank history for the evaluation report. Plain C++, game thread, no actors (rule 5).

// WaterTrip/WaterDamage (W4): first slot of a rack that water tripped/damaged. Value = that slot index.
enum class EVrgFaultKind : uint8 { LowCoolantFlow, OverTemperature, InventoryDepleted, HydraulicNotConverged, WaterTrip, WaterDamage };

struct VRG_TERMPROJ_API FVrgFaultSettings
{
	// Display / judgement setting, not a physical constant: flagged below this share of the design flow.
	double LowFlowRatio = 0.5;
	// ESTIMATE: component temperature limit [K] (85 C). Public maximum junction temperatures for the GB200
	// parts were not found; this is a generic limit for flagging, not a vendor specification.
	double MaxComponentTempK = 358.15;
};

struct VRG_TERMPROJ_API FVrgFaultRecord
{
	double TimeS = 0.0;
	int32 Rack = INDEX_NONE;     // routed order; INDEX_NONE for the whole network (hydraulic failure)
	EVrgFaultKind Kind = EVrgFaultKind::LowCoolantFlow;
	double Value = 0.0;          // flow ratio [-], temperature [K], tank row, or normalized residual
	bool operator==(const FVrgFaultRecord& O) const { return TimeS == O.TimeS && Rack == O.Rack && Kind == O.Kind && Value == O.Value; }
};

struct VRG_TERMPROJ_API FVrgLeakRecord
{
	double TimeS = 0.0; int32 Rack = INDEX_NONE; bool bReturnBranch = false; double HoleDiameterM = 0.0;
	// W1 live leak: which site (label) and where [m]; empty / zero for the S2b rack-branch events.
	FString Site; FVector PointM = FVector::ZeroVector;
	bool operator==(const FVrgLeakRecord& O) const { return TimeS == O.TimeS && Rack == O.Rack && bReturnBranch == O.bReturnBranch && HoleDiameterM == O.HoleDiameterM && Site == O.Site && PointM == O.PointM; }
};

struct VRG_TERMPROJ_API FVrgValveRecord
{
	double TimeS = 0.0; int32 Rack = INDEX_NONE; bool bClosed = false;
	TArray<int32> IsolatedRacks, ImpactedRacks;
	bool operator==(const FVrgValveRecord& O) const { return TimeS == O.TimeS && Rack == O.Rack && bClosed == O.bClosed && IsolatedRacks == O.IsolatedRacks && ImpactedRacks == O.ImpactedRacks; }
};

struct VRG_TERMPROJ_API FVrgTankSample
{
	double TimeS = 0.0; int32 Row = INDEX_NONE; double PressurePa = 0.0; double WaterM3 = 0.0; double LeakedM3 = 0.0; bool bDepleted = false;
	bool operator==(const FVrgTankSample& O) const { return TimeS == O.TimeS && Row == O.Row && PressurePa == O.PressurePa && WaterM3 == O.WaterM3 && LeakedM3 == O.LeakedM3 && bDepleted == O.bDepleted; }
};

// W4: one state change of one target from the water it touched (any equipment; rack targets are per slot).
struct VRG_TERMPROJ_API FVrgWaterImpactRecord
{
	double TimeS = 0.0; FString EquipmentId; int32 Rack = INDEX_NONE, Slot = INDEX_NONE; FString Device;
	FString From, To, Cause;
	double ImmersionM = 0.0, SprayM3 = 0.0, InteriorM3 = 0.0;
	// The power cut this state caused, when one was applied: its time and the whole-site IT heat over that shared step.
	double CutAppliedS = -1.0, ItBeforeW = 0.0, ItAfterW = 0.0;
	bool operator==(const FVrgWaterImpactRecord& O) const { return TimeS == O.TimeS && EquipmentId == O.EquipmentId && Rack == O.Rack && Slot == O.Slot && Device == O.Device && From == O.From && To == O.To && Cause == O.Cause && ImmersionM == O.ImmersionM && SprayM3 == O.SprayM3 && InteriorM3 == O.InteriorM3 && CutAppliedS == O.CutAppliedS && ItBeforeW == O.ItBeforeW && ItAfterW == O.ItAfterW; }
};

// One step's computed values, per rack in routed order.
struct VRG_TERMPROJ_API FVrgFaultInput
{
	double TimeS = 0.0;
	TArray<double> CoolantMassFlowKgs, DesignMassFlowKgs, MaxComponentTempK;
	TArray<bool> LoopDepleted;
	bool bHydraulicsConverged = true;
	double HydraulicResidual = 0.0;
};

class VRG_TERMPROJ_API FVrgFaultMonitor
{
public:
	void Reset(const FVrgFaultSettings& InSettings);
	// Adds the first occurrence of each (rack, kind); returns how many new faults this step produced.
	int32 Update(const FVrgFaultInput& In);
	void AddLeak(const FVrgLeakRecord& R) { Leaks.Add(R); }
	void AddValve(const FVrgValveRecord& R) { Valves.Add(R); }
	void AddTankSample(const FVrgTankSample& S) { TankSamples.Add(S); }
	// Keeps the record; a rack slot reaching tripped/damaged also adds the rack's first WaterTrip/WaterDamage fault. Returns its index.
	int32 AddWaterImpact(const FVrgWaterImpactRecord& R);
	void SetWaterImpactCut(int32 Index, double AppliedS, double ItBeforeW, double ItAfterW);

	const FVrgFaultSettings& GetSettings() const { return Settings; }
	const TArray<FVrgFaultRecord>& GetFaults() const { return Faults; }
	const TArray<FVrgLeakRecord>& GetLeaks() const { return Leaks; }
	const TArray<FVrgValveRecord>& GetValves() const { return Valves; }
	const TArray<FVrgTankSample>& GetTankSamples() const { return TankSamples; }
	const TArray<FVrgWaterImpactRecord>& GetWaterImpacts() const { return WaterImpacts; }
	// First fault time of any kind for a rack, or < 0 if none.
	double GetFirstFaultTimeS(int32 Rack) const;

	static const TCHAR* KindName(EVrgFaultKind Kind);
	TSharedRef<FJsonObject> ToJsonObject() const;
	// Reads what ToJsonObject wrote; an absent object (older report) gives an empty monitor and true.
	static bool FromJsonObject(const TSharedPtr<FJsonObject>& Object, FVrgFaultMonitor& Out);
	bool operator==(const FVrgFaultMonitor& O) const;

private:
	FVrgFaultSettings Settings;
	TArray<FVrgFaultRecord> Faults;
	TSet<uint32> Seen;   // (rack + 1) * 8 + kind
	TArray<FVrgLeakRecord> Leaks;
	TArray<FVrgValveRecord> Valves;
	TArray<FVrgTankSample> TankSamples;
	TArray<FVrgWaterImpactRecord> WaterImpacts;
	void Record(double TimeS, int32 Rack, EVrgFaultKind Kind, double Value, int32& InOutNew);
};
