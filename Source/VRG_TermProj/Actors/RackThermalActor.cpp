// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/SimMeshBuilder.h"
#include "Visualization/GjConnectionMesh.h"
#include "Visualization/GjRackLayout.h"
#include "Components/InstancedStaticMeshComponent.h"

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
	const int32 NumSleds = FRackThermalModel::NumSlotsOf(Params);
	if (Params.Slots.Num() > 0) Params.NumSleds = NumSleds;

	// Racks saved in a level carry an old copy of the preset devices (no fans, no idle fractions).
	// The code definition is the single source of physical defaults, so refresh them by name.
	if (FRackThermalModel::RefreshKnownDeviceTypes(Params))
	{
		UE_LOG(LogTemp, Verbose, TEXT("RackThermalActor %s: preset device definitions refreshed from code."), *GetName());
	}

	if (Input.ComponentHeatW.Num() != FRackThermalModel::CountNodes(Params))
	{
		// Legacy uniform parts carry no DefaultHeatW; keep the S1 per-sled defaults for them.
		Input.ComponentHeatW = Params.Slots.Num() > 0
			? FRackThermalModel::MakeDefaultHeatW(Params) : FRackThermalModel::MakeUniformHeatW(NumSleds);
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
	// 바깥이 돌릴 때는 바깥 시계를 쓴다. 이 액터의 시계는 멈춰 있다.
	return Model.MakeSnapshot(bDrivenExternally ? ExternalSimTimeS : Clock.GetSimTimeS());
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

void ARackThermalActor::ApplyGb200Nvl72Preset()
{
	Params = FRackThermalModel::MakeGb200Nvl72Params();
	Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.Slots.Num());
	Input.SlotAirInletTempK.Reset();
	Input.CoolantMassFlowKgs = 2.3;     // NVL72 branch design flow (S4 room hydraulics)
	Input.TotalAirMassFlowKgs = 1.2;
	ResetSim();
}

double ARackThermalActor::GetUplinkSwitchZM() const
{
	double Z = -1.0;
	for (const FRackSlot& Slot : Params.Slots)
		if (Params.DeviceTypes.IsValidIndex(Slot.DeviceType)
			&& Params.DeviceTypes[Slot.DeviceType].Name == FRackThermalModel::ManagementSwitch().Name)
			Z = FMath::Max(Z, Slot.BottomZM);
	return Z >= 0.0 ? Z : 1.91;
}

void ARackThermalActor::GetSledRowM(int32 SledIndex, double& OutBottomZM, double& OutRowHeightM) const
{
	if (Params.Slots.IsValidIndex(SledIndex))
	{
		// Heterogeneous rack: the slot says where the device is.
		const double ScaleZ = RackSizeM.Z / 2.236;
		OutBottomZM = Params.Slots[SledIndex].BottomZM * ScaleZ;
		OutRowHeightM = Params.Slots[SledIndex].HeightM * ScaleZ;
		return;
	}
	const int32 NumSleds = FMath::Max(1, Params.NumSleds);

	// 위아래에 캐비닛 패널이 들어갈 자리를 남긴다.
	const double BayBottomZM = PanelThicknessM;
	const double BayTopZM = RackSizeM.Z - PanelThicknessM;

	OutRowHeightM = (BayTopZM - BayBottomZM) / NumSleds;
	OutBottomZM = BayBottomZM + OutRowHeightM * SledIndex;
}

FVector ARackThermalActor::GjPointWorldCm(const FVector& LocalM) const
{
	const FVector Scale(RackSizeM.X / 1.068, RackSizeM.Y / .6, RackSizeM.Z / 2.236);
	return GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(LocalM * Scale));
}

void ARackThermalActor::RefreshVisuals()
{
	ConformInputArrays();
	RebuildMeshes();
	bMeshesDirty = false;
	UpdateSledSlide(0.0f);
}

bool ARackThermalActor::BuildGjMeshes()
{
	// The authored kit is the GB200 NVL72 device layout. Do not silently skin a different rack.
	const FName ComputeName = FRackThermalModel::Gb200ComputeTray().Name, SwitchTrayName = FRackThermalModel::Gb200NvLinkSwitchTray().Name;
	const FName ShelfName = FRackThermalModel::Gb200PowerShelf(1).Name, MgmtName = FRackThermalModel::ManagementSwitch().Name;
	if (Params.Slots.Num() == 0) return false;
	int32 NumCompute = 0, NumSwitchTrays = 0;
	for (const FRackSlot& Slot : Params.Slots)
	{
		if (!Params.DeviceTypes.IsValidIndex(Slot.DeviceType)) return false;
		const FName N = Params.DeviceTypes[Slot.DeviceType].Name;
		if (N != ComputeName && N != SwitchTrayName && N != ShelfName && N != MgmtName) return false;
		NumCompute += N == ComputeName; NumSwitchTrays += N == SwitchTrayName;
	}
	if (NumCompute != 18 || NumSwitchTrays != 9) return false;  // harnesses are baked per tray position

	UStaticMesh* Frame = GjConnectionMesh::Mesh(TEXT("SM_GB200_RackFrame"), true);
	UStaticMesh* Node = GjConnectionMesh::Mesh(TEXT("SM_GB200_ComputeNode"), true);
	UStaticMesh* Switch = GjConnectionMesh::Mesh(TEXT("SM_QuantumIB_Switch_Photo"), true);
	UStaticMesh* SwitchTray = GjConnectionMesh::Mesh(TEXT("SM_GJ_NVSwitchTray"), true);
	UStaticMesh* Shelf = GjConnectionMesh::Mesh(TEXT("SM_GJ_PowerShelf"), true);
	UStaticMesh* Manifolds = GjConnectionMesh::Mesh(TEXT("SM_GJ_Manifolds"));
	TArray<UStaticMesh*> Harnesses, SwitchHarnesses;
	for (int32 I=0; I<18; ++I) Harnesses.Add(GjConnectionMesh::Mesh(*FString::Printf(TEXT("SM_GJ_Harness_%02d"),I)));
	for (int32 I=0; I<9; ++I) SwitchHarnesses.Add(GjConnectionMesh::Mesh(*FString::Printf(TEXT("SM_GJ_SwitchHarness_%02d"),I)));
	if (!Frame || !Node || !Switch || !SwitchTray || !Shelf || !Manifolds || Harnesses.Contains(nullptr) || SwitchHarnesses.Contains(nullptr))
	{
		UE_LOG(LogTemp, Error, TEXT("GJ rack assets are incomplete. Run Tools/install_gj_models.py; using original visuals."));
		return false;
	}
	const FVector Scale(RackSizeM.X / 1.068, RackSizeM.Y / .6, RackSizeM.Z / 2.236);
	auto Add = [&](UStaticMesh* Mesh, FVector LocalM, const TCHAR* Name)
	{
		UStaticMeshComponent* C = SimMeshBuilder::CreateMeshComponent(this,Mesh,nullptr,Name);
		C->SetRelativeLocation(SimUnits::MetersToUnreal(LocalM * Scale));
		C->SetRelativeScale3D(Scale);
		return C;
	};
	CabinetMeshes.Add(Add(Frame,FVector::ZeroVector,TEXT("GJ_RackFrame")));
	CabinetMeshes.Add(Add(Manifolds,FVector::ZeroVector,TEXT("GJ_Manifolds")));
	// One mesh per slot, exactly where the thermal model puts the device ("what you see is what is solved").
	TArray<TPair<double, double>> Occupied;
	int32 ComputeOrdinal = 0, SwitchOrdinal = 0;
	for (int32 S = 0; S < Params.Slots.Num(); ++S)
	{
		const FRackSlot& Slot = Params.Slots[S];
		const FName N = Params.DeviceTypes[Slot.DeviceType].Name;
		Occupied.Add({Slot.BottomZM, Slot.BottomZM + Slot.HeightM});
		UStaticMeshComponent* Mesh = nullptr; UStaticMeshComponent* Harness = nullptr; double BaseX = .055; bool bFixed = false;
		if (N == ComputeName)
		{
			Mesh = Add(Node,FVector(BaseX,0,Slot.BottomZM),TEXT("GJ_ComputeNode"));
			Harness = Add(Harnesses[ComputeOrdinal++],FVector::ZeroVector,TEXT("GJ_SledHarness"));
		}
		else if (N == SwitchTrayName)
		{
			Mesh = Add(SwitchTray,FVector(BaseX,0,Slot.BottomZM),TEXT("GJ_NVSwitchTray"));
			Harness = Add(SwitchHarnesses[SwitchOrdinal++],FVector::ZeroVector,TEXT("GJ_SwitchHarness"));
		}
		else if (N == ShelfName) Mesh = Add(Shelf,FVector(BaseX,0,Slot.BottomZM),TEXT("GJ_PowerShelf"));
		else
		{
			// Mounted reversed so its ports face the rear (-X) with the coolant. Only the switch yaws.
			BaseX = GjRackLayout::SwitchXM; bFixed = true;
			Mesh = Add(Switch,FVector(BaseX,0,Slot.BottomZM),TEXT("GJ_ManagementSwitch"));
			Mesh->SetRelativeRotation(FRotator(0,180,0));
		}
		SledMeshes.Add(Mesh); SledHarnessMeshes.Add(Harness);
		SledBaseXCm.Add(SimUnits::MetersToCentimeters(BaseX * Scale.X)); bSledFixed.Add(bFixed);
	}
	// Empty U positions get 1U blank plates on the front rail (visual only; the air grid already treats
	// the rack body as solid, so this does not change any recirculation result).
	TArray<TPair<double, double>> Blanks;
	for (const GjRackLayout::FBlankSlot& Slot : GjRackLayout::BlankSlots(Occupied)) Blanks.Add({Slot.BottomM, Slot.HeightM});
	if (UInstancedStaticMeshComponent* Panels = GjConnectionMesh::AddBlankPanels(this, Blanks, Scale)) CabinetMeshes.Add(Panels);
	return true;
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
	ComponentSlot.Reset(); ComponentNodes.Reset(); SledBaseXCm.Reset(); bSledFixed.Reset();
	for (UStaticMeshComponent* Mesh : SledHarnessMeshes) if (Mesh) Mesh->DestroyComponent();
	SledHarnessMeshes.Reset();
	bUsingGjMeshes = false;
	if (bUseGjModels && BuildGjMeshes()) { bUsingGjMeshes = true; return; }

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
	TArray<FRackComponentParams> NodeParams; TArray<int32> SlotNodeStart;
	FRackThermalModel::ResolveLayout(Params, NodeParams, SlotNodeStart);

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
		SledBaseXCm.Add(0.0); bSledFixed.Add(false);

		// One box per part for small devices (the S1 4-part unit), else one box per part kind so a
		// 29-part compute tray stays readable. Front (+X) is the air inlet, list order = air path order.
		TArray<TArray<int32>> Groups;
		const int32 First = SlotNodeStart[SledIndex], End = SlotNodeStart[SledIndex + 1];
		if (End - First <= 6) { for (int32 N = First; N < End; ++N) Groups.Add({N}); }
		else
		{
			TArray<ERackPartKind> Kinds;
			for (int32 N = First; N < End; ++N)
			{
				const int32 G = Kinds.AddUnique(NodeParams[N].Kind);
				if (G == Groups.Num()) Groups.AddDefaulted();
				Groups[G].Add(N);
			}
		}
		const double SlotDepthCm = SledDepthCm / (Groups.Num() + 1);
		const double PartDepthCm = SlotDepthCm * 0.7;
		const double PartWidthCm = SledWidthCm * 0.55;
		const double PartCenterZCm = RowBottomZCm + TrayHeightCm + 0.5 * PartHeightCm;

		for (int32 G = 0; G < Groups.Num(); ++G)
		{
			const double PartXCm = 0.5 * SledDepthCm - SlotDepthCm * (G + 1);

			UStaticMeshComponent* Part = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Part"));
			SimMeshBuilder::SetBox(Part, FVector(PartXCm, 0.0, PartCenterZCm),
				FVector(PartDepthCm, PartWidthCm, PartHeightCm));
			ComponentMeshes.Add(Part);
			ComponentBaseXCm.Add(PartXCm);
			ComponentSlot.Add(SledIndex);
			ComponentNodes.Add(Groups[G]);
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
		if (Sled != nullptr && !(bSledFixed.IsValidIndex(SledIndex) && bSledFixed[SledIndex]))
		{
			FVector Location = Sled->GetRelativeLocation();
			Location.X = OffsetCm + (SledBaseXCm.IsValidIndex(SledIndex) ? SledBaseXCm[SledIndex] : 0.0);
			Sled->SetRelativeLocation(Location);
		}
		if (SledHarnessMeshes.IsValidIndex(SledIndex) && SledHarnessMeshes[SledIndex])
		{
			// Quick-disconnects release immediately; the physical branch also closes immediately.
			SledHarnessMeshes[SledIndex]->SetVisibility(IsSledDocked(SledIndex));
		}

	}
	// 부품은 유닛을 따라 함께 나온다. 꽂힌 상태의 X를 따로 들고 있다가 오프셋만 더한다.
	for (int32 FlatIndex = 0; FlatIndex < ComponentMeshes.Num(); ++FlatIndex)
	{
		if (!ComponentMeshes[FlatIndex] || !ComponentBaseXCm.IsValidIndex(FlatIndex) || !ComponentSlot.IsValidIndex(FlatIndex)
			|| !SledSlideAlpha.IsValidIndex(ComponentSlot[FlatIndex]))
		{
			continue;
		}
		UStaticMeshComponent* Part = ComponentMeshes[FlatIndex];
		FVector Location = Part->GetRelativeLocation();
		Location.X = ComponentBaseXCm[FlatIndex] + PullOutCm * SledSlideAlpha[ComponentSlot[FlatIndex]];
		Part->SetRelativeLocation(Location);
	}
}

void ARackThermalActor::UpdateMeshColors() const
{
	// Preserve authored PBR materials. Numerical temperatures remain in the existing readouts.
	if (bUsingGjMeshes) return;
	const FRackThermalSnapshot Snapshot = Model.MakeSnapshot(Clock.GetSimTimeS());
	const double MinTempK = SimUnits::CelsiusToKelvin(ColorMinCelsius);
	const double MaxTempK = SimUnits::CelsiusToKelvin(ColorMaxCelsius);

	for (int32 SledIndex = 0; SledIndex < SledMeshes.Num(); ++SledIndex)
	{
		SimMeshBuilder::SetMeshColor(SledMeshes[SledIndex],
			IsSledDocked(SledIndex) ? DockedSledColor : UndockedSledColor);
	}

	for (int32 FlatIndex = 0; FlatIndex < ComponentMeshes.Num() && FlatIndex < ComponentNodes.Num(); ++FlatIndex)
	{
		double HottestK = -1.0;
		for (const int32 Node : ComponentNodes[FlatIndex])
			if (Snapshot.ComponentTempK.IsValidIndex(Node)) HottestK = FMath::Max(HottestK, Snapshot.ComponentTempK[Node]);
		if (HottestK < 0.0) continue;
		SimMeshBuilder::SetMeshColor(ComponentMeshes[FlatIndex], SimMeshBuilder::TemperatureToColor(HottestK, MinTempK, MaxTempK));
	}
}

void ARackThermalActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 에디터 리스너에서는 BeginPlay 없이 틱이 돌아간다.
	// 모델이 아직 Params에 맞게 초기화되지 않았으면 먼저 맞춘다.
	if (Model.GetState().ComponentTempK.Num() != FRackThermalModel::CountNodes(Params))
	{
		ResetSim();
	}

	if (bMeshesDirty)
	{
		RebuildMeshes();
		bMeshesDirty = false;
	}

	// 규약 4: 통합 액터가 돌릴 때는 여기서 적분하지 않는다.
	// 두 군데서 돌리면 같은 시간 구간을 두 번 적분한다.
	if (bSimRunning && !bDrivenExternally)
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

			// Hottest part of the device in this slot.
			int32 Hot = INDEX_NONE;
			for (int32 N = Model.GetSlotNodeStart(SledIndex); N >= 0 && N < Model.GetSlotNodeStart(SledIndex) + Model.GetSlotNodeCount(SledIndex); ++N)
				if (Snapshot.ComponentTempK.IsValidIndex(N) && (Hot == INDEX_NONE || Snapshot.ComponentTempK[N] > Snapshot.ComponentTempK[Hot])) Hot = N;
			if (Hot == INDEX_NONE)
			{
				continue;
			}
			const FString Device = Params.Slots.IsValidIndex(SledIndex) && Params.DeviceTypes.IsValidIndex(Params.Slots[SledIndex].DeviceType)
				? Params.DeviceTypes[Params.Slots[SledIndex].DeviceType].Name.ToString() : TEXT("Unit");

			const FVector LabelCm = GetActorLocation()
				+ FVector(SimUnits::MetersToCentimeters(RackSizeM.X * 0.5 + 0.1),
					SimUnits::MetersToCentimeters(RackSizeM.Y * 0.5 + 0.05),
					SimUnits::MetersToCentimeters(LabelZM));

			// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
			DrawDebugString(World, LabelCm,
				FString::Printf(TEXT("S%02d %s %s | 최고 %s %.1f C | 공기 %.3f kg/s, %.0f W"),
					SledIndex,
					IsSledDocked(SledIndex) ? TEXT("[꽂힘]") : TEXT("[뽑힘]"),
					*Device, *Model.GetNodeParams(Hot).Name.ToString(),
					SimUnits::KelvinToCelsius(Snapshot.ComponentTempK[Hot]),
					Snapshot.SlotAirMassFlowKgs.IsValidIndex(SledIndex) ? Snapshot.SlotAirMassFlowKgs[SledIndex] : 0.0,
					Snapshot.SlotHeatToAirW.IsValidIndex(SledIndex) ? Snapshot.SlotHeatToAirW[SledIndex] : 0.0),
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
		int32 SledIndex = 0;
		while (SledIndex + 1 < Params.NumSleds && Model.GetSlotNodeStart(SledIndex + 1) <= HottestIndex) ++SledIndex;
		HottestLabel = FString::Printf(TEXT("S%d %s %.1f C"),
			SledIndex,
			*Model.GetNodeParams(HottestIndex).Name.ToString(),
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

	// Load and fans: utilization drives part heat, the fans' own operating point drives the air flow.
	double AirKgs = 0.0, MinRatio = 1.0, MaxRatio = 0.0;
	for (const double M : Snapshot.SlotAirMassFlowKgs) AirKgs += M;
	for (int32 Slot = 0; Slot < Snapshot.SlotFanSpeedRatio.Num(); ++Slot)
		if (FRackThermalModel::FansOf(Params, Slot)) { MinRatio = FMath::Min(MinRatio, Snapshot.SlotFanSpeedRatio[Slot]); MaxRatio = FMath::Max(MaxRatio, Snapshot.SlotFanSpeedRatio[Slot]); }
	const FString FanText = Snapshot.bFanMode
		? FString::Printf(TEXT(" | 부하율 %.0f%% | 팬 %.0f~%.0f%% %.0f W, 풍량 %.3f kg/s"), Input.Utilization * 100.0, MinRatio * 100.0, MaxRatio * 100.0, Snapshot.TotalFanPowerW, AirKgs)
		: FString::Printf(TEXT(" | 부하율 %.0f%% | 랙 팬(고정) 풍량 %.3f kg/s"), Input.Utilization * 100.0, AirKgs);

	// 같은 키로 덮어써서 프레임마다 쌓이지 않게 한다.
	GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor::Yellow, Readout + FanText);
}
