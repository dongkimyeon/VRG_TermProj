// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/AirStreamlines.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "Async/ParallelFor.h"

namespace
{
	bool InFluid(const FUniformAirGrid& G, const FVector& P, FAirStreamline::EStop& Why)
	{
		const double H = G.GetCellSizeM();
		const int32 X = FMath::FloorToInt32(P.X / H), Y = FMath::FloorToInt32(P.Y / H), Z = FMath::FloorToInt32(P.Z / H);
		if (!G.IsValidCell(X, Y, Z)) { Why = FAirStreamline::EStop::Outside; return false; }
		if (G.IsSolidCell(X, Y, Z)) { Why = FAirStreamline::EStop::Solid; return false; }
		return true;
	}

	float TemperatureAt(const FUniformAirGrid& G, const FVector& P)
	{
		const double H = G.GetCellSizeM();
		const int32 X = FMath::Clamp(FMath::FloorToInt32(P.X / H), 0, G.GetNumCellsX() - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32(P.Y / H), 0, G.GetNumCellsY() - 1);
		const int32 Z = FMath::Clamp(FMath::FloorToInt32(P.Z / H), 0, G.GetNumCellsZ() - 1);
		return float(G.TemperatureK[G.CellIndex(X, Y, Z)]);
	}
}

FAirStreamline AirStreamlines::Trace(const FUniformAirGrid& G, const FVector& SeedM, const FAirStreamlineSettings& S)
{
	FAirStreamline L;
	const double Sign = S.bBackward ? -1.0 : 1.0;
	// Unit direction of the field at P (zero where still).
	auto Dir = [&](const FVector& P, double& OutSpeed)
	{
		FVector U = G.SampleVelocityMs(P);
		if (S.bPlanar) U.Z = 0.0;
		OutSpeed = U.Size();
		return OutSpeed > S.MinSpeedMs ? U * (Sign / OutSpeed) : FVector::ZeroVector;
	};
	FVector P = SeedM;
	FAirStreamline::EStop Why = FAirStreamline::EStop::Length;
	if (!InFluid(G, P, Why)) { L.Stop = Why; return L; }
	const int32 MaxSteps = FMath::CeilToInt32(S.MaxLengthM / S.StepM);
	for (int32 I = 0; I <= MaxSteps; ++I)
	{
		double Speed = 0.0;
		const FVector K1 = Dir(P, Speed);
		L.PointsM.Add(P); L.SpeedMs.Add(float(Speed)); L.TemperatureK.Add(TemperatureAt(G, P));
		if (K1.IsZero()) { L.Stop = FAirStreamline::EStop::Still; return L; }
		double Unused = 0.0;
		const FVector K2 = Dir(P + K1 * (.5 * S.StepM), Unused);
		const FVector K3 = Dir(P + K2 * (.5 * S.StepM), Unused);
		const FVector K4 = Dir(P + K3 * S.StepM, Unused);
		const FVector Next = P + (K1 + 2.0 * K2 + 2.0 * K3 + K4) * (S.StepM / 6.0);
		if (!InFluid(G, Next, Why)) { L.Stop = Why; return L; }
		P = Next;
	}
	L.Stop = FAirStreamline::EStop::Length;
	return L;
}

TArray<FAirStreamline> AirStreamlines::TraceAll(const FUniformAirGrid& G, const TArray<FVector>& SeedsM, const FAirStreamlineSettings& S)
{
	TArray<FAirStreamline> Out;
	Out.SetNum(SeedsM.Num());
	ParallelFor(SeedsM.Num(), [&](int32 I) { Out[I] = Trace(G, SeedsM[I], S); });
	return Out;
}
