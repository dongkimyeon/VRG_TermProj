// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/AirFieldDebugActor.h"

#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"

#include "EngineUtils.h"

#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

namespace
{
	// 공기 온도장 색상표. 부품 온도(SimMeshBuilder)와 일부러 다르게 둔다.
	// 연속장은 반투명하게 겹쳐 그리므로 채도를 낮추고 파랑->빨강만 쓴다.
	FLinearColor AirTemperatureColor(double Normalized)
	{
		const float T = FMath::Clamp(static_cast<float>(Normalized), 0.0f, 1.0f);
		static const FLinearColor ColdColor(0.05f, 0.25f, 0.85f);
		static const FLinearColor MidColor(0.15f, 0.85f, 0.55f);
		static const FLinearColor HotColor(1.00f, 0.25f, 0.10f);

		return T < 0.5f
			? FMath::Lerp(ColdColor, MidColor, T * 2.0f)
			: FMath::Lerp(MidColor, HotColor, (T - 0.5f) * 2.0f);
	}
}

AAirFieldDebugActor::AAirFieldDebugActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// 기본 구성은 랙 한 줄이 들어갈 만한 작은 방이다.
	GridParams.NumCellsX = 24;
	GridParams.NumCellsY = 24;
	GridParams.NumCellsZ = 16;
	GridParams.CellSizeM = 0.25;

	// 공기 해석은 압력 풀이가 무겁다. 목표를 너무 조이면 프레임이 죽는다.
	// D5대로 목표와 상한을 함께 두고, 못 미치면 화면에 수렴 실패를 띄운다.
	// 실시간용이라 테스트(1e-8, 수천 회)보다 훨씬 느슨하다. 프레임 안에 끝나야 한다.
	// 1e-5 / 300회로 뒀더니 잔차 1.04e-5로 목표를 아슬아슬하게 못 넘겨 매번
	// 수렴 실패가 떴다. 목표를 현실적인 값으로 맞췄다.
	PressureSettings.NormalizedResidualTarget = 2.0e-5;
	PressureSettings.MaxIterations = 400;

	HeatSources.Add(FAirHeatSourceBox());
}

void AAirFieldDebugActor::BeginPlay()
{
	Super::BeginPlay();
	ResetSim();
	StartSim();
}

#if WITH_EDITOR
void AAirFieldDebugActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bNeedsReset = true;
}
#endif

void AAirFieldDebugActor::StartSim()
{
	bSimRunning = true;
}

void AAirFieldDebugActor::PauseSim()
{
	bSimRunning = false;
}

void AAirFieldDebugActor::ResetSim()
{
	Clock.Reset(FixedStepS);
	Solver.Initialize(GridParams, InitialTempK);
	Solver.GetMutablePressureSettings() = PressureSettings;

	Input.GravityZMs2 = GravityZMs2;
	Input.bEnableBuoyancy = bEnableBuoyancy;
	BuildCellHeat();

	bNeedsReset = false;
}

FAirSolverSnapshot AAirFieldDebugActor::GetSnapshot() const
{
	return Solver.MakeSnapshot(Clock.GetSimTimeS());
}

void AAirFieldDebugActor::BuildCellHeat()
{
	const FUniformAirGrid& Grid = Solver.GetGrid();
	Input.CellHeatW.Init(0.0, Grid.GetNumCells());

	for (const FAirHeatSourceBox& Source : HeatSources)
	{
		if (Source.HeatW == 0.0)
		{
			continue;
		}

		const FVector MinM = Source.CenterM - Source.SizeM * 0.5;
		const FVector MaxM = Source.CenterM + Source.SizeM * 0.5;

		// 상자에 중심이 들어가는 셀을 모은다.
		TArray<int32> CoveredCells;
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		{
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			{
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					const FVector CenterM = Grid.CellCenterM(X, Y, Z);
					if (CenterM.X >= MinM.X && CenterM.X <= MaxM.X
						&& CenterM.Y >= MinM.Y && CenterM.Y <= MaxM.Y
						&& CenterM.Z >= MinM.Z && CenterM.Z <= MaxM.Z)
					{
						CoveredCells.Add(Grid.CellIndex(X, Y, Z));
					}
				}
			}
		}

		if (CoveredCells.Num() == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("AirFieldDebugActor: 열원 상자가 격자에 걸리지 않습니다 (%.1f W가 버려집니다)."),
				Source.HeatW);
			continue;
		}

		// D1: 체적 열원으로 넣는다. 셀이 겹치면 합산한다 - 덮어쓰면 열이 사라진다.
		const double PerCellW = Source.HeatW / CoveredCells.Num();
		for (const int32 Index : CoveredCells)
		{
			Input.CellHeatW[Index] += PerCellW;
		}
	}
}

namespace
{
	// 레벨에 놓인 랙을 전부 모은다. 방 액터를 거치지 않는다 -
	// 공기 격자가 알아야 하는 것은 랙의 위치와 방향뿐이다.
	TArray<ARackThermalActor*> GatherRacks(UWorld* World)
	{
		TArray<ARackThermalActor*> Racks;
		if (World == nullptr)
		{
			return Racks;
		}
		for (TActorIterator<ARackThermalActor> It(World); It; ++It)
		{
			Racks.Add(*It);
		}
		return Racks;
	}
}

void AAirFieldDebugActor::FitToRacks()
{
	const TArray<ARackThermalActor*> Racks = GatherRacks(GetWorld());
	if (Racks.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AirFieldDebugActor: 레벨에서 랙을 찾지 못했습니다."));
		return;
	}

	FBox BoundsM(ForceInit);
	for (const ARackThermalActor* Rack : Racks)
	{
		const FVector RackM = SimUnits::UnrealToMeters(Rack->GetActorLocation());
		// 회전을 따지지 않고 넉넉히 감싼다. 격자 범위라 여유가 있어도 된다.
		const double HalfSpanM = FMath::Max(Rack->RackSizeM.X, Rack->RackSizeM.Y) * 0.5;
		BoundsM += FBox(
			RackM - FVector(HalfSpanM, HalfSpanM, 0.0),
			RackM + FVector(HalfSpanM, HalfSpanM, Rack->RackSizeM.Z));
	}

	// 배기 기둥이 랙 뒤와 위로 뻗으므로 넉넉하게 잡는다.
	// 격자 밖으로 나간 열은 버려지고, 그러면 열수지 장부가 어긋난 것처럼 보인다.
	const double MarginM = ExhaustStandoffM * 2.0 + 0.6;
	BoundsM = BoundsM.ExpandBy(FVector(MarginM, MarginM, 0.0));
	BoundsM.Max.Z = FMath::Max(BoundsM.Max.Z, ExhaustTopZM + 1.0);

	const double CellSizeM = FMath::Max(GridParams.CellSizeM, 0.01);

	// 도메인 원점은 격자 (0,0,0) 셀의 바깥 모서리다. 액터를 거기에 둔다.
	SetActorLocation(SimUnits::MetersToUnreal(BoundsM.Min));

	const FVector SpanM = BoundsM.GetSize();
	GridParams.NumCellsX = FMath::Clamp(FMath::CeilToInt32(SpanM.X / CellSizeM), 1, 128);
	GridParams.NumCellsY = FMath::Clamp(FMath::CeilToInt32(SpanM.Y / CellSizeM), 1, 128);
	GridParams.NumCellsZ = FMath::Clamp(FMath::CeilToInt32(SpanM.Z / CellSizeM), 1, 128);

	UE_LOG(LogTemp, Log,
		TEXT("AirFieldDebugActor: 랙 %d대에 맞췄습니다. %.2f x %.2f x %.2f m -> 격자 %dx%dx%d (셀 %d개)."),
		Racks.Num(), SpanM.X, SpanM.Y, SpanM.Z,
		GridParams.NumCellsX, GridParams.NumCellsY, GridParams.NumCellsZ,
		GridParams.NumCellsX * GridParams.NumCellsY * GridParams.NumCellsZ);

	bNeedsReset = true;
}

void AAirFieldDebugActor::PlaceHeatSourcesFromRacks()
{
	const TArray<ARackThermalActor*> Racks = GatherRacks(GetWorld());
	if (Racks.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("AirFieldDebugActor: 레벨에서 랙을 찾지 못했습니다."));
		return;
	}

	HeatSources.Reset();

	// 액터 기준 로컬 좌표로 바꿔 담는다.
	const FVector ActorLocationM = SimUnits::UnrealToMeters(GetActorLocation());
	const double ExhaustCenterZM = 0.5 * (ExhaustBottomZM + ExhaustTopZM);
	const double ExhaustHeightM = FMath::Max(ExhaustTopZM - ExhaustBottomZM, GridParams.CellSizeM);

	for (const ARackThermalActor* Rack : Racks)
	{
		// 열원을 랙 몸통 안이 아니라 **뒷면 배기 쪽**에 둔다.
		// 더운 공기는 랙 뒤로 나오므로 그게 물리적으로 맞고, 랙 안에 넣으면
		// 불투명한 랙 메쉬에 가려 화면에서 아무것도 안 보인다.
		// 랙 앞면이 로컬 +X이므로 뒷면은 그 반대다.
		const FVector RearDir = -Rack->GetActorForwardVector().GetSafeNormal2D();
		const FVector RackM = SimUnits::UnrealToMeters(Rack->GetActorLocation());
		const FVector ExhaustCenterM =
			RackM + RearDir * (Rack->RackSizeM.X * 0.5 + ExhaustStandoffM);

		FAirHeatSourceBox Source;
		Source.CenterM = ExhaustCenterM - ActorLocationM + FVector(0.0, 0.0, ExhaustCenterZM);

		// 상자는 축에 정렬돼 있다. 랙이 돌아가 있으면 폭과 깊이를 바꿔 넣는다.
		const bool bAlongX = FMath::Abs(RearDir.X) > FMath::Abs(RearDir.Y);
		Source.SizeM = bAlongX
			? FVector(ExhaustStandoffM * 2.0, Rack->RackSizeM.Y, ExhaustHeightM)
			: FVector(Rack->RackSizeM.Y, ExhaustStandoffM * 2.0, ExhaustHeightM);

		// D1: 랙 하나가 공기에 주는 열을 하나의 체적 열원으로 준다.
		// 부품별로 쪼개 넣지 않는다.
		Source.HeatW = AirHeatPerRackW;

		HeatSources.Add(Source);
	}

	UE_LOG(LogTemp, Log, TEXT("AirFieldDebugActor: 열원 %d개를 놓았습니다 (랙당 %.0f W, 합계 %.1f kW)."),
		HeatSources.Num(), AirHeatPerRackW, HeatSources.Num() * AirHeatPerRackW / 1000.0);

	bNeedsReset = true;
}

void AAirFieldDebugActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bNeedsReset)
	{
		ResetSim();
	}

	if (bSimRunning)
	{
		// 규약 4: 시계가 남은 시간을 버리지 않고 다음 프레임으로 넘긴다.
		// 다만 공기 해석은 무거워서 한 프레임에 돌릴 스텝 수를 따로 제한한다.
		const int32 RequestedSteps = Clock.BeginFrame(DeltaTime);
		const int32 NumSteps = FMath::Min(RequestedSteps, MaxStepsPerFrame);

		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			Solver.Step(Input, Clock.GetFixedStepS());
			Clock.CompleteStep();
		}
	}

	if (bDrawSlice)
	{
		DrawSlice();
	}

	if (bShowSummary)
	{
		DrawSummaryText();
	}
}

void AAirFieldDebugActor::DrawSlice() const
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const FUniformAirGrid& Grid = Solver.GetGrid();
	const double CellSizeM = Grid.GetCellSizeM();

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const FVector ActorLocationCm = GetActorLocation();
	const double HalfCellCm = SimUnits::MetersToCentimeters(CellSizeM) * 0.5;

	if (bDrawDomainBounds)
	{
		const FVector DomainSizeCm = SimUnits::MetersToUnreal(Grid.GetDomainSizeM());
		DrawDebugBox(World, ActorLocationCm + DomainSizeCm * 0.5, DomainSizeCm * 0.5,
			FColor(120, 140, 170), false, -1.0f, 0, 2.0f);
	}

	if (bDrawHeatSources)
	{
		for (const FAirHeatSourceBox& Source : HeatSources)
		{
			DrawDebugBox(World,
				ActorLocationCm + SimUnits::MetersToUnreal(Source.CenterM),
				SimUnits::MetersToUnreal(Source.SizeM) * 0.5,
				FColor(255, 160, 60), false, -1.0f, 0, 2.0f);
		}
	}

	// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
	const double MinTempK = SimUnits::CelsiusToKelvin(ColorMinCelsius);
	const double MaxTempK = SimUnits::CelsiusToKelvin(ColorMaxCelsius);
	const double SpanK = FMath::Max(MaxTempK - MinTempK, UE_DOUBLE_SMALL_NUMBER);

	const int32 Stride = FMath::Max(1, DrawStride);

	// 슬라이스 축에 따라 훑을 범위를 정한다.
	int32 FixedIndex = 0;
	switch (SliceAxis)
	{
	case EAirSliceAxis::X:
		FixedIndex = FMath::Clamp(FMath::RoundToInt32(SliceFraction * (Grid.GetNumCellsX() - 1)),
			0, Grid.GetNumCellsX() - 1);
		break;
	case EAirSliceAxis::Y:
		FixedIndex = FMath::Clamp(FMath::RoundToInt32(SliceFraction * (Grid.GetNumCellsY() - 1)),
			0, Grid.GetNumCellsY() - 1);
		break;
	default:
		FixedIndex = FMath::Clamp(FMath::RoundToInt32(SliceFraction * (Grid.GetNumCellsZ() - 1)),
			0, Grid.GetNumCellsZ() - 1);
		break;
	}

	// 슬라이스 면에 맞춰 납작한 상자를 그린다. 두께는 한 셀의 1/6로 둔다.
	const FVector ThicknessCm(
		SliceAxis == EAirSliceAxis::X ? HalfCellCm / 3.0 : HalfCellCm,
		SliceAxis == EAirSliceAxis::Y ? HalfCellCm / 3.0 : HalfCellCm,
		SliceAxis == EAirSliceAxis::Z ? HalfCellCm / 3.0 : HalfCellCm);

	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); Z += Stride)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); Y += Stride)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); X += Stride)
			{
				const bool bOnSlice =
					(SliceAxis == EAirSliceAxis::X && X == FixedIndex)
					|| (SliceAxis == EAirSliceAxis::Y && Y == FixedIndex)
					|| (SliceAxis == EAirSliceAxis::Z && Z == FixedIndex);
				if (!bOnSlice)
				{
					continue;
				}

				const double TempK = Grid.TemperatureK[Grid.CellIndex(X, Y, Z)];
				const double Normalized = (TempK - MinTempK) / SpanK;

				if (Normalized < MinVisibleFraction)
				{
					continue;
				}

				FLinearColor Color = AirTemperatureColor(Normalized);

				// 찬 영역은 옅게 깔고 뜨거운 영역만 진하게 올린다.
				// 지수를 1보다 크게 둬야 중간 온도가 화면을 덮지 않는다.
				const float Shaped = FMath::Pow(FMath::Clamp(static_cast<float>(Normalized), 0.0f, 1.0f), 1.5f);
				Color.A = FMath::Lerp(static_cast<float>(ColdOpacity), static_cast<float>(HotOpacity), Shaped);

				const FVector CenterCm = ActorLocationCm + SimUnits::MetersToUnreal(Grid.CellCenterM(X, Y, Z));
				DrawDebugSolidBox(World, CenterCm, ThicknessCm, Color.ToFColor(false), false, -1.0f, 0);
			}
		}
	}

	if (bDrawVelocityArrows)
	{
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); Z += Stride)
		{
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); Y += Stride)
			{
				for (int32 X = 0; X < Grid.GetNumCellsX(); X += Stride)
				{
					const bool bOnSlice =
						(SliceAxis == EAirSliceAxis::X && X == FixedIndex)
						|| (SliceAxis == EAirSliceAxis::Y && Y == FixedIndex)
						|| (SliceAxis == EAirSliceAxis::Z && Z == FixedIndex);
					if (!bOnSlice)
					{
						continue;
					}

					const FVector VelocityMs(
						0.5 * (Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)] + Grid.VelocityXMs[Grid.FaceXIndex(X + 1, Y, Z)]),
						0.5 * (Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)] + Grid.VelocityYMs[Grid.FaceYIndex(X, Y + 1, Z)]),
						0.5 * (Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)] + Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z + 1)]));

					if (VelocityMs.IsNearlyZero())
					{
						continue;
					}

					// 화살표 길이는 속도에 비례하되 셀 크기를 넘지 않게 자른다.
					const FVector StartCm = ActorLocationCm + SimUnits::MetersToUnreal(Grid.CellCenterM(X, Y, Z));
					const FVector OffsetCm = SimUnits::MetersToUnreal(
						VelocityMs.GetClampedToMaxSize(1.0) * CellSizeM * 0.8);
					DrawDebugDirectionalArrow(World, StartCm, StartCm + OffsetCm, 6.0f,
						FColor(230, 230, 230), false, -1.0f, 0, 1.0f);
				}
			}
		}
	}
}

void AAirFieldDebugActor::DrawSummaryText() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const FAirSolverSnapshot Snapshot = GetSnapshot();
	const FUniformAirGrid& Grid = Solver.GetGrid();

	double TotalHeatW = 0.0;
	for (const double HeatW : Input.CellHeatW)
	{
		TotalHeatW += HeatW;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		Snapshot.bPressureConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[%s] t=%.1f s | 격자 %dx%dx%d (h=%.2f m) | 압력 %s (반복 %d회, 정규화 잔차 %.2e)"),
			*GetName(), Snapshot.SimTimeS,
			Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ(), Grid.GetCellSizeM(),
			Snapshot.bPressureConverged ? TEXT("수렴") : TEXT("수렴 실패 - 속도장 믿지 말 것"),
			Snapshot.PressureIterations, Snapshot.PressureNormalizedResidual));

	// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("열원 %.0f W | 공기 %.2f ~ %.2f C | 최대 속도 %.3f m/s | CFL %.2f%s"),
			TotalHeatW,
			SimUnits::KelvinToCelsius(Snapshot.MinTemperatureK),
			SimUnits::KelvinToCelsius(Snapshot.MaxTemperatureK),
			Snapshot.MaxSpeedMs, Snapshot.CourantNumber,
			Snapshot.CourantNumber > 1.0 ? TEXT(" [CFL>1: 결과가 뭉개짐]") : TEXT("")));

	GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f, FColor::Yellow,
		FString::Printf(TEXT("정규화 발산 %.2e (무차원) | 유입 %.3e J / 축적 %.3e J / 정규화 열수지 잔차 %.2e"),
			Snapshot.NormalizedDivergence,
			Solver.GetEnergyInJ(), Solver.GetEnergyStoredJ(), Snapshot.NormalizedEnergyResidual));

	GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f, FColor(150, 150, 150),
		TEXT("공기 색 = 온도 (파랑 낮음 -> 빨강 높음). 막힌 상자이며 급배기는 S4에서 붙인다."));
}
