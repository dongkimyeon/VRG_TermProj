// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/FixedOctreeDebugActor.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AFixedOctreeDebugActor::AFixedOctreeDebugActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFixedOctreeDebugActor::BeginPlay()
{
	Super::BeginPlay();
	RebuildOctree();
}

void AFixedOctreeDebugActor::RebuildOctree()
{
	bNeedsBuild = true;
}

#if WITH_EDITOR
void AFixedOctreeDebugActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildOctree();
}
#endif

void AFixedOctreeDebugActor::BuildPreview()
{
	Octree.Reset();
	ActiveLeafCount = 0;
	if (!IsValid(AirField))
	{
		AAirFieldDebugActor* Candidate = nullptr;
		int32 Count = 0;
		for (TActorIterator<AAirFieldDebugActor> It(GetWorld()); It; ++It)
		{
			Candidate = *It;
			++Count;
		}
		if (Count != 1)
		{
			BuildStatus = TEXT("AirField를 지정하세요 (자동 선택은 공기 액터 1개일 때만 가능).");
			return;
		}
		AirField = Candidate;
	}
	if (!FMath::IsFinite(RackPaddingM) || RackPaddingM < 0.0)
	{
		BuildStatus = TEXT("RackPaddingM은 0 이상이어야 합니다.");
		return;
	}

	// 기존 공기 격자도 월드 축 정렬이며 액터 위치만 원점으로 사용한다.
	BuiltOriginCm = AirField->GetActorLocation();
	TArray<FBox> RackBoxesM;
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It)
	{
		const FVector SizeM = It->RackSizeM;
		FBox BoxM(ForceInit);
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector LocalM((Corner & 1) ? SizeM.X * 0.5 : -SizeM.X * 0.5,
				(Corner & 2) ? SizeM.Y * 0.5 : -SizeM.Y * 0.5, (Corner & 4) ? SizeM.Z : 0.0);
			const FVector WorldCm = It->GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(LocalM));
			BoxM += SimUnits::UnrealToMeters(WorldCm - BuiltOriginCm);
		}
		RackBoxesM.Add(BoxM.ExpandBy(RackPaddingM));
	}
	FFixedOctreeSettings Settings;
	Settings.MaxDepth = RefinementDepth;
	Settings.MaxLeaves = MaxLeaves;

	// PIE에서는 실제 솔버 도메인, 편집 중에는 다음 실행의 설정을 사용한다.
	const FUniformAirGrid& Source = AirField->GetMutableSolver().GetGrid();
	const bool bHasLiveGrid = GetWorld()->IsGameWorld() && Source.TemperatureK.Num() > 0;
	const FAirGridParams& Params = bHasLiveGrid ? Source.GetParams() : AirField->GridParams;
	FString Error;
	if (!Octree.Build(Params, Settings, RackBoxesM, Error))
	{
		BuildStatus = Error;
		return;
	}
	ActiveLeafCount = Octree.GetNumLeaves();
	BuildStatus = FString::Printf(TEXT("고정 옥트리 %d leaves / 랙 영역 %d개 | 기류 계산은 균일 격자 유지"),
		ActiveLeafCount, RackBoxesM.Num());
	if (bHasLiveGrid)
	{
		if (Octree.ImportUniform(Source, Error))
		{
			const double RefJ = Source.TotalThermalEnergyJ(Params.ReferenceTempK);
			const double ErrorJ = Octree.TotalThermalEnergyJ(Params.ReferenceTempK) - RefJ;
			BuildStatus += FString::Printf(TEXT(" | 재구축 시 온도 복사 ΔE=%.3e J"), ErrorJ);
		}
		else
		{
			BuildStatus += TEXT(" | 온도 복사 실패: ") + Error;
		}
	}
}

void AFixedOctreeDebugActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bNeedsBuild)
	{
		BuildPreview();
		bNeedsBuild = false;
	}
	int32 Drawn = 0;
	int32 Eligible = 0;
	const TArray<FFixedOctreeNode>& Nodes = Octree.GetNodes();
	if (bDrawWireframe)
	{
		for (int32 NodeIndex : Octree.GetLeafNodeIndices())
		{
			const int32 Depth = Nodes[NodeIndex].Depth;
			if (!bDrawCoarseCells && Depth == 0) { continue; }
			++Eligible;
			if (Drawn >= FMath::Max(1, MaxDrawnLeaves)) { continue; }
			const FBox BoxM = Octree.NodeBoundsM(NodeIndex);
			const FColor Color = Depth == 0 ? FColor(70, 110, 180)
				: FLinearColor::MakeFromHSV8(uint8(40 + Depth * 30), 200, 255).ToFColor(true);
			DrawDebugBox(GetWorld(), BuiltOriginCm + SimUnits::MetersToUnreal(BoxM.GetCenter()),
				SimUnits::MetersToUnreal(BoxM.GetExtent()), Color, false, -1.0f, 0, 0.5f);
			++Drawn;
		}
	}
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()) + 500000, 0.0f,
			ActiveLeafCount > 0 ? FColor::Cyan : FColor::Orange,
			FString::Printf(TEXT("[S5-A] %s | 표시 %d/%d (랙 이동 후 RebuildOctree)"), *BuildStatus, Drawn, Eligible));
	}
}
