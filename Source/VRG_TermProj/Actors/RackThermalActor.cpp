// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

ARackThermalActor::ARackThermalActor()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ARackThermalActor::BeginPlay()
{
	Super::BeginPlay();
	ResetSim();
	StartSim();
}

void ARackThermalActor::ConformInputArrays()
{
	const int32 NumSleds = FMath::Max(1, Params.NumSleds);

	if (Input.ComponentHeatW.Num() != NumSleds * NumRackComponents)
	{
		Input.ComponentHeatW = FRackThermalModel::MakeUniformHeatW(NumSleds);
	}

	if (Input.SledDocked.Num() != NumSleds)
	{
		Input.SledDocked.Init(true, NumSleds);
	}
}

void ARackThermalActor::StartSim()
{
	if (!Model.IsStepStable(Input, FixedStepS))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("RackThermalActor: FixedStepS=%.4f s가 명시적 오일러 단조 조건을 위반합니다. 결과를 믿지 마세요."),
			FixedStepS);
	}
	bSimRunning = true;
}

void ARackThermalActor::PauseSim()
{
	bSimRunning = false;
}

void ARackThermalActor::ResetSim()
{
	bSimRunning = false;
	ConformInputArrays();
	Clock.Reset(FixedStepS);
	Model.Initialize(Params, InitialState);
}

FRackThermalSnapshot ARackThermalActor::GetSnapshot() const
{
	return Model.MakeSnapshot(Clock.GetSimTimeS());
}

void ARackThermalActor::SetSledDocked(int32 SledIndex, bool bDocked)
{
	if (Input.SledDocked.IsValidIndex(SledIndex))
	{
		Input.SledDocked[SledIndex] = bDocked;
	}
}

bool ARackThermalActor::IsSledDocked(int32 SledIndex) const
{
	return Input.SledDocked.IsValidIndex(SledIndex) ? Input.SledDocked[SledIndex] : false;
}

void ARackThermalActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bSimRunning)
	{
		const int32 NumSteps = Clock.BeginFrame(DeltaTime);
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			Model.Step(Input, Clock.GetFixedStepS());
			Clock.CompleteStep();
		}
	}

	if (bShowReadout)
	{
		DrawReadout();
	}
}

void ARackThermalActor::DrawReadout() const
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const FRackThermalSnapshot Snapshot = GetSnapshot();

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const FVector HalfExtentCm = SimUnits::MetersToUnreal(RackSizeM) * 0.5;
	const FVector CenterCm = GetActorLocation() + FVector(0.0, 0.0, HalfExtentCm.Z);
	DrawDebugBox(World, CenterCm, HalfExtentCm, FColor::Cyan, false, -1.0f, 0, 2.0f);

	if (GEngine == nullptr)
	{
		return;
	}

	// 가장 뜨거운 부품을 찾아 같이 보여준다. 어느 유닛의 어느 부품인지가 중요하다.
	int32 HottestIndex = INDEX_NONE;
	double HottestTempK = 0.0;
	for (int32 Index = 0; Index < Snapshot.ComponentTempK.Num(); ++Index)
	{
		if (HottestIndex == INDEX_NONE || Snapshot.ComponentTempK[Index] > HottestTempK)
		{
			HottestIndex = Index;
			HottestTempK = Snapshot.ComponentTempK[Index];
		}
	}

	FString HottestLabel = TEXT("-");
	if (HottestIndex != INDEX_NONE)
	{
		const int32 SledIndex = HottestIndex / NumRackComponents;
		const int32 ComponentIndex = HottestIndex % NumRackComponents;
		HottestLabel = FString::Printf(TEXT("U%d %s %.1f C"),
			SledIndex,
			*Model.GetParams().Components[ComponentIndex].Name.ToString(),
			SimUnits::KelvinToCelsius(HottestTempK));
	}

	int32 NumDocked = 0;
	for (const bool bDocked : Input.SledDocked)
	{
		NumDocked += bDocked ? 1 : 0;
	}

	// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
	const FString Readout = FString::Printf(
		TEXT("[%s] t=%.1f s | 유닛 %d/%d 도킹 | 최고 %s | 냉각수 출구 %.2f C | 배기 %.2f C")
		TEXT(" | 냉각수 %.0f W + 공기 %.0f W + 자연대류 %.0f W = %.0f W (발열 %.0f W) | 정규화 잔차 %.2e%s"),
		*GetName(),
		Snapshot.SimTimeS,
		NumDocked, Params.NumSleds,
		*HottestLabel,
		SimUnits::KelvinToCelsius(Snapshot.CoolantOutletTempK),
		SimUnits::KelvinToCelsius(Snapshot.MixedAirOutletTempK),
		Snapshot.HeatRemovedByCoolantW,
		Snapshot.TotalHeatToAirW,
		Snapshot.TotalHeatToAmbientW,
		Snapshot.HeatRemovedByCoolantW + Snapshot.TotalHeatToAirW + Snapshot.TotalHeatToAmbientW,
		Snapshot.TotalHeatW,
		Snapshot.NormalizedEnergyResidual,
		Snapshot.bStepStable ? TEXT("") : TEXT(" [불안정]"));

	// 같은 키로 덮어써서 프레임마다 쌓이지 않게 한다.
	GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor::Yellow, Readout);
}
