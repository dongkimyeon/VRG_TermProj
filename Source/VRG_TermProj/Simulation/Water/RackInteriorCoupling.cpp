#include "Simulation/Water/RackInteriorCoupling.h"
#include "Simulation/Water/FloorWater.h"

bool FRackInteriorCoupling::Initialize(const TArray<int32>& Counts, const TArray<double>& Heights, const TArray<FRackWaterSite>& InSites)
{
	int64 Total = 0;
	for (int32 Count : Counts) { if (Count < 0) return false; Total += Count; }
	if (Total != Heights.Num()) return false;
	for (double H : Heights) if (!FMath::IsFinite(H)) return false;
	TSet<int32> Unique;
	for (const auto& S : InSites)
	{
		if (S.Leak < 0 || Unique.Contains(S.Leak) || !Counts.IsValidIndex(S.Rack) || S.Slot < 0 || S.Slot >= Counts[S.Rack]) return false;
		Unique.Add(S.Leak);
	}
	FRackInteriorCoupling Candidate;
	Candidate.Water.Initialize(Counts, Heights); Candidate.Sites = InSites;
	Candidate.ConsumedM3.Init(0.0, InSites.Num());
	Candidate.DeliveredM3.Init(0.0, Counts.Num()); Candidate.RearDeliveredM3 = Candidate.DeliveredM3;
	*this = MoveTemp(Candidate);
	return true;
}

bool FRackInteriorCoupling::Step(const TMap<int32, double>& Cumulative, double DtS, double NowS)
{
	if (!FMath::IsFinite(DtS) || DtS <= 0.0 || !FMath::IsFinite(NowS) || !FMath::IsFinite(NowS + DtS)
		|| !FMath::IsFinite(Water.CapacityM3) || Water.CapacityM3 < 0.0 || !FMath::IsFinite(Water.DrainTimeS) || Water.DrainTimeS <= 0.0
		|| !FMath::IsFinite(Water.RearCaptureFraction) || Water.RearCaptureFraction < 0.0 || Water.RearCaptureFraction > 1.0) return false;
	for (const auto& Entry : Cumulative)
		if (!FMath::IsFinite(Entry.Value) || Entry.Value < 0.0 || (Entry.Value > 0.0
			&& !Sites.ContainsByPredicate([&](const FRackWaterSite& S) { return S.Leak == Entry.Key; }))) return false;
	for (int32 S = 0; S < Sites.Num(); ++S) if (Cumulative.FindRef(Sites[S].Leak) < ConsumedM3[S]) return false;
	// Validate the entire delta batch before mutating any water or consumption baseline.
	FRackInteriorCoupling Candidate = *this;
	for (int32 S = 0; S < Sites.Num(); ++S)
	{
		const double Current = Cumulative.FindRef(Sites[S].Leak);
		const auto& Site = Sites[S];
		Candidate.Water.AddWater(Site.Rack, Site.Slot, Current - ConsumedM3[S], Site.bRear, NowS);
		Candidate.ConsumedM3[S] = Current;
	}
	// Explicit interval source at its start; arrivals at lower slots are timestamped at its end.
	Candidate.Water.Step(DtS, NowS + DtS);
	*this = MoveTemp(Candidate);
	return true;
}

double FRackInteriorCoupling::PendingInputM3(const TMap<int32, double>& Cumulative) const
{
	double Total = 0.0;
	for (const auto& E : Cumulative) if (FMath::IsFinite(E.Value) && E.Value > 0.0) Total += E.Value;
	return FMath::Max(0.0, Total - Water.ReceivedM3);
}

void FRackInteriorCoupling::TransferToFloor(FFloorWater& Floor, const TArray<FVector>& Front, const TArray<FVector>& Rear)
{
	if (Front.Num() != Water.FloorM3.Num() || Rear.Num() != Water.FloorM3.Num()) return;
	for (int32 R = 0; R < Water.FloorM3.Num(); ++R)
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const double Arrived = Side == 0 ? Water.FloorM3[R] : Water.RearFloorM3[R];
			double& Delivered = Side == 0 ? DeliveredM3[R] : RearDeliveredM3[R];
			const FVector& Point = Side == 0 ? Front[R] : Rear[R];
			const double Pending = Arrived - Delivered;
			if (Pending > 0.0 && FMath::IsFinite(Pending) && !Point.ContainsNaN() && Floor.AddWater(Point, Pending))
				Delivered = Arrived;
		}
}

double FRackInteriorCoupling::PendingFloorM3() const
{
	double Pending = 0.0;
	for (int32 R = 0; R < Water.FloorM3.Num(); ++R)
		Pending += Water.FloorM3[R] - DeliveredM3[R] + Water.RearFloorM3[R] - RearDeliveredM3[R];
	return Pending;
}
