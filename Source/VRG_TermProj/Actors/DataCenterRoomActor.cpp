// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/DataCenterRoomActor.h"

#include "Actors/RackThermalActor.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Visualization/SimMeshBuilder.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
	// 케이블 색. 실제 데이터센터에서 쓰는 색 구분(관리망/데이터/전원)을 흉내 낸 것이며
	// 계산 결과가 아니다. 배관 색(유량)과 헷갈리지 않게 채도를 다르게 뒀다.
	const FLinearColor CableColors[] = {
		FLinearColor(0.15f, 0.85f, 0.25f),   // 초록 - 관리망
		FLinearColor(0.95f, 0.45f, 0.10f),   // 주황 - 데이터
		FLinearColor(0.60f, 0.88f, 0.95f),   // 하늘 - 데이터
		FLinearColor(0.85f, 0.85f, 0.88f),   // 흰색 - 전원
	};
	constexpr int32 NumCableColors = UE_ARRAY_COUNT(CableColors);

	const FLinearColor TrayColor(0.16f, 0.17f, 0.19f);
	const FLinearColor CabinetPlaceholderColor(0.06f, 0.07f, 0.09f);
	const FLinearColor CduColor(0.85f, 0.65f, 0.15f);
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

void ADataCenterRoomActor::SetPumpSpeedRatio(double InSpeedRatio)
{
	PumpSpeedRatio = FMath::Clamp(InSpeedRatio, 0.0, 1.5);
	bNeedsRebuild = true;
}

double ADataCenterRoomActor::ColumnYM(int32 ColumnIndex) const
{
	return ColumnIndex * RackPitchM;
}

ADataCenterRoomActor::FRowGeometry ADataCenterRoomActor::ComputeRowGeometry(int32 RowIndex) const
{
	FRowGeometry Geometry;

	// 두 줄이 통로 하나를 사이에 두고 마주 본다. 쌍끼리는 서비스 통로로 벌린다.
	const int32 PairIndex = RowIndex / 2;
	const bool bSecondSide = (RowIndex % 2) == 1;

	const double HalfDepthM = RackSizeM.X * 0.5;
	const double PairPitchM = AisleWidthM + 2.0 * RackSizeM.X + ServiceAisleWidthM;
	const double PairCenterXM = PairIndex * PairPitchM;
	const double SideOffsetM = AisleWidthM * 0.5 + HalfDepthM;

	Geometry.FacingSign = bSecondSide ? -1.0 : 1.0;
	Geometry.RackCenterXM = PairCenterXM + (bSecondSide ? SideOffsetM : -SideOffsetM);

	// 배관은 전부 뒷면 쪽이다. 뒷면은 앞면의 반대이므로 -FacingSign 방향이다.
	const double RearXM = Geometry.RackCenterXM - Geometry.FacingSign * HalfDepthM;
	Geometry.SupplyXM = RearXM - Geometry.FacingSign * PipeStandoffM;
	Geometry.ReturnXM = RearXM - Geometry.FacingSign * (PipeStandoffM + SupplyReturnSpacingM);
	Geometry.TrayXM = RearXM - Geometry.FacingSign * (PipeStandoffM + SupplyReturnSpacingM * 2.0);

	return Geometry;
}

FVector ADataCenterRoomActor::GetRackWorldLocationM(int32 RowIndex, int32 ColumnIndex) const
{
	const FRowGeometry Geometry = ComputeRowGeometry(RowIndex);
	const FVector LocalM(Geometry.RackCenterXM, ColumnYM(ColumnIndex), 0.0);

	// 액터 위치는 cm이므로 m로 바꿔 더한다 (규약 1).
	return SimUnits::UnrealToMeters(GetActorLocation()) + LocalM;
}

FBox ADataCenterRoomActor::GetRoomBoundsM() const
{
	const int32 RowCount = FMath::Max(1, NumRows);
	const int32 ColumnCount = FMath::Max(1, RacksPerRow);

	FBox BoundsM(ForceInit);
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			const FVector RackM = GetRackWorldLocationM(RowIndex, ColumnIndex);
			BoundsM += FBox(
				RackM - FVector(RackSizeM.X * 0.5, RackSizeM.Y * 0.5, 0.0),
				RackM + FVector(RackSizeM.X * 0.5, RackSizeM.Y * 0.5, RackSizeM.Z));
		}
	}

	// 뒷면 배관과 천장 트레이까지 덮는다.
	const double RearMarginM = PipeStandoffM + SupplyReturnSpacingM * 2.0 + 0.4;
	return BoundsM.ExpandBy(FVector(RearMarginM, 0.4, 0.0))
		+ FBox(BoundsM.Min, FVector(BoundsM.Max.X, BoundsM.Max.Y, CableTrayHeightM + 0.3));
}

void ADataCenterRoomActor::BuildNetwork()
{
	Network.Reset();
	NodePositionM.Reset();
	RowGeometries.Reset();
	RowPumpIndices.Reset();
	RackBranchPipeIndices.Reset();

	const int32 RowCount = FMath::Max(1, NumRows);
	const int32 ColumnCount = FMath::Max(1, RacksPerRow);

	// 위상은 InRowLoopBuilder가 만든다. 이 액터는 거기에 좌표만 붙인다.
	// 솔버는 좌표를 모른다 (규약 1: 변환은 액터-솔버 경계에서만).
	FInRowLoopSpec Spec;
	Spec.NumRacks = ColumnCount;
	Spec.SledsPerRack = SledsPerRack;
	Spec.HeaderSegmentResistancePaS2M6 = HeaderSegmentResistancePaS2M6;
	Spec.RackBranchResistancePaS2M6 = RackBranchResistancePaS2M6;
	Spec.ReturnToCduResistancePaS2M6 = ReturnToCduResistancePaS2M6;
	Spec.CduReferencePressurePa = CduReferencePressurePa;
	Spec.PumpShutoffHeadPa = PumpShutoffHeadPa;
	Spec.PumpCurveCoeffPaS2M6 = PumpCurveCoeffPaS2M6;
	Spec.PumpEfficiency = PumpEfficiency;
	Spec.PumpSpeedRatio = PumpSpeedRatio;

	RackBranchPipeIndices.Init(INDEX_NONE, RowCount * ColumnCount);
	bPipeIsSupplySide.Reset();

	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		const FRowGeometry Geometry = ComputeRowGeometry(RowIndex);
		RowGeometries.Add(Geometry);

		Spec.DockedSledsPerRack.Reset();
		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			Spec.DockedSledsPerRack.Add(GetDockedSledsAt(RowIndex, ColumnIndex));
		}

		const FInRowLoopHandles Handles = InRowLoopBuilder::AddLoop(Network, Spec);
		RowPumpIndices.Add(Handles.PumpIndex);

		// 좌표 배열을 노드 인덱스와 같은 길이로 맞춘 뒤 자리마다 채운다.
		NodePositionM.SetNum(Network.GetNumNodes());

		// CDU는 행의 첫 랙보다 앞쪽(열 방향으로 음수)에 선다.
		NodePositionM[Handles.CduNodeIndex] =
			FVector(Geometry.SupplyXM, -CduOffsetM, RackManifoldZM * 0.5);

		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			const double YM = ColumnYM(ColumnIndex);

			// 헤더 노드는 천장 높이, 랙 노드는 뒷면 매니폴드 높이다.
			// 그래서 분기관이 헤더에서 랙 뒤로 내려오는 모양이 된다.
			NodePositionM[Handles.SupplyNodeIndices[ColumnIndex]] =
				FVector(Geometry.SupplyXM, YM, HeaderHeightM);
			NodePositionM[Handles.ReturnNodeIndices[ColumnIndex]] =
				FVector(Geometry.ReturnXM, YM, HeaderHeightM);

			if (Handles.RackNodeIndices[ColumnIndex] != INDEX_NONE)
			{
				NodePositionM[Handles.RackNodeIndices[ColumnIndex]] =
					FVector(Geometry.SupplyXM, YM, RackManifoldZM);
			}

			RackBranchPipeIndices[RowIndex * ColumnCount + ColumnIndex] =
				Handles.RackBranchPipeIndices[ColumnIndex];
		}

		// 관 색을 공급(파랑)/회수(빨강)로 나누기 위한 역할표.
		// 솔버는 이걸 모른다. 순수 표시용이다.
		bPipeIsSupplySide.SetNum(Network.GetNumPipes());
		for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
		{
			if (Handles.IsSupplySidePipe(PipeIndex))
			{
				bPipeIsSupplySide[PipeIndex] = true;
			}
		}
	}

	// D3: 노드 200개 미만이면 밀행렬 LU로 충분하다는 전제로 짠 솔버다.
	// 방이 커지면 그 전제를 넘는다 (노드 수 = 행 * (1 + 3 * 랙수)).
	// 밀행렬 LU는 O(n^3)이라 노드가 두 배면 여덟 배 걸린다.
	if (Network.GetNumNodes() >= 200)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterRoomActor: 노드 %d개로 D3의 밀행렬 LU 전제(200개 미만)를 넘었습니다. ")
			TEXT("결과는 맞지만 재계산이 느려집니다. 희소 솔버가 필요하면 별도 단계로 다룰 것."),
			Network.GetNumNodes());
	}

	LastResult = Network.Solve(SolveSettings);

	if (!LastResult.bConverged)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterRoomActor: 수렴 실패 (반복 %d회, 정규화 잔차 %.3e). 표시된 유량을 믿지 마세요."),
			LastResult.IterationsUsed, LastResult.NormalizedResidual);
	}
}

int32 ADataCenterRoomActor::GetDockedSledsAt(int32 RowIndex, int32 ColumnIndex) const
{
	// Play 중이면 실제 랙 액터의 도킹 상태를 읽는다. 유닛을 뽑으면 다음 재계산에
	// 분기 저항이 올라간다. 에디터에서는 만재로 본다.
	if (const ARackThermalActor* Rack = GetRackAt(RowIndex, ColumnIndex))
	{
		return Rack->GetNumDockedSleds();
	}
	return SledsPerRack;
}

FVector ADataCenterRoomActor::NodeWorldOffsetCm(int32 NodeIndex) const
{
	// 길이 단위 변환은 여기서만 한다 (규약 1). 컴포넌트는 상대 좌표를 쓰므로
	// 액터 위치를 더하지 않는다.
	return SimUnits::MetersToUnreal(NodePositionM[NodeIndex]);
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

	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	if (CylinderMesh == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("DataCenterRoomActor: 엔진 기본 원통 메쉬를 찾지 못했습니다."));
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	// 관 굵기는 헤더와 분기를 다르게 준다. 분기가 헤더보다 가늘어야 흐름이 읽힌다.
	const double HeaderDiameterCm = SimUnits::MetersToCentimeters(HeaderDiameterM);
	const double BranchDiameterCm = SimUnits::MetersToCentimeters(BranchDiameterM);

	PipeMeshes.Reserve(Network.GetNumPipes());
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		const bool bIsBranch = RackBranchPipeIndices.Contains(PipeIndex)
			|| (PipeIndex > 0 && RackBranchPipeIndices.Contains(PipeIndex - 1));

		UStaticMeshComponent* Pipe = SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("Pipe"));
		SimMeshBuilder::SetTube(Pipe,
			NodeWorldOffsetCm(Network.GetPipeFromNodeIndex(PipeIndex)),
			NodeWorldOffsetCm(Network.GetPipeToNodeIndex(PipeIndex)),
			bIsBranch ? BranchDiameterCm : HeaderDiameterCm);
		PipeMeshes.Add(Pipe);
	}
}

void ADataCenterRoomActor::BuildDecorMeshes()
{
	DestroyMeshes(DecorMeshes);

	UStaticMesh* CubeMesh = SimMeshBuilder::LoadCubeMesh();
	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	if (CubeMesh == nullptr || CylinderMesh == nullptr)
	{
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	const int32 RowCount = RowGeometries.Num();
	const int32 ColumnCount = FMath::Max(1, RacksPerRow);

	const double CableDiameterCm = SimUnits::MetersToCentimeters(CableDiameterM);
	const double TrayZCm = SimUnits::MetersToCentimeters(CableTrayHeightM);
	const double RackWidthCm = SimUnits::MetersToCentimeters(RackSizeM.Y);
	const double RowLengthCm = SimUnits::MetersToCentimeters(ColumnYM(ColumnCount - 1) + RackPitchM);

	auto AddTube = [&](const FVector& StartCm, const FVector& EndCm, double DiameterCm, const FLinearColor& Color)
	{
		UStaticMeshComponent* Tube = SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("Decor"));
		SimMeshBuilder::SetTube(Tube, StartCm, EndCm, DiameterCm);
		SimMeshBuilder::SetMeshColor(Tube, Color);
		DecorMeshes.Add(Tube);
	};

	auto AddBox = [&](const FVector& CenterCm, const FVector& SizeCm, const FLinearColor& Color)
	{
		UStaticMeshComponent* Box = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Decor"));
		SimMeshBuilder::SetBox(Box, CenterCm, SizeCm);
		SimMeshBuilder::SetMeshColor(Box, Color);
		DecorMeshes.Add(Box);
	};

	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		const FRowGeometry& Geometry = RowGeometries[RowIndex];
		const double TrayXCm = SimUnits::MetersToCentimeters(Geometry.TrayXM);
		const double StartYCm = SimUnits::MetersToCentimeters(ColumnYM(0) - RackPitchM * 0.5);

		// 케이블 트레이: 옆 레일 두 줄. 사다리 가로대는 개수가 많아 넣지 않는다.
		if (bShowCableTray)
		{
			const double TrayHalfWidthCm = 18.0;
			for (int32 RailIndex = 0; RailIndex < 2; ++RailIndex)
			{
				const double RailXCm = TrayXCm + (RailIndex == 0 ? -TrayHalfWidthCm : TrayHalfWidthCm);
				AddBox(FVector(RailXCm, StartYCm + RowLengthCm * 0.5, TrayZCm),
					FVector(4.0, RowLengthCm, 8.0), TrayColor);
			}
			// 트레이 바닥
			AddBox(FVector(TrayXCm, StartYCm + RowLengthCm * 0.5, TrayZCm - 5.0),
				FVector(TrayHalfWidthCm * 2.0, RowLengthCm, 2.0), TrayColor);
		}

		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			const double YCm = SimUnits::MetersToCentimeters(ColumnYM(ColumnIndex));

			// 케이블: 트레이에서 랙 뒷면 위쪽으로 내려온다.
			// 가닥마다 트레이 위 진입점과 랙 쪽 도착점을 조금씩 어긋나게 둬서
			// 한 다발처럼 보이게 한다.
			const double RearXCm = SimUnits::MetersToCentimeters(
				Geometry.RackCenterXM - Geometry.FacingSign * RackSizeM.X * 0.5);
			const double CableLandZCm = SimUnits::MetersToCentimeters(RackSizeM.Z * 0.92);

			for (int32 CableIndex = 0; CableIndex < CablesPerRack; ++CableIndex)
			{
				const double Spread = (CableIndex - (CablesPerRack - 1) * 0.5);
				const double CableYCm = YCm + Spread * 5.0;
				const double LandYCm = YCm + Spread * (RackWidthCm * 0.18);

				// 트레이에서 수직으로 조금 내려온 뒤 랙 뒤로 휘어 붙는다.
				const FVector TrayPointCm(TrayXCm, CableYCm, TrayZCm - 6.0);
				const FVector BendPointCm(TrayXCm, CableYCm, CableLandZCm + 22.0);
				const FVector LandPointCm(RearXCm, LandYCm, CableLandZCm);

				const FLinearColor Color = CableColors[CableIndex % NumCableColors];
				AddTube(TrayPointCm, BendPointCm, CableDiameterCm, Color);
				AddTube(BendPointCm, LandPointCm, CableDiameterCm, Color);
			}
		}
	}

	// CDU 표시. 노드 0번부터 행마다 하나씩이다.
	for (int32 RowIndex = 0; RowIndex < RowPumpIndices.Num(); ++RowIndex)
	{
		const FRowGeometry& Geometry = RowGeometries[RowIndex];
		const FVector CduCenterCm(
			SimUnits::MetersToCentimeters(Geometry.SupplyXM),
			SimUnits::MetersToCentimeters(-CduOffsetM),
			SimUnits::MetersToCentimeters(0.6));
		AddBox(CduCenterCm, FVector(70.0, 60.0, 120.0), CduColor);
	}

	// 랙 액터를 띄우지 않는 상태(에디터)에서는 자리를 상자로 보여준다.
	if (SpawnedRacks.Num() == 0)
	{
		const FVector RackSizeCm = SimUnits::MetersToUnreal(RackSizeM);
		for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
		{
			const FRowGeometry& Geometry = RowGeometries[RowIndex];
			for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
			{
				AddBox(FVector(
					SimUnits::MetersToCentimeters(Geometry.RackCenterXM),
					SimUnits::MetersToCentimeters(ColumnYM(ColumnIndex)),
					RackSizeCm.Z * 0.5),
					RackSizeCm, CabinetPlaceholderColor);
			}
		}
	}
}

void ADataCenterRoomActor::DestroySpawnedRacks()
{
	for (ARackThermalActor* Rack : SpawnedRacks)
	{
		if (Rack != nullptr)
		{
			Rack->Destroy();
		}
	}
	SpawnedRacks.Reset();
}

void ADataCenterRoomActor::SpawnRacks()
{
	DestroySpawnedRacks();

	UWorld* World = GetWorld();
	// 에디터 리스너에서는 액터를 띄우지 않는다. 자리만 상자로 보여준다.
	if (World == nullptr || !World->IsGameWorld() || !bSpawnRackActors)
	{
		return;
	}

	const int32 ColumnCount = FMath::Max(1, RacksPerRow);

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (int32 RowIndex = 0; RowIndex < RowGeometries.Num(); ++RowIndex)
	{
		const FRowGeometry& Geometry = RowGeometries[RowIndex];

		// 앞면이 통로를 향하도록 돌린다. FacingSign이 -1이면 180도다.
		const FRotator Rotation(0.0, Geometry.FacingSign > 0.0 ? 0.0 : 180.0, 0.0);

		for (int32 ColumnIndex = 0; ColumnIndex < ColumnCount; ++ColumnIndex)
		{
			const FVector LocationCm = GetActorLocation() + SimUnits::MetersToUnreal(
				FVector(Geometry.RackCenterXM, ColumnYM(ColumnIndex), 0.0));

			ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(
				ARackThermalActor::StaticClass(), LocationCm, Rotation, SpawnParams);
			if (Rack == nullptr)
			{
				continue;
			}

			// GB200 NVL72 프리셋. 컴퓨트 트레이 18장, 트레이당 Grace 2 + Blackwell 4.
			Rack->RackSizeM = RackSizeM;
			Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
			Rack->Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
			Rack->Input.SledDocked.Init(true, Rack->Params.NumSleds);

			// 유량과 라디에이터도 같은 운전점에서 역산한 값으로 맞춘다.
			// 랙 1대 액랭 97 kW를 냉각수 ΔT 약 10 K로 빼는 유량이 2.3 kg/s다.
			Rack->Input.CoolantMassFlowKgs = 2.3;
			Rack->Input.TotalAirMassFlowKgs = 0.6;
			Rack->Radiator.UAWK = 1.1e4;

			// 유닛 라벨은 방 전체에서는 글자가 너무 많아진다. 랙 하나를 볼 때만 켠다.
			Rack->bShowSledLabels = false;
			Rack->bShowReadout = false;
			Rack->ResetSim();
			Rack->StartSim();

			SpawnedRacks.Add(Rack);
		}
	}
}

ARackThermalActor* ADataCenterRoomActor::GetRackAt(int32 RowIndex, int32 ColumnIndex) const
{
	const int32 SlotIndex = RowIndex * FMath::Max(1, RacksPerRow) + ColumnIndex;
	return SpawnedRacks.IsValidIndex(SlotIndex) ? SpawnedRacks[SlotIndex] : nullptr;
}

void ADataCenterRoomActor::UpdatePipeColors() const
{
	double MaxFlowMagnitudeM3s = 0.0;
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
	}
	const double NormalizationM3s = FMath::Max(MaxFlowMagnitudeM3s, UE_DOUBLE_SMALL_NUMBER);

	// 색상(hue)은 공급/회수를 나타내고, 밝기는 계산된 유량 크기를 나타낸다.
	// 공급은 차갑게 들어오므로 파랑, 회수는 데워져 나오므로 빨강이다.
	// 온도를 직접 그린 것이 아니다 - 이 도메인에는 아직 온도가 없다.
	//
	// 낮은 쪽 색을 어둡게 두면 안 된다. 정규화 기준이 전체 최대 유량인데
	// 헤더가 분기의 8배쯤 흘러서 분기관의 정규화 값이 0.12 수준이 된다.
	// 처음에 낮은 쪽을 거의 검정으로 뒀더니 공급 분기관이 검은 랙에 묻혀
	// 화면에서 아예 사라졌다. 낮은 쪽에서도 색이 읽히게 밝기를 올렸다.
	static const FLinearColor SupplyLowColor(0.08f, 0.22f, 0.55f);
	static const FLinearColor SupplyHighColor(0.25f, 0.65f, 1.00f);
	static const FLinearColor ReturnLowColor(0.55f, 0.16f, 0.12f);
	static const FLinearColor ReturnHighColor(1.00f, 0.35f, 0.28f);

	for (int32 PipeIndex = 0; PipeIndex < PipeMeshes.Num(); ++PipeIndex)
	{
		const double Normalized = FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)) / NormalizationM3s;

		// 제곱근을 씌워 낮은 유량 구간을 펼친다. 표시용 보정이며 계산에는 쓰이지 않는다.
		// 선형으로 두면 분기관이 전부 같은 어두운 색이 되어 구분이 안 된다.
		const float Shaped = FMath::Sqrt(FMath::Clamp(static_cast<float>(Normalized), 0.0f, 1.0f));
		const bool bSupply = bPipeIsSupplySide.IsValidIndex(PipeIndex) && bPipeIsSupplySide[PipeIndex];

		SimMeshBuilder::SetMeshColor(PipeMeshes[PipeIndex],
			FMath::Lerp(bSupply ? SupplyLowColor : ReturnLowColor,
				bSupply ? SupplyHighColor : ReturnHighColor, Shaped));
	}
}

void ADataCenterRoomActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bNeedsRebuild)
	{
		BuildNetwork();
		SpawnRacks();
		BuildPipeMeshes();
		BuildDecorMeshes();
		bNeedsRebuild = false;
	}

	UpdatePipeColors();

	if (bShowSummary)
	{
		DrawSummary();
	}
}

void ADataCenterRoomActor::DrawSummary() const
{
	if (GEngine == nullptr || RowPumpIndices.Num() == 0)
	{
		return;
	}

	double TotalPumpFlowM3s = 0.0;
	double TotalPumpElectricalW = 0.0;
	for (const int32 PumpIndex : RowPumpIndices)
	{
		TotalPumpFlowM3s += Network.GetPumpFlowM3s(PumpIndex);
		TotalPumpElectricalW += Network.GetPumpElectricalInputPowerW(PumpIndex);
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

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		LastResult.bConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[%s] %s | 랙 %d대 (행 %d x %d) | Newton %d회 | 정규화 잔차 %.2e"),
			*GetName(),
			LastResult.bConverged ? TEXT("수렴") : TEXT("수렴 실패 - 아래 값 믿지 말 것"),
			GetNumRackSlots(), NumRows, RacksPerRow,
			LastResult.IterationsUsed, LastResult.NormalizedResidual));

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("펌프 %d대 합계 유량 %.2f L/s | 전기 입력 %.2f kW | 랙 분기 %.2f ~ %.2f L/s | 펌프 r=%.2f"),
			RowPumpIndices.Num(), TotalPumpFlowM3s * 1000.0, TotalPumpElectricalW / 1000.0,
			MinBranchFlowM3s * 1000.0, MaxBranchFlowM3s * 1000.0, PumpSpeedRatio));

	GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f, FColor(150, 150, 150),
		TEXT("배관: 파랑 = 공급, 빨강 = 회수 (색상은 역할, 밝기는 계산된 유량 크기). 온도를 그린 것이 아니다. 케이블 색은 용도 구분이며 계산 결과가 아니다."));
}
