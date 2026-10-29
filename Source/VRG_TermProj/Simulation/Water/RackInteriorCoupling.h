#pragma once
#include "Simulation/Water/RackInteriorWater.h"
class FFloorWater;

// Setup indices only; runtime water/exposure fields remain flattened SoA in FRackInteriorWater.
struct FRackWaterSite
{
	int32 Leak = INDEX_NONE, Rack = INDEX_NONE, Slot = INDEX_NONE;
	bool bRear = false;
};

// W3.5: consumes the inventory's cumulative m3 once. No hydraulic, heat or actor access.
class FRackInteriorCoupling
{
public:
	bool Initialize(const TArray<int32>& Counts, const TArray<double>& WorldHeightsM, const TArray<FRackWaterSite>& Sites);
	bool Step(const TMap<int32, double>& CumulativeM3, double DtS, double NowS);
	const FRackInteriorWater& GetWater() const { return Water; }
	// Rejected/unmapped finite input stays in the upstream ledger, never silently consumed.
	double PendingInputM3(const TMap<int32, double>& CumulativeM3) const;
	// Acknowledge ONLY what W3 accepted; failed destinations remain queued for retry.
	void TransferToFloor(FFloorWater& Floor, const TArray<FVector>& FrontM, const TArray<FVector>& RearM);
	const TArray<double>& GetFloorDeliveredM3(bool bRear) const { return bRear ? RearDeliveredM3 : DeliveredM3; }
	double PendingFloorM3() const;
private:
	FRackInteriorWater Water;
	TArray<FRackWaterSite> Sites;
	TArray<double> ConsumedM3;
	TArray<double> DeliveredM3, RearDeliveredM3;
};
