// Fill out your copyright notice in the Description page of Project Settings.

#include "HeatFieldDebugActor.h"
#include "DrawDebugHelpers.h"

AHeatFieldDebugActor::AHeatFieldDebugActor()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AHeatFieldDebugActor::BeginPlay()
{
	Super::BeginPlay();
	InitializeField();
	StartSim();
}

void AHeatFieldDebugActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bSimRunning)
	{
		AccumulatedTimeS += DeltaTime;
		while (AccumulatedTimeS >= TimeStepS)
		{
			StepSimulation(TimeStepS);
			AccumulatedTimeS -= TimeStepS;
		}
	}

	DrawSlice();
}

void AHeatFieldDebugActor::InitializeField()
{
	const int32 NumCells = GridSizeX * GridSizeY * GridSizeZ;
	TemperatureBufferA.Init(InitialTemperatureK, NumCells);
	TemperatureBufferB.Init(InitialTemperatureK, NumCells);
	bBufferAIsCurrent = true;
	AccumulatedTimeS = 0.0f;
}

bool AHeatFieldDebugActor::CheckStability() const
{
	const float Lambda = ThermalDiffusivityM2s * TimeStepS / (CellSizeM * CellSizeM);
	const bool bStable = Lambda <= (1.0f / 6.0f);
	if (!bStable)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("HeatFieldDebugActor: 안정 조건 위반 (alpha*dt/h^2 = %f > 1/6). ThermalDiffusivityM2s 또는 TimeStepS를 낮추세요."),
			Lambda);
	}
	return bStable;
}

void AHeatFieldDebugActor::StepSimulation(float DeltaTimeS)
{
	TArray<float>& OldBuf = bBufferAIsCurrent ? TemperatureBufferA : TemperatureBufferB;
	TArray<float>& NewBuf = bBufferAIsCurrent ? TemperatureBufferB : TemperatureBufferA;

	// 1) 소스 셀에 열 주입 (HeatW -> 온도 상승, 현재 버퍼에 직접 반영)
	const int32 SrcI = FMath::Clamp(SourceCellIndex.X, 0, GridSizeX - 1);
	const int32 SrcJ = FMath::Clamp(SourceCellIndex.Y, 0, GridSizeY - 1);
	const int32 SrcK = FMath::Clamp(SourceCellIndex.Z, 0, GridSizeZ - 1);
	const int32 SrcIdx = LinearIndex(SrcI, SrcJ, SrcK);
	const float CellVolumeM3 = CellSizeM * CellSizeM * CellSizeM;
	OldBuf[SrcIdx] += SourceHeatW * DeltaTimeS / (AirDensityKgM3 * AirSpecificHeatJKgK * CellVolumeM3);

	// 2) 이웃 6개 기반 명시적 확산 (경계는 Neumann: 바깥 이웃을 자기 자신으로 클램프)
	const float Lambda = ThermalDiffusivityM2s * DeltaTimeS / (CellSizeM * CellSizeM);

	for (int32 k = 0; k < GridSizeZ; ++k)
	{
		for (int32 j = 0; j < GridSizeY; ++j)
		{
			for (int32 i = 0; i < GridSizeX; ++i)
			{
				const int32 Idx = LinearIndex(i, j, k);
				const float Center = OldBuf[Idx];

				const float Txm = OldBuf[LinearIndex(FMath::Max(i - 1, 0), j, k)];
				const float Txp = OldBuf[LinearIndex(FMath::Min(i + 1, GridSizeX - 1), j, k)];
				const float Tym = OldBuf[LinearIndex(i, FMath::Max(j - 1, 0), k)];
				const float Typ = OldBuf[LinearIndex(i, FMath::Min(j + 1, GridSizeY - 1), k)];
				const float Tzm = OldBuf[LinearIndex(i, j, FMath::Max(k - 1, 0))];
				const float Tzp = OldBuf[LinearIndex(i, j, FMath::Min(k + 1, GridSizeZ - 1))];

				NewBuf[Idx] = Center + Lambda * (Txm + Txp + Tym + Typ + Tzm + Tzp - 6.0f * Center);
			}
		}
	}

	// 3) 버퍼 스왑
	bBufferAIsCurrent = !bBufferAIsCurrent;
}

void AHeatFieldDebugActor::StartSim()
{
	CheckStability();
	bSimRunning = true;
}

void AHeatFieldDebugActor::PauseSim()
{
	bSimRunning = false;
}

void AHeatFieldDebugActor::ResetField()
{
	bSimRunning = false;
	InitializeField();
}

FLinearColor AHeatFieldDebugActor::TemperatureToColor(float NormalizedT)
{
	// 열화상 카메라 팔레트: 파랑 -> 보라 -> 빨강 -> 주황 -> 노랑 -> 흰색(가장 뜨거움)
	static const FLinearColor Stops[] = {
		FLinearColor(0.0f, 0.05f, 0.6f),
		FLinearColor(0.35f, 0.0f, 0.55f),
		FLinearColor(0.9f, 0.05f, 0.05f),
		FLinearColor(1.0f, 0.45f, 0.0f),
		FLinearColor(1.0f, 0.9f, 0.1f),
		FLinearColor(1.0f, 1.0f, 1.0f)
	};
	static constexpr int32 NumStops = UE_ARRAY_COUNT(Stops);

	const float T = FMath::Clamp(NormalizedT, 0.0f, 1.0f);
	const float Scaled = T * (NumStops - 1);
	const int32 Idx0 = FMath::Min(FMath::FloorToInt(Scaled), NumStops - 1);
	const int32 Idx1 = FMath::Min(Idx0 + 1, NumStops - 1);
	const float Frac = Scaled - Idx0;
	return FMath::Lerp(Stops[Idx0], Stops[Idx1], Frac);
}

void AHeatFieldDebugActor::DrawSlice() const
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const TArray<float>& Buf = bBufferAIsCurrent ? TemperatureBufferA : TemperatureBufferB;
	if (Buf.Num() == 0)
	{
		return;
	}

	float MinT = TNumericLimits<float>::Max();
	float MaxT = TNumericLimits<float>::Lowest();
	for (const float T : Buf)
	{
		MinT = FMath::Min(MinT, T);
		MaxT = FMath::Max(MaxT, T);
	}
	const float Range = MaxT - MinT;

	const float CellSizeCm = CellSizeM * 100.0f;
	const FVector Origin = GetActorLocation();
	const FVector Extent(CellSizeCm * 0.5f);

	auto DrawCell = [&](int32 i, int32 j, int32 k)
	{
		const float T = Buf[LinearIndex(i, j, k)];
		const float NormalizedT = (Range > KINDA_SMALL_NUMBER) ? (T - MinT) / Range : 0.0f;
		const FColor Color = TemperatureToColor(NormalizedT).ToFColor(true);

		const FVector Center = Origin + FVector(
			(i + 0.5f) * CellSizeCm,
			(j + 0.5f) * CellSizeCm,
			(k + 0.5f) * CellSizeCm);

		DrawDebugSolidBox(World, Center, Extent, Color, false, -1.0f, 0);
	};

	switch (SliceAxis)
	{
	case EHeatFieldSliceAxis::X:
	{
		const int32 Mid = GridSizeX / 2;
		for (int32 k = 0; k < GridSizeZ; ++k)
		{
			for (int32 j = 0; j < GridSizeY; ++j)
			{
				DrawCell(Mid, j, k);
			}
		}
		break;
	}
	case EHeatFieldSliceAxis::Y:
	{
		const int32 Mid = GridSizeY / 2;
		for (int32 k = 0; k < GridSizeZ; ++k)
		{
			for (int32 i = 0; i < GridSizeX; ++i)
			{
				DrawCell(i, Mid, k);
			}
		}
		break;
	}
	case EHeatFieldSliceAxis::Z:
	default:
	{
		const int32 Mid = GridSizeZ / 2;
		for (int32 j = 0; j < GridSizeY; ++j)
		{
			for (int32 i = 0; i < GridSizeX; ++i)
			{
				DrawCell(i, j, Mid);
			}
		}
		break;
	}
	}
}
