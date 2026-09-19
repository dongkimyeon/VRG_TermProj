// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/HydraulicNetworkDebugActor.h"
#include "Simulation/Core/SimUnits.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Visualization/SimMeshBuilder.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

AHydraulicNetworkDebugActor::AHydraulicNetworkDebugActor()
{
	PrimaryActorTick.bCanEverTick = true;

	// 메쉬를 붙일 곳이 필요하다. 액터 위치가 네트워크 원점이 된다.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AHydraulicNetworkDebugActor::BeginPlay()
{
	Super::BeginPlay();
	bNeedsRebuild = true;
}

#if WITH_EDITOR
void AHydraulicNetworkDebugActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bNeedsRebuild = true;
}
#endif

void AHydraulicNetworkDebugActor::Resolve()
{
	bNeedsRebuild = true;
}

void AHydraulicNetworkDebugActor::SetPumpSpeedRatio(double InSpeedRatio)
{
	PumpSpeedRatio = FMath::Clamp(InSpeedRatio, 0.0, 1.5);
	bNeedsRebuild = true;
}

void AHydraulicNetworkDebugActor::SetRackDockedSleds(int32 RackIndex, int32 NumDockedSleds)
{
	const int32 RackCount = FMath::Max(1, NumRacks);
	if (DockedSledsPerRack.Num() != RackCount)
	{
		DockedSledsPerRack.Init(SledsPerRack, RackCount);
	}
	if (!DockedSledsPerRack.IsValidIndex(RackIndex))
	{
		return;
	}

	DockedSledsPerRack[RackIndex] = FMath::Clamp(NumDockedSleds, 0, SledsPerRack);
	bNeedsRebuild = true;
}

int32 AHydraulicNetworkDebugActor::GetRackDockedSleds(int32 RackIndex) const
{
	return DockedSledsPerRack.IsValidIndex(RackIndex) ? DockedSledsPerRack[RackIndex] : SledsPerRack;
}

void AHydraulicNetworkDebugActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bNeedsRebuild)
	{
		RebuildAndSolve();
		bNeedsRebuild = false;
	}

	if (bUseStaticMeshes)
	{
		UpdateMeshes();
	}

	DrawNetwork();

	if (bShowSummary)
	{
		DrawSummary();
	}
}

void AHydraulicNetworkDebugActor::RebuildAndSolve()
{
	Network.Reset();
	NodePositionM.Reset();
	RackBranchPipeIndices.Reset();

	const int32 RackCount = FMath::Max(1, NumRacks);
	const double RowCenterYM = 0.5 * (SupplyHeaderYM + ReturnHeaderYM);

	auto AddNodeAt = [this](const FHydraulicNode& Node, const FVector& PositionM) -> int32
	{
		const int32 NodeIndex = Network.AddNode(Node);
		NodePositionM.Add(PositionM);
		return NodeIndex;
	};

	// CDU 기준 노드. 네트워크에서 유일한 고정 압력 노드다.
	FHydraulicNode CduNode;
	CduNode.bFixedPressure = true;
	CduNode.PressurePa = CduReferencePressurePa;
	const int32 CduIndex = AddNodeAt(CduNode, FVector(-CduOffsetM, RowCenterYM, HeaderHeightM));

	TArray<int32> SupplyNodes;
	TArray<int32> RackNodes;
	TArray<int32> ReturnNodes;
	SupplyNodes.Reserve(RackCount);
	RackNodes.Reserve(RackCount);
	ReturnNodes.Reserve(RackCount);

	RackBoxPositionM.Reset();
	RackBoxPositionM.Reserve(RackCount);

	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		const double XM = RackIndex * RackSpacingM;
		const FVector RackPositionM(XM, RowCenterYM, RackNodeHeightM);
		RackBoxPositionM.Add(RackPositionM);

		SupplyNodes.Add(AddNodeAt(FHydraulicNode(), FVector(XM, SupplyHeaderYM, HeaderHeightM)));
		ReturnNodes.Add(AddNodeAt(FHydraulicNode(), FVector(XM, ReturnHeaderYM, HeaderHeightM)));

		// 유닛이 한 대도 안 꽂힌 랙은 연결될 곳이 없으므로 노드를 만들지 않는다.
		// 만들어 두면 어떤 배관에도 닿지 않는 자유 노드가 생겨 압력이 결정되지 않고
		// Jacobian이 특이해진다.
		const bool bHasDockedSleds = FMath::Clamp(GetRackDockedSleds(RackIndex), 0, FMath::Max(1, SledsPerRack)) > 0;
		RackNodes.Add(bHasDockedSleds ? AddNodeAt(FHydraulicNode(), RackPositionM) : INDEX_NONE);
	}

	// 펌프: CDU -> 공급 헤더 시작점
	FHydraulicPump Pump;
	Pump.FromNodeIndex = CduIndex;
	Pump.ToNodeIndex = SupplyNodes[0];
	Pump.ShutoffHeadPa = PumpShutoffHeadPa;
	Pump.CurveCoeffPaS2M6 = PumpCurveCoeffPaS2M6;
	Pump.Efficiency = PumpEfficiency;
	Pump.SpeedRatio = PumpSpeedRatio;
	Network.AddPump(Pump);

	auto AddPipe = [this](int32 FromNodeIndex, int32 ToNodeIndex, double ResistancePaS2M6) -> int32
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = FromNodeIndex;
		Pipe.ToNodeIndex = ToNodeIndex;
		Pipe.ResistancePaS2M6 = ResistancePaS2M6;
		return Network.AddPipe(Pipe);
	};

	for (int32 RackIndex = 0; RackIndex < RackCount; ++RackIndex)
	{
		// 공급 헤더 구간
		if (RackIndex + 1 < RackCount)
		{
			AddPipe(SupplyNodes[RackIndex], SupplyNodes[RackIndex + 1], HeaderSegmentResistancePaS2M6);
		}

		// 랙 분기 (공급 -> 랙 -> 회수).
		// 꽂힌 유닛 수가 줄면 병렬 콜드플레이트가 줄어 저항이 올라간다.
		// 0대면 연결 자체가 없으므로 노드와 배관을 만들지 않는다.
		// 큰 저항으로 흥내 내면 미세 유량이 남아 거짓 결과가 된다.
		const int32 DockedSleds = FMath::Clamp(GetRackDockedSleds(RackIndex), 0, FMath::Max(1, SledsPerRack));
		if (DockedSleds <= 0)
		{
			RackBranchPipeIndices.Add(INDEX_NONE);
		}
		else
		{
			const double ParallelScale = static_cast<double>(SledsPerRack) / static_cast<double>(DockedSleds);
			const double BranchResistancePaS2M6 = RackBranchResistancePaS2M6 * ParallelScale * ParallelScale;

			RackBranchPipeIndices.Add(AddPipe(SupplyNodes[RackIndex], RackNodes[RackIndex], BranchResistancePaS2M6));
			AddPipe(RackNodes[RackIndex], ReturnNodes[RackIndex], BranchResistancePaS2M6);
		}

		// 회수 헤더 구간. CDU 쪽으로 되돌아간다.
		if (RackIndex + 1 < RackCount)
		{
			AddPipe(ReturnNodes[RackIndex + 1], ReturnNodes[RackIndex], HeaderSegmentResistancePaS2M6);
		}
	}

	AddPipe(ReturnNodes[0], CduIndex, ReturnToCduResistancePaS2M6);

	LastResult = Network.Solve(SolveSettings);

	RebuildMeshes();

	if (!LastResult.bConverged)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("HydraulicNetworkDebugActor: 수렴 실패 (반복 %d회, 정규화 잔차 %.3e). 표시된 유량을 믿지 마세요."),
			LastResult.IterationsUsed, LastResult.NormalizedResidual);
	}
}

FVector AHydraulicNetworkDebugActor::NodeWorldLocation(int32 NodeIndex) const
{
	// 길이 단위 변환은 여기서만 한다 (규약 1).
	return GetActorLocation() + SimUnits::MetersToUnreal(NodePositionM[NodeIndex]);
}

FLinearColor AHydraulicNetworkDebugActor::FlowMagnitudeToColor(double NormalizedMagnitude)
{
	// 유량 크기 색상표. 온도 색상표(검정->흰색)와 일부러 다르게 둔다.
	static const FLinearColor Stops[] = {
		FLinearColor(0.05f, 0.10f, 0.40f),
		FLinearColor(0.00f, 0.55f, 0.75f),
		FLinearColor(0.10f, 0.80f, 0.35f),
		FLinearColor(1.00f, 0.90f, 0.20f)
	};
	static constexpr int32 NumStops = UE_ARRAY_COUNT(Stops);

	const float T = FMath::Clamp(static_cast<float>(NormalizedMagnitude), 0.0f, 1.0f);
	const float Scaled = T * (NumStops - 1);
	const int32 Index0 = FMath::Min(FMath::FloorToInt(Scaled), NumStops - 1);
	const int32 Index1 = FMath::Min(Index0 + 1, NumStops - 1);
	return FMath::Lerp(Stops[Index0], Stops[Index1], Scaled - Index0);
}

void AHydraulicNetworkDebugActor::DrawNetwork() const
{
	UWorld* World = GetWorld();
	if (World == nullptr || Network.GetNumNodes() == 0)
	{
		return;
	}

	// 매 틱 새로 그리므로 이전 프레임 문자열을 먼저 지운다.
	FlushDebugStrings(World);

	// 유량 크기 정규화 기준.
	double MaxFlowMagnitudeM3s = 0.0;
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
	}
	const double NormalizationM3s = FMath::Max(MaxFlowMagnitudeM3s, UE_DOUBLE_SMALL_NUMBER);

	// 배관
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		const double FlowM3s = Network.GetPipeFlowM3s(PipeIndex);
		const double MagnitudeM3s = FMath::Abs(FlowM3s);
		const double Normalized = MagnitudeM3s / NormalizationM3s;

		const FColor Color = FlowMagnitudeToColor(Normalized).ToFColor(true);
		const float Thickness = FMath::Lerp(2.0f, 9.0f, static_cast<float>(Normalized));

		// 파이프 인덱스에서 노드를 되찾기 위해 네트워크 API 대신 위치 배열을 쓴다.
		// 유량 부호가 음수면 실제 흐름은 반대 방향이므로 화살표를 뒤집는다.
		const int32 FromNodeIndex = Network.GetPipeFromNodeIndex(PipeIndex);
		const int32 ToNodeIndex = Network.GetPipeToNodeIndex(PipeIndex);
		const FVector StartCm = NodeWorldLocation(FlowM3s >= 0.0 ? FromNodeIndex : ToNodeIndex);
		const FVector EndCm = NodeWorldLocation(FlowM3s >= 0.0 ? ToNodeIndex : FromNodeIndex);

		// 메쉬를 쓰면 관 모양과 색은 메쉬가 맡고, 화살표는 흐름 방향만 보여주면 된다.
		// 그래도 방향 표시는 남긴다. 유량 부호가 바뀜는 것은 메쉬로 보이지 않는다.
		if (!bUseStaticMeshes)
		{
			DrawDebugDirectionalArrow(World, StartCm, EndCm, 18.0f, Color, false, -1.0f, 0, Thickness);
		}
		else
		{
			DrawDebugDirectionalArrow(World, FMath::Lerp(StartCm, EndCm, 0.35),
				FMath::Lerp(StartCm, EndCm, 0.65), 14.0f, FColor::White, false, -1.0f, 0, 2.0f);
		}

		const bool bIsRackBranch = RackBranchPipeIndices.Contains(PipeIndex);
		if (bShowAllFlowLabels || bIsRackBranch)
		{
			const FVector MidCm = (StartCm + EndCm) * 0.5;
			DrawDebugString(World, MidCm, FString::Printf(TEXT("%.1f L/s"), MagnitudeM3s * 1000.0),
				nullptr, FColor::White, 0.0f, true, 1.0f);
		}
	}

	// 노드
	for (int32 NodeIndex = 0; NodeIndex < Network.GetNumNodes(); ++NodeIndex)
	{
		const FVector NodeCm = NodeWorldLocation(NodeIndex);
		DrawDebugSphere(World, NodeCm, 6.0f, 8, FColor(200, 200, 200), false, -1.0f, 0, 1.5f);

		if (bShowNodePressures)
		{
			DrawDebugString(World, NodeCm + FVector(0, 0, 12.0),
				FString::Printf(TEXT("%.0f kPa"), Network.GetNodePressurePa(NodeIndex) / 1000.0),
				nullptr, FColor(180, 180, 255), 0.0f, true, 0.9f);
		}
	}

	// CDU(노드 0)를 크게 그려 구분한다.
	DrawDebugSphere(World, NodeWorldLocation(0), 16.0f, 12, FColor::Orange, false, -1.0f, 0, 2.5f);
	DrawDebugString(World, NodeWorldLocation(0) + FVector(0, 0, 30.0), TEXT("CDU / PUMP"),
		nullptr, FColor::Orange, 0.0f, true, 1.1f);

	// 랙 외형
	if (bShowRackBoxes)
	{
		const FVector HalfExtentCm = SimUnits::MetersToUnreal(RackSizeM) * 0.5;
		for (int32 RackIndex = 0; RackIndex < RackBoxPositionM.Num(); ++RackIndex)
		{
			const FVector RackCm = GetActorLocation() + SimUnits::MetersToUnreal(RackBoxPositionM[RackIndex]);
			const FVector CenterCm(RackCm.X, RackCm.Y, GetActorLocation().Z + HalfExtentCm.Z);

			// 유닛이 하나도 없어 분기가 끊긴 랙은 다른 색으로 구분한다.
			const bool bIsolated = !RackBranchPipeIndices.IsValidIndex(RackIndex)
				|| RackBranchPipeIndices[RackIndex] == INDEX_NONE;

			if (!bUseStaticMeshes)
			{
				DrawDebugBox(World, CenterCm, HalfExtentCm,
					bIsolated ? FColor(160, 70, 70) : FColor(90, 110, 140), false, -1.0f, 0, 1.5f);
			}

			DrawDebugString(World, CenterCm + FVector(0, 0, HalfExtentCm.Z + 20.0),
				FString::Printf(TEXT("R%d 유닛 %d/%d"), RackIndex,
					GetRackDockedSleds(RackIndex), SledsPerRack),
				nullptr, bIsolated ? FColor(255, 140, 140) : FColor(200, 220, 255), 0.0f, true, 1.0f);
		}
	}
}

void AHydraulicNetworkDebugActor::DrawSummary() const
{
	if (GEngine == nullptr || Network.GetNumPumps() == 0)
	{
		return;
	}

	// 랙별 유량 편차. 실제 데이터센터에서 앞쪽 랙이 더 많이 받는 현상이 보인다.
	double MinRackFlowM3s = TNumericLimits<double>::Max();
	double MaxRackFlowM3s = 0.0;
	int32 NumConnectedRacks = 0;
	int32 NumDockedSledsTotal = 0;
	for (int32 RackIndex = 0; RackIndex < RackBranchPipeIndices.Num(); ++RackIndex)
	{
		NumDockedSledsTotal += GetRackDockedSleds(RackIndex);

		// 분기가 없는 랙은 유량 편차 계산에서 뺀다. 0을 넣으면 편차가 100%로 보이지만
		// 그건 분배 불균형이 아니라 연결이 없는 것이다.
		if (RackBranchPipeIndices[RackIndex] == INDEX_NONE)
		{
			continue;
		}
		++NumConnectedRacks;

		const double FlowM3s = FMath::Abs(Network.GetPipeFlowM3s(RackBranchPipeIndices[RackIndex]));
		MinRackFlowM3s = FMath::Min(MinRackFlowM3s, FlowM3s);
		MaxRackFlowM3s = FMath::Max(MaxRackFlowM3s, FlowM3s);
	}

	if (NumConnectedRacks == 0)
	{
		MinRackFlowM3s = 0.0;
	}
	const double SpreadPercent = MaxRackFlowM3s > UE_DOUBLE_SMALL_NUMBER
		? (1.0 - MinRackFlowM3s / MaxRackFlowM3s) * 100.0
		: 0.0;

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		LastResult.bConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[%s] %s | Newton %d회 | 정규화 잔차 %.2e (기준 유량 %.4f m3/s, 기준 압력 %.0f Pa)"),
			*GetName(),
			LastResult.bConverged ? TEXT("수렴") : TEXT("수렴 실패 - 아래 값 믿지 말 것"),
			LastResult.IterationsUsed, LastResult.NormalizedResidual,
			LastResult.ReferenceFlowM3s, LastResult.ReferencePressurePa));

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("펌프 유량 %.1f L/s | 양정 %.1f kPa | 유체 동력 %.2f kW | 전기 입력 %.2f kW (효율 %.2f)"),
			Network.GetPumpFlowM3s(0) * 1000.0,
			Network.GetPumpDeltaPPa(0) / 1000.0,
			Network.GetPumpHydraulicPowerW(0) / 1000.0,
			Network.GetPumpElectricalInputPowerW(0) / 1000.0,
			PumpEfficiency));

	GEngine->AddOnScreenDebugMessage(BaseKey + 4, 0.0f, FColor(200, 160, 255),
		FString::Printf(TEXT("펌프 회전수비 r=%.2f | 체절양정 %.1f kPa (정격 %.1f kPa의 r^2=%.3f배) | 전력은 운전점에서 계산 (P_rated*r^3 아님)"),
			PumpSpeedRatio,
			PumpShutoffHeadPa * PumpSpeedRatio * PumpSpeedRatio / 1000.0,
			PumpShutoffHeadPa / 1000.0,
			PumpSpeedRatio * PumpSpeedRatio));

	GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f, FColor::Yellow,
		FString::Printf(TEXT("랙 %d대(연결 %d대) | 유닛 %d/%d대 | 분기 유량 %.2f ~ %.2f L/s | 편차 %.1f%% (앞쪽 랙이 더 받음)"),
			RackBranchPipeIndices.Num(), NumConnectedRacks,
			NumDockedSledsTotal, RackBranchPipeIndices.Num() * SledsPerRack,
			MinRackFlowM3s * 1000.0, MaxRackFlowM3s * 1000.0, SpreadPercent));

	GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f, FColor(150, 150, 150),
		TEXT("배관 색 = 유량 크기 (파랑 낮음 -> 노랑 높음). 온도가 아님 - S2에는 온도가 없다."));
}

void AHydraulicNetworkDebugActor::DestroyMeshes()
{
	for (UStaticMeshComponent* MeshComponent : PipeMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	for (UStaticMeshComponent* MeshComponent : RackMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	PipeMeshes.Reset();
	RackMeshes.Reset();
}

void AHydraulicNetworkDebugActor::RebuildMeshes()
{
	DestroyMeshes();

	if (!bUseStaticMeshes)
	{
		return;
	}

	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	UStaticMesh* CubeMesh = SimMeshBuilder::LoadCubeMesh();
	if (CylinderMesh == nullptr || CubeMesh == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("HydraulicNetworkDebugActor: 엔진 기본 메쉬를 찾지 못했습니다."));
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const double DiameterCm = SimUnits::MetersToCentimeters(PipeDiameterM);

	PipeMeshes.Reserve(Network.GetNumPipes());
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		const FVector StartCm = SimUnits::MetersToUnreal(NodePositionM[Network.GetPipeFromNodeIndex(PipeIndex)]);
		const FVector EndCm = SimUnits::MetersToUnreal(NodePositionM[Network.GetPipeToNodeIndex(PipeIndex)]);

		UStaticMeshComponent* Pipe = SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("Pipe"));
		SimMeshBuilder::SetTube(Pipe, StartCm, EndCm, DiameterCm);
		PipeMeshes.Add(Pipe);
	}

	if (bShowRackBoxes)
	{
		const FVector HalfExtentCm = SimUnits::MetersToUnreal(RackSizeM) * 0.5;
		RackMeshes.Reserve(RackBoxPositionM.Num());

		for (int32 RackIndex = 0; RackIndex < RackBoxPositionM.Num(); ++RackIndex)
		{
			const FVector RackCm = SimUnits::MetersToUnreal(RackBoxPositionM[RackIndex]);

			UStaticMeshComponent* Rack = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Rack"));
			SimMeshBuilder::SetBox(Rack, FVector(RackCm.X, RackCm.Y, HalfExtentCm.Z), HalfExtentCm * 2.0);
			RackMeshes.Add(Rack);
		}
	}
}

void AHydraulicNetworkDebugActor::UpdateMeshes() const
{
	// 배관 색 = 유량 크기. 온도가 아니다 (S2에는 온도가 없다).
	double MaxFlowMagnitudeM3s = 0.0;
	for (int32 PipeIndex = 0; PipeIndex < Network.GetNumPipes(); ++PipeIndex)
	{
		MaxFlowMagnitudeM3s = FMath::Max(MaxFlowMagnitudeM3s, FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)));
	}
	const double NormalizationM3s = FMath::Max(MaxFlowMagnitudeM3s, UE_DOUBLE_SMALL_NUMBER);

	for (int32 PipeIndex = 0; PipeIndex < PipeMeshes.Num(); ++PipeIndex)
	{
		const double Normalized = FMath::Abs(Network.GetPipeFlowM3s(PipeIndex)) / NormalizationM3s;
		SimMeshBuilder::SetMeshColor(PipeMeshes[PipeIndex], FlowMagnitudeToColor(Normalized));
	}

	// 유닛이 하나도 없어 분기가 끊긴 랙은 색으로 구분한다.
	for (int32 RackIndex = 0; RackIndex < RackMeshes.Num(); ++RackIndex)
	{
		const bool bIsolated = !RackBranchPipeIndices.IsValidIndex(RackIndex)
			|| RackBranchPipeIndices[RackIndex] == INDEX_NONE;
		SimMeshBuilder::SetMeshColor(RackMeshes[RackIndex],
			bIsolated ? FLinearColor(0.35f, 0.10f, 0.10f) : FLinearColor(0.10f, 0.12f, 0.16f));
	}
}
