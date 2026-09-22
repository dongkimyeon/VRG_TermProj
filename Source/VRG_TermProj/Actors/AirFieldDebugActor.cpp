// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/AirFieldDebugActor.h"

#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"

#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "HAL/IConsoleManager.h"
#include "SceneViewExtension.h"
#include "VrgVolumeSceneViewExtension.h"
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

namespace
{
	// S7-1 비교용. -1이면 액터 설정을 따르고, 0/1이면 강제한다.
	// 레벨 에셋을 고치지 않고도 같은 장면을 박스 모드와 볼륨 모드로 찍을 수 있어야
	// "두 방식이 같은 그림인가"를 자동으로 확인할 수 있다.
	static int32 GVrgForceDrawSlice = -1;
	static FAutoConsoleVariableRef CVarForceDrawSlice(
		TEXT("vrg.Air.DrawSlice"),
		GVrgForceDrawSlice,
		TEXT("디버그 박스 슬라이스 표시를 강제한다. -1=액터 설정, 0=끔, 1=켬"));

	static int32 GVrgForceDrawVolume = -1;
	static FAutoConsoleVariableRef CVarForceDrawVolume(
		TEXT("vrg.Air.DrawVolume"),
		GVrgForceDrawVolume,
		TEXT("볼륨 레이마칭 표시를 강제한다. -1=액터 설정, 0=끔, 1=켬"));

	// 표시 범위와 농도도 강제할 수 있어야 한다. 박스 모드와 볼륨 모드가 **같은 범위**를
	// 쓸 때만 "같은 그림인가"를 비교할 수 있고, 레벨 에셋을 고치지 않고 맞춰야 한다.
	static float GVrgColorMinC = -1000.0f;
	static FAutoConsoleVariableRef CVarColorMinC(
		TEXT("vrg.Air.ColorMinC"), GVrgColorMinC,
		TEXT("온도 색상표 하한 [C]. -1000이면 액터 설정을 쓴다."));

	static float GVrgColorMaxC = -1000.0f;
	static FAutoConsoleVariableRef CVarColorMaxC(
		TEXT("vrg.Air.ColorMaxC"), GVrgColorMaxC,
		TEXT("온도 색상표 상한 [C]. -1000이면 액터 설정을 쓴다."));

	static float GVrgVolumeOpacity = -1.0f;
	static FAutoConsoleVariableRef CVarVolumeOpacity(
		TEXT("vrg.Volume.Opacity"), GVrgVolumeOpacity,
		TEXT("볼륨 흡수 계수 [1/cm]. 음수면 액터 설정을 쓴다."));

	// S7-4: 압력 스윕을 컴퓨트 셰이더로 돌릴지. 켜면 솔버가 게임 스레드 전용이 된다.
	// 기본값을 1로 둔다. 뷰포트에서 Play만 눌러도 GPU 경로가 돌아야 의미가 있다.
	// 자동화 테스트는 액터를 거치지 않고 솔버를 직접 부르므로 영향을 받지 않는다
	// (FAirSolver::bUseGpuPressure의 기본값은 여전히 false다).
	static int32 GVrgUseGpuPressure = 1;
	static FAutoConsoleVariableRef CVarUseGpuPressure(
		TEXT("vrg.Pressure.Gpu"), GVrgUseGpuPressure,
		TEXT("압력 Jacobi를 컴퓨트 셰이더로 돌린다. 0=CPU FP64(기준), 1=GPU FP32"));

	static int32 GVrgDrawFlow = -1;
	static FAutoConsoleVariableRef CVarDrawFlow(
		TEXT("vrg.Flow.Draw"), GVrgDrawFlow,
		TEXT("기류 무늬 표시를 강제한다. -1=액터 설정, 0=끔, 1=켬"));

	static float GVrgFlowStrength = -1.0f;
	static FAutoConsoleVariableRef CVarFlowStrength(
		TEXT("vrg.Flow.Strength"), GVrgFlowStrength,
		TEXT("기류 무늬 진하기. 음수면 액터 설정."));

	static int32 GVrgDrawMode = -1;
	static FAutoConsoleVariableRef CVarDrawMode(
		TEXT("vrg.Air.DrawMode"), GVrgDrawMode,
		TEXT("박스 표시 방식. -1=액터 설정, 0=단면, 1=격자 전체, 2=둘 다"));

	static float GVrgMinVisibleFraction = -1.0f;
	static FAutoConsoleVariableRef CVarMinVisible(
		TEXT("vrg.Air.MinVisibleFraction"), GVrgMinVisibleFraction,
		TEXT("이 정규화 온도 미만은 박스를 안 그린다. 음수면 액터 설정."));

	bool ResolveDrawFlag(int32 Override, bool bActorSetting)
	{
		return Override < 0 ? bActorSetting : (Override != 0);
	}

	// 레벨에 놓인 공기 액터를 전부 찾는다. 콘솔 명령 전용이다.
	TArray<AAirFieldDebugActor*> FindAirActors()
	{
		TArray<AAirFieldDebugActor*> Found;
		if (GEngine == nullptr) { return Found; }
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) { continue; }
			for (TActorIterator<AAirFieldDebugActor> It(World); It; ++It)
			{
				Found.Add(*It);
			}
		}
		return Found;
	}
}

// 화면을 보고 확인해야 하는 단계라 시뮬레이션을 눈에 보이는 상태까지 앞당긴다.
// 물리는 평소와 같은 고정 스텝을 쓴다 (규약 4: 시계가 시간을 버리지 않는다).
// 한 프레임에 다 돌리면 창이 그대로 얼어붙으므로 프레임마다 나눠 소화한다.
static FAutoConsoleCommand GVrgAirRunStepsCommand(
	TEXT("vrg.Air.RunSteps"),
	TEXT("공기 솔버 N스텝을 예약한다. 프레임마다 나눠 소화해 창이 멈추지 않는다."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 NumSteps = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 200;
		for (AAirFieldDebugActor* Actor : FindAirActors())
		{
			Actor->QueueWarmupSteps(NumSteps);
		}
	}));

// 격자를 키우며 한계를 찾기 위한 명령. 물리 도메인은 그대로 두고 해상도만 바꾼다.
static FAutoConsoleCommand GVrgAirResizeCommand(
	TEXT("vrg.Air.Rescale"),
	TEXT("도메인 크기를 유지한 채 셀 수를 Scale배로 바꾼다. vrg.Air.Rescale <배수>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const double Scale = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 1.0;
		for (AAirFieldDebugActor* Actor : FindAirActors())
		{
			Actor->RescaleGrid(Scale);
		}
	}));

// CPU FP64 해와 GPU FP32 해를 같은 입력에서 따로 구해 비교한다.
// 비트 동일성은 검증이 아니므로 허용 오차로 본다 (검증 규칙).
static FAutoConsoleCommand GVrgPressureValidateCommand(
	TEXT("vrg.Pressure.Validate"),
	TEXT("같은 입력에 CPU FP64 해와 GPU FP32 해를 비교해 로그에 남긴다."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>&)
	{
		for (AAirFieldDebugActor* Actor : FindAirActors())
		{
			double Normalized = 0.0;
			FString Summary;
			if (Actor->GetMutableSolver().CompareGpuAndCpuPressure(Normalized, Summary))
			{
				UE_LOG(LogTemp, Display, TEXT("[VrgPressure] %s"), *Summary);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[VrgPressure] %s"), *Summary);
			}
		}
	}));

// 공기 도메인 전체가 보이도록 시점을 옮긴다.
// 레벨의 PlayerStart가 지오메트리 안에 있어서 연속장이 화면에 안 잡힌다.
static FAutoConsoleCommand GVrgAirFrameDomainCommand(
	TEXT("vrg.Air.FrameDomain"),
	TEXT("공기 도메인이 다 보이도록 시점을 옮긴다. vrg.Air.FrameDomain [거리배수]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const double DistanceScale = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 1.4;
		for (AAirFieldDebugActor* Actor : FindAirActors())
		{
			Actor->FrameDomainWithView(DistanceScale);
			break;
		}
	}));

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

	// 바깥이 돌릴 때는 열원도 바깥이 채운다. 여기서 만들면 덮어쓰게 된다.
	if (!bDrivenExternally)
	{
		BuildCellHeat();
	}

	bNeedsReset = false;
}

FAirSolverSnapshot AAirFieldDebugActor::GetSnapshot() const
{
	// 바깥이 돌릴 때는 바깥 시계를 쓴다. 이 액터의 시계는 멈춰 있다.
	return Solver.MakeSnapshot(bDrivenExternally ? ExternalSimTimeS : Clock.GetSimTimeS());
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

	// 표시 파라미터 강제. 계산에는 쓰이지 않는 값들이다.
	if (GVrgColorMinC > -999.0f) { ColorMinCelsius = GVrgColorMinC; }
	if (GVrgColorMaxC > -999.0f) { ColorMaxCelsius = GVrgColorMaxC; }
	if (GVrgVolumeOpacity > 0.0f) { VolumeOpacityPerCm = GVrgVolumeOpacity; }
	if (GVrgDrawMode >= 0)
	{
		DrawMode = static_cast<EAirDrawMode>(FMath::Clamp(GVrgDrawMode, 0, 2));
	}
	if (GVrgMinVisibleFraction >= 0.0f) { MinVisibleFraction = GVrgMinVisibleFraction; }
	if (GVrgDrawFlow >= 0) { bDrawFlow = GVrgDrawFlow != 0; }
	if (GVrgFlowStrength >= 0.0f) { FlowStrength = GVrgFlowStrength; }
	Solver.SetUseGpuPressure(GVrgUseGpuPressure != 0);

	// 예약된 예열을 먼저 나눠 소화한다. 다 끝나면 평소 적분으로 돌아간다.
	if (PendingWarmupSteps > 0)
	{
		VRG_SCOPE_COST(STAT_VrgAirSolverStep, SolverMs);
		DrainWarmupSteps();
	}

	// 규약 4: 통합 액터가 돌릴 때는 여기서 적분하지 않는다.
	if (bSimRunning && !bDrivenExternally && PendingWarmupSteps <= 0)
	{
		// 규약 4: 시계가 남은 시간을 버리지 않고 다음 프레임으로 넘긴다.
		// 다만 공기 해석은 무거워서 한 프레임에 돌릴 스텝 수를 따로 제한한다.
		const int32 RequestedSteps = Clock.BeginFrame(DeltaTime);
		const int32 NumSteps = FMath::Min(RequestedSteps, MaxStepsPerFrame);

		// S7-0 계측. 압력 시간은 솔버가 멤버에 담아 둔 값을 읽어 더한다 (규약 5).
		VRG_SCOPE_COST(STAT_VrgAirSolverStep, SolverMs);
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			Solver.Step(Input, Clock.GetFixedStepS());
			Clock.CompleteStep();
			VrgPerf::Accumulator().SolverSteps += 1;
			VrgPerf::Accumulator().PressureMs += Solver.GetLastPressureSolveMs();
			VrgPerf::Accumulator().PressureIterations += Solver.GetLastPressureResult().IterationsUsed;
		}
	}

	if (ResolveDrawFlag(GVrgForceDrawSlice, bDrawSlice))
	{
		VRG_SCOPE_COST(STAT_VrgDebugDraw, DebugDrawMs);
		DrawSlice();
	}

	{
		// 업로드도 표시 비용이므로 같은 항목에 넣는다.
		VRG_SCOPE_COST(STAT_VrgDebugDraw, DebugDrawMs);
		UpdateVolumeRender();
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

	// 그릴 셀 수를 먼저 세어 상한을 넘으면 간격을 키운다.
	// 디버그 상자 하나가 곧 드로우 호출이라, 격자 전체를 1:1로 그리면
	// D10의 30fps 최저선을 못 지킨다. 조용히 느려지는 것보다 간격을 키우고
	// 실제로 쓴 값을 화면에 적는 편이 낫다.
	const bool bDrawVolumeCells = (DrawMode != EAirDrawMode::SlicePlane);
	const bool bDrawSliceCells = (DrawMode != EAirDrawMode::Volume);

	int32 Stride = FMath::Max(1, DrawStride);
	if (bDrawVolumeCells)
	{
		const int32 CellLimit = FMath::Max(100, MaxDrawnCells);
		while (Stride < 64)
		{
			const int64 Drawn =
				static_cast<int64>(FMath::DivideAndRoundUp(Grid.GetNumCellsX(), Stride))
				* FMath::DivideAndRoundUp(Grid.GetNumCellsY(), Stride)
				* FMath::DivideAndRoundUp(Grid.GetNumCellsZ(), Stride);
			if (Drawn <= CellLimit)
			{
				break;
			}
			++Stride;
		}
	}
	EffectiveDrawStride = Stride;

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

	// 단면은 납작한 상자로, 볼륨은 셀 크기 그대로 그린다.
	const FVector SliceThicknessCm(
		SliceAxis == EAirSliceAxis::X ? HalfCellCm / 3.0 : HalfCellCm,
		SliceAxis == EAirSliceAxis::Y ? HalfCellCm / 3.0 : HalfCellCm,
		SliceAxis == EAirSliceAxis::Z ? HalfCellCm / 3.0 : HalfCellCm);

	// 볼륨은 셀을 살짝 줄여 그린다. 딱 맞추면 면이 겹쳐 z-파이팅이 생긴다.
	const FVector VolumeExtentCm(HalfCellCm * 0.92);

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

				const bool bDrawThisCell = (bDrawVolumeCells || (bDrawSliceCells && bOnSlice));
				if (!bDrawThisCell)
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

				// 단면 위의 셀은 볼륨 모드에서도 납작하게 그려 위치를 알아보게 한다.
				const bool bUseSliceShape = bDrawSliceCells && bOnSlice;
				const FVector ExtentCm = bUseSliceShape ? SliceThicknessCm : VolumeExtentCm;

				const FVector CenterCm = ActorLocationCm + SimUnits::MetersToUnreal(Grid.CellCenterM(X, Y, Z));
				DrawDebugSolidBox(World, CenterCm, ExtentCm, Color.ToFColor(false), false, -1.0f, 0);
				++VrgPerf::Accumulator().DrawnPrimitives;
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
					++VrgPerf::Accumulator().DrawnPrimitives;
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

	const TCHAR* DrawModeText =
		(DrawMode == EAirDrawMode::SlicePlane) ? TEXT("단면 한 장")
		: (DrawMode == EAirDrawMode::Volume) ? TEXT("격자 전체")
		: TEXT("단면 + 격자 전체");

	GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f, FColor(150, 150, 150),
		FString::Printf(
			TEXT("공기 색 = 온도 (파랑 낮음 -> 빨강 높음). 표시 %s, 간격 %d%s. 막힌 상자이며 급배기는 S4에서 붙인다."),
			DrawModeText, EffectiveDrawStride,
			EffectiveDrawStride > FMath::Max(1, DrawStride) ? TEXT(" (셀 수 상한으로 자동 조정)") : TEXT("")));
}

void AAirFieldDebugActor::UpdateVolumeRender()
{
	const bool bVolumeOn = ResolveDrawFlag(GVrgForceDrawVolume, bDrawVolume);
	if (!VolumeExtension.IsValid())
	{
		if (!bVolumeOn)
		{
			// 켜기 전에는 만들지 않는다. 뷰 확장은 등록되는 순간 모든 뷰를 훑는다.
			return;
		}
		VolumeExtension = FSceneViewExtensions::NewExtension<FVrgVolumeSceneViewExtension>();
	}

	VolumeExtension->SetEnabled_GameThread(bVolumeOn);
	VolumePassCount = VolumeExtension->GetLastPassCount();
	if (!bVolumeOn)
	{
		return;
	}

	const FUniformAirGrid& Grid = Solver.GetGrid();
	const int32 NumCells = Grid.GetNumCells();
	if (NumCells <= 0 || Grid.TemperatureK.Num() != NumCells)
	{
		return;
	}

	// 장이 안 바뀌었으면 다시 올리지 않는다. 시뮬 스텝보다 프레임이 잦으면
	// 같은 값을 여러 번 올리게 되고, 큰 격자에서는 그 업로드가 렌더 스레드를 지배한다.
	const double SimTimeS = bDrivenExternally ? ExternalSimTimeS : Clock.GetSimTimeS();
	if (SimTimeS == LastUploadedSimTimeS)
	{
		return;
	}
	LastUploadedSimTimeS = SimTimeS;

	// 실제 절대온도 K를 그대로 담는다 (규약 2). 고체 셀은 음수로 표시해
	// 셰이더가 건너뛰게 한다. 정규화해서 담으면 표시 범위를 바꿀 때마다 다시 올려야 한다.
	TArray<float> VoxelsK;
	VoxelsK.SetNumUninitialized(NumCells);
	for (int32 Index = 0; Index < NumCells; ++Index)
	{
		VoxelsK[Index] = Grid.bSolidCell.IsValidIndex(Index) && Grid.bSolidCell[Index]
			? -1.0f
			: float(Grid.TemperatureK[Index]);
	}

	const FVector OriginCm = GetActorLocation();
	const FVector DomainSizeCm = SimUnits::MetersToUnreal(Grid.GetDomainSizeM());
	const FBox WorldBoundsCm(OriginCm, OriginCm + DomainSizeCm);

	// 표시 범위를 장의 실제 온도 폭에 맞춘다. 고정 범위를 쓰면 공기 온도 폭이
	// 범위보다 훨씬 좁을 때 정규화가 0 근처에 몰려 화면이 거의 투명해진다.
	double DisplayMinK = ColorMinCelsius + 273.15;
	double DisplayMaxK = ColorMaxCelsius + 273.15;
	if (bAutoColorRange)
	{
		const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(0.0);
		DisplayMinK = Snapshot.MinTemperatureK;
		// 가장 뜨거운 셀에 상한을 맞추면 그 한 셀만 빨강이고 나머지는 파랗다.
		// 폭을 좁혀 중간 온도가 초록~노랑으로 올라오게 한다 (표시용 대비 조정).
		const double SpanK = FMath::Max(Snapshot.MaxTemperatureK - DisplayMinK, 0.5);
		DisplayMaxK = DisplayMinK + SpanK * FMath::Clamp(AutoColorSpanFraction, 0.2, 1.0);
	}

	const FIntVector GridSize(Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ());
	VolumeExtension->UpdateVolume_GameThread(GridSize,
		MoveTemp(VoxelsK), WorldBoundsCm,
		float(DisplayMinK), float(DisplayMaxK),
		float(VolumeOpacityPerCm), VolumeStepCount,
		float(VolumeFloorDensity), FMath::Clamp(VolumeColorMap, 0, 1));

	// S7-2: 속도장. MAC 격자라 면 속도를 셀 중심으로 평균해서 넘긴다.
	// 렌더는 셀 중심 값만 쓰므로 여기서 한 번만 평균한다.
	// 무늬를 안 그릴 때는 만들지도 올리지도 않는다 (복셀당 16바이트라 온도의 4배다).
	if (!bDrawFlow)
	{
		return;
	}
	TArray<FVector4f> VelocityMs;
	VelocityMs.SetNumUninitialized(NumCells);
	for (int32 Z = 0; Z < GridSize.Z; ++Z)
	{
		for (int32 Y = 0; Y < GridSize.Y; ++Y)
		{
			for (int32 X = 0; X < GridSize.X; ++X)
			{
				const double Vx = 0.5 * (Grid.VelocityXMs[Grid.FaceXIndex(X, Y, Z)]
					+ Grid.VelocityXMs[Grid.FaceXIndex(X + 1, Y, Z)]);
				const double Vy = 0.5 * (Grid.VelocityYMs[Grid.FaceYIndex(X, Y, Z)]
					+ Grid.VelocityYMs[Grid.FaceYIndex(X, Y + 1, Z)]);
				const double Vz = 0.5 * (Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z)]
					+ Grid.VelocityZMs[Grid.FaceZIndex(X, Y, Z + 1)]);
				VelocityMs[Grid.CellIndex(X, Y, Z)] =
					FVector4f(float(Vx), float(Vy), float(Vz), 0.0f);
			}
		}
	}

	// 정규화 기준은 지금 장의 최대 속도다. 고정값을 쓰면 느린 구성에서 무늬가 안 보인다.
	const double MaxSpeedMs = FMath::Max(Grid.MaxAbsVelocityMs(), 0.05);
	VolumeExtension->UpdateVelocity_GameThread(GridSize, MoveTemp(VelocityMs),
		float(MaxSpeedMs), bDrawFlow ? float(FlowStrength) : 0.0f,
		float(FlowSpeedScale), float(FlowFrequency));
}

void AAirFieldDebugActor::QueueWarmupSteps(int32 NumSteps)
{
	if (bNeedsReset)
	{
		ResetSim();
	}
	// 통합 액터가 돌리는 구성에서는 여기서 적분하면 같은 구간을 두 번 적분한다 (규약 4).
	if (bDrivenExternally)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[VrgAir] %s는 외부에서 구동되므로 여기서 스텝을 돌리지 않는다."), *GetName());
		return;
	}

	PendingWarmupSteps = FMath::Clamp(NumSteps, 1, 100000);
	WarmupStepsReported = PendingWarmupSteps;
	VrgPerf::PendingWarmupSteps() = PendingWarmupSteps;
	bSimRunning = true;
	UE_LOG(LogTemp, Display, TEXT("[VrgAir] 예열 %d스텝 예약 (%.2f s 분량)"),
		PendingWarmupSteps, PendingWarmupSteps * Clock.GetFixedStepS());
}

void AAirFieldDebugActor::DrainWarmupSteps()
{
	if (PendingWarmupSteps <= 0)
	{
		return;
	}
	if (bDrivenExternally)
	{
		// 통합 액터가 돌리는 구성에서 여기서 적분하면 같은 구간을 두 번 적분한다 (규약 4).
		UE_LOG(LogTemp, Warning,
			TEXT("[VrgAir] %s는 외부에서 구동되므로 예열을 취소한다."), *GetName());
		PendingWarmupSteps = 0;
		VrgPerf::PendingWarmupSteps() = 0;
		return;
	}

	// 프레임마다 이만큼만 쓴다. 창이 응답을 유지할 정도로 잡았다.
	// 큰 격자에서는 스텝 하나가 이 예산을 넘지만, 그래도 프레임마다 한 장은 그린다.
	constexpr double WarmupBudgetMs = 120.0;
	const double StepS = Clock.GetFixedStepS();
	const uint64 StartCycles = FPlatformTime::Cycles64();

	while (PendingWarmupSteps > 0)
	{
		// 시계에 같은 양의 시간을 먼저 넣는다. CompleteStep만 부르면 누적기가
		// 스텝 수 x 스텝 크기만큼 음수로 내려가고, 그 뒤 실시간 프레임이
		// 그만큼의 실제 시간을 다 채울 때까지 한 스텝도 안 돈다 (규약 4).
		// 실제로 겪었다 - 900스텝을 앞당긴 뒤 18초 동안 솔버가 멈춰 있었다.
		Clock.BeginFrame(StepS);
		Solver.Step(Input, StepS);
		Clock.CompleteStep();
		--PendingWarmupSteps;

		if (FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles) >= WarmupBudgetMs)
		{
			break;
		}
	}
	VrgPerf::PendingWarmupSteps() = PendingWarmupSteps;

	if (PendingWarmupSteps == 0)
	{
		const FAirSolverSnapshot Snapshot = Solver.MakeSnapshot(0.0);
		UE_LOG(LogTemp, Display,
			TEXT("[VrgAir] 예열 %d스텝 완료 (%.2f s). 공기 %.2f ~ %.2f C, 최대 속도 %.4f m/s"),
			WarmupStepsReported, WarmupStepsReported * StepS,
			Snapshot.MinTemperatureK - 273.15, Snapshot.MaxTemperatureK - 273.15,
			Snapshot.MaxSpeedMs);
	}
}

void AAirFieldDebugActor::FrameDomainWithView(double DistanceScale)
{
	UWorld* World = GetWorld();
	APlayerController* Controller = World != nullptr ? World->GetFirstPlayerController() : nullptr;
	AActor* ViewTarget = Controller != nullptr ? Controller->GetViewTarget() : nullptr;
	if (ViewTarget == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgAir] 시점을 옮길 대상이 없다."));
		return;
	}

	const FVector DomainSizeCm = SimUnits::MetersToUnreal(Solver.GetGrid().GetDomainSizeM());
	const FVector CenterCm = GetActorLocation() + DomainSizeCm * 0.5;

	// 대각선 길이만큼 떨어져서 살짝 위에서 내려다본다. 한 면만 보면 안쪽 분포를 못 본다.
	const double DistanceCm = DomainSizeCm.Size() * FMath::Max(0.2, DistanceScale);
	// 옆에서, 살짝 위에서 내려다본다. 부호를 잘못 두면 바닥 밑으로 들어가 화면이 막힌다.
	const FVector EyeCm = CenterCm + FVector(-0.75, -0.55, 0.45).GetSafeNormal() * DistanceCm;

	ViewTarget->SetActorLocation(EyeCm, false, nullptr, ETeleportType::TeleportPhysics);
	const FRotator Look = (CenterCm - EyeCm).Rotation();
	ViewTarget->SetActorRotation(Look);
	Controller->SetControlRotation(Look);

	UE_LOG(LogTemp, Display, TEXT("[VrgAir] 시점 %s -> 도메인 중심 %s (거리 %.0f cm)"),
		*EyeCm.ToCompactString(), *CenterCm.ToCompactString(), DistanceCm);
}

void AAirFieldDebugActor::RescaleGrid(double Scale)
{
	if (!FMath::IsFinite(Scale) || Scale <= 0.0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgAir] 배수는 양수여야 한다."));
		return;
	}

	// 도메인 = 셀 수 x 셀 폭이다. 둘을 반대로 움직여 물리 크기를 유지한다.
	// 그래야 "같은 방을 더 잘게 푼다"가 되고 셀 수만의 효과를 볼 수 있다.
	const FVector DomainBeforeM = FVector(GridParams.NumCellsX, GridParams.NumCellsY,
		GridParams.NumCellsZ) * GridParams.CellSizeM;

	GridParams.NumCellsX = FMath::Clamp(FMath::RoundToInt32(GridParams.NumCellsX * Scale), 1, 256);
	GridParams.NumCellsY = FMath::Clamp(FMath::RoundToInt32(GridParams.NumCellsY * Scale), 1, 256);
	GridParams.NumCellsZ = FMath::Clamp(FMath::RoundToInt32(GridParams.NumCellsZ * Scale), 1, 256);

	// 축마다 반올림이 달라질 수 있으므로 X 기준으로 셀 폭을 다시 잡는다.
	GridParams.CellSizeM = DomainBeforeM.X / GridParams.NumCellsX;

	bNeedsReset = true;
	ResetSim();

	const FVector DomainAfterM = FVector(GridParams.NumCellsX, GridParams.NumCellsY,
		GridParams.NumCellsZ) * GridParams.CellSizeM;
	UE_LOG(LogTemp, Display,
		TEXT("[VrgAir] 격자 %dx%dx%d = %d셀, 셀 폭 %.4f m | 도메인 %s -> %s m"),
		GridParams.NumCellsX, GridParams.NumCellsY, GridParams.NumCellsZ,
		GridParams.NumCellsX * GridParams.NumCellsY * GridParams.NumCellsZ,
		GridParams.CellSizeM, *DomainBeforeM.ToCompactString(), *DomainAfterM.ToCompactString());
}
