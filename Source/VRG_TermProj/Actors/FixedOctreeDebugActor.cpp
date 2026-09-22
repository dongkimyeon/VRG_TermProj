// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/FixedOctreeDebugActor.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Air/OctreePoissonSolver.h"
#include "Simulation/Core/SimUnits.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	// S7-1 비교용. 옥트리 와이어프레임이 화면을 덮으면 연속장 표시를 볼 수 없다.
	// 레벨 에셋을 고치지 않고 끌 수 있어야 같은 장면을 두 방식으로 찍을 수 있다.
	static int32 GVrgForceOctreeWireframe = -1;
	static FAutoConsoleVariableRef CVarForceOctreeWireframe(
		TEXT("vrg.Octree.DrawWireframe"),
		GVrgForceOctreeWireframe,
		TEXT("옥트리 와이어프레임 표시를 강제한다. -1=액터 설정, 0=끔, 1=켬"));
}

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
	OperatorStatus.Reset();
	BuildStatus = FString::Printf(
		TEXT("고정 옥트리 %d leaves / 랙 영역 %d개 | 면 %d개 (비정합 %d, 2:1 분할 %d) | 기류 계산은 균일 격자 유지"),
		ActiveLeafCount, RackBoxesM.Num(), Octree.GetFaces().Num(),
		Octree.GetNumHangingFaces(), Octree.GetNumBalanceSplits());
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

void AFixedOctreeDebugActor::CheckPoissonOperator()
{
	OperatorStatus.Reset();
	if (Octree.GetNumLeaves() == 0)
	{
		OperatorStatus = TEXT("옥트리가 없습니다. 먼저 RebuildOctree를 누르세요.");
		return;
	}

	// 물리량이 아니라 연산자 점검용 합성장이다. PIE가 아니어도 돌도록 리프 좌표에서 만든다.
	// 여기서 나오는 반복 수는 성능 보장값이 아니다.
	TArray<double> Field;
	Field.SetNum(Octree.GetNumLeaves());
	for (int32 Leaf = 0; Leaf < Octree.GetNumLeaves(); ++Leaf)
	{
		const FVector CenterM = Octree.LeafCenterM(Leaf);
		Field[Leaf] = FMath::Cos(CenterM.X) * FMath::Cos(CenterM.Y) + 0.5 * FMath::Cos(CenterM.Z);
	}
	OctreePoissonSolver::RemoveFluidVolumeMean(Octree, Field);

	// B = L(Field)로 두면 우변이 반드시 적합하다. 풀어서 Field가 되돌아오는지 본다.
	TArray<double> RightHandSide;
	RightHandSide.SetNum(Octree.GetNumLeaves());
	double AbsScale = 0.0;
	for (int32 Leaf = 0; Leaf < Octree.GetNumLeaves(); ++Leaf)
	{
		RightHandSide[Leaf] = OctreePoissonSolver::LaplacianAt(Octree, Field, Leaf);
		if (Octree.IsLeafFluid(Leaf))
		{
			AbsScale += Octree.LeafVolumeM3(Leaf) * FMath::Abs(RightHandSide[Leaf]);
		}
	}
	const double ConservationResidual = AbsScale > 0.0
		? FMath::Abs(OctreePoissonSolver::VolumeWeightedLaplacianSum(Octree, Field)) / AbsScale
		: 0.0;

	FPoissonSolveSettings Settings;
	Settings.NormalizedResidualTarget = PreviewResidualTarget;
	Settings.MaxIterations = PreviewMaxIterations;

	TArray<double> Solved;
	TArray<double> Scratch;
	const FPoissonSolveResult Result =
		OctreePoissonSolver::Solve(Octree, RightHandSide, Solved, Scratch, Settings);

	double MaxAbsError = 0.0;
	double MaxAbsField = 0.0;
	for (int32 Leaf = 0; Leaf < Octree.GetNumLeaves(); ++Leaf)
	{
		MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(Solved[Leaf] - Field[Leaf]));
		MaxAbsField = FMath::Max(MaxAbsField, FMath::Abs(Field[Leaf]));
	}

	// D5: 수렴 실패를 숨기지 않는다.
	OperatorStatus = FString::Printf(
		TEXT("보존 잔차 %.3e | %s (반복 %d회, 정규화 잔차 %.3e) | 되돌린 합성장 오차 %.3e / 최대 %.4f"),
		ConservationResidual,
		Result.bConverged ? TEXT("수렴") : TEXT("수렴 실패 - 반복 상한을 올리세요"),
		Result.IterationsUsed, Result.NormalizedResidual, MaxAbsError, MaxAbsField);
}

void AFixedOctreeDebugActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bNeedsBuild)
	{
		BuildPreview();
		bNeedsBuild = false;
	}
	// S7-0 계측. 와이어프레임 상자 하나가 곧 드로우 제출이다.
	VRG_SCOPE_COST(STAT_VrgDebugDraw, DebugDrawMs);
	int32 Drawn = 0;
	int32 Eligible = 0;
	const TArray<FFixedOctreeNode>& Nodes = Octree.GetNodes();
	const bool bWireframeOn = GVrgForceOctreeWireframe < 0
		? bDrawWireframe
		: (GVrgForceOctreeWireframe != 0);
	if (bWireframeOn)
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
			++VrgPerf::Accumulator().DrawnPrimitives;
		}
	}
	if (bDrawHangingFaces && bWireframeOn)
	{
		// 비정합 면의 중심에 점을 찍는다. 잘은 쪽 면적이라 한 굵은 면에 네 점이 찍힌다.
		int32 DrawnFaces = 0;
		for (const FFixedOctreeFace& Face : Octree.GetFaces())
		{
			if (Face.LeafLow == INDEX_NONE || Face.LeafHigh == INDEX_NONE
				|| Octree.LeafDepth(Face.LeafLow) == Octree.LeafDepth(Face.LeafHigh))
			{
				continue;
			}
			if (DrawnFaces >= FMath::Max(1, MaxDrawnLeaves)) { break; }
			// 면 중심 = 잘은 쪽 셀 중심에서 굵은 쪽으로 자기 반폭만큼 간 자리.
			const bool bLowIsFiner = Octree.LeafDepth(Face.LeafLow) > Octree.LeafDepth(Face.LeafHigh);
			const int32 FineLeaf = bLowIsFiner ? Face.LeafLow : Face.LeafHigh;
			FVector CenterM = Octree.LeafCenterM(FineLeaf);
			CenterM[Face.Axis] += (bLowIsFiner ? 0.5 : -0.5) * Octree.LeafSizeM(FineLeaf);
			DrawDebugPoint(GetWorld(), BuiltOriginCm + SimUnits::MetersToUnreal(CenterM),
				6.0f, FColor(255, 80, 200), false, -1.0f, 0);
			++DrawnFaces;
			++VrgPerf::Accumulator().DrawnPrimitives;
		}
	}
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()) + 500001, 0.0f,
			FColor::White, OperatorStatus.IsEmpty() ? TEXT("[S5-B] CheckPoissonOperator로 연산자 점검") : OperatorStatus);
		GEngine->AddOnScreenDebugMessage(uint64(GetUniqueID()) + 500000, 0.0f,
			ActiveLeafCount > 0 ? FColor::Cyan : FColor::Orange,
			FString::Printf(TEXT("[S5-A] %s | 표시 %d/%d (랙 이동 후 RebuildOctree)"), *BuildStatus, Drawn, Eligible));
	}
}
