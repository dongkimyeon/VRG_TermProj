// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/SimMeshBuilder.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"

namespace
{
	// 캐비닛 색. 부품 온도 색상표와 섞이지 않게 무채색으로 둔다.
	const FLinearColor CabinetColor(0.07f, 0.08f, 0.09f);
	const FLinearColor DockedSledColor(0.22f, 0.24f, 0.28f);
	const FLinearColor UndockedSledColor(0.45f, 0.20f, 0.20f);
}

ARackThermalActor::ARackThermalActor()
{
	PrimaryActorTick.bCanEverTick = true;

	// 메쉬를 붙일 곳이 필요하다. 액터 위치가 캐비닛 바닥 중심이 된다.
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ARackThermalActor::BeginPlay()
{
	Super::BeginPlay();
	ResetSim();
	StartSim();
}

#if WITH_EDITOR
void ARackThermalActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	ConformInputArrays();
	bMeshesDirty = true;
}
#endif

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

	if (SledSlideAlpha.Num() != NumSleds)
	{
		SledSlideAlpha.Init(0.0f, NumSleds);
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
	bMeshesDirty = true;
}

FRackThermalSnapshot ARackThermalActor::GetSnapshot() const
{
	return Model.MakeSnapshot(Clock.GetSimTimeS());
}

void ARackThermalActor::SetSledDocked(int32 SledIndex, bool bDocked)
{
	if (Input.SledDocked.IsValidIndex(SledIndex))
	{
		// 물리 상태는 즉시 바뀐다. 퀵디스커넥트는 레일이 다 나오기를 기다리지 않는다.
		Input.SledDocked[SledIndex] = bDocked;
	}
}

void ARackThermalActor::ToggleSledDocked(int32 SledIndex)
{
	SetSledDocked(SledIndex, !IsSledDocked(SledIndex));
}

bool ARackThermalActor::IsSledDocked(int32 SledIndex) const
{
	return Input.SledDocked.IsValidIndex(SledIndex) ? Input.SledDocked[SledIndex] : false;
}

int32 ARackThermalActor::GetNumDockedSleds() const
{
	int32 NumDocked = 0;
	for (const bool bDocked : Input.SledDocked)
	{
		NumDocked += bDocked ? 1 : 0;
	}
	return NumDocked;
}

void ARackThermalActor::GetSledRowM(int32 SledIndex, double& OutBottomZM, double& OutRowHeightM) const
{
	const int32 NumSleds = FMath::Max(1, Params.NumSleds);

	// 위아래에 캐비닛 패널이 들어갈 자리를 남긴다.
	const double BayBottomZM = PanelThicknessM;
	const double BayTopZM = RackSizeM.Z - PanelThicknessM;

	OutRowHeightM = (BayTopZM - BayBottomZM) / NumSleds;
	OutBottomZM = BayBottomZM + OutRowHeightM * SledIndex;
}

void ARackThermalActor::RebuildMeshes()
{
	for (UStaticMeshComponent* MeshComponent : CabinetMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	for (UStaticMeshComponent* MeshComponent : SledMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	for (UStaticMeshComponent* MeshComponent : ComponentMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	CabinetMeshes.Reset();
	SledMeshes.Reset();
	ComponentMeshes.Reset();
	ComponentBaseXCm.Reset();

	UStaticMesh* CubeMesh = SimMeshBuilder::LoadCubeMesh();
	if (CubeMesh == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("RackThermalActor: 엔진 기본 큐브 메쉬를 찾지 못했습니다."));
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	const int32 NumSleds = FMath::Max(1, Params.NumSleds);

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const FVector SizeCm = SimUnits::MetersToUnreal(RackSizeM);
	const double ThicknessCm = SimUnits::MetersToCentimeters(PanelThicknessM);

	auto AddCabinetPanel = [&](const FVector& CenterCm, const FVector& PanelSizeCm)
	{
		UStaticMeshComponent* Panel = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Cabinet"));
		SimMeshBuilder::SetBox(Panel, CenterCm, PanelSizeCm);
		SimMeshBuilder::SetMeshColor(Panel, CabinetColor);
		CabinetMeshes.Add(Panel);
	};

	// 앞뒤는 뚫려 있어야 한다. 공기가 앞(+X)으로 들어와 뒤(-X)로 빠져나가는 통로이고,
	// 막아 두면 안에 있는 유닛과 부품이 보이지 않는다.
	// 그래서 좌우 측판, 바닥, 천장만 두고 앞판과 뒷판은 만들지 않는다.
	// 대신 네 모서리에 기둥을 세워 랙 골조가 읽히게 한다.

	// 좌우 측판
	AddCabinetPanel(FVector(0.0, -0.5 * SizeCm.Y, 0.5 * SizeCm.Z), FVector(SizeCm.X, ThicknessCm, SizeCm.Z));
	AddCabinetPanel(FVector(0.0, 0.5 * SizeCm.Y, 0.5 * SizeCm.Z), FVector(SizeCm.X, ThicknessCm, SizeCm.Z));

	// 바닥과 천장
	AddCabinetPanel(FVector(0.0, 0.0, 0.5 * ThicknessCm), FVector(SizeCm.X, SizeCm.Y, ThicknessCm));
	AddCabinetPanel(FVector(0.0, 0.0, SizeCm.Z - 0.5 * ThicknessCm), FVector(SizeCm.X, SizeCm.Y, ThicknessCm));

	// 앞뒤 네 모서리 기둥. 측판보다 조금 두껍게 해서 골조처럼 보이게 한다.
	const double PostSizeCm = ThicknessCm * 1.6;
	for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
	{
		const double PostXCm = ((CornerIndex & 1) ? 0.5 : -0.5) * (SizeCm.X - PostSizeCm);
		const double PostYCm = ((CornerIndex & 2) ? 0.5 : -0.5) * (SizeCm.Y - PostSizeCm);
		AddCabinetPanel(FVector(PostXCm, PostYCm, 0.5 * SizeCm.Z),
			FVector(PostSizeCm, PostSizeCm, SizeCm.Z));
	}

	// 유닛과 부품
	SledMeshes.Reserve(NumSleds);
	ComponentMeshes.Reserve(NumSleds * NumRackComponents);

	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		double RowBottomZM = 0.0;
		double RowHeightM = 0.0;
		GetSledRowM(SledIndex, RowBottomZM, RowHeightM);

		const double RowBottomZCm = SimUnits::MetersToCentimeters(RowBottomZM);
		const double RowHeightCm = SimUnits::MetersToCentimeters(RowHeightM);

		const double SledDepthCm = SizeCm.X * 0.85;
		const double SledWidthCm = SizeCm.Y - 2.0 * ThicknessCm - 2.0;

		// 유닛은 꽉 찬 박스가 아니라 바닥 트레이다. 그래야 위에 얹힌 부품이 가려지지
		// 않으면서 트레이 + 부품을 합쳐도 칸 높이 안에 들어간다 (0.15 + 0.55 = 0.70).
		// 전에는 꽉 찬 유닛(칸의 0.8) 위에 부품(유닛의 0.55)을 또 얹어서 위 칸을
		// 침범했고, 맨 위 유닛은 부품이 캐비닛 천장판을 뚫고 나왔다.
		const double TrayHeightCm = RowHeightCm * 0.15;
		const double PartHeightCm = RowHeightCm * 0.55;

		UStaticMeshComponent* Sled = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Sled"));
		SimMeshBuilder::SetBox(Sled, FVector(0.0, 0.0, RowBottomZCm + 0.5 * TrayHeightCm),
			FVector(SledDepthCm, SledWidthCm, TrayHeightCm));
		SledMeshes.Add(Sled);

		// 부품은 트레이 위에 공랭 체인 순서대로 얹는다. 앞(+X)이 공기 입구다.
		// 배열 순서 그대로 앞에서 뒤로 놓으므로, 눈으로 보는 순서가 곧 공기가 지나는 순서다.
		const double SlotDepthCm = SledDepthCm / (NumRackComponents + 1);
		const double PartDepthCm = SlotDepthCm * 0.7;
		const double PartWidthCm = SledWidthCm * 0.55;
		const double PartCenterZCm = RowBottomZCm + TrayHeightCm + 0.5 * PartHeightCm;

		for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
		{
			const double PartXCm = 0.5 * SledDepthCm - SlotDepthCm * (ComponentIndex + 1);

			UStaticMeshComponent* Part = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Part"));
			SimMeshBuilder::SetBox(Part, FVector(PartXCm, 0.0, PartCenterZCm),
				FVector(PartDepthCm, PartWidthCm, PartHeightCm));
			ComponentMeshes.Add(Part);
			ComponentBaseXCm.Add(PartXCm);
		}
	}
}

void ARackThermalActor::UpdateSledSlide(float DeltaTime)
{
	const int32 NumSleds = FMath::Min(SledMeshes.Num(), SledSlideAlpha.Num());
	const double PullOutCm = SimUnits::MetersToCentimeters(SledPullOutM);

	for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
	{
		const float TargetAlpha = IsSledDocked(SledIndex) ? 0.0f : 1.0f;
		SledSlideAlpha[SledIndex] = FMath::FInterpConstantTo(
			SledSlideAlpha[SledIndex], TargetAlpha, DeltaTime, SledSlideSpeed);

		const double OffsetCm = PullOutCm * SledSlideAlpha[SledIndex];

		UStaticMeshComponent* Sled = SledMeshes[SledIndex];
		if (Sled != nullptr)
		{
			FVector Location = Sled->GetRelativeLocation();
			Location.X = OffsetCm;
			Sled->SetRelativeLocation(Location);
		}

		// 부품은 유닛을 따라 함께 나온다. 꽂힌 상태의 X를 따로 들고 있다가 오프셋만 더한다.
		for (int32 ComponentIndex = 0; ComponentIndex < NumRackComponents; ++ComponentIndex)
		{
			const int32 FlatIndex = SledIndex * NumRackComponents + ComponentIndex;
			if (!ComponentMeshes.IsValidIndex(FlatIndex) || ComponentMeshes[FlatIndex] == nullptr
				|| !ComponentBaseXCm.IsValidIndex(FlatIndex))
			{
				continue;
			}

			UStaticMeshComponent* Part = ComponentMeshes[FlatIndex];
			FVector Location = Part->GetRelativeLocation();
			Location.X = ComponentBaseXCm[FlatIndex] + OffsetCm;
			Part->SetRelativeLocation(Location);
		}
	}
}

void ARackThermalActor::UpdateMeshColors() const
{
	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(Clock.GetSimTimeS());
	const double MinTempK = SimUnits::CelsiusToKelvin(ColorMinCelsius);
	const double MaxTempK = SimUnits::CelsiusToKelvin(ColorMaxCelsius);

	for (int32 SledIndex = 0; SledIndex < SledMeshes.Num(); ++SledIndex)
	{
		SimMeshBuilder::SetMeshColor(SledMeshes[SledIndex],
			IsSledDocked(SledIndex) ? DockedSledColor : UndockedSledColor);
	}

	for (int32 FlatIndex = 0; FlatIndex < ComponentMeshes.Num(); ++FlatIndex)
	{
		if (!Snapshot.ComponentTempK.IsValidIndex(FlatIndex))
		{
			continue;
		}
		SimMeshBuilder::SetMeshColor(ComponentMeshes[FlatIndex],
			SimMeshBuilder::TemperatureToColor(Snapshot.ComponentTempK[FlatIndex], MinTempK, MaxTempK));
	}
}

void ARackThermalActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 에디터 리스너에서는 BeginPlay 없이 틱이 돌아간다.
	// 모델이 아직 Params에 맞게 초기화되지 않았으면 먼저 맞춘다.
	if (Model.GetState().ComponentTempK.Num() != FMath::Max(1, Params.NumSleds) * NumRackComponents)
	{
		ResetSim();
	}

	if (bMeshesDirty)
	{
		RebuildMeshes();
		bMeshesDirty = false;
	}

	if (bSimRunning)
	{
		const int32 NumSteps = Clock.BeginFrame(DeltaTime);
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			if (bCloseCoolantLoopThroughRadiator)
			{
				// 랙 출구 = 냉각수 노드 온도. 라디에이터를 지나 다시 랙 입구가 된다.
				Input.CoolantInletTempK = RadiatorModel::OutletTempK(
					Radiator, Model.GetState().CoolantTempK, Input.CoolantMassFlowKgs, Params.CoolantCpJKgK);
			}

			Model.Step(Input, Clock.GetFixedStepS());
			Clock.CompleteStep();
		}
	}

	UpdateSledSlide(DeltaTime);
	UpdateMeshColors();

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

	if (bShowSledLabels)
	{
		for (int32 SledIndex = 0; SledIndex < Params.NumSleds; ++SledIndex)
		{
			double RowBottomZM = 0.0;
			double RowHeightM = 0.0;
			GetSledRowM(SledIndex, RowBottomZM, RowHeightM);
			const double LabelZM = RowBottomZM + 0.5 * RowHeightM;

			const int32 CpuIndex = FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Cpu);
			const int32 GpuIndex = FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Gpu);
			const int32 RamIndex = FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Ram);
			const int32 VrmIndex = FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Vrm);
			if (!Snapshot.ComponentTempK.IsValidIndex(VrmIndex))
			{
				continue;
			}

			const FVector LabelCm = GetActorLocation()
				+ FVector(SimUnits::MetersToCentimeters(RackSizeM.X * 0.5 + 0.1),
					SimUnits::MetersToCentimeters(RackSizeM.Y * 0.5 + 0.05),
					SimUnits::MetersToCentimeters(LabelZM));

			// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
			DrawDebugString(World, LabelCm,
				FString::Printf(TEXT("U%02d %s C%.0f G%.0f R%.0f V%.0f | 공기 %.3f kg/s"),
					SledIndex,
					IsSledDocked(SledIndex) ? TEXT("[꽂힘]") : TEXT("[뽑힘]"),
					SimUnits::KelvinToCelsius(Snapshot.ComponentTempK[CpuIndex]),
					SimUnits::KelvinToCelsius(Snapshot.ComponentTempK[GpuIndex]),
					SimUnits::KelvinToCelsius(Snapshot.ComponentTempK[RamIndex]),
					SimUnits::KelvinToCelsius(Snapshot.ComponentTempK[VrmIndex]),
					Snapshot.SlotAirMassFlowKgs.IsValidIndex(SledIndex) ? Snapshot.SlotAirMassFlowKgs[SledIndex] : 0.0),
				nullptr,
				IsSledDocked(SledIndex) ? FColor(200, 220, 255) : FColor(255, 150, 150),
				0.0f, true, 0.9f);
		}
	}

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

	const FString Readout = FString::Printf(
		TEXT("[%s] t=%.1f s | 유닛 %d/%d 꽂힘 | 최고 %s | 냉각수 %.2f->%.2f C | 배기 %.2f C")
		TEXT(" | 냉각수 %.0f W + 공기 %.0f W + 자연대류 %.0f W = %.0f W (발열 %.0f W) | 정규화 잔차 %.2e%s"),
		*GetName(),
		Snapshot.SimTimeS,
		GetNumDockedSleds(), Params.NumSleds,
		*HottestLabel,
		SimUnits::KelvinToCelsius(Input.CoolantInletTempK),
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
