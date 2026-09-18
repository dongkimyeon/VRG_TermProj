// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Core/SimClock.h"

FSimClock::FSimClock(double InFixedStepS)
{
	Reset(InFixedStepS);
}

void FSimClock::Reset(double InFixedStepS)
{
	FixedStepS = FMath::Max(InFixedStepS, UE_DOUBLE_SMALL_NUMBER);
	SimTimeS = 0.0;
	AccumulatorS = 0.0;
	DiscardedTimeS = 0.0;
	bClampedLastFrame = false;
}

void FSimClock::SetMaxSubStepsPerFrame(int32 InMaxSubSteps)
{
	MaxSubStepsPerFrame = FMath::Max(1, InMaxSubSteps);
}

int32 FSimClock::BeginFrame(double RealDeltaS)
{
	bClampedLastFrame = false;

	if (RealDeltaS > 0.0)
	{
		AccumulatorS += RealDeltaS;
	}

	int32 NumSteps = static_cast<int32>(AccumulatorS / FixedStepS);

	if (NumSteps > MaxSubStepsPerFrame)
	{
		const int32 SkippedSteps = NumSteps - MaxSubStepsPerFrame;
		DiscardedTimeS += SkippedSteps * FixedStepS;
		AccumulatorS -= SkippedSteps * FixedStepS;
		NumSteps = MaxSubStepsPerFrame;
		bClampedLastFrame = true;
	}

	return NumSteps;
}

void FSimClock::CompleteStep()
{
	AccumulatorS -= FixedStepS;
	SimTimeS += FixedStepS;
}
