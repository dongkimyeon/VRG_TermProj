// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/DataCenterRoomActor.h"
#include "Simulation/Core/VrgHud.h"

#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Visualization/SimMeshBuilder.h"
#include "Visualization/GjConnectionMesh.h"
#include "Visualization/GjRackLayout.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CrahUnitActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> PipeTemperatureMode(TEXT("vrg.Pipes.Temperature"), 0,
		TEXT("0: supply/return + flow brightness (default). 1: rack branch temperature, blue 20 C to red 60 C; unknown grey."));
	// 케이블 색. 실제 데이터센터의 용도 구분(관리망/데이터/전원)을 흉내 낸 것이며
	// 계산 결과가 아니다.
	const FLinearColor CableColors[] = {
		FLinearColor(0.15f, 0.85f, 0.25f),
		FLinearColor(0.95f, 0.45f, 0.10f),
		FLinearColor(0.60f, 0.88f, 0.95f),
		FLinearColor(0.85f, 0.85f, 0.88f),
	};
	constexpr int32 NumCableColors = UE_ARRAY_COUNT(CableColors);

	const FLinearColor TrayColor(0.16f, 0.17f, 0.19f);

	// 줄 묶기용 방향 반올림. 거의 같은 방향을 보는 랙을 한 줄로 본다.
	FVector QuantizeDirection(const FVector& Direction)
	{
		const FVector Unit = Direction.GetSafeNormal2D();
		return FVector(FMath::RoundToDouble(Unit.X), FMath::RoundToDouble(Unit.Y), 0.0).GetSafeNormal();
	}
	struct FRowKey
	{
		int32 DirX, DirY, OffsetBucket;
		bool operator==(const FRowKey& Other) const
		{
			return DirX == Other.DirX && DirY == Other.DirY && OffsetBucket == Other.OffsetBucket;
		}
	};
	FRowKey RackRowKey(const FTransform& Transform, double ToleranceM)
	{
		const FVector Rear = QuantizeDirection(-Transform.GetRotation().GetForwardVector());
		return { FMath::RoundToInt32(Rear.X), FMath::RoundToInt32(Rear.Y),
			FMath::RoundToInt32(FVector::DotProduct(SimUnits::UnrealToMeters(Transform.GetLocation()), Rear) / FMath::Max(ToleranceM, 0.05)) };
	}
}

ADataCenterRoomActor::ADataCenterRoomActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ADataCenterRoomActor::BeginPlay()
{
	Super::BeginPlay();
	bNeedsRebuild = true;
}

#if WITH_EDITOR
void ADataCenterRoomActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bNeedsRebuild = true;
}
#endif

void ADataCenterRoomActor::RebuildRoom()
{
	bNeedsRebuild = true;
}

bool ADataCenterRoomActor::ConsumeRebuiltFlag()
{
	const bool bWasRebuilt = bRebuiltSinceLastQuery;
	bRebuiltSinceLastQuery = false;
	return bWasRebuilt;
}

void ADataCenterRoomActor::AssignNodeElevationsFromRoutes()
{
	const int32 NumNodes = Network.GetNumNodes();
	TArray<double> SumZ;
	TArray<int32> Count;
	SumZ.Init(0.0, NumNodes);
	Count.Init(0, NumNodes);
	auto AddEnd = [&](int32 Node, const FVector& PointM)
	{
		if (SumZ.IsValidIndex(Node)) { SumZ[Node] += PointM.Z; ++Count[Node]; }
	};
	// The same ends BuildPipeMeshes / BuildGjPipeMeshes draw: node anchors, replaced by GJ port overrides where set.
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		const int32 From = Network.GetPipeFromNodeIndex(PipeIndex), To = Network.GetPipeToNodeIndex(PipeIndex);
		const FVector* Start = PipeStartOverrideCm.Find(PipeIndex);
		const FVector* End = PipeEndOverrideCm.Find(PipeIndex);
		if (NodePositionM.IsValidIndex(From)) AddEnd(From, Start ? SimUnits::UnrealToMeters(*Start) : NodePositionM[From]);
		if (NodePositionM.IsValidIndex(To)) AddEnd(To, End ? SimUnits::UnrealToMeters(*End) : NodePositionM[To]);
	}
	for (const FGjPumpRoute& Route : GjPumpRoutes)
	{
		AddEnd(Network.GetPumpFromNodeIndex(Route.Pump), SimUnits::UnrealToMeters(Route.StartCm));
		AddEnd(Network.GetPumpToNodeIndex(Route.Pump), SimUnits::UnrealToMeters(Route.EndCm));
	}
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		const double AnchorZ = NodePositionM.IsValidIndex(Node) ? NodePositionM[Node].Z : 0.0;
		Network.SetNodeElevationM(Node, Count[Node] > 0 ? SumZ[Node] / Count[Node] : AnchorZ);
	}
}

int32 ADataCenterRoomActor::AddLeak(ARackThermalActor* Rack, bool bReturnBranch, double HoleDiameterM, double DischargeCoefficient)
{
	if (Rack == nullptr || !(HoleDiameterM >= 0.0) || !(DischargeCoefficient > 0.0 && DischargeCoefficient <= 1.0)) return INDEX_NONE;
	FRoomLeakSpec& Leak = LeakSpecs.AddDefaulted_GetRef();
	Leak.Rack = Rack;
	Leak.bReturnBranch = bReturnBranch;
	Leak.HoleDiameterM = HoleDiameterM;
	Leak.DischargeCoefficient = DischargeCoefficient;
	RebuildRoom();
	return LeakSpecs.Num() - 1;
}

void ADataCenterRoomActor::ClearLeaks()
{
	if (LeakSpecs.Num() == 0) return;
	LeakSpecs.Reset();
	RebuildRoom();
}

bool ADataCenterRoomActor::SetLeakHoleDiameterM(int32 LeakSpec, double HoleDiameterM)
{
	if (!LeakSpecs.IsValidIndex(LeakSpec) || !(HoleDiameterM >= 0.0)) return false;
	LeakSpecs[LeakSpec].HoleDiameterM = HoleDiameterM;
	// A live size change needs no rebuild; the next inventory step solves with it. Restart restores the built size,
	// so record the new size as the as-built one only through a rebuild (an event-driven opening is step 5).
	Network.SetLeakHoleDiameterM(LeakSpecs[LeakSpec].NetworkLeak, HoleDiameterM);
	return true;
}

void ADataCenterRoomActor::StepCoolantInventory(double DtS, double NowS)
{
	FHydraulicSolveResult Result;
	const uint64 StartCycles = FPlatformTime::Cycles64();
	if (!Inventory.Step(Network, SolveSettings, DtS, NowS, Result)) return;
	InventorySolveMsSum += FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
	++InventorySolveCount;
	LastResult = Result;
	bPipeColorsDirty = true;
	if (!LastResult.bConverged)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Leak] t=%.2f s 수력 수렴 실패 (반복 %d, 정규화 잔차 %.3e)"), NowS, LastResult.IterationsUsed, LastResult.NormalizedResidual);
	}
	// One line per simulated second while the inventory is active (and at a depletion).
	const int32 Second = FMath::FloorToInt32(NowS);
	bool bJustDepleted = false;
	for (const FCoolantInventory::FTank& Tank : Inventory.GetTanks()) bJustDepleted |= Tank.State.bDepleted && FMath::Abs(Tank.State.DepletedAtS - NowS) < 0.5 * DtS;
	if (Second == LastInventoryLogSecond && !bJustDepleted) return;
	LastInventoryLogSecond = Second;
	UE_LOG(LogTemp, Display, TEXT("[Leak] t=%.2f s 재고 재풀이 평균 %.3f ms (%d회, 노드 %d, %s, 반복 %d)"), NowS,
		InventorySolveMsSum / FMath::Max(InventorySolveCount, 1), InventorySolveCount, Network.GetNumNodes(),
		LastResult.bUsedSparse ? TEXT("희소 LU") : TEXT("밀행렬 LU"), LastResult.IterationsUsed);
	InventorySolveMsSum = 0.0;
	InventorySolveCount = 0;
	const TArray<FCoolantInventory::FTank>& Tanks = Inventory.GetTanks();
	for (int32 T = 0; T < Tanks.Num(); ++T)
	{
		const FCoolantInventory::FTank& Tank = Tanks[T];
		if (Tank.LeakIndices.Num() == 0 && !Tank.State.bDepleted) continue;
		UE_LOG(LogTemp, Display, TEXT("[Leak] t=%.2f s 줄 %d: 탱크 %.1f kPa(g), 탱크 물 %.3f L (처음 %.3f), 누수 %.4f L/s, 누적 누수 %.3f L, 보충 %.3f L, 펌프 %.3f L/s%s"),
			NowS, T, Inventory.GetTankGaugePressurePa(T) * 1e-3, Tank.State.WaterVolumeM3 * 1e3, Tank.State.InitialWaterM3 * 1e3,
			Tank.LastLeakM3s * 1e3, Tank.State.LeakedM3 * 1e3, Tank.State.MakeupM3 * 1e3, Network.GetPumpFlowM3s(Tank.PumpIndex) * 1e3,
			Tank.State.bDepleted ? *FString::Printf(TEXT(" | 재고 고갈 (t=%.2f s): 펌프 정지, 누수 닫음, 2상 거동은 모델 없음"), Tank.State.DepletedAtS) : TEXT(""));
	}
}

void ADataCenterRoomActor::RestartCoolantInventory()
{
	FHydraulicSolveResult Result;
	if (Inventory.Restart(Network, SolveSettings, Result))
	{
		LastResult = Result;
		bPipeColorsDirty = true;
	}
	LastInventoryLogSecond = -1;
}

void ADataCenterRoomActor::SetCoolantDensityKgM3(double InDensityKgM3)
{
	if (!(InDensityKgM3 > 0.0) || InDensityKgM3 == Network.GetFluidDensityKgM3())
	{
		return;
	}
	Network.SetFluidDensityKgM3(InDensityKgM3);
	if (Network.GetNumNodes() > 0)
	{
		LastResult = Network.Solve(SolveSettings);
		bPipeColorsDirty = true;
	}
}

void ADataCenterRoomActor::SetPumpSpeedRatio(double InSpeedRatio)
{
	LastPumpSpeedRatio = FMath::Clamp(InSpeedRatio, 0.0, 1.5);

	// 회전수는 CDU가 들고 있다. 줄마다 붙은 CDU에 같은 값을 준다.
	for (const FRackRow& Row : Rows)
	{
		if (Row.Cdu != nullptr)
		{
			Row.Cdu->SetPumpSpeedRatio(LastPumpSpeedRatio);
		}
	}

	bNeedsRebuild = true;
}

int32 ADataCenterRoomActor::GetNumRoutedRacks() const
{
	int32 Count = 0;
	for (const FRackRow& Row : Rows)
	{
		Count += Row.Racks.Num();
	}
	return Count;
}

TArray<int32> ADataCenterRoomActor::GetRoutedRackRows() const
{
	// Same walk and the same null skip as GetRoutedRacks(), so the two arrays line up.
	TArray<int32> Result;
	for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		for (const TObjectPtr<ARackThermalActor>& Rack : Rows[RowIndex].Racks)
			if (Rack != nullptr) Result.Add(RowIndex);
	return Result;
}

TArray<ARackThermalActor*> ADataCenterRoomActor::GetRoutedRacks() const
{
	TArray<ARackThermalActor*> Result;
	for (const FRackRow& Row : Rows)
	{
		for (const TObjectPtr<ARackThermalActor>& Rack : Row.Racks)
		{
			if (Rack != nullptr)
			{
				Result.Add(Rack);
			}
		}
	}
	return Result;
}

FBox ADataCenterRoomActor::GetRoomBoundsM() const
{
	FBox BoundsM(ForceInit);

	for (const FRackRow& Row : Rows)
	{
		for (const TObjectPtr<ARackThermalActor>& Rack : Row.Racks)
		{
			if (Rack == nullptr)
			{
				continue;
			}

			const FVector RackM = SimUnits::UnrealToMeters(Rack->GetActorLocation());
			// 회전을 따지지 않고 넉넉히 감싼다. 격자 범위라 여유가 있어도 된다.
			const double HalfSpanM = FMath::Max(Rack->RackSizeM.X, Rack->RackSizeM.Y) * 0.5;
			BoundsM += FBox(
				RackM - FVector(HalfSpanM, HalfSpanM, 0.0),
				RackM + FVector(HalfSpanM, HalfSpanM, Rack->RackSizeM.Z));
		}
	}

	if (!BoundsM.IsValid)
	{
		const FVector HereM = SimUnits::UnrealToMeters(GetActorLocation());
		return FBox(HereM, HereM + FVector(1.0));
	}

	// 뒷면 배관과 천장 트레이까지 덮는다.
	const double RearMarginM = PipeStandoffM + SupplyReturnSpacingM * 2.0 + 0.5;
	BoundsM = BoundsM.ExpandBy(FVector(RearMarginM, RearMarginM, 0.0));
	BoundsM.Max.Z = FMath::Max(BoundsM.Max.Z, CableTrayHeightM + 0.3);
	return BoundsM;
}

FVector ADataCenterRoomActor::RackRearAnchorM(const ARackThermalActor& Rack, const FVector& RearDir,
	double ExtraOffsetM) const
{
	const FVector RackM = SimUnits::UnrealToMeters(Rack.GetActorLocation());
	return RackM + RearDir * (Rack.RackSizeM.X * 0.5 + ExtraOffsetM);
}

TArray<FRackPlacementRow> ADataCenterRoomActor::GetRackRowsForPlacement(const ARackThermalActor* Excluded,
	const TArray<ARackThermalActor*>* ExcludedGroup) const
{
	TArray<FRackPlacementRow> Result;
	if (!GetWorld() || !FMath::IsFinite(RowGroupToleranceM)) return Result;
	TArray<FRowKey> Keys;
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It)
	{
		ARackThermalActor* Rack = *It;
		if (Rack == Excluded || (ExcludedGroup && ExcludedGroup->Contains(Rack))
			|| !IsValid(Rack) || Rack->GetActorTransform().ContainsNaN()) continue;
		const FVector Rear = QuantizeDirection(-Rack->GetActorForwardVector());
		if (Rear.IsNearlyZero()) continue;
		const FRowKey Key = RackRowKey(Rack->GetActorTransform(), RowGroupToleranceM);
		int32 Index = Keys.IndexOfByKey(Key);
		if (Index == INDEX_NONE)
		{
			Index = Result.Num(); Keys.Add(Key);
			FRackPlacementRow Row;
			Row.RearDir = Rear; Row.AlongDir = FVector::CrossProduct(FVector::UpVector, Rear).GetSafeNormal();
			Result.Add(MoveTemp(Row));
		}
		Result[Index].Racks.Add(Rack);
	}
	for (FRackPlacementRow& Row : Result)
		Row.Racks.Sort([Along = Row.AlongDir](const ARackThermalActor& A, const ARackThermalActor& B) {
			return FVector::DotProduct(A.GetActorLocation(), Along) < FVector::DotProduct(B.GetActorLocation(), Along);
		});
	return Result;
}

bool ADataCenterRoomActor::AreTransformsInSameRackRow(const FTransform& A, const FTransform& B) const
{
	return FMath::IsFinite(RowGroupToleranceM) && !A.ContainsNaN() && !B.ContainsNaN()
		&& RackRowKey(A, RowGroupToleranceM) == RackRowKey(B, RowGroupToleranceM);
}

void ADataCenterRoomActor::DiscoverRows()
{
	Rows.Reset();
	GridSpawnedRacks.RemoveAll([](const TObjectPtr<ARackThermalActor>& Rack) { return !IsValid(Rack.Get()); });

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	TArray<ACoolingDistributionUnitActor*> Cdus;
	for (TActorIterator<ACoolingDistributionUnitActor> It(World); It; ++It)
	{
		Cdus.Add(*It);
	}

	// Placement and routing share the exact same direction/position bucket key.
	for (const FRackPlacementRow& Placement : GetRackRowsForPlacement(nullptr))
	{
		FRackRow Row;
		Row.RearDir = Placement.RearDir; Row.AlongDir = Placement.AlongDir;
		for (ARackThermalActor* Rack : Placement.Racks) Row.Racks.Add(Rack);
		Rows.Add(MoveTemp(Row));
	}

	for (FRackRow& Row : Rows)
	{
		// 줄 방향으로 정렬한다. 헤더가 한쪽 끝에서 다른 끝으로 흐르려면 순서가 필요하다.
		const FVector AlongDir = Row.AlongDir;
		Row.Racks.Sort([AlongDir](const ARackThermalActor& A, const ARackThermalActor& B)
		{
			return FVector::DotProduct(SimUnits::UnrealToMeters(A.GetActorLocation()), AlongDir)
				< FVector::DotProduct(SimUnits::UnrealToMeters(B.GetActorLocation()), AlongDir);
		});

		// 줄의 첫 랙에서 가장 가까운 CDU를 맡긴다.
		if (Cdus.Num() > 0 && Row.Racks.Num() > 0)
		{
			const FVector FirstRackM = SimUnits::UnrealToMeters(Row.Racks[0]->GetActorLocation());
			double BestDistanceSq = TNumericLimits<double>::Max();
			for (ACoolingDistributionUnitActor* Cdu : Cdus)
			{
				const double DistanceSq =
					FVector::DistSquared(SimUnits::UnrealToMeters(Cdu->GetActorLocation()), FirstRackM);
				if (DistanceSq < BestDistanceSq)
				{
					BestDistanceSq = DistanceSq;
					Row.Cdu = Cdu;
				}
			}
		}
	}

	if (Rows.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterRoomActor: 레벨에서 ARackThermalActor를 찾지 못했습니다. ")
			TEXT("랙을 직접 놓거나 SpawnRackGrid 버튼을 누르세요."));
	}
	else if (Cdus.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterRoomActor: 레벨에 CDU가 없어 줄 시작점 앞에 가상 CDU를 둡니다. ")
			TEXT("ACoolingDistributionUnitActor를 놓으면 그 자리로 배관이 이어집니다."));
	}
}

void ADataCenterRoomActor::BuildNetwork()
{
	Network.Reset();
	Inventory.Reset();
	LeakBranchPipes.Reset();
	for (FRoomLeakSpec& Leak : LeakSpecs) Leak.NetworkLeak = INDEX_NONE;
	LastInventoryLogSecond = -1;
	NodePositionM.Reset();
	RackBranchPipeIndices.Reset();
	RackReturnPipeIndices.Reset();
	bPipeIsSupplySide.Reset();
	PipeStartOverrideCm.Reset();
	PipeEndOverrideCm.Reset();
	GjPumpRoutes.Reset();

	for (FRackRow& Row : Rows)
	{
		if (Row.Racks.Num() == 0)
		{
			continue;
		}

		// 위상은 InRowLoopBuilder가 만든다. 이 액터는 거기에 좌표만 붙인다.
		FInRowLoopSpec Spec;
		Spec.NumRacks = Row.Racks.Num();
		// Branch resistance scales with the liquid-cooled devices that are docked (compute and NVLink
		// switch trays of the GB200 NVL72).
		Spec.SledsPerRack = Row.Racks.Num() > 0 && Row.Racks[0] ? FMath::Max(1, Row.Racks[0]->GetNumLiquidSlots()) : FMath::Max(1, SledsPerRack);
		Spec.HeaderSegmentResistancePaS2M6 = HeaderSegmentResistancePaS2M6;
		Spec.RackBranchResistancePaS2M6 = RackBranchResistancePaS2M6;
		Spec.ReturnToCduResistancePaS2M6 = ReturnToCduResistancePaS2M6;

		// 펌프와 기준 압력은 CDU가 들고 있다. 없으면 이 액터의 예비값을 쓴다.
		if (Row.Cdu != nullptr)
		{
			Spec.CduReferencePressurePa = Row.Cdu->ReferencePressurePa;
			Spec.PumpShutoffHeadPa = Row.Cdu->PumpShutoffHeadPa;
			Spec.PumpCurveCoeffPaS2M6 = Row.Cdu->PumpCurveCoeffPaS2M6;
			Spec.PumpEfficiency = Row.Cdu->PumpEfficiency;
			Spec.PumpSpeedRatio = Row.Cdu->PumpSpeedRatio;
		}
		else
		{
			Spec.CduReferencePressurePa = FallbackCduReferencePressurePa;
			Spec.PumpShutoffHeadPa = FallbackPumpShutoffHeadPa;
			Spec.PumpCurveCoeffPaS2M6 = FallbackPumpCurveCoeffPaS2M6;
			Spec.PumpSpeedRatio = LastPumpSpeedRatio;
		}

		// 랙마다 지금 꽂혀 있는 유닛 수를 읽는다. 뽑으면 분기 저항이 올라간다 (D4).
		Spec.DockedSledsPerRack.Reset();
		for (const TObjectPtr<ARackThermalActor>& Rack : Row.Racks)
		{
			Spec.DockedSledsPerRack.Add(Rack != nullptr ? Rack->GetNumDockedLiquidSlots() : Spec.SledsPerRack);
		}

		const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, Spec);
		Row.PumpIndex = Handles.PumpIndex;

		NodePositionM.SetNum(Network.GetNumNodes());

		// CDU 노드. 실제 CDU가 있으면 그 포트 자리에, 없으면 줄 시작 앞쪽에 둔다.
		if (Row.Cdu != nullptr)
		{
			// 노드는 하나인데 포트가 둘이라 가운데에 둔다.
			NodePositionM[Handles.CduNodeIndex] =
				(Row.Cdu->GetSupplyPortWorldM() + Row.Cdu->GetReturnPortWorldM()) * 0.5;
		}
		else
		{
			const FVector FirstAnchorM = RackRearAnchorM(*Row.Racks[0], Row.RearDir, PipeStandoffM);
			NodePositionM[Handles.CduNodeIndex] =
				FVector(FirstAnchorM.X, FirstAnchorM.Y, HeaderHeightM) - Row.AlongDir * 1.5;
		}

		for (int32 ColumnIndex = 0; ColumnIndex < Row.Racks.Num(); ++ColumnIndex)
		{
			const ARackThermalActor* Rack = Row.Racks[ColumnIndex];
			if (Rack == nullptr)
			{
				continue;
			}

			const FVector SupplyAnchorM = RackRearAnchorM(*Rack, Row.RearDir, PipeStandoffM);
			const FVector ReturnAnchorM =
				RackRearAnchorM(*Rack, Row.RearDir, PipeStandoffM + SupplyReturnSpacingM);

			// 헤더는 바닥 높이, 랙 노드는 뒷면 매니폴드 높이다.
			// 그래서 분기관이 바닥에서 랙 뒤로 올라왔다가 다시 내려가는 모양이 된다.
			NodePositionM[Handles.SupplyNodeIndices[ColumnIndex]] =
				FVector(SupplyAnchorM.X, SupplyAnchorM.Y, HeaderHeightM);
			NodePositionM[Handles.ReturnNodeIndices[ColumnIndex]] =
				FVector(ReturnAnchorM.X, ReturnAnchorM.Y, HeaderHeightM);

			if (Handles.RackNodeIndices[ColumnIndex] != INDEX_NONE)
			{
				NodePositionM[Handles.RackNodeIndices[ColumnIndex]] =
					FVector(SupplyAnchorM.X, SupplyAnchorM.Y, RackManifoldZM);
			}

			RackBranchPipeIndices.Add(Handles.RackBranchPipeIndices[ColumnIndex]);
			RackReturnPipeIndices.Add(Handles.RackReturnPipeIndices[ColumnIndex]);
			if (bUseGjConnections && Rack->bUseGjModels && Handles.RackNodeIndices[ColumnIndex] != INDEX_NONE)
			{
				// The single lumped rack node has two distinct visual manifold ports.
				PipeEndOverrideCm.Add(Handles.RackBranchPipeIndices[ColumnIndex], Rack->GjPointWorldCm(FVector(-.65,.245,.50)));
				PipeStartOverrideCm.Add(Handles.RackReturnPipeIndices[ColumnIndex], Rack->GjPointWorldCm(FVector(-.65,-.245,.50)));
			}
		}
		if (bUseGjConnections && Row.Cdu)
		{
			PipeEndOverrideCm.Add(Handles.ReturnToCduPipeIndex, SimUnits::MetersToUnreal(Row.Cdu->GetReturnPortWorldM()));
			// Pump supply was invisible in the primitive renderer (pumps are not pipe edges).
			GjPumpRoutes.Add({Handles.PumpIndex, SimUnits::MetersToUnreal(Row.Cdu->GetSupplyPortWorldM()),
				SimUnits::MetersToUnreal(NodePositionM[Handles.SupplyNodeIndices[0]]), Row.Cdu->GetActorForwardVector()});
		}

		// 관 색을 공급/회수로 나누기 위한 역할표. 솔버는 이걸 모른다.
		bPipeIsSupplySide.SetNum(Network.GetNumPipes());
		for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
		{
			if (Handles.IsSupplySidePipe(PipeIndex))
			{
				bPipeIsSupplySide[PipeIndex] = true;
			}
		}

		// S2b step 2: leaks on this row's racks. The branch is split at mid length (series halves keep the flows);
		// the rack keeps reading the half that enters it (supply) or leaves it (return), so a supply leak lowers the
		// rack's coolant flow. The original pipe keeps its start; its end override (GJ port) moves to the new half.
		TArray<int32> RowLeaks;
		for (FRoomLeakSpec& Leak : LeakSpecs)
		{
			const int32 Column = Row.Racks.IndexOfByKey(Leak.Rack.Get());
			if (Column == INDEX_NONE) continue;
			const int32 Pipe = Leak.bReturnBranch ? Handles.RackReturnPipeIndices[Column] : Handles.RackBranchPipeIndices[Column];
			if (Pipe == INDEX_NONE) continue;
			FVector A = NodePositionM[Network.GetPipeFromNodeIndex(Pipe)], B = NodePositionM[Network.GetPipeToNodeIndex(Pipe)];
			if (const FVector* S = PipeStartOverrideCm.Find(Pipe)) A = SimUnits::UnrealToMeters(*S);
			const FVector* EndOverride = PipeEndOverrideCm.Find(Pipe);
			if (EndOverride) B = SimUnits::UnrealToMeters(*EndOverride);
			const int32 Mid = Network.SplitPipe(Pipe, 0.5, 0.5 * (A.Z + B.Z));
			const int32 NewPipe = Network.GetNumPipes() - 1;
			NodePositionM.SetNum(Network.GetNumNodes());
			NodePositionM[Mid] = 0.5 * (A + B);
			if (EndOverride) { const FVector End = *EndOverride; PipeEndOverrideCm.Remove(Pipe); PipeEndOverrideCm.Add(NewPipe, End); }
			bPipeIsSupplySide.SetNum(Network.GetNumPipes());
			bPipeIsSupplySide[NewPipe] = bPipeIsSupplySide[Pipe];
			LeakBranchPipes.Add(Pipe); LeakBranchPipes.Add(NewPipe);
			if (Handles.RackReturnPipeIndices[Column] != INDEX_NONE) LeakBranchPipes.Add(Handles.RackReturnPipeIndices[Column]);
			if (!Leak.bReturnBranch)
			{
				const int32 Slot = RackBranchPipeIndices.IndexOfByKey(Pipe);
				if (Slot != INDEX_NONE) RackBranchPipeIndices[Slot] = NewPipe;
			}
			FHydraulicLeak Orifice;
			Orifice.NodeIndex = Mid;
			Orifice.HoleDiameterM = Leak.HoleDiameterM;
			Orifice.DischargeCoefficient = Leak.DischargeCoefficient;
			Leak.NetworkLeak = Network.AddLeak(Orifice);
			RowLeaks.Add(Leak.NetworkLeak);
		}
		Inventory.AddTank(Network, Handles.CduNodeIndex, Handles.PumpIndex, RowLeaks,
			Row.Cdu != nullptr ? Row.Cdu->ExpansionTank : FallbackExpansionTank);
	}

	if (Network.GetNumNodes() == 0)
	{
		LastResult = FHydraulicSolveResult();
		return;
	}

	AssignNodeElevationsFromRoutes();


	LastResult = Network.Solve(SolveSettings);
	bPipeColorsDirty = true;

	if (!LastResult.bConverged)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterRoomActor: 수렴 실패 (반복 %d회, 정규화 잔차 %.3e). 표시된 유량을 믿지 마세요."),
			LastResult.IterationsUsed, LastResult.NormalizedResidual);
	}
}

FVector ADataCenterRoomActor::NodeWorldOffsetCm(int32 NodeIndex) const
{
	// 노드 좌표는 월드 m다. 컴포넌트는 액터 기준 상대 cm라 액터 위치를 뺀다 (규약 1).
	return SimUnits::MetersToUnreal(NodePositionM[NodeIndex]) - GetActorLocation();
}

void ADataCenterRoomActor::DestroyMeshes(TArray<TObjectPtr<UStaticMeshComponent>>& Meshes)
{
	for (UStaticMeshComponent* MeshComponent : Meshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	Meshes.Reset();
}

void ADataCenterRoomActor::BuildPipeMeshes()
{
	DestroyMeshes(PipeMeshes);
	PipeMeshFlowIndices.Reset();
	PipeMeshSupply.Reset();
	PipeRoutesM.Reset();
	if (bUseGjConnections) { BuildGjPipeMeshes(); return; }

	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	if (CylinderMesh == nullptr || Network.GetNumPipes() == 0)
	{
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const double HeaderDiameterCm = SimUnits::MetersToCentimeters(HeaderDiameterM);
	const double BranchDiameterCm = SimUnits::MetersToCentimeters(BranchDiameterM);

	PipeMeshes.Reserve(Network.GetNumPipes());
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		// 분기관은 헤더보다 가늘어야 흐름이 읽힌다.
		const bool bIsBranch = RackBranchPipeIndices.Contains(PipeIndex) || LeakBranchPipes.Contains(PipeIndex)
			|| (PipeIndex > 0 && RackBranchPipeIndices.Contains(PipeIndex - 1));

		UStaticMeshComponent* Pipe = SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("Pipe"));
		SimMeshBuilder::SetTube(Pipe,
			NodeWorldOffsetCm(Network.GetPipeFromNodeIndex(PipeIndex)),
			NodeWorldOffsetCm(Network.GetPipeToNodeIndex(PipeIndex)),
			bIsBranch ? BranchDiameterCm : HeaderDiameterCm);
		PipeMeshes.Add(Pipe);
		PipeRoutesM.Add({PipeIndex, bPipeIsSupplySide.IsValidIndex(PipeIndex) && bPipeIsSupplySide[PipeIndex], bIsBranch,
			{NodePositionM[Network.GetPipeFromNodeIndex(PipeIndex)], NodePositionM[Network.GetPipeToNodeIndex(PipeIndex)]}});
	}
}

void ADataCenterRoomActor::BuildDecorMeshes()
{
	const double TDestroy=FPlatformTime::Seconds();
	DestroyMeshes(DecorMeshes);
	DecorBuildTimesMs[0]=(FPlatformTime::Seconds()-TDestroy)*1e3;
	if (bUseGjConnections) { BuildGjDecorMeshes(); return; }

	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	if (CylinderMesh == nullptr || !bShowCableTray)
	{
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	const double CableDiameterCm = SimUnits::MetersToCentimeters(CableDiameterM);

	auto AddTube = [&](const FVector& StartCm, const FVector& EndCm, double DiameterCm, const FLinearColor& Color)
	{
		UStaticMeshComponent* Tube = SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("Decor"));
		SimMeshBuilder::SetTube(Tube, StartCm, EndCm, DiameterCm);
		SimMeshBuilder::SetMeshColor(Tube, Color);
		DecorMeshes.Add(Tube);
	};

	// 액터 상대 좌표로 옮기는 변환. 노드 좌표와 같은 규칙이다 (규약 1).
	auto ToLocalCm = [this](const FVector& WorldM) -> FVector
	{
		return SimUnits::MetersToUnreal(WorldM) - GetActorLocation();
	};

	for (const FRackRow& Row : Rows)
	{
		if (Row.Racks.Num() == 0)
		{
			continue;
		}

		// 트레이는 랙 뒷면 바깥, 배관보다 더 뒤쪽 천장에 건다.
		const double TrayOffsetM = PipeStandoffM + SupplyReturnSpacingM * 2.0;

		const FVector FirstTrayM = RackRearAnchorM(*Row.Racks[0], Row.RearDir, TrayOffsetM);
		const FVector LastTrayM = RackRearAnchorM(*Row.Racks.Last(), Row.RearDir, TrayOffsetM);

		const FVector TrayStartCm =
			ToLocalCm(FVector(FirstTrayM.X, FirstTrayM.Y, CableTrayHeightM)) - Row.AlongDir * 40.0;
		const FVector TrayEndCm =
			ToLocalCm(FVector(LastTrayM.X, LastTrayM.Y, CableTrayHeightM)) + Row.AlongDir * 40.0;

		// 사다리 옆 레일 두 줄. 가로대는 개수가 많아 넣지 않는다.
		const FVector RailOffsetCm = Row.RearDir * 16.0;
		AddTube(TrayStartCm - RailOffsetCm, TrayEndCm - RailOffsetCm, 7.0, TrayColor);
		AddTube(TrayStartCm + RailOffsetCm, TrayEndCm + RailOffsetCm, 7.0, TrayColor);

		// 랙마다 케이블 다발을 트레이에서 뒷면 위쪽으로 내린다.
		for (const TObjectPtr<ARackThermalActor>& Rack : Row.Racks)
		{
			if (Rack == nullptr)
			{
				continue;
			}

			const FVector TrayPointM = RackRearAnchorM(*Rack, Row.RearDir, TrayOffsetM);
			const FVector LandPointM = RackRearAnchorM(*Rack, Row.RearDir, 0.02);
			const double LandZM = Rack->RackSizeM.Z * 0.92;

			for (int32 CableIndex = 0; CableIndex < CablesPerRack; ++CableIndex)
			{
				const double Spread = (CableIndex - (CablesPerRack - 1) * 0.5);
				const FVector SpreadCm = Row.AlongDir * (Spread * 5.0);
				const FVector LandSpreadCm = Row.AlongDir * (Spread * Rack->RackSizeM.Y * 18.0);

				const FVector TrayCm =
					ToLocalCm(FVector(TrayPointM.X, TrayPointM.Y, CableTrayHeightM))
					+ SpreadCm - FVector(0.0, 0.0, 6.0);
				const FVector BendCm =
					FVector(TrayCm.X, TrayCm.Y, SimUnits::MetersToCentimeters(LandZM) + 22.0);
				const FVector LandCm =
					ToLocalCm(FVector(LandPointM.X, LandPointM.Y, LandZM)) + LandSpreadCm;

				const FLinearColor Color = CableColors[CableIndex % NumCableColors];
				AddTube(TrayCm, BendCm, CableDiameterCm, Color);
				AddTube(BendCm, LandCm, CableDiameterCm, Color);
			}
		}
	}
}

bool ADataCenterRoomActor::TryGetPipeTemperatureK(int32 PipeIndex, double& OutTempK) const
{
	OutTempK = 0;
	if (!GetWorld() || !GetWorld()->IsGameWorld() || !LastResult.bConverged ||
		PipeIndex < 0 || PipeIndex >= Network.GetNumPipes() || Network.GetPipeFlowM3s(PipeIndex) <= 0) return false;
	int32 RackIndex = RackBranchPipeIndices.IndexOfByKey(PipeIndex);
	const bool bSupply = RackIndex != INDEX_NONE;
	if (!bSupply) RackIndex = RackReturnPipeIndices.IndexOfByKey(PipeIndex);
	if (RackIndex == INDEX_NONE) return false;
	int32 Index = 0;
	for (const FRackRow& Row : Rows)
		for (const auto& Rack : Row.Racks)
		{
			if (Index++ != RackIndex) continue;
			if (!IsValid(Rack)) return false;
			const FRackCoolantDisplaySnapshot Snapshot = Rack->GetCoolantDisplaySnapshot();
			OutTempK = bSupply ? Snapshot.InletTempK : Snapshot.OutletTempK;
			return Snapshot.bValid && FMath::IsFinite(OutTempK) && OutTempK > 0;
		}
	return false;
}

void ADataCenterRoomActor::UpdatePipeColors() const
{
	const bool bTemperatureMode = PipeTemperatureMode.GetValueOnGameThread() != 0;
	const double NowS = FPlatformTime::Seconds();
	// Wall time throttles display uploads only; never advances or changes the simulation clock.
	const bool bTemperatureRefresh = bTemperatureMode && NowS - LastPipeTemperatureDisplayS >= .25;
	// Colours follow the solved flows, which change only when the network is solved again. Recolouring every pipe
	// segment every editor frame cost time for nothing.
	if (!bPipeColorsDirty && bTemperatureMode == bLastPipeTemperatureMode && !bTemperatureRefresh)
	{
		return;
	}
	bPipeColorsDirty = false;
	bLastPipeTemperatureMode = bTemperatureMode;
	LastPipeTemperatureDisplayS = NowS;
	if (bTemperatureMode)
	{
		// One colour snapshot per hydraulic edge, reused by all its GJ spline spans.
		TArray<FLinearColor> Colors;
		for (int32 Edge = 0; Edge < Network.GetNumPipes(); ++Edge)
		{
			double TempK;
			const bool bValid = TryGetPipeTemperatureK(Edge, TempK);
			Colors.Add(PipeTemperatureDisplay::Color(TempK, bValid));
		}
		for (int32 MeshIndex = 0; MeshIndex < PipeMeshes.Num(); ++MeshIndex)
		{
			const int32 Edge = bUseGjConnections ? PipeMeshFlowIndices[MeshIndex] : MeshIndex;
			SimMeshBuilder::SetMeshColor(PipeMeshes[MeshIndex], Colors.IsValidIndex(Edge) ? Colors[Edge] : PipeTemperatureDisplay::UnknownColor());
		}
		return;
	}
	double MaxFlowMagnitudeM3s = 0.0;
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
	}
	const double NormalizationM3s = FMath::Max(MaxFlowMagnitudeM3s, UE_DOUBLE_SMALL_NUMBER);

	// 색상(hue)은 공급/회수 역할, 밝기는 계산된 유량 크기다.
	// 낮은 쪽을 어둡게 두면 분기관이 검은 랙에 묻혀 화면에서 사라진다.
	static const FLinearColor SupplyLowColor(0.08f, 0.22f, 0.55f);
	static const FLinearColor SupplyHighColor(0.25f, 0.65f, 1.00f);
	static const FLinearColor ReturnLowColor(0.55f, 0.16f, 0.12f);
	static const FLinearColor ReturnHighColor(1.00f, 0.35f, 0.28f);

	for (int32 PipeIndex = 0; PipeIndex < PipeMeshes.Num(); ++PipeIndex)
	{
		const int32 FlowIndex = bUseGjConnections ? PipeMeshFlowIndices[PipeIndex] : PipeIndex;
		const double Flow = FlowIndex >= 0 ? Network.GetPipeFlowM3s(FlowIndex) : Network.GetPumpFlowM3s(-1-FlowIndex);
		const double Normalized = FMath::Abs(Flow) / NormalizationM3s;

		// 제곱근을 씌워 낮은 유량 구간을 펼친다. 표시용 보정이며 계산과 무관하다.
		const float Shaped = FMath::Sqrt(FMath::Clamp(static_cast<float>(Normalized), 0.0f, 1.0f));
		const bool bSupply = bUseGjConnections ? PipeMeshSupply[PipeIndex] : (bPipeIsSupplySide.IsValidIndex(PipeIndex) && bPipeIsSupplySide[PipeIndex]);

		SimMeshBuilder::SetMeshColor(PipeMeshes[PipeIndex],
			FMath::Lerp(bSupply ? SupplyLowColor : ReturnLowColor,
				bSupply ? SupplyHighColor : ReturnHighColor, Shaped));
	}
}

void ADataCenterRoomActor::SpawnRackGrid()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	DestroySpawnedRackGrid();

	const int32 RowCount = FMath::Max(1, NumRows);
	const int32 ColumnCount = FMath::Max(1, RacksPerRow);

	const double HalfDepthM = RackSizeM.X * 0.5;
	const double PairPitchM = AisleWidthM + 2.0 * RackSizeM.X + ServiceAisleWidthM;
	const double SideOffsetM = AisleWidthM * 0.5 + HalfDepthM;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	const FVector OriginM = SimUnits::UnrealToMeters(GetActorLocation());

	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		// 두 줄이 통로 하나를 사이에 두고 마주 본다.
		const int32 PairIndex = RowIndex / 2;
		const bool bSecondSide = (RowIndex % 2) == 1;
		const double RowXM = PairIndex * PairPitchM + (bSecondSide ? SideOffsetM : -SideOffsetM);

		// 앞면이 통로를 향하게 돌린다.
		const FRotator Rotation(0.0, bSecondSide ? 180.0 : 0.0, 0.0);

		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			const FVector LocationM = OriginM + FVector(RowXM, ColumnIndex * RackPitchM, 0.0);

			ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(
				ARackThermalActor::StaticClass(), SimUnits::MetersToUnreal(LocationM), Rotation, SpawnParams);
			if (Rack == nullptr)
			{
				continue;
			}

			// GB200 NVL72 프리셋. 컴퓨트 트레이 18장, 트레이당 Grace 2 + Blackwell 4.
			Rack->RackSizeM = RackSizeM;
			Rack->bUseGjModels = bUseGjConnections;
			// 유량과 라디에이터도 같은 운전점에서 역산한 값으로 맞춘다.
			Rack->ApplyGb200Nvl72Preset();
			Rack->Radiator.UAWK = 1.1e4;

			// 방 전체에서는 유닛 라벨이 너무 많아진다. 랙 하나를 볼 때만 켠다.
			Rack->bShowSledLabels = false;
			Rack->bShowReadout = false;
			Rack->ResetSim();

			GridSpawnedRacks.Add(Rack);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("DataCenterRoomActor: 랙 %d대를 찍어 냈습니다. 이제 자유롭게 옮길 수 있습니다."),
		GridSpawnedRacks.Num());

	bNeedsRebuild = true;
}

void ADataCenterRoomActor::DestroySpawnedRackGrid()
{
	for (ARackThermalActor* Rack : GridSpawnedRacks)
	{
		if (Rack != nullptr)
		{
			Rack->Destroy();
		}
	}
	GridSpawnedRacks.Reset();
	bNeedsRebuild = true;
}

void ADataCenterRoomActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// A moved rack / CDU changes the layout hash every frame while it is dragged. Rebuilding on each of those
	// frames froze the editor (a full rebuild takes seconds with the GJ meshes), so wait until the layout has
	// stopped changing for LayoutSettleS of real time, then rebuild once. Explicit RebuildRoom() stays immediate.
	if (bUseGjConnections)
	{
		const uint32 Hash = GjLayoutHash();
		const double NowS = FPlatformTime::Seconds();
		if (Hash == LastGjLayoutHash)
		{
			PendingLayoutHash = Hash;
		}
		else if (Hash != PendingLayoutHash)
		{
			PendingLayoutHash = Hash;
			LayoutChangedAtS = NowS;
		}
		else if (NowS - LayoutChangedAtS >= LayoutSettleS)
		{
			bNeedsRebuild = true;
		}
	}

	if (bNeedsRebuild)
	{
		RebuildTimed();
	}
	DrainPendingSegments(MeshBuildBudgetMs);

	UpdatePipeColors();

	if (bShowSummary)
	{
		DrawSummary();
	}
}

uint32 ADataCenterRoomActor::GjLayoutHash() const
{
	uint32 Hash=0;
	if (!GetWorld()) return Hash;
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It)
	{
		Hash=HashCombine(Hash,GetTypeHash(It->GetActorLocation()));
		Hash=HashCombine(Hash,GetTypeHash(It->GetActorRotation().Euler()));
		Hash=HashCombine(Hash,GetTypeHash(It->GetActorScale3D()));
		Hash=HashCombine(Hash,GetTypeHash(It->RackSizeM));
		Hash=HashCombine(Hash,GetTypeHash(It->GetNumDockedSleds()));
	}
	for (TActorIterator<ASpineCabinetActor> It(GetWorld()); It; ++It)
	{
		Hash=HashCombine(Hash,GetTypeHash(It->GetActorLocation()));
		Hash=HashCombine(Hash,GetTypeHash(It->GetActorRotation().Euler()));
		Hash=HashCombine(Hash,GetTypeHash(It->NumSwitches));
	}
	for (TActorIterator<ACoolingDistributionUnitActor> It(GetWorld()); It; ++It)
	{
		Hash=HashCombine(Hash,GetTypeHash(It->GetSupplyPortWorldM()));
		Hash=HashCombine(Hash,GetTypeHash(It->GetReturnPortWorldM()));
		Hash=HashCombine(Hash,GetTypeHash(It->PumpSpeedRatio));
	}
	return Hash;
}

void ADataCenterRoomActor::RefreshGjConnections()
{
	RebuildTimed();
	DrainPendingSegments(0.0);
	UpdatePipeColors();
}

int32 ADataCenterRoomActor::AddPendingAsset(UStaticMesh* Body, UMaterialInterface* Surface)
{
	for (int32 I = 0; I + 1 < PendingAssets.Num(); I += 2)
		if (PendingAssets[I] == Body && PendingAssets[I + 1] == Surface) return I;
	PendingAssets.Add(Body); PendingAssets.Add(Surface);
	return PendingAssets.Num() - 2;
}

void ADataCenterRoomActor::QueuePath(const TArray<FVector>& LocalPointsCm, UStaticMesh* Body, UMaterialInterface* Surface,
	double BendCm, double Scale, bool bPipe, int32 Edge, bool bSupply)
{
	TArray<GjConnectionMesh::FSplineSegment> Segments;
	GjConnectionMesh::BuildSegments(LocalPointsCm, BendCm, Segments);
	const int32 Asset = AddPendingAsset(Body, Surface);
	for (const GjConnectionMesh::FSplineSegment& S : Segments)
		PendingSegments.Add({S, Asset, Scale, bPipe, Edge, bSupply});
}

void ADataCenterRoomActor::DrainPendingSegments(double BudgetMs)
{
	if (PendingCursor >= PendingSegments.Num()) return;
	const double StartS = FPlatformTime::Seconds();
	++PendingFrames;
	bool bAnyPipe = false;
	while (PendingCursor < PendingSegments.Num())
	{
		const FPendingSegment& P = PendingSegments[PendingCursor++];
		UStaticMesh* Body = Cast<UStaticMesh>(PendingAssets[P.Asset]);
		UMaterialInterface* Surface = Cast<UMaterialInterface>(PendingAssets[P.Asset + 1]);
		if (UStaticMeshComponent* M = GjConnectionMesh::AddSegment(this, P.Segment, Body, Surface, P.Scale))
		{
			if (P.bPipe)
			{
				PipeMeshes.Add(M); PipeMeshFlowIndices.Add(P.Edge); PipeMeshSupply.Add(P.bSupply);
				M->ComponentTags.Add(*FString::Printf(TEXT("HydraulicEdge_%d"), P.Edge));
				bAnyPipe = true;
			}
			else DecorMeshes.Add(M);
		}
		if (BudgetMs > 0.0 && (FPlatformTime::Seconds() - StartS) * 1e3 >= BudgetMs) break;
	}
	if (bAnyPipe) bPipeColorsDirty = true;
	if (PendingCursor >= PendingSegments.Num())
	{
		UE_LOG(LogTemp, Display, TEXT("[Room] 배관·케이블 메시 %d개 생성 완료 (%.0f ms, %d프레임에 나눔)"),
			PendingSegments.Num(), (FPlatformTime::Seconds() - PendingStartS) * 1e3, PendingFrames);
		PendingSegments.Reset(); PendingCursor = 0; PendingAssets.Reset();
	}
}

void ADataCenterRoomActor::RebuildTimed()
{
	PendingSegments.Reset(); PendingCursor = 0; PendingAssets.Reset(); PendingFrames = 0;
	PendingStartS = FPlatformTime::Seconds();
	// Stage timing in the log: a full rebuild takes seconds with the GJ meshes, so it is worth knowing where.
	double T[5]; T[0] = FPlatformTime::Seconds();
	DiscoverRows(); T[1] = FPlatformTime::Seconds();
	BuildNetwork(); T[2] = FPlatformTime::Seconds();
	BuildPipeMeshes(); T[3] = FPlatformTime::Seconds();
	BuildDecorMeshes(); T[4] = FPlatformTime::Seconds();
	bNeedsRebuild = false; bRebuiltSinceLastQuery = true; LastGjLayoutHash = PendingLayoutHash = GjLayoutHash();
	UE_LOG(LogTemp, Display, TEXT("[Room] 재구축 %.0f ms (줄 %.0f, 수력 %.0f, 배관 메시 %.0f, 케이블·장식 %.0f = 지우기 %.0f + 배선 계획 %.0f + 간섭 검사 %.0f + 케이블 메시 %.0f) | 컴포넌트 배관 %d, 장식 %d | 캐비닛 관통 배관 구간 %d개 (돌려 간 CDU 연결 %d개, 원래 경로는 %d구간 관통)"),
		(T[4] - T[0]) * 1e3, (T[1] - T[0]) * 1e3, (T[2] - T[1]) * 1e3, (T[3] - T[2]) * 1e3, (T[4] - T[3]) * 1e3,
		DecorBuildTimesMs[0], DecorBuildTimesMs[1], DecorBuildTimesMs[2], DecorBuildTimesMs[3], PipeMeshes.Num(), DecorMeshes.Num(), LastPipeCabinetHits, LastRoutedAround, LastPreferredHits);
}

void ADataCenterRoomActor::GatherCabinetBoxes()
{
	CabinetBoxes.Reset();
	UWorld* World = GetWorld();
	if (World == nullptr) return;
	auto Add = [&](const AActor* A, const FVector& SizeM)
	{
		// Unrotated, unscaled frame: the rack geometry uses RackSizeM, not the actor scale.
		FCabinetBox Box;
		Box.ToWorld = FTransform(A->GetActorQuat(), A->GetActorLocation());
		Box.HalfCm = FVector(SizeM.X * 50.0, SizeM.Y * 50.0, 0.0);
		Box.HeightCm = SizeM.Z * 100.0;
		CabinetBoxes.Add(Box);
	};
	for (TActorIterator<ARackThermalActor> It(World); It; ++It) Add(*It, It->RackSizeM);
	for (TActorIterator<ASpineCabinetActor> It(World); It; ++It)
		Add(*It, FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM));
	for (TActorIterator<ACrahUnitActor> It(World); It; ++It) Add(*It, It->SizeM);
}

bool ADataCenterRoomActor::SegmentHitsCabinet(const FVector& ACm, const FVector& BCm) const
{
	// Slab test in each box frame, shrunk 1 cm so a pipe touching a rear rail or a port does not count.
	constexpr double ShrinkCm = 1.0;
	for (const FCabinetBox& Box : CabinetBoxes)
	{
		const FVector A = Box.ToWorld.InverseTransformPosition(ACm), B = Box.ToWorld.InverseTransformPosition(BCm);
		const FVector Lo(-Box.HalfCm.X + ShrinkCm, -Box.HalfCm.Y + ShrinkCm, ShrinkCm);
		const FVector Hi(Box.HalfCm.X - ShrinkCm, Box.HalfCm.Y - ShrinkCm, Box.HeightCm - ShrinkCm);
		double T0 = 0.0, T1 = 1.0;
		bool bHit = true;
		for (int32 Axis = 0; Axis < 3 && bHit; ++Axis)
		{
			const double D = B[Axis] - A[Axis];
			if (FMath::Abs(D) < 1e-9) { bHit = A[Axis] > Lo[Axis] && A[Axis] < Hi[Axis]; continue; }
			double U0 = (Lo[Axis] - A[Axis]) / D, U1 = (Hi[Axis] - A[Axis]) / D;
			if (U0 > U1) Swap(U0, U1);
			T0 = FMath::Max(T0, U0); T1 = FMath::Min(T1, U1);
			bHit = T0 < T1;
		}
		if (bHit) return true;
	}
	return false;
}

int32 ADataCenterRoomActor::CountCabinetHits(const TArray<FVector>& PointsCm) const
{
	int32 Hits = 0;
	for (int32 I = 0; I + 1 < PointsCm.Num(); ++I) Hits += SegmentHitsCabinet(PointsCm[I], PointsCm[I + 1]) ? 1 : 0;
	return Hits;
}

TArray<FVector> ADataCenterRoomActor::RouteAroundCabinets(const TArray<FVector>& Preferred, const FVector& From,
	const FVector& FromLead, const FVector& To, const FVector& ToLead) const
{
	if (CountCabinetHits(Preferred) == 0) return Preferred;
	const FVector A1 = From + FromLead * 30.0, B1 = To + ToLead * 30.0;
	auto Path = [&](std::initializer_list<FVector> Mid)
	{
		TArray<FVector> P; P.Add(From);
		if (!FromLead.IsNearlyZero()) P.Add(A1);
		for (const FVector& M : Mid) P.Add(M);
		if (!ToLead.IsNearlyZero()) P.Add(B1);
		P.Add(To);
		return P;
	};
	// Floor-level L at the lead height, both corner orders.
	for (const TArray<FVector>& Candidate : {Path({FVector(B1.X, A1.Y, A1.Z), FVector(B1.X, B1.Y, A1.Z)}),
		Path({FVector(A1.X, B1.Y, A1.Z), FVector(B1.X, B1.Y, A1.Z)})})
		if (CountCabinetHits(Candidate) == 0) return Candidate;
	// Overhead, above every cabinet (overhead piping), both corner orders.
	double TopCm = 0.0;
	for (const FCabinetBox& Box : CabinetBoxes) TopCm = FMath::Max(TopCm, Box.ToWorld.GetLocation().Z + Box.HeightCm);
	const double Z = TopCm + 25.0;
	for (const TArray<FVector>& Candidate : {Path({FVector(A1.X, A1.Y, Z), FVector(B1.X, A1.Y, Z), FVector(B1.X, B1.Y, Z)}),
		Path({FVector(A1.X, A1.Y, Z), FVector(A1.X, B1.Y, Z), FVector(B1.X, B1.Y, Z)})})
		if (CountCabinetHits(Candidate) == 0) return Candidate;
	return Preferred;  // nothing clear: keep it and let the count in the log show it
}

void ADataCenterRoomActor::BuildGjPipeMeshes()
{
	GatherCabinetBoxes();
	LastPipeCabinetHits = LastRoutedAround = LastPreferredHits = 0;
	UStaticMesh* Header=GjConnectionMesh::Mesh(TEXT("SM_Connection_Header"));
	UStaticMesh* Riser=GjConnectionMesh::Mesh(TEXT("SM_Connection_Riser"));
	UMaterialInterface* Surface=GjConnectionMesh::Material(TEXT("MC_FlowBody"));
	if (!Header || !Riser || !Surface)
	{
		UE_LOG(LogTemp,Error,TEXT("Missing GJ pipe assets. Run Tools/install_gj_models.py.")); return;
	}
	auto Add=[&](TArray<FVector> Points, bool Branch, int32 Edge, bool Supply)
	{
		LastPipeCabinetHits += CountCabinetHits(Points);
		FRoomPipeRoute& Route=PipeRoutesM.AddDefaulted_GetRef(); Route.Edge=Edge; Route.bSupply=Supply; Route.bBranch=Branch;
		for (const FVector& P:Points) Route.PointsM.Add(SimUnits::UnrealToMeters(P));
		for (FVector& P:Points) P=GetActorTransform().InverseTransformPosition(P);
		QueuePath(Points,Branch?Riser:Header,Surface,Branch?6.0:14.0,Branch?BranchDiameterM/.05:HeaderDiameterM/.11,true,Edge,Supply);
	};
	for (int32 I=0;I<Network.GetNumPipes();++I)
	{
		FVector A=SimUnits::MetersToUnreal(NodePositionM[Network.GetPipeFromNodeIndex(I)]);
		FVector B=SimUnits::MetersToUnreal(NodePositionM[Network.GetPipeToNodeIndex(I)]);
		const FVector* Start=PipeStartOverrideCm.Find(I); const FVector* End=PipeEndOverrideCm.Find(I);
		if(Start) A=*Start; if(End) B=*End;
		const bool Branch=RackBranchPipeIndices.Contains(I) || LeakBranchPipes.Contains(I) || (I>0 && RackBranchPipeIndices.Contains(I-1));
		TArray<FVector> Points={A,B};
		if (Branch) Points={A,FVector(A.X,A.Y,FMath::Max(A.Z,B.Z)),FVector(B.X,B.Y,FMath::Max(A.Z,B.Z)),B};
		// Return terminal: honour the real CDU return port and its outward direction.
		if (!Branch && End)
		{
			for (const FRackRow& Row:Rows) if(Row.Cdu && B.Equals(SimUnits::MetersToUnreal(Row.Cdu->GetReturnPortWorldM()),.01))
			{
				const FVector D=Row.Cdu->GetActorForwardVector(), Lead=B+D*30;
				const TArray<FVector> Preferred={A,A-D*FVector::DotProduct(A-Lead,D),Lead,B};
				Points=RouteAroundCabinets(Preferred,A,FVector::ZeroVector,B,D);
				LastRoutedAround += Points!=Preferred ? 1 : 0; LastPreferredHits += CountCabinetHits(Preferred);
				break;
			}
		}
		Add(Points,Branch,I,bPipeIsSupplySide[I]);
	}
	for (const FGjPumpRoute& R:GjPumpRoutes)
	{
		const FVector Lead=R.StartCm+R.Outward*30;
		const TArray<FVector> Preferred={R.StartCm,Lead,R.EndCm-R.Outward*FVector::DotProduct(R.EndCm-Lead,R.Outward),R.EndCm};
		const TArray<FVector> Points=RouteAroundCabinets(Preferred,R.StartCm,R.Outward,R.EndCm,FVector::ZeroVector);
		LastRoutedAround += Points!=Preferred ? 1 : 0; LastPreferredHits += CountCabinetHits(Preferred);
		Add(Points,false,-1-R.Pump,true);
	}
}

void ADataCenterRoomActor::BuildGjDecorMeshes()
{
	PatchPlan=Patch::FPlan();
	PatchDevices.Reset();
	UStaticMesh* Cable=GjConnectionMesh::Mesh(TEXT("SM_Connection_Data"));
	UStaticMesh* Plug=GjConnectionMesh::Mesh(TEXT("SM_Plug_Switch"));
	UStaticMesh* Adapter=GjConnectionMesh::Mesh(TEXT("SM_CDU_Adapter"));
	if (!Cable || !Plug || !Adapter) return;
	auto Fit=[&](UStaticMesh* Mesh,const FVector& WorldCm,const FVector& Direction)
	{
		auto* C=SimMeshBuilder::CreateMeshComponent(this,Mesh,nullptr,TEXT("GJ_Fitting"));
		C->SetWorldLocationAndRotation(WorldCm,Direction.Rotation()); DecorMeshes.Add(C);
	};
	for (const FRackRow& Row:Rows) if(Row.Cdu)
	{
		Fit(Adapter,SimUnits::MetersToUnreal(Row.Cdu->GetSupplyPortWorldM()),Row.Cdu->GetActorForwardVector());
		Fit(Adapter,SimUnits::MetersToUnreal(Row.Cdu->GetReturnPortWorldM()),Row.Cdu->GetActorForwardVector());
	}
	if(!bShowCableTray) return;

	// Patch manager input: GJ racks and spine cabinets per row, tray centreline over each row's hot aisle.
	Patch::FInput In; In.SpineLinksPerRowPair=SpineLinksPerRowPair;
	const int32 Uplinks=FMath::Clamp(CablesPerRack,0,GjRackLayout::PortsPerSwitchRow);
	for (const FRackRow& Row:Rows)
	{
		if (Row.Racks.IsEmpty() || !Row.Racks[0]) continue;
		In.Rows.Add({RackRearAnchorM(*Row.Racks[0],Row.RearDir,TrayStandoffM)*FVector(1,1,0)+FVector(0,0,CableTrayHeightM),
			Row.AlongDir,Row.RearDir});
		for (ARackThermalActor* Rack:Row.Racks)
		{
			if (!Rack || !Rack->bUseGjModels) continue;
			Patch::FDevice D; D.Row=In.Rows.Num()-1; D.Label=Rack->GetActorNameOrLabel();
			D.ToWorld=FTransform(Rack->GetActorQuat(),SimUnits::UnrealToMeters(Rack->GetActorLocation()));
			D.HalfExtentM=Rack->RackSizeM*.5; D.HeightM=Rack->RackSizeM.Z; D.Outward=-Rack->GetActorForwardVector();
			for (int32 Col=1;Col<=GjRackLayout::PortsPerSwitchRow;++Col)
				D.PortM.Add(SimUnits::UnrealToMeters(Rack->GjPointWorldCm(GjRackLayout::SwitchPortM(Rack->GetUplinkSwitchZM(),Col,true))));
			// Upper-row cages centred on the switch; the lower row belongs to the node harness.
			for (int32 J=0;J<Uplinks;++J) D.UplinkPorts.Add(GjRackLayout::PortsPerSwitchRow/2-Uplinks/2+J);
			In.Devices.Add(MoveTemp(D));
		}
	}
	for (TActorIterator<ASpineCabinetActor> It(GetWorld()); It; ++It)
	{
		Patch::FDevice D; D.bSpine=true; D.Label=It->GetActorNameOrLabel();
		const FVector LocM=SimUnits::UnrealToMeters(It->GetActorLocation());
		D.ToWorld=FTransform(It->GetActorQuat(),LocM); D.Outward=It->GetPortOutwardWorld();
		for (int32 P=0;P<It->GetNumPorts();++P) D.PortM.Add(It->GetPortWorldM(P));
		// Same row = same facing and the same rear line as the row's racks.
		for (int32 R=0;R<In.Rows.Num();++R)
		{
			const Patch::FTrayRow& T=In.Rows[R];
			const double Off=FVector::DotProduct(LocM-T.OriginM,T.Out)+TrayStandoffM+GjRackLayout::FrameDepthM*.5;
			if (FVector::DotProduct(D.Outward,T.Out)>.9 && FMath::Abs(Off)<RowGroupToleranceM) { D.Row=R; break; }
		}
		In.Devices.Add(MoveTemp(D));
	}
	const double TPlan0=FPlatformTime::Seconds();
	DecorBuildTimesMs[3]=0.0;
	PatchPlan=Patch::PlanPatches(In);
	PatchDevices=In.Devices;
	const double TPlan1=FPlatformTime::Seconds();
	// Authored diameter, Art/GJ_PIPE&CABLE/reports/connection_manifest.json (estimated visual asset).
	PatchPlan.Interferences=Patch::FindInterferences(PatchPlan.Cables,.0048);
	const double TPlan2=FPlatformTime::Seconds();
	DecorBuildTimesMs[1]=(TPlan1-TPlan0)*1e3; DecorBuildTimesMs[2]=(TPlan2-TPlan1)*1e3;

	UMaterialInterface* Uplink=GjConnectionMesh::Material(TEXT("MC_Cyan"));
	UMaterialInterface* Trunk=GjConnectionMesh::Material(TEXT("MC_White"));
	for (const Patch::FCable& C:PatchPlan.Cables)
	{
		TArray<FVector> Points;
		for (const FVector& P:C.PointsM) Points.Add(GetActorTransform().InverseTransformPosition(SimUnits::MetersToUnreal(P)));
		QueuePath(Points,Cable,C.bSpineToSpine?Trunk:Uplink,6,1.0,false,INDEX_NONE,false);
		Fit(Plug,SimUnits::MetersToUnreal(C.PointsM[0]),In.Devices[C.FromDevice].Outward);
		Fit(Plug,SimUnits::MetersToUnreal(C.PointsM.Last()),In.Devices[C.ToDevice].Outward);
		DecorBuildTimesMs[3]=(FPlatformTime::Seconds()-TPlan2)*1e3;
	}
	// Ladder tray just under the lowest cable layer: two rails and rungs every 0.5 m.
	UStaticMesh* Cylinder=SimMeshBuilder::LoadCylinderMesh();
	UMaterialInterface* Colorable=SimMeshBuilder::LoadColorableMaterial();
	auto Tube=[&](const FVector& AM,const FVector& BM,double DiameterCm)
	{
		auto* T=SimMeshBuilder::CreateMeshComponent(this,Cylinder,Colorable,TEXT("GJ_Tray"));
		SimMeshBuilder::SetTube(T,GetActorTransform().InverseTransformPosition(SimUnits::MetersToUnreal(AM)),
			GetActorTransform().InverseTransformPosition(SimUnits::MetersToUnreal(BM)),DiameterCm);
		SimMeshBuilder::SetMeshColor(T,TrayColor); DecorMeshes.Add(T);
	};
	const double HalfWidthM=FMath::Max(1,In.LanesPerLayer)*In.LanePitchM*.5+.03;
	for (const TPair<FVector,FVector>& Run:PatchPlan.TrayRunsM)
	{
		if (!Cylinder) break;
		const FVector A=Run.Key-FVector(0,0,.02), B=Run.Value-FVector(0,0,.02), Dir=(B-A).GetSafeNormal();
		const FVector Side=FVector::CrossProduct(FVector::UpVector,Dir)*HalfWidthM;
		Tube(A-Side-Dir*HalfWidthM,B-Side+Dir*HalfWidthM,3); Tube(A+Side-Dir*HalfWidthM,B+Side+Dir*HalfWidthM,3);
		for (double X=0;X<=FVector::Distance(A,B);X+=.5) Tube(A+Dir*X-Side,A+Dir*X+Side,1.5);
	}
}

void ADataCenterRoomActor::SpawnSpineCabinets()
{
	UWorld* World=GetWorld();
	if (!World) return;
	DiscoverRows();
	TArray<FVector> Existing;
	for (TActorIterator<ASpineCabinetActor> It(World); It; ++It) Existing.Add(SimUnits::UnrealToMeters(It->GetActorLocation()));
	for (const FRackRow& Row:Rows)
	{
		if (Row.Racks.Num()==0 || !Row.Racks[0] || !Row.Racks.Last()) continue;
		const FVector First=SimUnits::UnrealToMeters(Row.Racks[0]->GetActorLocation());
		const FVector Last=SimUnits::UnrealToMeters(Row.Racks.Last()->GetActorLocation());
		// Tightest neighbour spacing: rows may have gaps (L_Test row B), the cabinet goes one slot past the end.
		double Pitch=Row.Racks[0]->RackSizeM.Y+.06;
		for (int32 I=1;I<Row.Racks.Num();++I) if (Row.Racks[I] && Row.Racks[I-1])
			Pitch=FMath::Min(Pitch,FVector::Distance(Row.Racks[I]->GetActorLocation(),Row.Racks[I-1]->GetActorLocation())/100.0);
		// Row end with the larger world coordinate along the row axis, so parallel rows pick the same side.
		const FVector Axis=(Row.AlongDir.X+Row.AlongDir.Y)>=0?Row.AlongDir:-Row.AlongDir;
		const FVector A=First-Row.AlongDir*Pitch, B=Last+Row.AlongDir*Pitch;
		const FVector Spot=FVector::DotProduct(A,Axis)>FVector::DotProduct(B,Axis)?A:B;
		bool bTaken=false;  // a row already has a spine if one stands on its rear line
		for (const FVector& E:Existing) bTaken|=FMath::Abs(FVector::DotProduct(E-First,Row.RearDir))<RowGroupToleranceM;
		if (bTaken) continue;
		FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<ASpineCabinetActor>(SimUnits::MetersToUnreal(Spot),Row.Racks[0]->GetActorRotation(),Params);
		Existing.Add(Spot);
	}
	bNeedsRebuild=true;
}

void ADataCenterRoomActor::DrawSummary() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	if (Rows.Num() == 0)
	{
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f, FColor::Orange,
			FString::Printf(TEXT("[%s] 레벨에서 랙을 찾지 못했습니다. 랙을 놓거나 SpawnRackGrid를 누르세요."),
				*GetName()));
		return;
	}

	double TotalPumpFlowM3s = 0.0;
	double TotalPumpElectricalW = 0.0;
	int32 NumCduRows = 0;
	for (const FRackRow& Row : Rows)
	{
		if (Row.PumpIndex == INDEX_NONE)
		{
			continue;
		}
		TotalPumpFlowM3s += Network.GetPumpFlowM3s(Row.PumpIndex);
		TotalPumpElectricalW += Network.GetPumpElectricalInputPowerW(Row.PumpIndex);
		NumCduRows += (Row.Cdu != nullptr) ? 1 : 0;
	}

	double MinBranchFlowM3s = TNumericLimits<double>::Max();
	double MaxBranchFlowM3s = 0.0;
	for (const int32 PipeIndex : RackBranchPipeIndices)
	{
		if (PipeIndex == INDEX_NONE)
		{
			continue;
		}
		const double FlowM3s = FMath::Abs(Network.GetPipeFlowM3s(PipeIndex));
		MinBranchFlowM3s = FMath::Min(MinBranchFlowM3s, FlowM3s);
		MaxBranchFlowM3s = FMath::Max(MaxBranchFlowM3s, FlowM3s);
	}
	if (MaxBranchFlowM3s <= 0.0)
	{
		MinBranchFlowM3s = 0.0;
	}

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		LastResult.bConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[%s] %s | 랙 %d대 / 줄 %d개 (CDU 연결 %d줄) | Newton %d회 | 정규화 잔차 %.2e"),
			*GetName(),
			LastResult.bConverged ? TEXT("수렴") : TEXT("수렴 실패 - 아래 값 믿지 말 것"),
			GetNumRoutedRacks(), Rows.Num(), NumCduRows,
			LastResult.IterationsUsed, LastResult.NormalizedResidual));

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("펌프 합계 유량 %.2f L/s | 전기 입력 %.2f kW | 랙 분기 %.2f ~ %.2f L/s | 펌프 r=%.2f"),
			TotalPumpFlowM3s * 1000.0, TotalPumpElectricalW / 1000.0,
			MinBranchFlowM3s * 1000.0, MaxBranchFlowM3s * 1000.0, LastPumpSpeedRatio));

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f, FColor(150, 150, 150),
		TEXT("배관: 파랑 = 공급, 빨강 = 회수 (색상은 역할, 밝기는 계산된 유량 크기). 온도를 그린 것이 아니다."));

	if (bUseGjConnections && bShowCableTray)
	{
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f,
			PatchPlan.Unconnected.IsEmpty() && PatchPlan.RejectedClaims == 0 ? FColor(150, 200, 255) : FColor::Orange,
			PatchPlan.Summary());
	}
}

double ADataCenterRoomActor::GetTotalPumpElectricalW() const
{
	double TotalW = 0.0;
	for (const FRackRow& Row : Rows)
	{
		if (Row.PumpIndex != INDEX_NONE) TotalW += Network.GetPumpElectricalInputPowerW(Row.PumpIndex);
	}
	return TotalW;
}


// ---- S2b step 2: leak console (Tools/S2b_누수_계획.md) ----
namespace
{
	TArray<ADataCenterRoomActor*> LeakRoomActors()
	{
		TArray<ADataCenterRoomActor*> Out;
		if (GEngine == nullptr) return Out;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It) Out.Add(*It);
		}
		return Out;
	}
}

static FAutoConsoleCommand GVrgLeakAddCommand(
	TEXT("vrg.Leak.Add"),
	TEXT("랙 분기 누수를 넣는다(배관 위상이 바뀌므로 방을 다시 짓는다. 시뮬레이션 진입 전에). vrg.Leak.Add <랙 번호(줄 순서)> <supply|return> <구멍 지름 mm> [Cd]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterRoomActor* Room : LeakRoomActors())
		{
			if (Args.Num() < 3) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 사용법: vrg.Leak.Add <랙 번호> <supply|return> <mm> [Cd]")); return; }
			if (Room->GetRoutedRacks().Num() == 0) Room->RebuildRoomNow();   // right after load the room has not found its racks yet
			const TArray<ARackThermalActor*> Racks = Room->GetRoutedRacks();
			const int32 Index = FCString::Atoi(*Args[0]);
			if (!Racks.IsValidIndex(Index)) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 랙 번호 %d 없음 (0~%d)"), Index, Racks.Num() - 1); return; }
			const bool bReturn = Args[1].ToLower() == TEXT("return");
			const double Cd = Args.Num() >= 4 ? FCString::Atod(*Args[3]) : 0.61;
			const int32 Spec = Room->AddLeak(Racks[Index], bReturn, FCString::Atod(*Args[2]) * 1e-3, Cd);
			UE_LOG(LogTemp, Display, TEXT("[Leak] 누수 %d 추가: 랙 %d(%s) %s 분기, 구멍 %.2f mm, Cd %.2f — 방을 다시 짓는다"),
				Spec, Index, *Racks[Index]->GetName(), bReturn ? TEXT("회수") : TEXT("공급"), FCString::Atod(*Args[2]), Cd);
		}
	}));

static FAutoConsoleCommand GVrgLeakSizeCommand(
	TEXT("vrg.Leak.Size"),
	TEXT("누수 구멍 크기를 바로 바꾼다(다시 짓지 않음). vrg.Leak.Size <누수 번호> <mm>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterRoomActor* Room : LeakRoomActors())
		{
			if (Args.Num() < 2 || !Room->SetLeakHoleDiameterM(FCString::Atoi(*Args[0]), FCString::Atod(*Args[1]) * 1e-3))
			{
				UE_LOG(LogTemp, Warning, TEXT("[Leak] 사용법: vrg.Leak.Size <누수 번호 0~%d> <mm>"), Room->GetNumLeakSpecs() - 1);
				continue;
			}
			UE_LOG(LogTemp, Display, TEXT("[Leak] 누수 %s 구멍 %s mm"), *Args[0], *Args[1]);
		}
	}));

static FAutoConsoleCommand GVrgLeakClearCommand(
	TEXT("vrg.Leak.Clear"),
	TEXT("누수를 모두 지운다(방을 다시 짓는다)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		for (ADataCenterRoomActor* Room : LeakRoomActors()) { Room->ClearLeaks(); UE_LOG(LogTemp, Display, TEXT("[Leak] 누수 모두 지움")); }
	}));

static FAutoConsoleCommand GVrgLeakStatusCommand(
	TEXT("vrg.Leak.Status"),
	TEXT("줄별 팽창탱크·재고·누수 상태를 로그에 쓴다."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		for (ADataCenterRoomActor* Room : LeakRoomActors())
		{
			const FCoolantInventory& Inv = Room->GetCoolantInventory();
			const FHydraulicNetwork& Net = Room->GetNetwork();
			for (int32 T = 0; T < Inv.GetTanks().Num(); ++T)
			{
				const FCoolantInventory::FTank& Tank = Inv.GetTanks()[T];
				UE_LOG(LogTemp, Display, TEXT("[Leak] 줄 %d: 탱크 %.1f L, 사전 충전 = 충전 - %.0f kPa, 압력 %.1f kPa(g) (충전 %.1f), 물 %.3f / %.3f L, 누수 %d개, 누적 누수 %.3f L, 보충 %s, 고갈 %s, 펌프 %.3f L/s"),
					T, Tank.Params.TankVolumeM3 * 1e3, Tank.Params.PrechargeBelowFillPa * 1e-3, Inv.GetTankGaugePressurePa(T) * 1e-3, Tank.FillGaugePa * 1e-3,
					Tank.State.WaterVolumeM3 * 1e3, Tank.State.InitialWaterM3 * 1e3, Tank.LeakIndices.Num(), Tank.State.LeakedM3 * 1e3,
					Tank.Params.bMakeupWater ? TEXT("켬") : TEXT("끔"), Tank.State.bDepleted ? TEXT("예") : TEXT("아니오"), Net.GetPumpFlowM3s(Tank.PumpIndex) * 1e3);
				for (const int32 L : Tank.LeakIndices)
					UE_LOG(LogTemp, Display, TEXT("[Leak]   누수 망 %d: 구멍 %.2f mm, Cd %.2f, 노드 정압 %.1f kPa(g), 높이 %.3f m, 유량 %.4f L/s"),
						L, Net.GetLeak(L).HoleDiameterM * 1e3, Net.GetLeak(L).DischargeCoefficient, Net.GetNodePressurePa(Net.GetLeak(L).NodeIndex) * 1e-3,
						Net.GetNodeElevationM(Net.GetLeak(L).NodeIndex), Net.GetLeakFlowM3s(L) * 1e3);
			}
		}
	}));
