#include "Simulation/Water/RackInteriorWater.h"

void FRackInteriorWater::Initialize(const TArray<int32>& Counts, const TArray<double>& Heights)
{
	Start.Reset(); Start.Add(0);
	for (int32 Count : Counts) Start.Add(Start.Last() + Count);
	const int32 N = Start.Last(); check(Heights.Num() == N);
	Below.Init(INDEX_NONE, N); HeldM3.Init(0.0, N); TransitM3.Init(0.0, N);
	PassedM3.Init(0.0, N); RearExposureM3.Init(0.0, N); FirstWetS.Init(-1.0, N);
	RearTransitM3.Init(0.0, N);
	FloorM3.Init(0.0, Counts.Num()); RearFloorM3.Init(0.0, Counts.Num());
	LastFloorM3 = FloorM3; LastRearFloorM3 = FloorM3; ReceivedM3 = 0.0;
	for (int32 R = 0; R < Counts.Num(); ++R)
		for (int32 S = Start[R]; S < Start[R + 1]; ++S)
			for (int32 Other = Start[R]; Other < Start[R + 1]; ++Other)
				if (Heights[Other] < Heights[S] && (Below[S] == INDEX_NONE || Heights[Other] > Heights[Below[S]])) Below[S] = Other;
}

void FRackInteriorWater::AddWater(int32 Rack, int32 Slot, double VolumeM3, bool bRear, double NowS)
{
	if (Rack < 0 || Rack + 1 >= Start.Num() || Slot < 0 || Start[Rack] + Slot >= Start[Rack + 1] || !(VolumeM3 > 0.0)) return;
	const int32 I = Start[Rack] + Slot;
	ReceivedM3 += VolumeM3;
	(bRear ? RearTransitM3[I] : TransitM3[I]) += VolumeM3;
	PassedM3[I] += VolumeM3;
	if (bRear) RearExposureM3[I] += VolumeM3;
	if (FirstWetS[I] < 0.0) FirstWetS[I] = NowS;
}

void FRackInteriorWater::Step(double Dt, double NowS)
{
	check(Dt > 0.0 && DrainTimeS > 0.0 && CapacityM3 >= 0.0);
	NextHeldM3 = HeldM3; NextTransitM3.Init(0.0, HeldM3.Num()); NextRearTransitM3.Init(0.0, HeldM3.Num());
	LastFloorM3.Init(0.0, FloorM3.Num()); LastRearFloorM3.Init(0.0, FloorM3.Num());
	const double Fraction = 1.0 - FMath::Exp(-Dt / DrainTimeS);
	for (int32 R = 0; R + 1 < Start.Num(); ++R)
		for (int32 I = Start[R]; I < Start[R + 1]; ++I)
		{
			const double Incoming = TransitM3[I] + RearTransitM3[I];
			if (Incoming > 0.0 && FirstWetS[I] < 0.0) FirstWetS[I] = NowS;
			const double Captured = RearTransitM3[I] * RearCaptureFraction;
			const double Available = HeldM3[I] + TransitM3[I] + Captured;
			const double Excess = FMath::Max(0.0, Available - CapacityM3);
			const double Drained = Excess * Fraction;
			const double RearDrained = RearTransitM3[I] - Captured;
			NextHeldM3[I] = FMath::Min(Available, CapacityM3);
			NextTransitM3[I] += Excess - Drained;
			if (Below[I] != INDEX_NONE)
			{
				NextTransitM3[Below[I]] += Drained;
				NextRearTransitM3[Below[I]] += RearDrained;
				PassedM3[Below[I]] += Drained + RearDrained;
				RearExposureM3[Below[I]] += RearDrained;
				if (Drained + RearDrained > 0.0 && FirstWetS[Below[I]] < 0.0) FirstWetS[Below[I]] = NowS;
			}
			else { LastFloorM3[R] += Drained; LastRearFloorM3[R] += RearDrained; }
		}
	Swap(HeldM3, NextHeldM3); Swap(TransitM3, NextTransitM3); Swap(RearTransitM3, NextRearTransitM3);
	for (int32 R = 0; R < FloorM3.Num(); ++R) { FloorM3[R] += LastFloorM3[R]; RearFloorM3[R] += LastRearFloorM3[R]; }
}

double FRackInteriorWater::BalanceErrorM3() const
{
	double Stored = 0.0;
	for (int32 I = 0; I < HeldM3.Num(); ++I) Stored += HeldM3[I] + TransitM3[I] + RearTransitM3[I];
	for (int32 R = 0; R < FloorM3.Num(); ++R) Stored += FloorM3[R] + RearFloorM3[R];
	return ReceivedM3 - Stored;
}
