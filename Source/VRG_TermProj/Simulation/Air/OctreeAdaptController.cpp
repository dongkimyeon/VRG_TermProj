// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/OctreeAdaptController.h"

FString FOctreeAdaptSettings::Validate() const
{
	if (!(CoarsenGradientKPerM < RefineGradientKPerM)) return TEXT("CoarsenGradientKPerM must be below RefineGradientKPerM (hysteresis)");
	if (!(CoarsenSpeedMs < RefineSpeedMs)) return TEXT("CoarsenSpeedMs must be below RefineSpeedMs (hysteresis)");
	if (MinHoldS < 0.0 || IntervalS < 0.0 || MaxRefinedRoots < 0) return TEXT("negative hold time, interval or budget");
	if (TargetStepMs < 0.0 || LodBand <= 0.0 || LodBand >= 1.0) return TEXT("TargetStepMs must be >= 0 and LodBand in (0, 1)");
	return FString();
}

void FOctreeAdaptController::Reset(int32 NumRoots)
{
	Refined.Init(0, NumRoots);
	PendingRefined.Reset();
	bPending = false;
	LastChangeS.Init(TNumericLimits<double>::Lowest(), NumRoots);
	GradientKPerM.Init(0.0, NumRoots);
	SpeedMs.Init(0.0, NumRoots);
	LastEvalS = TNumericLimits<double>::Lowest();
	NumRefined = 0;
	NumTopologyChanges = NumRootFlips = 0;
	CostSumMs = 0.0; CostSamples = LastLeaves = 0;
	EffectiveBudget = MAX_int32; NumBudgetChanges = 0;
	LodHistory.Reset();
}

void FOctreeAdaptController::UpdateLodBudget(const FOctreeAdaptSettings& Settings, double NowS)
{
	const int32 Cap = Settings.MaxRefinedRoots > 0 ? Settings.MaxRefinedRoots : MAX_int32;
	if (Settings.TargetStepMs <= 0.0)
	{
		EffectiveBudget = Cap;
		return;
	}
	if (CostSamples == 0) return;
	const double MeanMs = CostSumMs / CostSamples;
	CostSumMs = 0.0; CostSamples = 0;
	const int32 Before = EffectiveBudget;
	if (MeanMs > Settings.TargetStepMs * (1.0 + Settings.LodBand))
	{
		EffectiveBudget = FMath::FloorToInt32(.8 * NumRefined);
	}
	else if (MeanMs < Settings.TargetStepMs * (1.0 - Settings.LodBand) && EffectiveBudget < Cap)
	{
		const int64 Grown = int64(FMath::CeilToInt32(1.25 * EffectiveBudget)) + 1;
		EffectiveBudget = int32(FMath::Min<int64>(Grown, Cap));
	}
	EffectiveBudget = FMath::Min(EffectiveBudget, Cap);
	if (EffectiveBudget != Before) ++NumBudgetChanges;
	LodHistory.Add({NowS, LastLeaves, MeanMs, EffectiveBudget});
}

void FOctreeAdaptController::MeasureIndicators(const FUniformAirGrid& Grid)
{
	const double H = Grid.GetCellSizeM();
	const int32 NX = Grid.GetNumCellsX(), NY = Grid.GetNumCellsY(), NZ = Grid.GetNumCellsZ();
	for (int32 Z = 0; Z < NZ; ++Z)
		for (int32 Y = 0; Y < NY; ++Y)
			for (int32 X = 0; X < NX; ++X)
			{
				const int32 I = Grid.CellIndex(X, Y, Z);
				GradientKPerM[I] = SpeedMs[I] = 0.0;
				if (!Grid.IsFluidCell(X, Y, Z)) continue;
				const double T = Grid.TemperatureK[I];
				double SumSq = 0.0;
				const FIntVector Step[3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
				for (const FIntVector& D : Step)
				{
					const bool bPlus = Grid.IsFluidCell(X + D.X, Y + D.Y, Z + D.Z);
					const bool bMinus = Grid.IsFluidCell(X - D.X, Y - D.Y, Z - D.Z);
					const double TP = bPlus ? Grid.TemperatureK[Grid.CellIndex(X + D.X, Y + D.Y, Z + D.Z)] : T;
					const double TM = bMinus ? Grid.TemperatureK[Grid.CellIndex(X - D.X, Y - D.Y, Z - D.Z)] : T;
					const int32 Span = (bPlus ? 1 : 0) + (bMinus ? 1 : 0);
					const double G = Span > 0 ? (TP - TM) / (Span * H) : 0.0;
					SumSq += G * G;
				}
				GradientKPerM[I] = FMath::Sqrt(SumSq);
				const FVector U(
					.5 * (Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] + Grid.VelocityXMs[Grid.FaceXIndex(X + 1, Y, Z)]),
					.5 * (Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)] + Grid.VelocityYMs[Grid.FaceYIndex(X, Y + 1, Z)]),
					.5 * (Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] + Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z + 1)]));
				SpeedMs[I] = U.Size();
			}
}

bool FOctreeAdaptController::Update(const FUniformAirGrid& Grid, const FOctreeAdaptSettings& Settings, double NowS, TArray<FBox>& OutBoxesM)
{
	const int32 N = Grid.GetNumCells();
	if (Refined.Num() != N)
	{
		// First use (or a new grid): size the arrays but keep the step costs reported before this call.
		const double KeptSumMs = CostSumMs; const int32 KeptSamples = CostSamples, KeptLeaves = LastLeaves;
		Reset(N);
		CostSumMs = KeptSumMs; CostSamples = KeptSamples; LastLeaves = KeptLeaves;
	}
	if (NowS < LastEvalS + Settings.IntervalS) return false;
	LastEvalS = NowS;
	UpdateLodBudget(Settings, NowS);
	MeasureIndicators(Grid);

	TArray<uint8> Desired = Refined;
	TArray<double> Score; Score.Init(0.0, N);
	for (int32 I = 0; I < N; ++I)
	{
		const double G = GradientKPerM[I], S = SpeedMs[I];
		Score[I] = FMath::Max(Settings.RefineGradientKPerM > 0.0 ? G / Settings.RefineGradientKPerM : 0.0,
			Settings.RefineSpeedMs > 0.0 ? S / Settings.RefineSpeedMs : 0.0);
		bool bWant = Refined[I] != 0;
		if (!bWant && (G > Settings.RefineGradientKPerM || S > Settings.RefineSpeedMs)) bWant = true;
		else if (bWant && G < Settings.CoarsenGradientKPerM && S < Settings.CoarsenSpeedMs) bWant = false;
		// Hold time: a fresh state is kept for MinHoldS.
		if (bWant != (Refined[I] != 0) && NowS - LastChangeS[I] < Settings.MinHoldS) bWant = Refined[I] != 0;
		Desired[I] = bWant ? 1 : 0;
	}

	// Budget (hard): keep the largest normalised indicators. MaxRefinedRoots, tightened by the S5-E LOD budget.
	const int32 Budget = FMath::Min(Settings.MaxRefinedRoots > 0 ? Settings.MaxRefinedRoots : MAX_int32, EffectiveBudget);
	if (Budget < MAX_int32)
	{
		TArray<int32> On;
		for (int32 I = 0; I < N; ++I) if (Desired[I]) On.Add(I);
		if (On.Num() > Budget)
		{
			On.Sort([&](int32 A, int32 B) { return Score[A] != Score[B] ? Score[A] > Score[B] : A < B; });
			for (int32 K = Budget; K < On.Num(); ++K) Desired[On[K]] = 0;
		}
	}

	bPending = false;
	if (Desired == Refined) return false;
	PendingRefined = MoveTemp(Desired);
	PendingS = NowS;
	bPending = true;
	MakeBoxes(Grid, PendingRefined, OutBoxesM);
	return true;
}

void FOctreeAdaptController::Commit()
{
	if (!bPending) return;
	int32 Flips = 0;
	NumRefined = 0;
	for (int32 I = 0; I < Refined.Num(); ++I)
	{
		if (PendingRefined[I] != Refined[I]) { ++Flips; LastChangeS[I] = PendingS; }
		NumRefined += PendingRefined[I];
	}
	Refined = PendingRefined;
	++NumTopologyChanges;
	NumRootFlips += Flips;
	bPending = false;
}

void FOctreeAdaptController::MakeBoxes(const FUniformAirGrid& Grid, const TArray<uint8>& Flags, TArray<FBox>& OutBoxesM)
{
	// Boxes on cell faces overlap only the flagged roots with positive volume (neighbours touch a face only).
	OutBoxesM.Reset();
	const double H = Grid.GetCellSizeM();
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!Flags[Grid.CellIndex(X, Y, Z)]) continue;
				int32 End = X;
				while (End + 1 < Grid.GetNumCellsX() && Flags[Grid.CellIndex(End + 1, Y, Z)]) ++End;
				OutBoxesM.Add(FBox(FVector(X * H, Y * H, Z * H), FVector((End + 1) * H, (Y + 1) * H, (Z + 1) * H)));
				X = End;
			}
}
