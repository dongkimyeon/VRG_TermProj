// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/AirFlowAverager.h"

void FAirFlowAverager::Reset()
{
	Blocks.Reset();
	Open = FBlock();
	Dims = FIntVector::ZeroValue;
	CoveredS = 0.0;
}

void FAirFlowAverager::StartOpen(const FUniformAirGrid& G)
{
	Open.Vx.Init(0.0, G.VelocityXMs.Num()); Open.Vy.Init(0.0, G.VelocityYMs.Num()); Open.Vz.Init(0.0, G.VelocityZMs.Num());
	Open.T.Init(0.0, G.TemperatureK.Num()); Open.DurationS = 0.0;
}

void FAirFlowAverager::Add(const FUniformAirGrid& G, double DtS)
{
	if (DtS <= 0.0) return;
	const FIntVector Now(G.GetNumCellsX(), G.GetNumCellsY(), G.GetNumCellsZ());
	if (Now != Dims || Open.T.Num() != G.TemperatureK.Num()) { Reset(); Dims = Now; StartOpen(G); }
	auto Acc = [DtS](TArray<double>& To, const TArray<double>& From) { for (int32 I = 0; I < From.Num(); ++I) To[I] += From[I] * DtS; };
	Acc(Open.Vx, G.VelocityXMs); Acc(Open.Vy, G.VelocityYMs); Acc(Open.Vz, G.VelocityZMs); Acc(Open.T, G.TemperatureK);
	Open.DurationS += DtS;
	// Close the block when it is full (a hair under BlockS counts: the clock's steps do not sum exactly).
	if (Open.DurationS >= BlockS * (1.0 - 1e-9))
	{
		Blocks.Add(MoveTemp(Open));
		CoveredS += Blocks.Last().DurationS;
		StartOpen(G);
		while (Blocks.Num() > 1 && CoveredS - Blocks[0].DurationS >= WindowS * (1.0 - 1e-9))
		{
			CoveredS -= Blocks[0].DurationS;
			Blocks.RemoveAt(0);
		}
	}
}

bool FAirFlowAverager::Build(const FUniformAirGrid& Like, FUniformAirGrid& Out) const
{
	if (Blocks.IsEmpty() || Blocks[0].T.Num() != Like.TemperatureK.Num()) return false;
	Out.Initialize(Like.GetParams());
	Out.bSolidCell = Like.bSolidCell;
	double Total = 0.0;
	for (const FBlock& B : Blocks) Total += B.DurationS;
	auto Mean = [&](TArray<double>& To, TArray<double> FBlock::* Field)
	{
		To.Init(0.0, (Blocks[0].*Field).Num());
		for (const FBlock& B : Blocks) { const TArray<double>& F = B.*Field; for (int32 I = 0; I < F.Num(); ++I) To[I] += F[I]; }
		for (double& V : To) V /= Total;
	};
	Mean(Out.VelocityXMs, &FBlock::Vx); Mean(Out.VelocityYMs, &FBlock::Vy); Mean(Out.VelocityZMs, &FBlock::Vz); Mean(Out.TemperatureK, &FBlock::T);
	return true;
}
