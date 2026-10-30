// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/FacilityPlantDisplayActor.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Simulation/Core/VrgHud.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "UI/VrgWebDashboard.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Interaction/DataCenterDebugController.h"
#include "Visualization/GjRackLayout.h"
#include "Visualization/CrahVisualModel.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Air/AirConnectivity.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Thermal/RackThermalModel.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "VrgFloorWaterGpu.h"
#include "Actors/AirFlowDisplayActor.h"
#include "Visualization/PlantVisual.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

ADataCenterSimulationActor::ADataCenterSimulationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// Referenced from the CDO so the cooker packages it (a runtime LoadObject would not be found in a build).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	CrahMesh = Cube.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShellMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	RoomShellMaterial = ShellMaterial.Object;
}

#if WITH_EDITOR
// 2026-10-22 (docs/기록/09): a level default edited without the editor UI. Sets the containment of the integrated actor
// in the open editor level and saves that actor's package (world partition external actor: Modify() before the change,
// or nothing is saved). Run in the editor, not -game: UnrealEditor <proj> <map> -ExecCmds="vrg.Dev.SaveLevelContainment 2,quit".
static FAutoConsoleCommand GVrgDevSaveLevelContainment(
	TEXT("vrg.Dev.SaveLevelContainment"),
	TEXT("에디터 전용: 열린 레벨의 통합 액터 컨테인먼트 기본값을 바꾸고 저장한다. vrg.Dev.SaveLevelContainment <0|1|2>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const int32 Mode = FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2);
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (Context.WorldType != EWorldType::Editor || World == nullptr) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->Modify();
				It->Containment = static_cast<EAisleContainment>(Mode);
				UPackage* Package = It->GetExternalPackage() ? It->GetExternalPackage() : It->GetPackage();
				const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(),
					Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Standalone;
				const bool bSaved = UPackage::SavePackage(Package, nullptr, *File, SaveArgs);
				UE_LOG(LogTemp, Display, TEXT("[VrgDev] %s 컨테인먼트 %d 저장 %s: %s"), *It->GetName(), Mode, bSaved ? TEXT("성공") : TEXT("실패"), *File);
			}
		}
	}));
#endif

// Shows or hides the automatic room shell of every running integrated actor (display only).
static FAutoConsoleCommand GVrgRoomShellCommand(
	TEXT("vrg.Room.Shell"),
	TEXT("자동 방 벽·천장 표시. 표시 전용이며 계산은 바뀌지 않는다. vrg.Room.Shell <0|1> [잘라보기 0|1]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->bAutoRoomShell = FCString::Atoi(*Args[0]) != 0;
				if (Args.Num() > 1) It->bRoomShellCutaway = FCString::Atoi(*Args[1]) != 0;
				It->UpdateRoomShell();
			}
		}
	}));

// Switches the air cooling of every running integrated actor and rebuilds it (the clock restarts at 0).
static FAutoConsoleCommand GVrgSimCrahLayoutCommand(
	TEXT("vrg.Sim.CrahLayout"),
	TEXT("공기 냉각 방식. 0 = 천장 뭉친 코일, 1 = 인로우 쿨러, 2 = 벽면 CRAH. vrg.Sim.CrahLayout <0|1|2>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const ECrahLayout Layout = static_cast<ECrahLayout>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->CrahLayout = Layout;
				It->RebuildCoupling();
				UE_LOG(LogTemp, Display, TEXT("[VrgSim] 공기 냉각 방식 %d로 재구축"), int32(Layout));
			}
		}
	}));

static FAutoConsoleCommand GVrgSimCoolantCommand(
	TEXT("vrg.Sim.CoolantRejection"),
	TEXT("냉각수 열 방출. 0 = 랙별 라디에이터, 1 = 줄마다 CDU 열교환기. vrg.Sim.CoolantRejection <0|1>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const ECoolantRejection Mode = static_cast<ECoolantRejection>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 1));
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->CoolantRejection = Mode;
				It->RebuildCoupling();
				UE_LOG(LogTemp, Display, TEXT("[VrgSim] 냉각수 열 방출 %d로 재구축"), int32(Mode));
			}
		}
	}));

static FAutoConsoleCommand GVrgPlantEnableCommand(
	TEXT("vrg.Plant.Enable"),
	TEXT("S8 설비 계통 결합. 0 = CDU 설비수·CRAH 냉수 고정 경계(기본, 설비 마당은 표시만), 1 = 설비 망·열 모델이 CDU·CRAH에 결합. 다음 시뮬레이션 진입 때 적용. vrg.Plant.Enable <0|1>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->bFacilityPlant = FCString::Atoi(*Args[0]) != 0;
				UE_LOG(LogTemp, Display, TEXT("[Plant] 설비수 수력망 %s (다음 시뮬레이션 진입 때 적용)"), It->bFacilityPlant ? TEXT("켬") : TEXT("끔"));
			}
		}
	}));

static FAutoConsoleCommand GVrgPlantTopologyCommand(
	TEXT("vrg.Plant.Topology"),
	TEXT("S8 설비 구성. 0 = 분리(FWS 온수 + 냉수, 기본), 1 = 공용 냉수(CDU가 냉수 루프에 CRAH와 병렬), 2 = 직렬(CRAH 회수가 CDU로). 다음 시뮬레이션 진입 때 적용. vrg.Plant.Topology <0|1|2>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const EPlantTopology T = static_cast<EPlantTopology>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->PlantThermalSpec.Topology = T;
				UE_LOG(LogTemp, Display, TEXT("[Plant] 구성 %s (다음 시뮬레이션 진입 때 적용)"), FFacilityPlant::TopologyName(T));
			}
		}
	}));

static FAutoConsoleCommand GVrgPlantFaultCommand(
	TEXT("vrg.Plant.Fault"),
	TEXT("S8 설비 장애를 지금 넣는다(시뮬레이션 모드, 설비 켬). 시각을 정하려면 vrg.Eval.Event <t> plant <인자>. vrg.Plant.Fault leak <fws|chw|cw> <supply|return|branchsupply|branchreturn> <분기> <mm> | leakclear | pump <루프> <번호> | chiller <대수> | trim <대수> | drycooler <fws|cw> <대수>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (GEngine == nullptr) return;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) It->ApplyPlantFault(Args);
		}
	}));

static FAutoConsoleCommand GVrgPlantShowCommand(
	TEXT("vrg.Plant.Show"),
	TEXT("S8 설비 마당(장비·설비 배관) 표시. 0 = 숨김, 1 = 표시(기본, 배치·시뮬레이션 모드, vrg.Plant.Enable과 무관). 설비가 계산될 때 색은 유량, vrg.Pipes.Temperature 1이면 온도, 아니면 계통 색. vrg.Plant.Show <0|1>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->bShowPlant = FCString::Atoi(*Args[0]) != 0;
				It->UpdatePlantDisplay(true);
			}
		}
	}));

static FAutoConsoleCommand GVrgPlantAmbientCommand(
	TEXT("vrg.Plant.Ambient"),
	TEXT("S8 외기 건구온도 [C]를 바로 바꾼다(드라이쿨러). 시각을 정하려면 vrg.Eval.Event <t> ambient <C>. vrg.Plant.Ambient <C>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const double K = FCString::Atod(*Args[0]) + 273.15;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->PlantThermalSpec.AmbientK = K;
				It->GetMutableFacilityPlant().SetAmbientK(K);
				UE_LOG(LogTemp, Display, TEXT("[Plant] 외기 %.2f C"), K - 273.15);
			}
		}
	}));

static FAutoConsoleCommand GVrgSimContainmentCommand(
	TEXT("vrg.Sim.Containment"),
	TEXT("공기 차단. 0 = 없음, 1 = 블랭킹(줄 틈 막기), 2 = 블랭킹 + 콜드 아일 지붕. vrg.Sim.Containment <0|1|2>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0 || GEngine == nullptr) return;
		const EAisleContainment Mode = static_cast<EAisleContainment>(FMath::Clamp(FCString::Atoi(*Args[0]), 0, 2));
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				It->Containment = Mode;
				It->RebuildCoupling();
				UE_LOG(LogTemp, Display, TEXT("[VrgSim] 공기 차단 %d로 재구축"), int32(Mode));
			}
		}
	}));

void ADataCenterSimulationActor::BeginPlay()
{
	Super::BeginPlay();
	if (bStartInLayoutMode)
	{
		EnterLayoutMode();
		return;
	}
	if (RoomAirDomain.bManual)
	{
		EnterLayoutMode();
		EnterSimulationMode();
		return;
	}
	Mode = ESimMode::Simulation;
	RebuildCoupling();
	StartSim();
}

void ADataCenterSimulationActor::HoldModelsForLayout()
{
	UWorld* World = GetWorld();
	if (World == nullptr) return;
	for (TActorIterator<ARackThermalActor> It(World); It; ++It)
	{
		It->bDrivenExternally = true;  // no standalone integration either
		It->PauseSim();
	}
	for (TActorIterator<AAirFieldDebugActor> It(World); It; ++It)
	{
		It->bDrivenExternally = true;
		It->PauseSim();
		It->SetLayoutMode(true);
	}
}

void ADataCenterSimulationActor::EnterLayoutMode()
{
	Mode = ESimMode::Layout;
	bSimRunning = false;
	if (Room != nullptr) Room->SetLeakTopologyLocked(false);
	PendingWarmupSteps = 0;
	VrgPerf::PendingWarmupSteps() = 0;
	HoldModelsForLayout();
	if (FlowDisplay != nullptr) FlowDisplay->Clear();   // the air view's tubes are components: they would stay in layout mode
	UpdatePlantDisplay(true);   // the yard stays (structure colours) while the layout is edited
	UE_LOG(LogTemp, Display, TEXT("[VrgSim] 배치 모드: 물리 정지. 배치를 바꾼 뒤 시뮬레이션 모드로 들어가면 새로 구성한다."));
}

void ADataCenterSimulationActor::EnterSimulationMode()
{
	FAirGridParams ManualGrid;
	FBox ManualBounds(ForceInit);
	if (!ValidateRoomAirDomain(RoomAirDomain, ManualGrid, ManualBounds, RoomAirDomainMessage))
	{
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 방 설정 거절: %s"), *RoomAirDomainMessage);
		if (GEngine) GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 6.0f, FColor::Red, RoomAirDomainMessage);
		return; // Before mode, clock, rows, models, solver or bridge are changed.
	}
	bDisplaySnapshotValid = false;
	bEvalReady = false;  // the evaluation starts with the run (reset at the first step, when the inlets exist)
	for (FEvalEvent& E : EvalEvents) E.bApplied = false;
	FaultMonitor.Reset(FaultSettings);
	Instruments.Reset(); // New run: no previous latch/contact baseline before the first rebuilt sample.
	LastTankSampleSecond = -1;
	Mode = ESimMode::Simulation;
	FindTargetActors();
	// Rows and hydraulics first: the coupling reads the routed racks and branch pipes.
	// S2b step 5: leak events open holes that exist from the start of the run (built closed, so the topology and
	// the design flows do not change when they open). The rows must exist first: right after load (-ExecCmds on the
	// first frame) the room has not routed its racks yet, the holes were skipped and the event then rebuilt the room
	// mid-run, renumbering the pipes of later rows under the coupling (found 2026-10-20 through the web leak layer).
	if (Room != nullptr)
	{
		Room->SetLeakTopologyLocked(false);
		Room->RebuildRoomNow();   // no-op unless a rebuild is pending or the room has no rows yet
		const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
		for (const FEvalEvent& E : EvalEvents)
			if (E.Kind == TEXT("leak") && Routed.IsValidIndex(E.Index)) Room->EnsureLeakSpec(Routed[E.Index], E.bFlag);
	}
	if (Room != nullptr) { Room->RebuildRoomNow(); Room->RestartCoolantInventory(); Room->SetLeakTopologyLocked(true); }   // S2b: tanks start full
	FindTargetActors();
	if (AirField != nullptr)
	{
		AirField->SetLayoutMode(false);
		if (RoomAirDomain.bManual) AirField->ApplyRoomAirGrid(ManualGrid, ManualBounds.Min);
		else if (AirField->bAutoFitToRacks) AirField->FitToRacks();
		AirField->ResetSim();
		AirField->StartSim();
	}
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr) Rack->ResetSim();
	}
	BuildCoupling();
	bNeedsRebuild = HasUnroutedRacks();
	bSimRunning = true;
	RebuildLiveLeakSites();   // W1: room sites are built closed with the rows; the plant nodes exist once BuildCoupling built it
	// W2: no water anywhere at t = 0; the tanks start full (their leaked volume is the spray's starting point).
	Spray.Reset(BuildSprayGeometry(), Spray.GetSettings(), Spray.GetFluid());
	FloorWater.ResetForRoom(Spray.GetGeometry(), FloorWaterSettings);   // W3: dry floor
	FloorOffGridM3 = 0.0;
	FlowAverager.Reset(); bFlowGridValid = false;   // the air view averages this run only
	SetAirView(AirView);
	SprayTankLeakedM3.Reset();
	// Capture the inventory baseline BEFORE the first integrated step. Initializing it
	// inside SprayAfterStep would discard water from a hole opened at t=0.
	if (Room) for (const auto& Tank : Room->GetCoolantInventory().GetTanks()) SprayTankLeakedM3.Add(Tank.State.LeakedM3);
	InternalLeakedM3.Reset();
	InitializeRackInteriorWater();
	LastSprayLogSecond = -1;
	UE_LOG(LogTemp, Display, TEXT("[VrgSim] 시뮬레이션 모드: 랙 %d대 / 스파인 %d대 / CRAH %d대로 구성하고 t=0부터 시작."),
		Racks.Num(), Spines.Num(), CrahPlacements.Num());
}

bool ADataCenterSimulationActor::TrySetRoomAirDomain(const FRoomAirDomainSettings& Settings, FString& Reason)
{
	if (Mode != ESimMode::Layout) { Reason = TEXT("방 설정은 배치 모드에서만 변경할 수 있습니다."); return false; }
	FAirGridParams Grid; FBox Bounds(ForceInit);
	if (!ValidateRoomAirDomain(Settings, Grid, Bounds, Reason))
	{ RoomAirDomainMessage = Reason; return false; }
	RoomAirDomain = Settings;
	RoomAirDomainMessage = Settings.bManual
		? FString::Printf(TEXT("수동 방: 실제 격자 %dx%dx%d, 원점 %s m, 최대 %s m (시뮬레이션 진입 시 적용)"),
			Grid.NumCellsX, Grid.NumCellsY, Grid.NumCellsZ, *Bounds.Min.ToString(), *Bounds.Max.ToString())
		: TEXT("자동 방 맞춤 사용");
	Reason.Reset();
	return true;
}

bool ADataCenterSimulationActor::TrySetRoomCeilingPlates(const TArray<FRoomCeilingPlate>& Plates, FString& Reason)
{
	if (Mode != ESimMode::Layout) { Reason = TEXT("천장 판은 배치 모드에서만 변경할 수 있습니다."); return false; }
	FAirGridParams Grid; FBox Bounds(ForceInit);
	if (!ValidateRoomAirDomain(RoomAirDomain, Grid, Bounds, Reason, &Plates))
	{ RoomAirDomainMessage = Reason; return false; }
	RoomCeilingPlates = Plates;
	RoomAirDomainMessage = FString::Printf(TEXT("천장 판 %d개 설정 (고체 셀은 다음 시뮬레이션 진입 시 적용)"), Plates.Num());
	return true;
}

namespace
{
	// One required box against the manual room box and its ceiling plates (shared by ValidateRoomAirDomain and the
	// layout placement check, so a placement the layout accepts is one Load and simulation entry accept, 2026-10-23).
	bool VrgRoomBoxFits(const FBox& Requested, const TArray<FRoomCeilingPlan>& CeilingPlans, double H, const FBox& Required, const FString& Name, FString& Reason)
	{
		if (!Required.IsValid || Required.Min.ContainsNaN() || Required.Max.ContainsNaN())
		{ Reason = Name + TEXT(": 유효하지 않은 장비/덕트 경계입니다."); return false; }
		for (int32 Axis = 0; Axis < 3; ++Axis)
			if (Required.Min[Axis] < Requested.Min[Axis] || Required.Max[Axis] > Requested.Max[Axis])
			{
				Reason = FString::Printf(TEXT("%s: %c축 필요 범위 [%.6f, %.6f]m가 방 [%.6f, %.6f]m 밖입니다."),
					*Name, TCHAR('X' + Axis), Required.Min[Axis], Required.Max[Axis], Requested.Min[Axis], Requested.Max[Axis]);
				return false;
			}
		for (int32 I = 0; I < CeilingPlans.Num(); ++I)
		{
			const FBox& B = CeilingPlans[I].ActualBoundsM;
			if (VrgRoomAirDomain::CeilingIntersectsBox(CeilingPlans[I], Required, H))
			{ Reason = FString::Printf(TEXT("천장 판 %d 계산 범위 %s m가 %s와 겹칩니다."), I, *B.ToString(), *Name); return false; }
		}
		return true;
	}

	bool VrgRoomEquipmentFits(const FBox& Requested, const TArray<FRoomCeilingPlan>& CeilingPlans, double H, double InletSampleDepthM, double ExhaustDepthM,
		const FVector& SizeM, const FTransform& Transform, const FString& Name, bool bCabinet, bool bCrah, bool bTopIntake, FString& Reason)
	{
		if (SizeM.ContainsNaN() || SizeM.GetMin() <= 0 || !Transform.IsValid() || Transform.GetScale3D().GetAbsMin() <= 0)
		{ Reason = Name + TEXT(": 장비 크기/변환이 유효하지 않습니다."); return false; }
		FTransform M = Transform; M.SetTranslation(SimUnits::UnrealToMeters(Transform.GetTranslation()));
		const FVector MinM(-SizeM.X * .5, -SizeM.Y * .5, 0), MaxM(SizeM.X * .5, SizeM.Y * .5, SizeM.Z);
		if (!VrgRoomBoxFits(Requested, CeilingPlans, H, VrgRoomAirDomain::TransformedBoxM(MinM, MaxM, M), Name + TEXT(" 외형"), Reason)) return false;
		if (!bCabinet && !bCrah) return true;
		// Current coupling uses unscaled, upright boxes and a horizontal forward axis. Check this envelope
		// in addition to the actual full actor transform; do not silently replace the existing physics model.
		FVector Forward = Transform.GetRotation().GetForwardVector().GetSafeNormal2D();
		if (Forward.IsNearlyZero()) { Reason = Name + TEXT(": 수평 공기 방향이 없습니다."); return false; }
		if (bCrah) Forward = FMath::Abs(Forward.X) >= FMath::Abs(Forward.Y)
			? FVector(Forward.X >= 0 ? 1 : -1, 0, 0) : FVector(0, Forward.Y >= 0 ? 1 : -1, 0);
		M.SetRotation(Forward.Rotation().Quaternion()); M.SetScale3D(FVector::OneVector);
		FVector DuctMin = MinM, DuctMax = MaxM;
		if (bCabinet)
		{
			if (!FMath::IsFinite(InletSampleDepthM) || !FMath::IsFinite(ExhaustDepthM)
				|| InletSampleDepthM <= 0 || ExhaustDepthM <= 0)
			{ Reason = TEXT("흡기/배기 표본 깊이가 유효하지 않습니다."); return false; }
			DuctMin.X -= FMath::Max(H, ExhaustDepthM); DuctMax.X += FMath::Max(H, InletSampleDepthM);
		}
		else
		{
			// Required neighbouring fluid cell: front supply; rear in-row intake or top perimeter intake.
			DuctMax.X += H;
			if (bTopIntake) DuctMax.Z += H; else DuctMin.X -= H;
		}
		return VrgRoomBoxFits(Requested, CeilingPlans, H, VrgRoomAirDomain::TransformedBoxM(DuctMin, DuctMax, M), Name + TEXT(" 공기 덕트"), Reason);
	}
}

bool ADataCenterSimulationActor::CheckLayoutBoxInManualRoom(const FVector& SizeM, const FTransform& Transform, bool bCabinet, bool bCrah, bool bTopIntake, FString& Reason) const
{
	Reason.Reset();
	if (!RoomAirDomain.bManual || GetWorld() == nullptr) return true;
	const AAirFieldDebugActor* Field = nullptr;
	for (TActorIterator<AAirFieldDebugActor> It(GetWorld()); It; ++It) { Field = *It; break; }
	if (Field == nullptr) return true;
	FAirGridParams Grid; FBox Actual(ForceInit);
	TArray<FRoomCeilingPlan> CeilingPlans;
	if (!VrgRoomAirDomain::BuildGrid(RoomAirDomain, Field->GridParams, Grid, Actual, Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(RoomAirDomain, RoomCeilingPlates, Grid, Actual, CeilingPlans, Reason)) return false;
	const FBox Requested(FVector(RoomAirDomain.MinXYM.X, RoomAirDomain.MinXYM.Y, 0),
		FVector(RoomAirDomain.MaxXYM.X, RoomAirDomain.MaxXYM.Y, RoomAirDomain.CeilingHeightM));
	return VrgRoomEquipmentFits(Requested, CeilingPlans, Grid.CellSizeM, InletSampleDepthM, ExhaustDepthM, SizeM, Transform,
		TEXT("배치 장비"), bCabinet, bCrah, bTopIntake, Reason);
}
bool ADataCenterSimulationActor::ValidateRoomAirDomain(const FRoomAirDomainSettings& Settings,
	FAirGridParams& OutGrid, FBox& OutActualBoundsM, FString& Reason, const TArray<FRoomCeilingPlate>* CeilingOverride)
{
	Reason.Reset();
	const auto& Plates = CeilingOverride ? *CeilingOverride : RoomCeilingPlates;
	if (!Settings.bManual)
	{
		if (Plates.IsEmpty()) return true; // Preserve legacy auto-fit, including worlds with no room setting.
		Reason = TEXT("천장 판은 수동 방 범위에서만 설정할 수 있습니다. 자동 전환 전 판을 제거하세요."); return false;
	}
	if (!GetWorld()) { Reason = TEXT("방 검증에 실행 월드가 필요합니다."); return false; }
	AAirFieldDebugActor* Field = nullptr;
	for (TActorIterator<AAirFieldDebugActor> It(GetWorld()); It; ++It)
	{
		if (Field) { Reason = TEXT("수동 방은 공기 격자 액터 한 대를 요구합니다."); return false; }
		Field = *It;
	}
	if (!Field) { Reason = TEXT("공기 격자 액터가 없습니다."); return false; }
	FAirGridParams Grid; FBox Actual(ForceInit);
	if (!VrgRoomAirDomain::BuildGrid(Settings, Field->GridParams, Grid, Actual, Reason)) return false;
	const FBox Requested(FVector(Settings.MinXYM.X, Settings.MinXYM.Y, 0),
		FVector(Settings.MaxXYM.X, Settings.MaxXYM.Y, Settings.CeilingHeightM));
	const double H = Grid.CellSizeM;
	TArray<FRoomCeilingPlan> CeilingPlans;
	if (!VrgRoomAirDomain::BuildCeilingPlans(Settings, Plates, Grid, Actual, CeilingPlans, Reason)) return false;
	auto Contains = [&](const FBox& Required, const FString& Name)
	{
		return VrgRoomBoxFits(Requested, CeilingPlans, H, Required, Name, Reason);
	};
	auto CheckBox = [&](const FVector& SizeM, const FTransform& Transform, const FString& Name,
		bool bCabinet, bool bCrah, bool bTopIntake)
	{
		return VrgRoomEquipmentFits(Requested, CeilingPlans, H, InletSampleDepthM, ExhaustDepthM, SizeM, Transform, Name, bCabinet, bCrah, bTopIntake, Reason);
	};	bool bPlacedCrahs = false;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		FVector SizeM;
		if (!ADataCenterDebugController::PlaceableBoxM(*It, SizeM)) continue;
		// Walls/curtains are clipped to the room (cells outside the grid are simply not solid); a wall on the
		// room boundary is common and must not refuse the room.
		if (It->IsA<ALayoutBarrierActor>()) continue;
		// S8 F8: plant units stand outside the air domain by rule (layout placement refuses the room footprint).
		if (It->IsA<APlantEquipmentActor>()) continue;
		const ACrahUnitActor* Crah = Cast<ACrahUnitActor>(*It);
		bPlacedCrahs |= Crah != nullptr;
		if (!CheckBox(SizeM, It->GetActorTransform(), It->GetName(),
			It->IsA<ARackThermalActor>() || It->IsA<ASpineCabinetActor>(),
			Crah != nullptr && CrahLayout != ECrahLayout::CeilingCoil, Crah && Crah->Kind == ECrahUnitKind::Perimeter)) return false;
	}
	// Read the fresh world, not potentially stale routed racks. Share the planner, but neither filter to
	// the OLD grid nor spawn/convert automatic CRAHs. Restore every planning cache on exit.
	TGuardValue<TArray<TObjectPtr<ARackThermalActor>>> RackGuard(Racks, {});
	TGuardValue<TArray<TObjectPtr<ASpineCabinetActor>>> SpineGuard(Spines, {});
	TGuardValue<TArray<FCabinetRow>> RowGuard(CabinetRows, {});
	TGuardValue<TArray<FColdAisle>> AisleGuard(ColdAisles, {});
	TGuardValue<TArray<FCrahPlacement>> PlacementGuard(CrahPlacements, {});
	TGuardValue<int32> UnpairedGuard(NumUnpairedRows, 0);
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It) Racks.Add(*It);
	for (TActorIterator<ASpineCabinetActor> It(GetWorld()); It; ++It) Spines.Add(*It);
	GroupCabinetRows();
	if (!bPlacedCrahs && CrahLayout != ECrahLayout::CeilingCoil)
	{
		PlanAutoCrahUnits();
		for (const FCrahPlacement& P : CrahPlacements)
			if (!CheckBox(P.SizeM, FTransform(P.ForwardM.Rotation(), SimUnits::MetersToUnreal(P.BaseM)),
				TEXT("자동 CRAH"), false, true, P.bTopIntake)) return false;
	}
	if (Containment == EAisleContainment::BlankingAndColdAisleRoof)
		for (const FColdAisle& Aisle : ColdAisles)
			if (Aisle.TopZ + H > Requested.Max.Z)
			{ Reason = TEXT("콜드아일 지붕의 한 셀 두께가 천장 밖입니다."); return false; }
	OutGrid = Grid; OutActualBoundsM = Actual;
	return true;
}

#if WITH_EDITOR
void ADataCenterSimulationActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bNeedsRebuild = true;
}
#endif

void ADataCenterSimulationActor::RebuildCoupling()
{
	bNeedsRebuild = true;
	if (Mode == ESimMode::Layout)
	{
		// A deleted actor must not remain in the bridge's raw thermal-model pointers until the next run/GC.
		Bridge.SetAirSolver(nullptr);
		Bridge.SetHydraulicNetwork(nullptr);
		Bridge.Initialize({}, {});
		Racks.Reset();
		Spines.Reset();
		bDisplaySnapshotValid = false;
	}
}

void ADataCenterSimulationActor::StartSim()
{
	bSimRunning = true;
}

void ADataCenterSimulationActor::PauseSim()
{
	bSimRunning = false;
}

FCouplingSnapshot ADataCenterSimulationActor::GetSnapshot() const
{
	return Bridge.MakeSnapshot(Clock.GetSimTimeS());
}

bool ADataCenterSimulationActor::HasUnroutedRacks() const
{
	if (!Racks.IsEmpty() || !GetWorld()) return false;
	// Preserve startup's row-discovery retry, but an intentionally empty layout is not a failed setup.
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It) return true;
	return false;
}

void ADataCenterSimulationActor::FindTargetActors()
{
	Room = nullptr;
	AirField = nullptr;
	Racks.Reset();
	Spines.Reset();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It)
	{
		Room = *It;
		break;
	}
	for (TActorIterator<AAirFieldDebugActor> It(World); It; ++It)
	{
		AirField = *It;
		break;
	}

	for (TActorIterator<ASpineCabinetActor> It(World); It; ++It)
	{
		It->EnsureThermalModel();
		Spines.Add(*It);
	}

	// 랙 순서는 방 액터가 줄로 묶은 순서를 쓴다. 분기 파이프 인덱스가 그 순서로 나온다.
	if (Room != nullptr)
	{
		for (ARackThermalActor* Rack : Room->GetRoutedRacks())
		{
			Racks.Add(Rack);
		}
	}
	else
	{
		for (TActorIterator<ARackThermalActor> It(World); It; ++It)
		{
			Racks.Add(*It);
		}
	}
}

void ADataCenterSimulationActor::MarkRackSolidCells()
{
	SolidOwner.Reset();
	CeilingSolidCells = 0;
	if (AirField == nullptr || (!bMarkRacksAsSolid && RoomCeilingPlates.IsEmpty()))
	{
		return;
	}

	FAirSolver& Solver = AirField->GetMutableSolver();
	FUniformAirGrid& Grid = Solver.GetMutableGrid();
	Grid.ClearSolidCells();
	SolidOwner.Init(INDEX_NONE, Grid.GetNumCells());

	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
	if (!bMarkRacksAsSolid)
	{
		// Keep the existing equipment-solid debug switch independent of the new ceiling geometry.
		MarkCeilingSolidCells(Grid, GridOriginM);
		Solver.OnSolidCellsChanged();
		return;
	}

	for (int32 I = 0; I < Racks.Num() + Spines.Num(); ++I)
	{
		const ARackThermalActor* Rack = I < Racks.Num() ? Racks[I].Get() : nullptr;
		const AActor* Cabinet = I < Racks.Num() ? static_cast<const AActor*>(Rack) : static_cast<const AActor*>(Spines[I - Racks.Num()].Get());
		const FVector SizeM = Rack ? Rack->RackSizeM : FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		if (Cabinet == nullptr)
		{
			continue;
		}

		const FVector RackM = SimUnits::UnrealToMeters(Cabinet->GetActorLocation());
		const FVector Forward = Cabinet->GetActorForwardVector().GetSafeNormal2D();
		const FVector Right = Cabinet->GetActorRightVector().GetSafeNormal2D();

		// 앞뒤 한 겹은 남긴다. 랙은 공기가 앞으로 들어와 뒤로 나가는 통로라
		// 통째로 막으면 흡기·배기 셀이 격자에서 끊긴다.
		const double SolidDepthM = FMath::Max(SizeM.X - 2.0 * Grid.GetCellSizeM(), 0.0);
		if (SolidDepthM <= 0.0)
		{
			continue;
		}

		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		{
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			{
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					// 셀 중심을 랙 로컬 좌표로 옮겨 상자 안인지 본다.
					const FVector CellWorldM = GridOriginM + Grid.CellCenterM(X, Y, Z);
					const FVector OffsetM = CellWorldM - RackM;

					const double AlongDepthM = FMath::Abs(FVector::DotProduct(OffsetM, Forward));
					const double AlongWidthM = FMath::Abs(FVector::DotProduct(OffsetM, Right));

					if (AlongDepthM <= SolidDepthM * 0.5
						&& AlongWidthM <= SizeM.Y * 0.5
						&& OffsetM.Z >= 0.0 && OffsetM.Z <= SizeM.Z)
					{
						Grid.bSolidCell[Grid.CellIndex(X, Y, Z)] = true;
						SolidOwner[Grid.CellIndex(X, Y, Z)] = I;
					}
				}
			}
		}
	}

	// CRAH units own the ids after the cabinets. In-row units leave one cell front and rear open like a rack;
	// perimeter units are solid through (their intake is the top face). Cells a cabinet already owns stay its.
	const int32 NumCabinets = Racks.Num() + Spines.Num();
	for (int32 U = 0; U < CrahPlacements.Num(); ++U)
	{
		const FCrahPlacement& P = CrahPlacements[U];
		const double HalfDepthM = .5 * (P.bTopIntake ? P.SizeM.X : FMath::Max(P.SizeM.X - 2.0 * Grid.GetCellSizeM(), 0.0));
		const FVector Right(-P.ForwardM.Y, P.ForwardM.X, 0.0);
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					const int32 Index = Grid.CellIndex(X, Y, Z);
					const FVector OffsetM = GridOriginM + Grid.CellCenterM(X, Y, Z) - P.BaseM;
					if (SolidOwner[Index] == INDEX_NONE && FMath::Abs(FVector::DotProduct(OffsetM, P.ForwardM)) <= HalfDepthM
						&& FMath::Abs(FVector::DotProduct(OffsetM, Right)) <= .5 * P.SizeM.Y && OffsetM.Z >= 0.0 && OffsetM.Z <= P.SizeM.Z)
					{
						Grid.bSolidCell[Index] = true;
						SolidOwner[Index] = NumCabinets + U;
					}
				}
	}

	// Containment (passive solids, owner PassiveSolidOwner). Only free cells: cabinets and CRAH units keep theirs.
	//  Blanking: the row's body band (same depth band as a rack body) between its first and last cabinet, full
	//   height. Fills the gaps between cabinets and the slivers between neighbours so air cannot cross the row line.
	//  Cold-aisle roof: one cell layer just above the rack tops, from rack body front to rack body front over the
	//   whole aisle length. The aisle ends stay open: the fans here do not respond to pressure, so a sealed aisle
	//   could not balance the rack intake against the cooler supply.
	PassiveSolidCells = 0;
	if (Containment != EAisleContainment::None)
	{
		const double H = Grid.GetCellSizeM();
		auto MarkPassive = [&](int32 Index)
		{
			if (SolidOwner[Index] != INDEX_NONE) return;
			Grid.bSolidCell[Index] = true;
			SolidOwner[Index] = PassiveSolidOwner;
			++PassiveSolidCells;
		};
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					const FVector CellM = GridOriginM + Grid.CellCenterM(X, Y, Z);
					for (const FCabinetRow& Row : CabinetRows)
					{
						const double BodyHalf = .5 * FMath::Max(Row.Depth - 2.0 * H, 0.0);
						const double A = FVector::DotProduct(CellM, Row.Along);
						if (FMath::Abs(FVector::DotProduct(CellM, Row.Forward) - Row.Across) <= BodyHalf
							&& A >= Row.MinAlong && A <= Row.MaxAlong && CellM.Z >= Row.BaseZ && CellM.Z <= Row.BaseZ + Row.Height)
						{
							MarkPassive(Grid.CellIndex(X, Y, Z));
						}
					}
					if (Containment != EAisleContainment::BlankingAndColdAisleRoof) continue;
					for (const FColdAisle& Aisle : ColdAisles)
					{
						const FCabinetRow& RA = CabinetRows[Aisle.RowA];
						const FCabinetRow& RB = CabinetRows[Aisle.RowB];
						const double Across = FVector::DotProduct(CellM, Aisle.Forward);
						const double LoAcross = RA.Across + .5 * FMath::Max(RA.Depth - 2.0 * H, 0.0);
						const double HiAcross = -RB.Across - .5 * FMath::Max(RB.Depth - 2.0 * H, 0.0);
						const double A = FVector::DotProduct(CellM, Aisle.Along);
						if (Across >= LoAcross && Across <= HiAcross && A >= Aisle.MinAlong && A <= Aisle.MaxAlong
							&& CellM.Z > Aisle.TopZ && CellM.Z <= Aisle.TopZ + H)
						{
							MarkPassive(Grid.CellIndex(X, Y, Z));
						}
					}
				}
	}

	MarkBarrierSolidCells(Grid, GridOriginM);
	MarkCeilingSolidCells(Grid, GridOriginM);
	Solver.OnSolidCellsChanged();
}

void ADataCenterSimulationActor::MarkCeilingSolidCells(FUniformAirGrid& Grid, const FVector& GridOriginM)
{
	CeilingSolidCells = 0;
	if (RoomCeilingPlates.IsEmpty()) return;
	const auto& GP = Grid.GetParams();
	const FBox Bounds(GridOriginM, GridOriginM + FVector(GP.NumCellsX, GP.NumCellsY, GP.NumCellsZ) * GP.CellSizeM);
	TArray<FRoomCeilingPlan> Plans; FString Reason;
	if (!VrgRoomAirDomain::BuildCeilingPlans(RoomAirDomain, RoomCeilingPlates, GP, Bounds, Plans, Reason))
	{
		UE_LOG(LogTemp, Error, TEXT("[VrgSim] 천장 셀 계획 실패: %s"), *Reason);
		return; // Entry prevalidation supplies this same grid; never publish a partial plan.
	}
	const auto Added = VrgRoomAirDomain::MarkCeilingCells(Grid, Plans);
	for (int32 Index : Added) SolidOwner[Index] = CeilingSolidOwner;
	CeilingSolidCells = Added.Num(); // existing equipment/roof/wall ownership is preserved
}

void ADataCenterSimulationActor::MarkBarrierSolidCells(FUniformAirGrid& Grid, const FVector& GridOriginM)
{
	BarrierSolidCells = 0;
	BarrierCellsKeptOpen = 0;
	const double H = Grid.GetCellSizeM();
	if (!GetWorld() || H <= 0.0) return;

	// Air paths of the ducted equipment: its box widened by one cell along its airflow axis (front and rear) and,
	// for a perimeter unit, one cell above its top intake. Duct faces sit between the equipment cells and these
	// fluid cells; a barrier cell there would make the duct reject its faces. Those cells stay open and are
	// counted (a wall flush against a rack front therefore keeps a one-cell gap in front of the rack).
	struct FReserved { FTransform Frame; FVector HalfM; double ZMaxM; };
	TArray<FReserved> Reserved;
	auto Reserve = [&](const FVector& BaseM, const FQuat& Rotation, const FVector& SizeM, bool bTopIntake)
	{
		Reserved.Add({FTransform(Rotation, BaseM), FVector(.5 * SizeM.X + H, .5 * SizeM.Y, 0.0), SizeM.Z + (bTopIntake ? H : 0.0)});
	};
	auto ReserveActor = [&](const AActor* Actor, const FVector& SizeM, bool bTopIntake)
	{
		const FTransform T = Actor->GetActorTransform();
		Reserve(SimUnits::UnrealToMeters(T.GetLocation()), T.GetRotation(), SizeM * T.GetScale3D().GetAbs(), bTopIntake);
	};
	for (const ARackThermalActor* Rack : Racks) if (Rack) ReserveActor(Rack, Rack->RackSizeM, false);
	for (const ASpineCabinetActor* Spine : Spines)
		if (Spine) ReserveActor(Spine, FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM), false);
	for (const FCrahPlacement& P : CrahPlacements)
		Reserve(P.BaseM, P.ForwardM.Rotation().Quaternion(), P.SizeM, P.bTopIntake);

	// Each half extent is at least 0.51 cell: a face step moves a cell centre by at most one cell along any unit
	// normal, so a slab at least one cell thick catches every face path (watertight). Grid-resolution limit:
	// a 2 cm curtain is calculated one cell thick. Display thickness is the authored one.
	const double MinHalfM = .51 * H;
	for (TActorIterator<ALayoutBarrierActor> It(GetWorld()); It; ++It)
	{
		const FTransform T = It->GetActorTransform();
		if (!VrgBarrierLifecycle::IsValidTransform(T)) continue;
		const FVector SizeM = It->GetSizeM();
		const FQuat Rotation = T.GetRotation();
		const FVector CentreM = SimUnits::UnrealToMeters(T.TransformPosition(FVector(0, 0, 50.0)));  // local unit-cube centre
		const FVector HalfM(FMath::Max(.5 * SizeM.X, MinHalfM), FMath::Max(.5 * SizeM.Y, MinHalfM), FMath::Max(.5 * SizeM.Z, MinHalfM));
		// World AABB of the box -> grid index range.
		const FVector Extent = Rotation.GetAxisX().GetAbs() * HalfM.X + Rotation.GetAxisY().GetAbs() * HalfM.Y + Rotation.GetAxisZ().GetAbs() * HalfM.Z;
		const FVector LoM = CentreM - Extent - GridOriginM, HiM = CentreM + Extent - GridOriginM;
		const int32 X0 = FMath::Max(0, FMath::FloorToInt32(LoM.X / H)), X1 = FMath::Min(Grid.GetNumCellsX() - 1, FMath::FloorToInt32(HiM.X / H));
		const int32 Y0 = FMath::Max(0, FMath::FloorToInt32(LoM.Y / H)), Y1 = FMath::Min(Grid.GetNumCellsY() - 1, FMath::FloorToInt32(HiM.Y / H));
		const int32 Z0 = FMath::Max(0, FMath::FloorToInt32(LoM.Z / H)), Z1 = FMath::Min(Grid.GetNumCellsZ() - 1, FMath::FloorToInt32(HiM.Z / H));
		for (int32 Z = Z0; Z <= Z1; ++Z)
			for (int32 Y = Y0; Y <= Y1; ++Y)
				for (int32 X = X0; X <= X1; ++X)
				{
					const int32 Index = Grid.CellIndex(X, Y, Z);
					if (SolidOwner[Index] != INDEX_NONE) continue;  // equipment, containment or another barrier
					const FVector CellM = GridOriginM + Grid.CellCenterM(X, Y, Z);
					const FVector Local = Rotation.UnrotateVector(CellM - CentreM);
					if (FMath::Abs(Local.X) > HalfM.X || FMath::Abs(Local.Y) > HalfM.Y || FMath::Abs(Local.Z) > HalfM.Z) continue;
					bool bReserved = false;
					for (const FReserved& R : Reserved)
					{
						const FVector L = R.Frame.InverseTransformPositionNoScale(CellM);
						if (FMath::Abs(L.X) <= R.HalfM.X && FMath::Abs(L.Y) <= R.HalfM.Y && L.Z >= 0.0 && L.Z <= R.ZMaxM) { bReserved = true; break; }
					}
					if (bReserved) { ++BarrierCellsKeptOpen; continue; }
					Grid.bSolidCell[Index] = true;
					SolidOwner[Index] = BarrierSolidOwner;
					++BarrierSolidCells;
				}
	}
}

void ADataCenterSimulationActor::BuildCoolingCoil()
{
	if (AirField == nullptr)
	{
		Bridge.AirSolverInput.CellCoolingUAWK.Reset();
		return;
	}

	const FUniformAirGrid& Grid = AirField->GetMutableSolver().GetGrid();
	Bridge.AirSolverInput.CellCoolingUAWK.Init(0.0, Grid.GetNumCells());
	Bridge.AirSolverInput.CoolingSupplyTempK = CoilSupplyTempK;
	Bridge.AirSolverInput.GravityZMs2 = AirField->GravityZMs2;
	Bridge.AirSolverInput.bEnableBuoyancy = AirField->bEnableBuoyancy;

	// CRAH units carry the cooling; the lumped coil stays off (both at once would double the capacity).
	if (CrahUnits.Num() > 0)
	{
		Bridge.AirSolverInput.CellCoolingUAWK.Reset();
		EffectiveCoilUAWK = 0.0;
		return;
	}

	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());

	// 코일 높이 이상인 공기 셀을 모은다.
	TArray<int32> CoilCells;
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
	{
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
		{
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!Grid.IsFluidCell(X, Y, Z))
				{
					continue;
				}
				const double CellZM = GridOriginM.Z + Grid.CellCenterM(X, Y, Z).Z;
				if (CellZM >= CoilBottomZM)
				{
					CoilCells.Add(Grid.CellIndex(X, Y, Z));
				}
			}
		}
	}

	if (CoilCells.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterSimulationActor: 코일 높이 %.2f m 위에 공기 셀이 없습니다. ")
			TEXT("CoilBottomZM을 낮추거나 격자를 더 높게 잡으세요."),
			CoilBottomZM);
		return;
	}

	EffectiveCoilUAWK = bAutoSizeCoil && DesignAirLoadW > 0.0 ? DesignAirLoadW / CoilDesignApproachK : CoilTotalUAWK;

	// 전체 UA를 코일 셀에 고르게 나눈다.
	const double PerCellUAWK = EffectiveCoilUAWK / CoilCells.Num();
	for (const int32 Index : CoilCells)
	{
		Bridge.AirSolverInput.CellCoolingUAWK[Index] = PerCellUAWK;
	}
}

void ADataCenterSimulationActor::BuildCoupling()
{
	FindTargetActors();

	LastSetupMessage.Reset();

	if (Racks.Num() == 0)
	{
		LastSetupMessage = TEXT("레벨에서 랙을 찾지 못했습니다.");
		// Last-rack deletion still rebuilds solids/ducts/clock and retains any spine thermal targets below.
	}

	// 바깥이 돌리므로 랙과 공기 액터는 자기 적분을 멈춘다 (규약 4).
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->bDrivenExternally = true;
			Rack->ConformInputs();
		}
	}
	if (AirField != nullptr)
	{
		AirField->bDrivenExternally = true;
	}

	// Design air load: component heat that leaves through air (per-part UA split for mixed parts).
	// Sizes both the lumped coil and the CRAH units.
	DesignAirLoadW = 0.0;
	for (const ARackThermalActor* Rack : Racks)
	{
		if (Rack) DesignAirLoadW += Rack->GetDesignHeatToAirW();
	}
	for (const ASpineCabinetActor* Spine : Spines)
	{
		if (Spine) DesignAirLoadW += Spine->GetDesignHeatToAirW();
	}

	// 고체 표시가 먼저다. 코일 셀을 고를 때 고체를 빼야 한다.
	GroupCabinetRows();
	PlanCrahUnits();
	MarkRackSolidCells();
	BuildCrahUnits();
	BuildCoolingCoil();
	UpdateCrahMeshes();
	UpdateContainmentMeshes();

	const TArray<int32>& BranchIndices = Room != nullptr
		? Room->GetRackBranchPipeIndices()
		: TArray<int32>();

	TArray<FRackCouplingTarget> Targets;
	TArray<FRackCouplingLink> Links;
	Targets.Reserve(Racks.Num());
	Links.Reserve(Racks.Num());

	const bool bHasAir = (AirField != nullptr);
	const FUniformAirGrid* Grid = bHasAir ? &AirField->GetMutableSolver().GetGrid() : nullptr;
	const FVector GridOriginM = bHasAir
		? SimUnits::UnrealToMeters(AirField->GetActorLocation())
		: FVector::ZeroVector;

	int32 NumRacksWithAir = 0;
	int32 NumFanFaces = 0;

	for (int32 RackIndex = 0; RackIndex < Racks.Num() + Spines.Num(); ++RackIndex)
	{
		ARackThermalActor* Rack = Racks.IsValidIndex(RackIndex) ? Racks[RackIndex].Get() : nullptr;
		ASpineCabinetActor* Spine = RackIndex >= Racks.Num() ? Spines[RackIndex - Racks.Num()].Get() : nullptr;
		AActor* Cabinet = Rack ? static_cast<AActor*>(Rack) : static_cast<AActor*>(Spine);
		const FVector SizeM = Rack ? Rack->RackSizeM : FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		if (Cabinet == nullptr)
		{
			continue;
		}

		FRackCouplingTarget Target;
		Target.Model = Rack ? &Rack->GetMutableModel() : &Spine->GetMutableModel();
		Target.Input = Rack ? &Rack->GetMutableInput() : &Spine->GetMutableInput();
		Target.Radiator = Rack ? &Rack->GetRadiatorParams() : nullptr;
		Targets.Add(Target);

		FRackCouplingLink Link;
		Link.BranchPipeIndex = Rack && BranchIndices.IsValidIndex(RackIndex) ? BranchIndices[RackIndex] : INDEX_NONE;
		Link.FallbackCoolantMassFlowKgs = Target.Input->CoolantMassFlowKgs;
		if (Rack != nullptr && Room != nullptr) Room->GetRackLeakIndices(Rack, Link.SupplyLeakIndices, Link.ReturnLeakIndices);
		if (Rack && Room)
			if (const FInRackHandles* Internal = Room->GetInRackHandles(Rack))
			{
				Link.Internal = *Internal;
				// The original return edge starts at the rack, before the external leak split.
				Link.ReturnPipeIndex = Internal->ReturnNode != INDEX_NONE
					? Room->GetRackReturnPipeIndex(Rack) : INDEX_NONE;
			}

		if (bHasAir)
		{
			// 랙 앞뒤로 한 덩어리씩 셀을 잡는다.
			// 앞(+X 로컬)이 흡기, 뒤가 배기다. 셀 높이는 슬롯별로 다시 나눌 때 쓴다.
			TArray<double> InletCellZ, ExhaustCellZ;
			const FVector RackM = SimUnits::UnrealToMeters(Cabinet->GetActorLocation());
			const FVector Forward = Cabinet->GetActorForwardVector().GetSafeNormal2D();
			const FVector Right = Cabinet->GetActorRightVector().GetSafeNormal2D();
			const double HalfDepthM = SizeM.X * 0.5;

			for (int32 Z = 0; Z < Grid->GetNumCellsZ(); ++Z)
			{
				for (int32 Y = 0; Y < Grid->GetNumCellsY(); ++Y)
				{
					for (int32 X = 0; X < Grid->GetNumCellsX(); ++X)
					{
						if (!Grid->IsFluidCell(X, Y, Z))
						{
							continue;
						}

						const FVector CellWorldM = GridOriginM + Grid->CellCenterM(X, Y, Z);
						const FVector OffsetM = CellWorldM - RackM;

						// 랙 폭과 높이 안쪽이어야 한다.
						if (FMath::Abs(FVector::DotProduct(OffsetM, Right)) > SizeM.Y * 0.5
							|| OffsetM.Z < 0.0 || OffsetM.Z > SizeM.Z)
						{
							continue;
						}

						const double AlongM = FVector::DotProduct(OffsetM, Forward);

						// 앞쪽: 랙 앞면 바깥 InletSampleDepthM 안.
						if (AlongM > HalfDepthM && AlongM <= HalfDepthM + InletSampleDepthM)
						{
							Link.InletCellIndices.Add(Grid->CellIndex(X, Y, Z));
							InletCellZ.Add(OffsetM.Z);
						}
						// 뒤쪽: 랙 뒷면 바깥 ExhaustDepthM 안.
						else if (AlongM < -HalfDepthM && AlongM >= -HalfDepthM - ExhaustDepthM)
						{
							Link.ExhaustCellIndices.Add(Grid->CellIndex(X, Y, Z));
							ExhaustCellZ.Add(OffsetM.Z);
						}
					}
				}
			}

			if (Link.ExhaustCellIndices.Num() > 0)
			{
				++NumRacksWithAir;
			}

			// Per-slot cells: a cell serves every slot whose height range it overlaps. With 0.25 m cells
			// and 1U devices several slots share one cell layer; the air grid resolves height to its cell size.
			const double HalfCellM = .5 * Grid->GetCellSizeM();
			auto BuildSlotLists = [&](const TArray<int32>& Cells, const TArray<double>& CellZ, TArray<int32>& Start, TArray<int32>& Out)
			{
				Start.Reset(); Out.Reset();
				for (int32 Slot = 0; Slot < Target.Model->GetNumSleds(); ++Slot)
				{
					double BottomM = 0.0, HeightM = 0.0;
					if (Rack) Rack->GetSlotHeightRangeM(Slot, BottomM, HeightM);
					else { BottomM = Spine->GetSwitchZM(Slot); HeightM = GjRackLayout::UnitM; }
					Start.Add(Out.Num());
					for (int32 K = 0; K < Cells.Num(); ++K)
						if (CellZ[K] + HalfCellM > BottomM && CellZ[K] - HalfCellM < BottomM + HeightM) Out.Add(Cells[K]);
				}
				Start.Add(Out.Num());
			};
			BuildSlotLists(Link.InletCellIndices, InletCellZ, Link.SlotInletStart, Link.SlotInletCells);
			BuildSlotLists(Link.ExhaustCellIndices, ExhaustCellZ, Link.SlotExhaustStart, Link.SlotExhaustCells);

			if (bFanThroughFlow)
			{
				BuildFanFaces(RackIndex, *Cabinet, *Grid, GridOriginM, Rack, Spine, Target.Model->GetNumSleds(), Link);
				NumFanFaces += Link.FanFaceIndex.Num();
			}
		}

		Links.Add(Link);
	}

	Bridge.Initialize(Targets, Links);
	Bridge.SetCrahUnits(CrahUnits);
	BuildCduGroups();
	Bridge.SetHydraulicNetwork(Room != nullptr ? &Room->GetMutableNetwork() : nullptr);
	Bridge.SetAirSolver(bHasAir ? &AirField->GetMutableSolver() : nullptr);
	Bridge.SetCoolantDensityKgM3(CoolantDensityKgM3);
	if (Room != nullptr) Room->SetCoolantDensityKgM3(CoolantDensityKgM3);   // S2b height term: same density (one source)

	// A barrier (or containment) that separates a duct's intake from its exhaust leaves both air regions with a
	// net source: the pressure problem has no solution. Report it at setup instead of only as a solver failure.
	SplitDucts = bHasAir ? VrgAirConnectivity::CountSplitDucts(*Grid, Bridge.AirSolverInput) : 0;
	if (BarrierSolidCells > 0 || BarrierCellsKeptOpen > 0 || SplitDucts > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 벽/차단막 고체 셀 %d개 | 장비 공기 통로라 비운 셀 %d개 | 흡기·배기가 갈린 덕트 %d개"),
			BarrierSolidCells, BarrierCellsKeptOpen, SplitDucts);
	}
	if (SplitDucts > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgSim] 벽/차단막이 덕트 %d개의 흡기와 배기를 다른 공기 영역으로 나눴습니다. 압력 해가 없어 수렴 실패가 납니다."), SplitDucts);
		// An error, not a debug readout: shown even with vrg.Hud.Debug 0.
		if (GEngine) GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()) + 40, 8.0f, FColor::Red,
			FString::Printf(TEXT("벽/차단막이 장비 %d개 덕트의 흡기와 배기를 막아 갈라 놓았습니다 - 벽에 통로를 두세요"), SplitDucts));
	}

	Clock.Reset(FixedStepS);

	// 어디가 안 붙었는지 화면에 적는다. 조용히 예비값으로 도는 것보다 낫다.
	TArray<FString> Missing;
	if (Room == nullptr)
	{
		Missing.Add(TEXT("수력(DataCenterRoomActor)"));
	}
	if (!bHasAir)
	{
		Missing.Add(TEXT("공기(AirFieldDebugActor)"));
	}
	else if (NumRacksWithAir == 0)
	{
		Missing.Add(TEXT("공기 격자와 랙이 겹치지 않음 (FitToRacks를 누르세요)"));
	}
	else if (bFanThroughFlow && NumFanFaces == 0)
	{
		Missing.Add(TEXT("팬 관통 기류 면이 없음 (bMarkRacksAsSolid를 켜야 랙 앞뒤 면이 생긴다)"));
	}
	if (bCrahFellBackToCoil)
	{
		Missing.Add(TEXT("CRAH 유닛을 쓸 수 없어(격자 밖, 고체 표시 꺼짐, 또는 옥트리 경로) 천장 뭉친 코일로 대신함"));
	}
	else if (CrahUnitsDropped > 0)
	{
		Missing.Add(FString::Printf(TEXT("CRAH 유닛 %d대가 공기 격자 밖이거나 다른 장비·CRAH와 겹쳐 빠짐"), CrahUnitsDropped));
	}
	if (SplitDucts > 0)
	{
		Missing.Add(FString::Printf(TEXT("벽/차단막이 덕트 %d개의 흡기와 배기를 갈라 놓음 (압력 해 없음)"), SplitDucts));
	}

	LastSetupMessage = Missing.Num() > 0
		? FString::Printf(TEXT("안 붙은 도메인: %s"), *FString::Join(Missing, TEXT(", ")))
		: FString();
	UpdatePlantDisplay(true);   // the plant was rebuilt: draw it now, not after the display throttle
}

void ADataCenterSimulationActor::BuildFanFaces(int32 CabinetIndex, const AActor& Cabinet, const FUniformAirGrid& Grid,
	const FVector& GridOriginM, const ARackThermalActor* Rack, const ASpineCabinetActor* Spine, int32 NumSlots,
	FRackCouplingLink& Link) const
{
	// Faces between this cabinet's solid cells and the fluid in front of / behind it. The cabinet's forward
	// axis (+X local = intake side) is snapped to the nearest grid axis; racks are placed at 0/90/180/270
	// degrees in this project. Air flows from the front to the rear, i.e. along -Forward.
	const FVector Forward = Cabinet.GetActorForwardVector().GetSafeNormal2D();
	const int32 Axis = FMath::Abs(Forward.X) >= FMath::Abs(Forward.Y) ? 0 : 1;
	const int32 Sign = (Axis == 0 ? Forward.X : Forward.Y) >= 0.0 ? 1 : -1;
	if (FMath::Abs(Axis == 0 ? Forward.X : Forward.Y) < 0.99)
	{
		UE_LOG(LogTemp, Warning, TEXT("DataCenterSimulationActor: %s is not aligned with the air grid; fan faces use the nearest axis."), *Cabinet.GetName());
	}
	const double CabinetZM = SimUnits::UnrealToMeters(Cabinet.GetActorLocation()).Z;

	TMap<int32, int32> LayerOfZ;  // grid Z index -> local layer
	auto AddFace = [&](int32 SolidX, int32 SolidY, int32 Z, int32 Step, bool bExhaust)
	{
		const int32 FX = SolidX + (Axis == 0 ? Step : 0), FY = SolidY + (Axis == 1 ? Step : 0);
		if (!Grid.IsFluidCell(FX, FY, Z)) return;
		// MAC face between two cells along Axis sits at the larger index.
		const int32 FaceIndex = Axis == 0 ? Grid.FaceXIndex(FMath::Max(SolidX, FX), SolidY, Z) : Grid.FaceYIndex(SolidX, FMath::Max(SolidY, FY), Z);
		int32& Layer = LayerOfZ.FindOrAdd(Z, LayerOfZ.Num());
		Link.FanFaceAxis.Add(uint8(Axis));
		Link.FanFaceIndex.Add(FaceIndex);
		Link.FanFaceFluidCell.Add(Grid.CellIndex(FX, FY, Z));
		Link.FanFaceLayer.Add(Layer);
		Link.FanFaceIsExhaust.Add(bExhaust ? 1 : 0);
		Link.FanFaceSign.Add(int8(-Sign));
	};
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				if (!SolidOwner.IsValidIndex(Grid.CellIndex(X, Y, Z)) || SolidOwner[Grid.CellIndex(X, Y, Z)] != CabinetIndex) continue;
				AddFace(X, Y, Z, Sign, false);   // front neighbour: intake
				AddFace(X, Y, Z, -Sign, true);   // rear neighbour: exhaust
			}

	// Layer -> slot weights: the fraction of each slot's height that falls inside the layer's cell height.
	TArray<double> LayerZ; LayerZ.SetNum(LayerOfZ.Num());
	for (const TPair<int32, int32>& P : LayerOfZ) LayerZ[P.Value] = GridOriginM.Z + Grid.CellCenterM(0, 0, P.Key).Z - CabinetZM;
	TArray<double> SlotBottom, SlotHeight;
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		double BottomM = 0.0, HeightM = 0.0;
		if (Rack) Rack->GetSlotHeightRangeM(Slot, BottomM, HeightM);
		else if (Spine) { BottomM = Spine->GetSwitchZM(Slot); HeightM = GjRackLayout::UnitM; }
		SlotBottom.Add(BottomM); SlotHeight.Add(HeightM);
	}
	FCouplingBridge::AssignSlotsToFanLayers(Link, LayerZ, Grid.GetCellSizeM(), SlotBottom, SlotHeight);
}

void ADataCenterSimulationActor::BuildCduGroups()
{
	bPlantBuilt = false;
	CduSummaries.Reset();
	CduGroupRows.Reset();
	TArray<FCduCouplingGroup> Groups;
	if (CoolantRejection == ECoolantRejection::CduHeatExchanger && Room != nullptr)
	{
		const TArray<int32> RackRows = Room->GetRoutedRackRows();
		TMap<int32, int32> GroupOfRow;
		TArray<double> HotWK;
		for (int32 R = 0; R < Racks.Num() && R < RackRows.Num(); ++R)
		{
			const ARackThermalActor* Rack = Racks[R].Get();
			const ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(RackRows[R]);
			if (Rack == nullptr || Cdu == nullptr) continue;  // no CDU for this row: the rack keeps its radiator
			const int32* Found = GroupOfRow.Find(RackRows[R]);
			const int32 G = Found ? *Found : Groups.Num();
			if (!Found)
			{
				GroupOfRow.Add(RackRows[R], G);
				Groups.AddDefaulted();
				HotWK.Add(0.0);
				FCduSummary& S = CduSummaries.AddDefaulted_GetRef();
				S.Name = Cdu->GetName();
				S.FacilitySupplyTempK = Cdu->FacilitySupplyTempK;
			}
			Groups[G].RackIndices.Add(R);
			if (!Found) CduGroupRows.Add(RackRows[R]);
			CduSummaries[G].NumRacks += 1;
			CduSummaries[G].DesignDutyW += FRackThermalModel::DesignHeatToCoolantW(Rack->Params, Rack->Input);
			// Input is overwritten with solved flow while running. Reusing that value on
			// re-entry would resize the HX from the previous run (including leak/valve effects).
			// The room already solved the closed-leak design point; reuse that operating point.
			const TArray<double>& DesignFlowM3s = Room->GetRackDesignFlowM3s();
			const double DesignKgs = DesignFlowM3s.IsValidIndex(R) ? DesignFlowM3s[R] * CoolantDensityKgM3 : Rack->Input.CoolantMassFlowKgs;
			HotWK[G] += DesignKgs * Rack->Params.CoolantCpJKgK;
		}

		// Size every exchanger at its row's design point (assumptions on the CDU actor, not a product rating):
		// facility flow carries the design duty at FacilityDesignRiseK, and UA brings the supply to
		// HxDesignApproachK above the facility water with the racks' design coolant flow.
		for (int32 G = 0; G < Groups.Num(); ++G)
		{
			const ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(RackRows[Groups[G].RackIndices[0]]);
			FCduHeatExchangerParams& Hx = Groups[G].Hx;
			FCduSummary& S = CduSummaries[G];
			Hx.FacilitySupplyTempK = Cdu->FacilitySupplyTempK;
			Hx.FacilityMassFlowKgs = Cdu->FacilityMassFlowKgs > 0.0 ? Cdu->FacilityMassFlowKgs
				: S.DesignDutyW / (Hx.FacilityCpJKgK * Cdu->FacilityDesignRiseK);
			if (Cdu->HxUAWK > 0.0)
			{
				Hx.UAWK = Cdu->HxUAWK;
			}
			else if (S.DesignDutyW > 0.0 && HotWK[G] > 0.0)
			{
				const double FacilityWK = Hx.FacilityMassFlowKgs * Hx.FacilityCpJKgK;
				const double CMin = FMath::Min(HotWK[G], FacilityWK), CMax = FMath::Max(HotWK[G], FacilityWK);
				const double ReturnAboveFacilityK = Cdu->HxDesignApproachK + S.DesignDutyW / HotWK[G];
				double Eps = S.DesignDutyW / (CMin * ReturnAboveFacilityK);
				if (Eps >= .99)
				{
					UE_LOG(LogTemp, Warning, TEXT("%s: 설계 접근 온도 %.1f K는 이 유량으로 못 맞춘다(eps %.3f). eps 0.99로 둔다."),
						*S.Name, Cdu->HxDesignApproachK, Eps);
					Eps = .99;
				}
				Hx.UAWK = CduHeatExchanger::NtuForEffectiveness(Eps, CMin / CMax) * CMin;
			}
			S.UAWK = Hx.UAWK;
			S.FacilityMassFlowKgs = Hx.FacilityMassFlowKgs;
		}
		bPlantActive = false;
		// Built with vrg.Plant.Enable 0 too (2026-10-21): the yard is always drawn; only Enable 1 couples it.
		BuildFacilityLoop(Groups);
	}
	Bridge.SetCduGroups(Groups);
}

void ADataCenterSimulationActor::BuildFacilityLoop(TArray<FCduCouplingGroup>& Groups)
{
	bPlantActive = false;
	LastPlantLogSecond = -1;
	// The design primary flow of every branch is the exchanger's sized facility flow (one density, rule 3: the
	// coupling's coolant density converts m^3/s <-> kg/s, so enabling the plant at design changes no flow).
	FFacilityLoopSpec Spec = PlantSpec;
	Spec.BranchDesignFlowM3s.Reset();
	double DesignHeatW = 0.0;
	for (int32 G = 0; G < Groups.Num(); ++G)
	{
		Spec.BranchDesignFlowM3s.Add(Groups[G].Hx.FacilityMassFlowKgs / CoolantDensityKgM3);
		if (CduSummaries.IsValidIndex(G)) DesignHeatW += CduSummaries[G].DesignDutyW;
		// F5: the CDU pump's input ends up in the coolant and so in the facility water (2026-10-22 audit: left out of the
		// design heat, the trim chiller was sized exactly that short; docs/기록/09).
		const int32 Pump = Room != nullptr && CduGroupRows.IsValidIndex(G) ? Room->GetRowPumpIndex(CduGroupRows[G]) : INDEX_NONE;
		if (Pump != INDEX_NONE) DesignHeatW += Room->GetNetwork().GetPumpElectricalInputPowerW(Pump);
	}
	const double WaterCp = Groups.Num() > 0 ? Groups[0].Hx.FacilityCpJKgK : 4180.0;
	// S8 F3: the CRAH coils as sized by BuildCrahUnits (design air flow, old infinite-water UA). No units (ceiling coil
	// mode) = no chilled water loop; the coil keeps its fixed water temperature.
	TArray<FPlantCrahCoil> Coils;
	if (AirField != nullptr)
	{
		const FAirGridParams& Air = AirField->GetSolver().GetGrid().GetParams();
		const double DesignEps = CrahDesignAirDeltaTK / (CrahDesignAirDeltaTK + CrahDesignApproachK);
		for (const FCrahCouplingUnit& U : CrahUnits)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = Air.DensityKgM3 * U.VolumeFlowM3s * Air.SpecificHeatJKgK;
			C.DesignAirEps = DesignEps;
			C.DesignDutyW = C.AirWK * CrahDesignAirDeltaTK;
			C.OldUAWK = U.CoilUAWK;
			C.CoilLeavingSetpointK = CoilSupplyTempK + CrahDesignApproachK;   // design coil leaving air (water + approach)
		}
	}
	// S8 F8: units placed in layout mode set the count of their kind; kinds with none keep the automatic sizing rule.
	// F8-2: the automatic plant (no placed units) is built too; its units become actors on the first direct addition.
	AutomaticPlant = MakeUnique<FFacilityPlant>();
	if (Groups.Num() == 0 || !AutomaticPlant->Build(Spec, PlantThermalSpec, DesignHeatW, CoolantDensityKgM3, WaterCp, FHydraulicSolveSettings(), Coils, CoilSupplyTempK))
		AutomaticPlant.Reset();
	PlantBuild.bValid = Groups.Num() > 0;
	PlantBuild.Spec = Spec; PlantBuild.DesignHeatW = DesignHeatW; PlantBuild.WaterCp = WaterCp; PlantBuild.Coils = Coils;
	const FPlantEquipmentCounts Placed = VrgPlantEquipment::CountInWorld(GetWorld());
	if (Placed.Any())
	{
		int32 InsideRoom = 0;
		for (TActorIterator<APlantEquipmentActor> It(GetWorld()); It; ++It)
			InsideRoom += VrgPlantEquipment::OverlapsRoom(GetWorld(), It->SizeM, It->GetActorTransform()) ? 1 : 0;
		UE_LOG(LogTemp, Display, TEXT("[Plant] 배치한 설비 장비: FWS 드라이쿨러 %d, CW 드라이쿨러 %d, 냉수 칠러 %d, 트림 칠러 %d (0 = 자동 크기 규칙)%s"),
			Placed.FwsDryCoolers, Placed.CwDryCoolers, Placed.Chillers, Placed.TrimChillers,
			InsideRoom > 0 ? *FString::Printf(TEXT(", 경고: %d대가 공기 영역과 겹친다(공기 격자에는 없다)"), InsideRoom) : TEXT(""));
	}
	if (Groups.Num() == 0 || !BuildPlantWithCounts(Placed))
	{
		const FFacilityLoopResult& R = Plant.GetLoopResult();
		UE_LOG(LogTemp, Warning, TEXT("[Plant] 설비를 만들지 못했다(CDU 분기 %d, 수력 %s, 차압 잔차 %.3e, 속도 %.4f%s). 고정 설비수 경계를 쓴다."),
			Groups.Num(), R.Hydraulics.bConverged ? TEXT("수렴") : TEXT("실패"), R.ControlResidual, R.PumpSpeedRatio,
			R.bSpeedSaturatedHigh ? TEXT(", 최고 속도에서 차압 미달") : TEXT(""));
		return;
	}
	bPlantBuilt = true;
	if (!bFacilityPlant)
	{
		// Display only: the CDUs keep the fixed facility water and the CRAH coils their fixed chilled water.
		UE_LOG(LogTemp, Display, TEXT("[Plant] 설비 계통은 표시만 한다(vrg.Plant.Enable 0): 구성 %s, FWS 드라이쿨러 %d, CW 드라이쿨러 %d. CDU·CRAH는 고정 경계."),
			FFacilityPlant::TopologyName(Plant.GetTopology()), Plant.GetNumDryCoolers(), Plant.GetNumCwDryCoolers());
		return;
	}
	bPlantActive = true;
	{
		// Placed units of a kind this plant does not build (topology, no CRAH units, no trim gap) are not used.
		const bool bFws = Plant.IsThermalActive() && Plant.GetTopology() == EPlantTopology::Separate;
		const int32 Unused = (bFws ? 0 : Placed.FwsDryCoolers) + (Plant.IsCondenserActive() ? 0 : Placed.CwDryCoolers)
			+ (Plant.IsChilledWaterActive() ? 0 : Placed.Chillers) + (Plant.IsTrimActive() ? 0 : Placed.TrimChillers);
		if (Unused > 0)
			UE_LOG(LogTemp, Warning, TEXT("[Plant] 배치한 설비 장비 %d대는 이 설비(구성 %s)에 없는 종류라 쓰이지 않는다."), Unused, FFacilityPlant::TopologyName(Plant.GetTopology()));
	}
	for (int32 G = 0; G < Groups.Num(); ++G)
	{
		Groups[G].Hx.FacilityMassFlowKgs = Plant.GetBranchMassFlowKgs(G);
		if (Plant.IsThermalActive()) Groups[G].Hx.FacilitySupplyTempK = Plant.GetSupplyTempK();
		if (CduSummaries.IsValidIndex(G))
		{
			CduSummaries[G].FacilityMassFlowKgs = Groups[G].Hx.FacilityMassFlowKgs;
			CduSummaries[G].FacilitySupplyTempK = Groups[G].Hx.FacilitySupplyTempK;
		}
	}
	const FFacilityWaterLoop& Loop = Plant.GetLoop();
	const FFacilityLoopResult& R = Plant.GetLoopResult();
	UE_LOG(LogTemp, Display, TEXT("[Plant] 구성 %s%s"), FFacilityPlant::TopologyName(Plant.GetTopology()),
		Plant.GetTopology() != PlantThermalSpec.Topology ? TEXT(" (요청 구성은 CRAH 유닛이 있어야 한다: 분리로 대체)") : TEXT(""));
	if (Plant.GetTopology() == EPlantTopology::Separate)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] FWS: CDU 분기 %d, 펌프 %d+%d(속도 %.6f), 차압 %.1f kPa(설정 %.1f), 총 유량 %.3f L/s(설계 %.3f), 펌프 전력 %.2f kW, 제어 %d회, 수력 반복 %d"),
			Groups.Num(), Loop.GetSpec().DutyPumps, Loop.GetSpec().StandbyPumps, R.PumpSpeedRatio, R.HeaderDeltaPPa * 1e-3, R.SetpointPa * 1e-3,
			Loop.GetTotalPumpFlowM3s() * 1e3, Loop.GetDesignTotalFlowM3s() * 1e3, Loop.GetTotalPumpElectricW() * 1e-3, R.ControlIterations, R.Hydraulics.IterationsUsed);
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] CDU 1차측 %d분기가 냉수 루프에 있다(%s): 설계 CDU 열 %.1f kW, 냉수 루프 총 유량 %.3f L/s, 바이패스 %.3f L/s"),
			Groups.Num(), Plant.GetTopology() == EPlantTopology::Series ? TEXT("CRAH 뒤 직렬") : TEXT("CRAH와 병렬"), DesignHeatW * 1e-3,
			Plant.GetChwLoop().GetTotalPumpFlowM3s() * 1e3, Plant.GetChwLoop().GetBypassFlowM3s() * 1e3);
	}
	if (Plant.IsThermalActive() && Plant.GetTopology() == EPlantTopology::Separate)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] 드라이쿨러 %d대(UA %.1f kW/K, 정격 풍량 %.2f m3/s, 팬 %.2f kW 각), 설계 열 %.1f kW, 공급 설정 %.2f C, 외기 %.2f C(설계 %.2f C), 물 열용량 공급 %.1f / 회수 %.1f MJ/K"),
			Plant.GetNumDryCoolers(), Plant.GetDryCoolerParams().RatedUAWK * 1e-3, Plant.GetDryCoolerParams().RatedAirFlowM3s, Plant.GetDryCoolerParams().RatedFanElectricW * 1e-3,
			DesignHeatW * 1e-3, PlantThermalSpec.SupplySetpointK - 273.15, Plant.GetAmbientK() - 273.15, PlantThermalSpec.DesignAmbientK - 273.15,
			Plant.GetSupplyCapacityJK() * 1e-6, Plant.GetReturnCapacityJK() * 1e-6);
	}
	if (Plant.IsChilledWaterActive())
	{
		for (int32 U = 0; U < CrahUnits.Num(); ++U) Bridge.SetCrahCoil(U, Plant.GetCrahEquivalentUAWK(U), Plant.GetChwSupplyTempK());
		const FFacilityThermalSpec& T = Plant.GetThermalSpec();
		UE_LOG(LogTemp, Display, TEXT("[Plant] CHW: CRAH 코일 %d, 설계 냉수 부하 %.1f kW(공급 %.2f C, 상승 %.2f K), 냉수 %.3f L/s, 칠러 %d+%d(기준 용량 %.1f kW 각, 범위 안 최고 COP %.2f), CW: 설계 응축 열 %.1f kW, 드라이쿨러 %d대, 응축수 설정 %.2f C, 펌프 합계 %.2f kW"),
			CrahUnits.Num(), Plant.GetDesignChwLoadW() * 1e-3, Plant.GetChwSetpointK() - 273.15, T.ChwDesignRiseK,
			Plant.GetChwLoop().GetTotalPumpFlowM3s() * 1e3, T.DutyChillers, T.StandbyChillers, Plant.GetChillerRefCapacityW() * 1e-3, Plant.GetChillerMaxCop(),
			Plant.GetDesignCondenserW() * 1e-3, Plant.GetNumCwDryCoolers(), T.CwSupplySetpointK - 273.15, Plant.GetPumpElectricW() * 1e-3);
	}
	else if (PlantThermalSpec.bChilledWater && !PlantThermalSpec.bFixedSupplyTemp)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] CHW 없음: CRAH 유닛이 없다(천장 코일 모드). 코일 냉수는 고정 %.2f C."), CoilSupplyTempK - 273.15);
	}
	if (Plant.IsTrimActive())
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] 트림 칠러: 설계 외기에서 드라이쿨러가 남기는 열 %.1f kW, %d+%d대(기준 용량 %.1f kW 각), 이코노마이저 UA %.1f kW/K, 응축 설계 합계 %.1f kW"),
			Plant.GetDesignTrimW() * 1e-3, Plant.GetThermalSpec().TrimDutyChillers, Plant.GetThermalSpec().TrimStandbyChillers, Plant.GetTrimRefCapacityW() * 1e-3,
			Plant.GetEconomizerUAWK() * 1e-3, Plant.GetDesignCondenserW() * 1e-3);
	}
}

void ADataCenterSimulationActor::PlantBeforeStep()
{
	if (!bPlantActive || !Plant.IsThermalActive()) return;
	for (int32 G = 0; G < Bridge.GetNumCduGroups(); ++G)
	{
		Bridge.SetCduFacilitySupplyTempK(G, Plant.GetSupplyTempK());
		Bridge.SetCduFacilityFlowKgs(G, Plant.GetBranchMassFlowKgs(G));   // F6: series flows follow the CRAH valves
	}
	// F5: each CDU's pump input becomes heat in its group's coolant (operating point of this step's network solution).
	if (Room != nullptr)
	{
		const FHydraulicNetwork& Net = Room->GetNetwork();
		for (int32 G = 0; G < Bridge.GetNumCduGroups(); ++G)
		{
			const int32 Pump = CduGroupRows.IsValidIndex(G) ? Room->GetRowPumpIndex(CduGroupRows[G]) : INDEX_NONE;
			Bridge.SetCduPumpHeatW(G, Pump != INDEX_NONE ? Net.GetPumpElectricalInputPowerW(Pump) : 0.0);
		}
	}
	if (Plant.IsChilledWaterActive())
	{
		// CRAH valves from the units' mixed intake of the last step (CPU uniform path records it; others hold).
		TArray<double> RemovedW, MixK, SupplyK;
		if (Bridge.GetLastCrahUnitReadings(RemovedW, MixK, SupplyK)) Plant.ControlCrahValves(Clock.GetFixedStepS(), MixK);
		else Plant.MarkCrahReadingsMissing();
		for (int32 U = 0; U < Bridge.GetCrahUnits().Num(); ++U) Bridge.SetCrahCoil(U, Plant.GetCrahEquivalentUAWK(U), Plant.GetChwSupplyTempK());
	}
}

void ADataCenterSimulationActor::PlantAfterStep(double DtS)
{
	if (!bPlantActive || !Plant.IsThermalActive()) return;
	// CRAH coil heat of this step: the air solver's own duct coil total (rule 6; every solver path records it).
	const double CrahW = (Plant.IsChilledWaterActive() && AirField != nullptr) ? AirField->GetSolver().GetLastDuctCoilRemovedW() : 0.0;
	Plant.Step(DtS, Bridge.GetLastCduDutyW(), CrahW);
	const int32 Bucket = FMath::FloorToInt32(Clock.GetSimTimeS() / 5.0);
	if (Bucket == LastPlantLogSecond) return;
	LastPlantLogSecond = Bucket;
	const FFacilityPlantState& S = Plant.GetState();
	const FString NotMet = S.bSetpointNotMet ? FString::Printf(TEXT(" | 드라이쿨러 단독으로는 설정 미달(누적 %.1f s)%s"), S.SetpointNotMetS, Plant.IsTrimActive() ? TEXT(", 나머지는 트림 칠러") : TEXT(""))
		: S.bOverCooling ? FString(TEXT(" | 과냉: 팬이 멈춰도 설정점 아래(회수가 설정보다 차다)")) : FString();
	UE_LOG(LogTemp, Display, TEXT("[Plant] t=%.2f s 설비 전력 %.1f kW(펌프 %.1f, 팬 %.1f, 압축기 %.1f), CDU 펌프 열 %.1f kW, 외기까지 장부 %.3e"),
		Clock.GetSimTimeS(), Plant.GetPlantElectricW() * 1e-3, Plant.GetPumpElectricW() * 1e-3, (Plant.GetFanElectricW() + Plant.GetCwFanElectricW()) * 1e-3,
		(S.ChillerPowerW + S.TrimPowerW) * 1e-3, Bridge.GetLastCduPumpHeatW() * 1e-3, GetSiteEnergyResidual());
	if (Plant.GetTopology() == EPlantTopology::Separate)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant] t=%.2f s 외기 %.2f C | 공급 %.3f C(설정 %.2f) 회수 %.3f C | CDU 듀티 %.1f kW, 드라이쿨러 %.1f kW, 팬 %.3f(목표 %.3f) %.2f kW%s | 설비 장부 %.3e"),
			Clock.GetSimTimeS(), Plant.GetAmbientK() - 273.15, S.SupplyK - 273.15, PlantThermalSpec.SupplySetpointK - 273.15, S.ReturnK - 273.15,
			S.DutyW * 1e-3, S.DryCoolerHeatW * 1e-3, S.FanRatio, S.FanTargetRatio, Plant.GetFanElectricW() * 1e-3, *NotMet, Plant.GetLedgerResidual());
	}
	else
	{
		// Shared / series: no FWS loop; the CDU primaries take chilled water (their inlet is the plant's CDU supply).
		UE_LOG(LogTemp, Display, TEXT("[Plant] t=%.2f s 외기 %.2f C | 구성 %s: CDU 1차측 입구 %.3f C | CDU 듀티 %.1f kW | 설비 장부 %.3e"),
			Clock.GetSimTimeS(), Plant.GetAmbientK() - 273.15, FFacilityPlant::TopologyName(Plant.GetTopology()), Plant.GetSupplyTempK() - 273.15,
			S.DutyW * 1e-3, Plant.GetLedgerResidual());
	}
	if (S.LeakKgs > 0.0)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant]   설비 누수 %.3f kg/s(누적 %.1f kg), 누수 열 %.1f kW(보충수 %.1f C 기준)"),
			S.LeakKgs, S.LeakMassKg, S.LeakHeatW * 1e-3, PlantThermalSpec.MakeupWaterK - 273.15);
	}
	if (Plant.IsChilledWaterActive())
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant]   CHW 공급 %.3f C(설정 %.2f) 회수 %.3f C, 유량 %.2f L/s(밸브 전개 %d·닫힘 %d%s) | CRAH %.1f kW, 칠러 %d대 %.1f kW, 압축기 %.1f kW(COP %.2f)%s%s | CW 공급 %.3f 회수 %.3f C, 드라이쿨러 %.1f kW, 팬 %.2f kW"),
			S.ChwSupplyK - 273.15, Plant.GetChwSetpointK() - 273.15, S.ChwReturnK - 273.15, Plant.GetChwLoop().GetTotalPumpFlowM3s() * 1e3,
			S.CrahValvesFullOpen, S.CrahValvesShut, S.bCrahReadingsMissing ? TEXT(", 유닛별 흡입 온도 없음: 밸브 유지") : TEXT(""),
			S.CrahDutyW * 1e-3, S.ChillersRunning, S.ChillerLoadW * 1e-3,
			S.ChillerPowerW * 1e-3, S.ChillerPowerW > 0.0 ? S.ChillerLoadW / S.ChillerPowerW : 0.0,
			S.bChillerCapacityLimited ? TEXT(" | 칠러 용량 부족") : TEXT(""), S.bChillerOutsideCurves ? TEXT(" | 곡선 범위 밖(카르노 외삽, 추정)") : TEXT(""),
			S.CwSupplyK - 273.15, S.CwReturnK - 273.15, S.CwDryCoolerHeatW * 1e-3, Plant.GetCwFanElectricW() * 1e-3);
	}
	if (Plant.IsTrimActive() || S.bEconomizerOn)
	{
		UE_LOG(LogTemp, Display, TEXT("[Plant]   트림 칠러 %d대 %.1f kW(압축기 %.1f kW)%s, FWS 공급 %s | 이코노마이저 %s %.1f kW(누적 %.0f s, 전환 %d회)"),
			S.TrimRunning, S.TrimLoadW * 1e-3, S.TrimPowerW * 1e-3, S.bTrimLimited ? TEXT(" 용량 부족") : TEXT(""),
			S.bFwsSupplyNotMet ? TEXT("설정 미달") : TEXT("설정 충족"), S.bEconomizerOn ? TEXT("켬") : TEXT("끔"), S.EconomizerW * 1e-3, S.EconomizerOnS, S.EconomizerSwitches);
	}
}

void ADataCenterSimulationActor::GroupCabinetRows()
{
	CabinetRows.Reset();
	ColdAisles.Reset();
	NumUnpairedRows = 0;

	// Rows: cabinets (racks and spines) facing the same grid direction whose centres share the across-row
	// coordinate. Along is a fixed axis per orientation so rows facing each other share it.
	for (int32 I = 0; I < Racks.Num() + Spines.Num(); ++I)
	{
		const ARackThermalActor* Rack = I < Racks.Num() ? Racks[I].Get() : nullptr;
		const AActor* Cabinet = I < Racks.Num() ? static_cast<const AActor*>(Rack) : static_cast<const AActor*>(Spines[I - Racks.Num()].Get());
		if (Cabinet == nullptr) continue;
		const FVector SizeM = Rack ? Rack->RackSizeM : FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		const FVector F2 = Cabinet->GetActorForwardVector().GetSafeNormal2D();
		const bool bAlongX = FMath::Abs(F2.X) < FMath::Abs(F2.Y);
		const FVector Forward = bAlongX ? FVector(0.0, FMath::Sign(F2.Y), 0.0) : FVector(FMath::Sign(F2.X), 0.0, 0.0);
		const FVector M = SimUnits::UnrealToMeters(Cabinet->GetActorLocation());
		const double Across = FVector::DotProduct(M, Forward);
		FCabinetRow* Row = CabinetRows.FindByPredicate([&](const FCabinetRow& R) { return R.Forward.Equals(Forward) && FMath::Abs(R.Across - Across) < .5 * SizeM.X; });
		if (Row == nullptr)
		{
			Row = &CabinetRows.AddDefaulted_GetRef();
			Row->Forward = Forward; Row->Along = bAlongX ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
			Row->Across = Across; Row->Depth = SizeM.X; Row->BaseZ = M.Z;
		}
		Row->Height = FMath::Max(Row->Height, SizeM.Z);
		const double A = FVector::DotProduct(M, Row->Along);
		Row->MinAlong = FMath::Min(Row->MinAlong, A - .5 * SizeM.Y);
		Row->MaxAlong = FMath::Max(Row->MaxAlong, A + .5 * SizeM.Y);
		Row->Spans.Add(FVector2D(A - .5 * SizeM.Y, A + .5 * SizeM.Y));
	}
	for (FCabinetRow& Row : CabinetRows) Row.Spans.Sort([](const FVector2D& L, const FVector2D& R) { return L.X < R.X; });

	// Cold aisles: two rows facing each other (opposite forwards, front planes toward each other).
	TArray<bool> bPaired; bPaired.Init(false, CabinetRows.Num());
	for (int32 I = 0; I < CabinetRows.Num(); ++I)
		for (int32 J = I + 1; J < CabinetRows.Num(); ++J)
		{
			const FCabinetRow& A = CabinetRows[I];
			const FCabinetRow& B = CabinetRows[J];
			if (bPaired[I] || bPaired[J] || !A.Forward.Equals(-B.Forward)) continue;
			const double FrontA = A.Across + .5 * A.Depth;        // in A's forward coordinate
			const double FrontB = -B.Across - .5 * B.Depth;       // B's front, in A's forward coordinate
			if (FrontB <= FrontA) continue;                        // backs toward each other: a hot aisle
			bPaired[I] = bPaired[J] = true;
			FColdAisle& Aisle = ColdAisles.AddDefaulted_GetRef();
			Aisle.RowA = I; Aisle.RowB = J;
			Aisle.Forward = A.Forward; Aisle.Along = A.Along;
			Aisle.Centre = A.Forward * (.5 * (FrontA + FrontB));
			Aisle.MinAlong = FMath::Min(A.MinAlong, B.MinAlong); Aisle.MaxAlong = FMath::Max(A.MaxAlong, B.MaxAlong);
			Aisle.BaseZ = FMath::Min(A.BaseZ, B.BaseZ);
			Aisle.TopZ = FMath::Max(A.BaseZ + A.Height, B.BaseZ + B.Height);
		}
	for (const bool bP : bPaired) NumUnpairedRows += bP ? 0 : 1;
}

void ADataCenterSimulationActor::UpdateContainmentMeshes()
{
	for (UStaticMeshComponent* Mesh : ContainmentMeshes)
	{
		if (Mesh) Mesh->DestroyComponent();
	}
	ContainmentMeshes.Reset();
	if (CrahMesh == nullptr || Containment == EAisleContainment::None || PassiveSolidCells == 0)
	{
		return;
	}
	auto AddBox = [&](const FVector& CentreM, const FVector& Forward, const FVector& SizeM)
	{
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this);
		Mesh->SetStaticMesh(CrahMesh);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetupAttachment(RootComponent);
		Mesh->RegisterComponent();
		Mesh->SetWorldLocationAndRotation(SimUnits::MetersToUnreal(CentreM), Forward.Rotation());
		Mesh->SetWorldScale3D(SizeM);  // X along Forward, Y across it
		ContainmentMeshes.Add(Mesh);
	};
	for (const FCabinetRow& Row : CabinetRows)
		for (int32 I = 0; I + 1 < Row.Spans.Num(); ++I)
		{
			const double GapStart = Row.Spans[I].Y, GapEnd = Row.Spans[I + 1].X;
			if (GapEnd - GapStart < .05) continue;
			const FVector Centre = Row.Forward * Row.Across + Row.Along * (.5 * (GapStart + GapEnd)) + FVector(0.0, 0.0, Row.BaseZ + .5 * Row.Height);
			AddBox(Centre, Row.Forward, FVector(.02, GapEnd - GapStart, Row.Height));  // a panel across the gap at the row centre line
		}
	if (Containment == EAisleContainment::BlankingAndColdAisleRoof)
		for (const FColdAisle& Aisle : ColdAisles)
		{
			const FCabinetRow& RA = CabinetRows[Aisle.RowA];
			const FCabinetRow& RB = CabinetRows[Aisle.RowB];
			const double Lo = RA.Across + .5 * RA.Depth, Hi = -RB.Across - .5 * RB.Depth;  // front to front
			const FVector Centre = Aisle.Forward * (.5 * (Lo + Hi)) + Aisle.Along * (.5 * (Aisle.MinAlong + Aisle.MaxAlong))
				+ FVector(0.0, 0.0, Aisle.TopZ + .01);
			AddBox(Centre, Aisle.Forward, FVector(Hi - Lo, Aisle.MaxAlong - Aisle.MinAlong, .02));
		}
}

bool ADataCenterSimulationActor::GetRoomShellBoundsM(FBox& OutBoundsM) const
{
	const AAirFieldDebugActor* Field = AirField;
	if (Field == nullptr && GetWorld() != nullptr)
	{
		for (TActorIterator<AAirFieldDebugActor> It(GetWorld()); It; ++It) { Field = *It; break; }
	}
	if (Field == nullptr) return false;
	// Simulation: the live grid. Layout: the grid the next simulation entry will build.
	if (Mode == ESimMode::Layout)
	{
		if (RoomAirDomain.bManual)
		{
			FAirGridParams Grid; FBox Actual(ForceInit); FString Reason;
			if (VrgRoomAirDomain::BuildGrid(RoomAirDomain, Field->GridParams, Grid, Actual, Reason))
			{
				OutBoundsM = Actual;
				return true;
			}
		}
		else if (Field->bAutoFitToRacks)
		{
			FAirGridParams Grid; FVector OriginM;
			if (!Field->PlanFitToRacks(Grid, OriginM)) return false;  // nothing to enclose yet
			OutBoundsM = FBox(OriginM, OriginM + FVector(Grid.NumCellsX, Grid.NumCellsY, Grid.NumCellsZ) * Grid.CellSizeM);
			return true;
		}
	}
	OutBoundsM = Field->GetDomainBoundsM();
	return OutBoundsM.IsValid != 0;
}

void ADataCenterSimulationActor::ComputeRoomShellVisibility(const FBox& B, const FVector& C, bool bCutaway, bool OutVisible[NumRoomShellPieces])
{
	// A wall is cut away when the camera is on its outer side; the ceiling when the camera is above it.
	OutVisible[0] = !bCutaway || C.X >= B.Min.X;
	OutVisible[1] = !bCutaway || C.X <= B.Max.X;
	OutVisible[2] = !bCutaway || C.Y >= B.Min.Y;
	OutVisible[3] = !bCutaway || C.Y <= B.Max.Y;
	OutVisible[4] = !bCutaway || C.Z <= B.Max.Z;
}

int32 ADataCenterSimulationActor::GetNumVisibleRoomShellPieces() const
{
	int32 N = 0;
	for (const UStaticMeshComponent* Mesh : RoomShellMeshes) if (Mesh && Mesh->IsVisible()) ++N;
	return N;
}

void ADataCenterSimulationActor::UpdateRoomShell()
{
	FBox BoundsM(ForceInit);
	if (!bAutoRoomShell || CrahMesh == nullptr || !GetRoomShellBoundsM(BoundsM))
	{
		for (UStaticMeshComponent* Mesh : RoomShellMeshes) if (Mesh) Mesh->DestroyComponent();
		RoomShellMeshes.Reset();
		RoomShellBoundsM = FBox(ForceInit);
		return;
	}
	if (RoomShellMeshes.Num() != NumRoomShellPieces)
	{
		for (UStaticMeshComponent* Mesh : RoomShellMeshes) if (Mesh) Mesh->DestroyComponent();
		RoomShellMeshes.Reset();
		for (int32 I = 0; I < NumRoomShellPieces; ++I)
		{
			UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this);
			Mesh->SetStaticMesh(CrahMesh);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetupAttachment(RootComponent);
			Mesh->RegisterComponent();
			if (RoomShellMaterial)
			{
				Mesh->SetMaterial(0, RoomShellMaterial);
				if (UMaterialInstanceDynamic* Mid = Mesh->CreateDynamicMaterialInstance(0))
				{
					// Display colours only: pale walls, slightly darker ceiling.
					Mid->SetVectorParameterValue(TEXT("Color"), I == 4 ? FLinearColor(.62f, .63f, .65f) : FLinearColor(.78f, .78f, .76f));
				}
			}
			RoomShellMeshes.Add(Mesh);
		}
		RoomShellBoundsM = FBox(ForceInit);
	}
	const double T = FMath::Max(RoomShellThicknessM, .01);
	if (!(RoomShellBoundsM.IsValid && RoomShellBoundsM.Equals(BoundsM, 1e-9)) || RoomShellBuiltThicknessM != T)
	{
		// Every piece lies outside the domain; the X walls and the ceiling overlap the corners so they close.
		const FVector Mid = BoundsM.GetCenter(), Size = BoundsM.GetSize();
		const double ZMid = BoundsM.Min.Z + .5 * Size.Z;
		const FVector Centres[NumRoomShellPieces] = {
			FVector(BoundsM.Min.X - .5 * T, Mid.Y, ZMid), FVector(BoundsM.Max.X + .5 * T, Mid.Y, ZMid),
			FVector(Mid.X, BoundsM.Min.Y - .5 * T, ZMid), FVector(Mid.X, BoundsM.Max.Y + .5 * T, ZMid),
			FVector(Mid.X, Mid.Y, BoundsM.Max.Z + .5 * T)};
		const FVector Sizes[NumRoomShellPieces] = {
			FVector(T, Size.Y + 2 * T, Size.Z), FVector(T, Size.Y + 2 * T, Size.Z),
			FVector(Size.X, T, Size.Z), FVector(Size.X, T, Size.Z),
			FVector(Size.X + 2 * T, Size.Y + 2 * T, T)};
		for (int32 I = 0; I < NumRoomShellPieces; ++I)
		{
			// m -> cm only here (rule 1). The engine cube is 100 cm, so the scale is the size in metres.
			RoomShellMeshes[I]->SetWorldLocationAndRotation(SimUnits::MetersToUnreal(Centres[I]), FRotator::ZeroRotator);
			RoomShellMeshes[I]->SetWorldScale3D(Sizes[I]);
		}
		RoomShellBoundsM = BoundsM;
		RoomShellBuiltThicknessM = T;
	}
	bool Visible[NumRoomShellPieces] = {true, true, true, true, true};
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		ComputeRoomShellVisibility(BoundsM, SimUnits::UnrealToMeters(PC->PlayerCameraManager->GetCameraLocation()), bRoomShellCutaway, Visible);
	}
	for (int32 I = 0; I < NumRoomShellPieces; ++I)
	{
		if (RoomShellMeshes[I]->IsVisible() != Visible[I]) RoomShellMeshes[I]->SetVisibility(Visible[I]);
	}
}

void ADataCenterSimulationActor::PlanAutoCrahUnits()
{
	auto Place = [&](const FVector& BaseM, const FVector& Forward, const FVector& SizeM, bool bTopIntake)
	{
		CrahPlacements.Add({BaseM, Forward, SizeM, bTopIntake});
	};

	if (CrahLayout == ECrahLayout::InRow)
	{
		// One cooler at each end of every row, same depth line and orientation as the row: it breathes the hot
		// aisle behind the row and supplies the cold aisle in front of it.
		for (const FCabinetRow& Row : CabinetRows)
		{
			const double HalfWidth = .5 * CrahInRowSizeM.Y;
			for (const double A : {Row.MinAlong - HalfWidth, Row.MaxAlong + HalfWidth})
				Place(Row.Forward * Row.Across + Row.Along * A + FVector(0.0, 0.0, Row.BaseZ), Row.Forward, CrahInRowSizeM, false);
		}
	}
	else
	{
		// One unit at each end of every cold aisle, centred on it, blowing along it.
		const double Offset = CrahPerimeterClearanceM + .5 * CrahPerimeterSizeM.X;
		for (const FColdAisle& Aisle : ColdAisles)
		{
			const FVector Z(0.0, 0.0, Aisle.BaseZ);
			Place(Aisle.Centre + Aisle.Along * (Aisle.MinAlong - Offset) + Z, Aisle.Along, CrahPerimeterSizeM, true);
			Place(Aisle.Centre + Aisle.Along * (Aisle.MaxAlong + Offset) + Z, -Aisle.Along, CrahPerimeterSizeM, true);
		}
		if (NumUnpairedRows > 0) UE_LOG(LogTemp, Warning, TEXT("DataCenterSimulationActor: 마주 보는 줄이 없는 줄 %d개에는 벽면 CRAH를 붙이지 않는다."), NumUnpairedRows);
	}

}

void ADataCenterSimulationActor::PlanCrahUnits()
{
	CrahPlacements.Reset();
	CrahUnitsDropped = 0;
	bUsingPlacedCrahs = false;
	if (CrahLayout == ECrahLayout::CeilingCoil || AirField == nullptr)
	{
		return;
	}

	// Hand-placed units win over the automatic layout. The airflow axis snaps to the nearest grid axis.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ACrahUnitActor> It(World); It; ++It)
		{
			const FVector F2 = It->GetActorForwardVector().GetSafeNormal2D();
			const bool bAlongY = FMath::Abs(F2.X) < FMath::Abs(F2.Y);
			if (FMath::Abs(bAlongY ? F2.Y : F2.X) < .99)
				UE_LOG(LogTemp, Warning, TEXT("%s: 격자 축에 맞춰 90도 단위로 돌려 놓으세요. 가까운 축으로 맞춥니다."), *It->GetName());
			FCrahPlacement P;
			P.BaseM = SimUnits::UnrealToMeters(It->GetActorLocation());
			P.ForwardM = bAlongY ? FVector(0.0, FMath::Sign(F2.Y), 0.0) : FVector(FMath::Sign(F2.X), 0.0, 0.0);
			P.SizeM = It->SizeM;
			P.bTopIntake = It->Kind == ECrahUnitKind::Perimeter;
			P.FlowM3s = It->VolumeFlowM3s;
			P.bPlaced = true;
			CrahPlacements.Add(P);
		}
	}
	if (CrahPlacements.Num() > 0)
	{
		bUsingPlacedCrahs = true;
	}
	else
	{
		PlanAutoCrahUnits();
	}
 FilterCrahPlacementsToGrid(AirField);
}

void ADataCenterSimulationActor::FilterCrahPlacementsToGrid(AAirFieldDebugActor* Field)
{
	// Units must sit inside the air grid, otherwise their faces are missing and they would carry no air.
	const FUniformAirGrid& Grid = Field->GetMutableSolver().GetGrid();
	const FVector GridMinM = SimUnits::UnrealToMeters(Field->GetActorLocation());
	const FVector GridMaxM = GridMinM + Grid.GetCellSizeM() * FVector(Grid.GetNumCellsX(), Grid.GetNumCellsY(), Grid.GetNumCellsZ());
	const int32 Planned = CrahPlacements.Num();
	CrahPlacements.RemoveAll([&](const FCrahPlacement& P)
	{
		// Forward is a grid axis: depth lies along it, width across it.
		const double AlongXM = FMath::Abs(P.ForwardM.X) > .5 ? P.SizeM.X : P.SizeM.Y;
		const double AlongYM = FMath::Abs(P.ForwardM.X) > .5 ? P.SizeM.Y : P.SizeM.X;
		const FVector Half(.5 * AlongXM, .5 * AlongYM, 0.0);
		return P.BaseM.X - Half.X < GridMinM.X || P.BaseM.X + Half.X > GridMaxM.X || P.BaseM.Y - Half.Y < GridMinM.Y
			|| P.BaseM.Y + Half.Y > GridMaxM.Y || P.BaseM.Z < GridMinM.Z || P.BaseM.Z + P.SizeM.Z > GridMaxM.Z;
	});
	CrahUnitsDropped = Planned - CrahPlacements.Num();
}

void ADataCenterSimulationActor::BuildCrahUnits()
{
	CrahUnits.Reset();
	CrahUnitLabels.Reset();
	CrahUnitFlowM3s = CrahUnitUAWK = CrahUnitFanW = 0.0;
	bCrahFellBackToCoil = false;
	if (CrahLayout == ECrahLayout::CeilingCoil || AirField == nullptr)
	{
		return;
	}
	const FUniformAirGrid& Grid = AirField->GetMutableSolver().GetGrid();
	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
	const int32 NumCabinets = Racks.Num() + Spines.Num();
	// (S5 finish, 2026-09-27: the octree path carries ducts now, so CRAH units no longer fall back to the coil there.)
	if (SolidOwner.Num() != Grid.GetNumCells())
	{
		// Nothing marked solid (bMarkRacksAsSolid off): a unit has no body to push air through.
		CrahUnitsDropped += CrahPlacements.Num();
		CrahPlacements.Reset();
		bCrahFellBackToCoil = CrahUnitsDropped > 0;
		return;
	}

	for (int32 U = 0; U < CrahPlacements.Num(); ++U)
	{
		const FCrahPlacement& P = CrahPlacements[U];
		const int32 Axis = FMath::Abs(P.ForwardM.X) > .5 ? 0 : 1;
		const int32 Sign = (Axis == 0 ? P.ForwardM.X : P.ForwardM.Y) > 0.0 ? 1 : -1;
		FCrahCouplingUnit Unit;
		auto AddFace = [&](int32 FaceAxis, int32 FaceIndex, int32 FluidX, int32 FluidY, int32 FluidZ, bool bExhaust, int8 FaceSign)
		{
			if (!Grid.IsFluidCell(FluidX, FluidY, FluidZ)) return;
			Unit.FaceAxis.Add(uint8(FaceAxis)); Unit.FaceIndex.Add(FaceIndex);
			Unit.FaceFluidCell.Add(Grid.CellIndex(FluidX, FluidY, FluidZ));
			Unit.FaceIsExhaust.Add(bExhaust ? 1 : 0); Unit.FaceSign.Add(FaceSign);
		};
		for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
			for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
				for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
				{
					if (SolidOwner[Grid.CellIndex(X, Y, Z)] != NumCabinets + U) continue;
					const int32 FX = X + (Axis == 0 ? Sign : 0), FY = Y + (Axis == 1 ? Sign : 0);   // front neighbour
					const int32 RX = X - (Axis == 0 ? Sign : 0), RY = Y - (Axis == 1 ? Sign : 0);   // rear neighbour
					const auto AlongFace = [&](int32 NX, int32 NY)
					{
						return Axis == 0 ? Grid.FaceXIndex(FMath::Max(X, NX), Y, Z) : Grid.FaceYIndex(X, FMath::Max(Y, NY), Z);
					};
					// Supply leaves through the front: every height for in-row, the lower half for perimeter units.
					const double CellZM = GridOriginM.Z + Grid.CellCenterM(X, Y, Z).Z - P.BaseM.Z;
					if (!P.bTopIntake || CellZM < .5 * P.SizeM.Z)
					{
						if (FX >= 0 && FY >= 0 && FX < Grid.GetNumCellsX() && FY < Grid.GetNumCellsY())
							AddFace(Axis, AlongFace(FX, FY), FX, FY, Z, true, int8(Sign));
					}
					if (P.bTopIntake)
					{
						// Return air enters from above, moving -Z.
						if (Z + 1 < Grid.GetNumCellsZ()) AddFace(2, Grid.FaceZIndex(X, Y, Z + 1), X, Y, Z + 1, false, -1);
					}
					else if (RX >= 0 && RY >= 0 && RX < Grid.GetNumCellsX() && RY < Grid.GetNumCellsY())
					{
						AddFace(Axis, AlongFace(RX, RY), RX, RY, Z, false, int8(Sign));
					}
				}
		Unit.WaterSupplyTempK = CoilSupplyTempK;
		Unit.VolumeFlowM3s = P.FlowM3s;  // 0 = share of the design airflow (sized below)
		if (Unit.HasBothSides())
		{
			CrahUnits.Add(MoveTemp(Unit));
			CrahUnitLabels.Add({ FVector2D(P.BaseM.X, P.BaseM.Y), P.bTopIntake, P.bPlaced });
		}
		else ++CrahUnitsDropped;
	}

	if (CrahUnits.Num() == 0)
	{
		bCrahFellBackToCoil = CrahPlacements.Num() > 0 || CrahUnitsDropped > 0;
		return;
	}

	// Design sizing (assumptions, see the property comments): total airflow carries the design air load at
	// CrahDesignAirDeltaTK; the coil reaches CrahDesignApproachK above the water at that flow.
	// Units with a set airflow keep it; the rest share what the design airflow still needs (never below 0).
	const FAirGridParams& Air = Grid.GetParams();
	const double TotalFlowM3s = DesignAirLoadW / (Air.DensityKgM3 * Air.SpecificHeatJKgK * CrahDesignAirDeltaTK);
	double SetFlowM3s = 0.0; int32 NumShared = 0;
	for (const FCrahCouplingUnit& Unit : CrahUnits) { SetFlowM3s += Unit.VolumeFlowM3s; NumShared += Unit.VolumeFlowM3s > 0.0 ? 0 : 1; }
	const double SharedFlowM3s = NumShared > 0 ? FMath::Max(TotalFlowM3s - SetFlowM3s, 0.0) / NumShared : 0.0;
	const double Effectiveness = CrahDesignAirDeltaTK / (CrahDesignAirDeltaTK + CrahDesignApproachK);
	double SumFlow = 0.0, SumUA = 0.0, SumFan = 0.0;
	for (FCrahCouplingUnit& Unit : CrahUnits)
	{
		if (Unit.VolumeFlowM3s <= 0.0) Unit.VolumeFlowM3s = SharedFlowM3s;
		// Coil sized at this unit's own flow; D8: fan electric input from its operating point, divided once.
		Unit.CoilUAWK = -FMath::Loge(1.0 - Effectiveness) * Air.DensityKgM3 * Unit.VolumeFlowM3s * Air.SpecificHeatJKgK;
		Unit.FanPowerW = CrahFanPressurePa * Unit.VolumeFlowM3s / CrahFanEfficiency;
		SumFlow += Unit.VolumeFlowM3s; SumUA += Unit.CoilUAWK; SumFan += Unit.FanPowerW;
	}
	// Per-unit means, for the summary.
	CrahUnitFlowM3s = SumFlow / CrahUnits.Num();
	CrahUnitUAWK = SumUA / CrahUnits.Num();
	CrahUnitFanW = SumFan / CrahUnits.Num();
}

void ADataCenterSimulationActor::SpawnCrahActorsFromLayout()
{
	UWorld* World = GetWorld();
	if (World == nullptr) return;
	// Pressing twice used to stack a second set on the first (identical boxes: z-fighting, and the copies get no
	// faces). Refuse while any unit is placed.
	int32 Existing = 0;
	for (TActorIterator<ACrahUnitActor> It(World); It; ++It) ++Existing;
	if (Existing > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnCrahActorsFromLayout: 레벨에 CRAH 액터가 이미 %d대 있습니다. 다시 만들려면 먼저 지우세요."), Existing);
		return;
	}
	FindTargetActors();
	if (AirField == nullptr || Racks.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpawnCrahActorsFromLayout: 랙이나 공기 격자를 찾지 못했습니다. 방 액터가 줄을 파악한 뒤 다시 누르세요."));
		return;
	}
	const ECrahLayout Layout = CrahLayout == ECrahLayout::CeilingCoil ? ECrahLayout::InRow : CrahLayout;
	const ECrahLayout Saved = CrahLayout;
	CrahLayout = Layout;
	GroupCabinetRows();
	CrahPlacements.Reset();
	PlanAutoCrahUnits();
	CrahLayout = Saved;
	int32 Spawned = 0;
	for (const FCrahPlacement& P : CrahPlacements)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACrahUnitActor* Unit = World->SpawnActor<ACrahUnitActor>(SimUnits::MetersToUnreal(P.BaseM), P.ForwardM.Rotation(), Params);
		if (Unit == nullptr) continue;
		Unit->Kind = P.bTopIntake ? ECrahUnitKind::Perimeter : ECrahUnitKind::InRow;
		Unit->SizeM = P.SizeM;
		// Refresh the native visual components in editor and packaged runtime alike.
		Unit->RefreshVisuals();
		++Spawned;
	}
	CrahPlacements.Reset();
	bNeedsRebuild = true;
	UE_LOG(LogTemp, Display, TEXT("SpawnCrahActorsFromLayout: CRAH 액터 %d대를 놓았습니다 (%s 배치 기준). 옮긴 뒤 레벨을 저장하세요."),
		Spawned, Layout == ECrahLayout::InRow ? TEXT("인로우") : TEXT("벽면"));
}

void ADataCenterSimulationActor::UpdateCrahMeshes()
{
	for (UStaticMeshComponent* Mesh : CrahMeshes)
	{
		if (Mesh) Mesh->DestroyComponent();
	}
	CrahMeshes.Reset();
	if (CrahMesh == nullptr)
	{
		return;
	}
	// Resolve once per kind; CrahMesh remains the engine cube for containment and missing-asset fallback.
	UStaticMesh* InRowModel = bUseCrahReferenceModels ? CrahVisualModel::LoadReference(false) : nullptr;
	UStaticMesh* PerimeterModel = bUseCrahReferenceModels ? CrahVisualModel::LoadReference(true) : nullptr;
	for (const FCrahPlacement& P : CrahPlacements)
	{
		if (P.bPlaced) continue;  // an ACrahUnitActor draws itself
		const auto Visual = CrahVisualModel::Resolve(P.SizeM, P.bTopIntake,
			P.bTopIntake ? PerimeterModel : InRowModel, CrahMesh);
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this);
		Mesh->SetStaticMesh(Visual.Mesh);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetupAttachment(RootComponent);
		Mesh->RegisterComponent();
		Mesh->SetWorldLocationAndRotation(SimUnits::MetersToUnreal(P.BaseM) + Visual.OffsetCm, P.ForwardM.Rotation());
		Mesh->SetWorldScale3D(Visual.Scale);
		CrahMeshes.Add(Mesh);
	}
}

void ADataCenterSimulationActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdateRoomShell();
	TickWaterDemo();

	if (Mode == ESimMode::Layout)
	{
		// Racks placed or spawned while in layout must not start integrating on their own.
		HoldModelsForLayout();
		// The plant yard is drawn from a built plant. A game that starts in layout mode has none yet: enter the
		// simulation once and come straight back (the same as Enter twice), so the yard shows from the first frame.
		if (!bPlantPreviewTried && GetWorld() != nullptr && GetWorld()->HasBegunPlay())
		{
			bPlantPreviewTried = true;
			if (!bPlantBuilt && bShowPlant)
			{
				UE_LOG(LogTemp, Display, TEXT("[Plant] 배치 모드 시작: 설비 계통을 그리려고 한 번 구성한다(물리 시간은 진행하지 않음)."));
				EnterSimulationMode();
				if (Mode == ESimMode::Simulation) EnterLayoutMode();
			}
		}
		RebuildPlantIfCountsChanged();   // an added / deleted unit gets its pipes now
		UpdatePlantDisplay(false);   // placed units move while dragged
		// The controller's one-line key hint covers layout mode; this banner is a debug readout now.
		if (bShowSummary && GEngine != nullptr && VrgHud::ShowDebugText())
		{
			GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor(120, 220, 255),
				FString::Printf(TEXT("[배치 모드] 물리 정지 | Enter: 시뮬레이션 시작 | G: 바라보는 장비 잡기/놓기 | T: 90도 회전 | C: 컨테인먼트 %s"),
					Containment == EAisleContainment::None ? TEXT("없음") : Containment == EAisleContainment::Blanking ? TEXT("블랭킹") : TEXT("블랭킹 + 지붕")));
		}
		PublishDashboard();
		return;
	}

	if (bNeedsRebuild || HasUnroutedRacks())
	{
		BuildCoupling();

		// 방 액터는 RebuildRoom()이 플래그만 세우고 실제 배치 파악은 자기 Tick에서 한다.
		// 이 액터가 먼저 돌면 GetRoutedRacks()가 비어 있어 랙 0대로 나온다.
		// 한 번 실패하고 포기하면 영원히 연결되지 않으므로 될 때까지 다시 시도한다.
		// 실제로 겪었다 - 통합 액터를 배치했는데 화면에 "랙 0대 / IT 발열 0.0 kW"가 뜨고
		// 공기 액터가 자기 더미 열원으로 계속 돌았다 (시계도 둘로 갈렸다).
		bNeedsRebuild = HasUnroutedRacks();
	}

	// 예약된 예열을 먼저 나눠 소화한다. 다 끝나면 평소 적분으로 돌아간다.
	if (PendingWarmupSteps > 0)
	{
		DrainWarmupSteps();
	}

	if (bSimRunning && PendingWarmupSteps <= 0)
	{
		// 규약 4: 시계가 남은 시간을 버리지 않고 다음 프레임으로 넘긴다.
		// 다만 공기 압력 풀이가 무거워 프레임당 스텝 수는 따로 제한한다.
		const int32 RequestedSteps = Clock.BeginFrame(DeltaTime);
		const int32 NumSteps = FMath::Min(RequestedSteps, MaxStepsPerFrame);

		// 배관을 다시 이었으면 유량이 바뀐 것이다. 다리에 알려 준다.
		const bool bHydraulicsResolved = (Room != nullptr) && Room->ConsumeRebuiltFlag();

		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			{
				// S7-0 계측. 통합 스텝 안에 랙·수력·공기가 모두 들어 있다.
				VRG_SCOPE_COST(STAT_VrgCouplingStep, CouplingMs);
				EvalBeforeStep();
				// S2b step 2: tank pressure -> hydraulic re-solve (only while a leak is open) before the coupling reads flows.
				if (Room != nullptr) Room->StepCoolantInventory(Clock.GetFixedStepS(), Clock.GetSimTimeS());
				const bool bHydraulicsConverged = (Room == nullptr) || Room->GetLastSolveResult().bConverged;
				PlantBeforeStep();
				Bridge.Step(Clock.GetFixedStepS(), bHydraulicsResolved && StepIndex == 0, bHydraulicsConverged);
				PlantAfterStep(Clock.GetFixedStepS());
				SprayAfterStep(Clock.GetFixedStepS()); AirFlowAfterStep(Clock.GetFixedStepS());   // W2: the water that left the holes in this step
			}
			Clock.CompleteStep();
			EvalAfterStep();
			VrgPerf::Accumulator().SolverSteps += 1;
			VrgPerf::Accumulator().SolverMs += Bridge.GetLastAirSolverMs();
			VrgPerf::Accumulator().RackModelsMs += Bridge.GetLastRackModelsMs();
 for(int32 I=0;I<5;++I) VrgPerf::Accumulator().DetailMs[9+I]+=Bridge.GetLastCouplingStageMs(I);
 for(int32 I=0;I<7;++I) VrgPerf::Accumulator().DetailMs[14+I]+=Bridge.GetLastAirStageMs(I);
			if (AirField != nullptr)
			{
				VrgPerf::Accumulator().PressureMs += AirField->GetMutableSolver().GetLastPressureSolveMs();
				VrgPerf::Accumulator().PressureIterations +=
					AirField->GetMutableSolver().GetLastPressureResult().IterationsUsed;
			}
		}

		// 표시용 시각을 맞춰 준다. 이 액터의 시계가 유일한 시계다.
		for (ARackThermalActor* Rack : Racks)
		{
			if (Rack != nullptr)
			{
				Rack->SetExternalSimTimeS(Clock.GetSimTimeS());
			}
		}
		if (AirField != nullptr)
		{
			AirField->SetExternalSimTimeS(Clock.GetSimTimeS());
		}
	}

	if (bShowSummary)
	{
		DrawSummary();
	}
	DrawSpray();
	UpdateAirFlowView();
	UpdatePlantDisplay(false);
	// After the steps so the published copy is the state at this frame's SimTime.
	PublishDashboard();
}

bool ADataCenterSimulationActor::BuildPlantWithCounts(const FPlantEquipmentCounts& Placed)
{
	// S8 F8: units placed in layout mode set the count of their kind; kinds with none keep the automatic sizing rule.
	FFacilityThermalSpec Thermal = PlantThermalSpec;
	FFacilityLoopSpec Spec = PlantBuild.Spec;
	FacilityPlantLayout::ApplyEquipmentCounts(Placed, Thermal, Spec);
	PlantBuild.Placed = Placed;
	return PlantBuild.bValid && Plant.Build(Spec, Thermal, PlantBuild.DesignHeatW, CoolantDensityKgM3, PlantBuild.WaterCp, FHydraulicSolveSettings(),
		PlantBuild.Coils, CoilSupplyTempK);
}

void ADataCenterSimulationActor::RebuildPlantIfCountsChanged()
{
	if (Mode != ESimMode::Layout || !bPlantBuilt || !PlantBuild.bValid || GetWorld() == nullptr) return;
	const FPlantEquipmentCounts Now = VrgPlantEquipment::CountInWorld(GetWorld());
	if (Now == PlantBuild.Placed) return;
	bPlantBuilt = BuildPlantWithCounts(Now);
	UE_LOG(LogTemp, Display, TEXT("[Plant] 배치 모드: 설비 장비 대수가 바뀌어 표시용 설비를 다시 지었다(FWS 드라이쿨러 %d, CW 드라이쿨러 %d, 냉수 칠러 %d, 트림 칠러 %d; 0 = 자동)%s"),
		Now.FwsDryCoolers, Now.CwDryCoolers, Now.Chillers, Now.TrimChillers, bPlantBuilt ? TEXT("") : TEXT(" - 실패: 다음 진입 때 다시 짓는다"));
	UpdatePlantDisplay(true);
}

FPlantDisplayInput ADataCenterSimulationActor::MakePlantDisplayInput() const
{
	FPlantDisplayInput In;
	In.RoomM = AirField != nullptr ? AirField->GetDomainBoundsM() : Room != nullptr ? Room->GetRoomBoundsM() : FBox(ForceInit);
	for (int32 G = 0; G < CduGroupRows.Num(); ++G)
	{
		const ACoolingDistributionUnitActor* Cdu = Room != nullptr ? Room->GetRowCdu(CduGroupRows[G]) : nullptr;
		In.CduPositionsM.Add(Cdu != nullptr ? SimUnits::UnrealToMeters(Cdu->GetActorLocation()) : FVector::ZeroVector);
	}
	for (const FCrahUnitLabel& L : CrahUnitLabels) In.CrahPositionsM.Add(FVector(L.XYM.X, L.XYM.Y, 0.0));
	// S8 F8: placed units in the model's unit order (VrgPlantEquipment::Gather).
	auto Units = [this](EPlantUnitKind Kind)
	{
		TArray<FPlantDisplayUnit> Out;
		for (const APlantEquipmentActor* A : VrgPlantEquipment::Gather(GetWorld(), Kind))
		{
			FPlantDisplayUnit& U = Out.AddDefaulted_GetRef();
			U.CenterM = SimUnits::UnrealToMeters(A->GetActorTransform().TransformPosition(FVector(0, 0, 50.0 * A->SizeM.Z)));
			U.SizeM = A->SizeM;
			U.Rotation = A->GetActorQuat();
		}
		return Out;
	};
	In.FwsDryCoolers = Units(EPlantUnitKind::FwsDryCooler);
	In.CwDryCoolers = Units(EPlantUnitKind::CwDryCooler);
	In.Chillers = Units(EPlantUnitKind::Chiller);
	In.TrimChillers = Units(EPlantUnitKind::TrimChiller);
	In.FwsPumps = Units(EPlantUnitKind::FwsPump);
	In.ChwPumps = Units(EPlantUnitKind::ChwPump);
	In.CwPumps = Units(EPlantUnitKind::CwPump);
	In.Economizers = Units(EPlantUnitKind::Economizer);
	return In;
}

bool ADataCenterSimulationActor::CaptureAutomaticPlantUnits(EPlantUnitKind Kind, TArray<FPlantEquipmentRecord>& Out, FString& Reason) const
{
	Out.Reset();
	if (!AutomaticPlant.IsValid() || !AutomaticPlant->IsBuilt())
	{
		Reason = TEXT("설비 자동 대수를 아직 모른다: CDU가 있는 배치로 한 번 시뮬레이션에 들어간 뒤 추가할 수 있다.");
		return false;
	}
	if (!VrgPlantEquipment::Gather(GetWorld(), Kind).IsEmpty()) return true;
	// The units as drawn now (2026-10-22): the drawn plant has the automatic count of every kind with no placed unit
	// (layout mode rebuilds it when placed counts change), and its yard lanes already make room for the other kinds'
	// placed units, so the converted actors stand exactly where the user saw them. Before a drawn plant exists, the
	// automatic plant's schematic positions (no placed units in the input).
	FPlantDisplayInput In = MakePlantDisplayInput();
	const bool bDrawn = bPlantBuilt && Plant.IsBuilt();
	if (!bDrawn) In.ClearPlaced();
	const FPlantDisplaySnapshot S = PlantDisplay::Build(bDrawn ? Plant : *AutomaticPlant, In);
	for (const FPlantDisplayEquipment& E : S.Equipment)
	{
		EPlantUnitKind EKind;
		if (!VrgPlantEquipment::KindOf(E, EKind) || EKind != Kind || E.bPlaced) continue;
		FPlantEquipmentRecord& R = Out.AddDefaulted_GetRef();
		R.Kind = Kind;
		R.SizeM = E.SizeM;
		// Pumps are drawn turned (suction toward the header); the box origin is its bottom centre.
		R.Transform = FTransform(E.Rotation, SimUnits::MetersToUnreal(E.CenterM - E.Rotation.RotateVector(FVector(0, 0, 0.5 * E.SizeM.Z))));
	}
	return true;
}

void ADataCenterSimulationActor::RebuildLiveLeakSites()
{
	LiveLeakSites.Reset();
	if (Room == nullptr) FindTargetActors();   // before the first entry (e.g. vrg.Leak.Sites in layout mode)
	if (Room != nullptr)
	{
		const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
		for (int32 S = 0; S < Room->GetNumLeakSpecs(); ++S)
		{
			if (Room->GetLeakNetworkIndex(S) == INDEX_NONE) continue;   // not built yet (added after the last rebuild)
			FLiveLeakSite& Site = LiveLeakSites.AddDefaulted_GetRef();
			Site.RoomSpec = S;
			Site.PositionM = Room->GetLeakSpecPositionM(S);
			const int32 RackIndex = Routed.IndexOfByKey(Room->GetLeakSpecRack(S));
			Site.Label = FString::Printf(TEXT("랙 %d %s"), RackIndex,
				Room->IsLeakSpecHeader(S) ? TEXT("공급 헤더") : Room->IsLeakSpecReturnBranch(S) ? TEXT("회수 분기") : TEXT("공급 분기"));
		}
	}
	if (bPlantActive && Plant.IsThermalActive())
	{
		// Only while the plant model runs (vrg.Plant.Enable 1): with it off the yard is drawn as structure only (F7b).
		// Positions: the drawn yard (schematic, F7); the same nodes the plant display draws its leak markers on.
		const FPlantDisplaySnapshot Yard = PlantDisplay::Build(Plant, MakePlantDisplayInput());
		for (const EPlantLoopId Id : { EPlantLoopId::Fws, EPlantLoopId::Chw, EPlantLoopId::Cw })
		{
			const FFacilityWaterLoop* L = Plant.GetLoopFor(Id);
			if (L == nullptr) continue;
			const TArray<FVector>& Pos = Yard.NodePositionsM[uint8(Id)];
			auto Add = [&](EPlantLeakSite PS, int32 Branch, const FString& Name)
			{
				const int32 Node = L->GetLeakSiteNode(PS, Branch);
				if (!Pos.IsValidIndex(Node)) return;
				FLiveLeakSite& Site = LiveLeakSites.AddDefaulted_GetRef();
				Site.bPlant = true; Site.Loop = Id; Site.PlantSite = PS; Site.Branch = Branch; Site.PositionM = Pos[Node];
				Site.Label = FString::Printf(TEXT("설비 %s %s"), FFacilityPlant::LoopName(Id), *Name);
			};
			Add(EPlantLeakSite::SupplyHeader, INDEX_NONE, TEXT("공급 헤더"));
			Add(EPlantLeakSite::ReturnHeader, INDEX_NONE, TEXT("회수 헤더"));
			for (int32 B = 0; B < L->GetNumBranches(); ++B)
			{
				Add(EPlantLeakSite::BranchSupply, B, FString::Printf(TEXT("분기 %d 공급"), B));
				Add(EPlantLeakSite::BranchReturn, B, FString::Printf(TEXT("분기 %d 회수"), B));
			}
		}
	}
	AppendInternalLiveLeakSites();
	for (FLiveLeakSite& Site : LiveLeakSites) Site.SprayDirectionM = DefaultSprayDirection(Site.PositionM, Spray.GetSettings().ConeHalfAngleRad);
}

int32 ADataCenterSimulationActor::FindLiveLeakSite(const FVector& PointM, double MaxDistanceM) const
{
	int32 Best = INDEX_NONE;
	double BestD = MaxDistanceM;
	for (int32 I = 0; I < LiveLeakSites.Num(); ++I)
	{
		if (LiveLeakSites[I].InternalSite != INDEX_NONE) continue; // W1 coordinate commands keep targeting external sites; internal uses an explicit name.
		const double D = FVector::Dist(LiveLeakSites[I].PositionM, PointM);
		if (D <= BestD) { BestD = D; Best = I; }
	}
	return Best;
}

double ADataCenterSimulationActor::GetLiveLeakHoleDiameterM(int32 SiteIndex) const
{
	if (!LiveLeakSites.IsValidIndex(SiteIndex)) return 0.0;
	const FLiveLeakSite& Site = LiveLeakSites[SiteIndex];
	if (Site.InternalSite != INDEX_NONE) return Room ? Room->GetNetwork().GetLeak(Site.NetworkLeak).HoleDiameterM : 0.0;
	if (!Site.bPlant) return Room != nullptr ? Room->GetLeakSpecHoleDiameterM(Site.RoomSpec) : 0.0;
	const FFacilityWaterLoop* L = Plant.GetLoopFor(Site.Loop);
	const int32 K = L != nullptr ? L->FindLeak(Site.PlantSite, Site.Branch) : INDEX_NONE;
	return K != INDEX_NONE ? L->GetNetwork().GetLeak(L->GetLeak(K).NetworkLeak).HoleDiameterM : 0.0;
}

double ADataCenterSimulationActor::GetLiveLeakRuptureDiameterM(int32 SiteIndex) const
{
	if (!LiveLeakSites.IsValidIndex(SiteIndex)) return 0.0;
	const FLiveLeakSite& Site = LiveLeakSites[SiteIndex];
	if (Site.InternalSite != INDEX_NONE) return InRackNetwork::HoseDiameterM;
	if (!Site.bPlant) return Room != nullptr ? Room->GetLeakSpecRuptureDiameterM(Site.RoomSpec) : 0.0;
	// The plant networks carry resistances, not pipe sizes: 150 mm header / 80 mm branch are ESTIMATES for a rupture.
	return Site.PlantSite == EPlantLeakSite::SupplyHeader || Site.PlantSite == EPlantLeakSite::ReturnHeader ? 0.15 : 0.08;
}

bool ADataCenterSimulationActor::SetLiveLeak(int32 SiteIndex, double HoleDiameterM, const TCHAR* Source)
{
	if (Mode != ESimMode::Simulation || !bSimRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Leak] 실시간 누수는 시뮬레이션 모드에서만 낸다(배치 모드는 vrg.Leak.Add)."));
		return false;
	}
	if (!LiveLeakSites.IsValidIndex(SiteIndex) || !FMath::IsFinite(HoleDiameterM) || !(HoleDiameterM >= 0.0)) return false;
	const FLiveLeakSite& Site = LiveLeakSites[SiteIndex];
	const double Before = GetLiveLeakHoleDiameterM(SiteIndex);
	const double NowS = Clock.GetSimTimeS();
	FVrgLeakRecord R;
	R.TimeS = NowS; R.HoleDiameterM = HoleDiameterM; R.Site = Site.Label; R.PointM = Site.PositionM;
	if (Site.InternalSite != INDEX_NONE)
	{
		const auto Routed = Room ? Room->GetRoutedRacks() : TArray<ARackThermalActor*>();
		if (!Routed.IsValidIndex(Site.RackIndex) || !Room->SetInternalLeakHoleDiameterM(Routed[Site.RackIndex], Site.Tray, Site.InternalSite, HoleDiameterM)) return false;
		R.Rack = Site.RackIndex; R.bReturnBranch = Site.InternalSite >= 3;
		UE_LOG(LogTemp, Display, TEXT("[Leak] t=%.2f s 실시간 내부 누수(%s) %s [%s]: %.2f -> %.2f mm"), NowS, Source, *Site.Label, *Site.Key, Before * 1e3, HoleDiameterM * 1e3);
	}
	else if (!Site.bPlant)
	{
		if (Room == nullptr || !Room->SetLeakHoleDiameterM(Site.RoomSpec, HoleDiameterM)) return false;
		R.Rack = Room->GetRoutedRacks().IndexOfByKey(Room->GetLeakSpecRack(Site.RoomSpec));
		R.bReturnBranch = Room->IsLeakSpecReturnBranch(Site.RoomSpec);
		UE_LOG(LogTemp, Display, TEXT("[Leak] t=%.2f s 실시간 누수(%s) 지점 %d %s: %.2f -> %.2f mm, 위치 (%.2f, %.2f, %.2f) m"),
			NowS, Source, SiteIndex, *Site.Label, Before * 1e3, HoleDiameterM * 1e3, Site.PositionM.X, Site.PositionM.Y, Site.PositionM.Z);
	}
	else
	{
		if (!bPlantActive || !Plant.IsThermalActive())
		{
			UE_LOG(LogTemp, Warning, TEXT("[Leak] 설비 지점은 설비 열 모델이 도는 시뮬레이션에서만(vrg.Plant.Enable 1 후 진입)."));
			return false;
		}
		const int32 FaultsBefore = Plant.GetFaultLog().Num();
		if (Plant.SetLeakAt(Site.Loop, Site.PlantSite, Site.Branch, HoleDiameterM, NowS) == INDEX_NONE && HoleDiameterM > 0.0) return false;
		UE_LOG(LogTemp, Display, TEXT("[Leak] t=%.2f s 실시간 누수(%s) 지점 %d %s: %.2f -> %.2f mm, 위치 (%.2f, %.2f, %.2f) m, 설비 장애 기록 %d건"),
			NowS, Source, SiteIndex, *Site.Label, Before * 1e3, HoleDiameterM * 1e3, Site.PositionM.X, Site.PositionM.Y, Site.PositionM.Z,
			Plant.GetFaultLog().Num() - FaultsBefore);
		UpdatePlantDisplay(true);
	}
	FaultMonitor.AddLeak(R);
	return true;
}

bool ADataCenterSimulationActor::SetLiveLeakDirection(int32 SiteIndex, const FVector& DirectionM)
{
	if (!LiveLeakSites.IsValidIndex(SiteIndex) || LiveLeakSites[SiteIndex].InternalSite != INDEX_NONE) return false; // no interior W2 jets
	FLiveLeakSite& Site = LiveLeakSites[SiteIndex];
	Site.SprayDirectionM = DirectionM.IsNearlyZero() ? DefaultSprayDirection(Site.PositionM, Spray.GetSettings().ConeHalfAngleRad) : DirectionM.GetSafeNormal();
	return true;
}

FVector ADataCenterSimulationActor::DefaultSprayDirection(const FVector& PositionM, double HalfAngleRad)
{
	// Seeded by the site position (mm), so a site points the same way in every run and in every site order.
	const FIntVector Mm(FMath::RoundToInt32(PositionM.X * 1e3), FMath::RoundToInt32(PositionM.Y * 1e3), FMath::RoundToInt32(PositionM.Z * 1e3));
	return FLeakSpray::ConeDirection(HashCombine(HashCombine(GetTypeHash(Mm.X), GetTypeHash(Mm.Y)), GetTypeHash(Mm.Z)), HalfAngleRad);
}

FSprayGeometry ADataCenterSimulationActor::BuildSprayGeometry() const
{
	// Boxes in metres, converted here at the actor boundary (rule 1). The same equipment boxes as the layout checks
	// (bottom-centre origin); not the air solid cells (those leave a cell open front and rear for the air path).
	FSprayGeometry G;
	G.RoomM = AirField != nullptr ? AirField->GetDomainBoundsM() : Room != nullptr ? Room->GetRoomBoundsM() : FBox(ForceInit);
	G.GroundZM = G.RoomM.IsValid ? G.RoomM.Min.Z : 0.0;
	auto AddBox = [&G](const FVector& BaseM, const FQuat& Rotation, const FVector& SizeM, ESprayObstacleKind Kind, const FString& Name)
	{
		if (SizeM.ContainsNaN() || SizeM.GetMin() <= 0.0) return;
		FSprayObstacle& O = G.Obstacles.AddDefaulted_GetRef();
		O.CenterM = BaseM + Rotation.RotateVector(FVector(0, 0, .5 * SizeM.Z));
		O.Rotation = Rotation; O.HalfM = .5 * SizeM; O.Kind = Kind; O.Name = Name;
	};
	auto AddActor = [&AddBox](const AActor* A, const FVector& SizeM, const FString& Name)
	{
		const FTransform T = A->GetActorTransform();
		AddBox(SimUnits::UnrealToMeters(T.GetLocation()), T.GetRotation(), SizeM * T.GetScale3D().GetAbs(), ESprayObstacleKind::Equipment, Name);
	};
	for (int32 I = 0; I < Racks.Num(); ++I) if (Racks[I]) AddActor(Racks[I], Racks[I]->RackSizeM, FString::Printf(TEXT("랙 %d"), I));
	for (int32 I = 0; I < Spines.Num(); ++I)
		if (Spines[I]) AddActor(Spines[I], FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM), FString::Printf(TEXT("스파인 %d"), I));
	for (int32 I = 0; I < CrahPlacements.Num(); ++I)
	{
		const FCrahPlacement& P = CrahPlacements[I];
		AddBox(P.BaseM, P.ForwardM.Rotation().Quaternion(), P.SizeM, ESprayObstacleKind::Equipment, FString::Printf(TEXT("CRAH %d"), I));
	}
	if (UWorld* World = GetWorld())
	{
		int32 Cdu = 0;
		for (TActorIterator<ACoolingDistributionUnitActor> It(World); It; ++It) AddActor(*It, It->SizeM, FString::Printf(TEXT("CDU %d"), Cdu++));
		for (TActorIterator<ALayoutBarrierActor> It(World); It; ++It)
		{
			const FTransform T = It->GetActorTransform();
			const FVector SizeM = It->GetSizeM();
			// The barrier's unit cube is centred 50 cm above its origin (local), like MarkBarrierSolidCells.
			AddBox(SimUnits::UnrealToMeters(T.TransformPosition(FVector(0, 0, 50.0))) - T.GetRotation().RotateVector(FVector(0, 0, .5 * SizeM.Z)),
				T.GetRotation(), SizeM, ESprayObstacleKind::Barrier, TEXT("벽"));
		}
		for (int32 K = 0; K < NumPlantUnitKinds; ++K)
			for (const APlantEquipmentActor* A : VrgPlantEquipment::Gather(World, static_cast<EPlantUnitKind>(K)))
				AddActor(A, A->SizeM, TEXT("설비 장비"));
	}
	// Ceiling plates: one air cell thick (their calculated thickness); holes are not cut out (limitation of W2).
	const double PlateM = AirField != nullptr ? AirField->GetMutableSolver().GetGrid().GetCellSizeM() : 0.25;
	for (const FRoomCeilingPlate& P : RoomCeilingPlates)
		AddBox(FVector(.5 * (P.MinXYM.X + P.MaxXYM.X), .5 * (P.MinXYM.Y + P.MaxXYM.Y), P.HeightM), FQuat::Identity,
			FVector(P.MaxXYM.X - P.MinXYM.X, P.MaxXYM.Y - P.MinXYM.Y, PlateM), ESprayObstacleKind::Ceiling, TEXT("천장 판"));
	return G;
}

void ADataCenterSimulationActor::SprayAfterStep(double DtS)
{
	VrgPerf::FScopeTimer WaterCost(&VrgPerf::FFrameCosts::WaterMs); // observer only, includes source routing/interior/W2/W3/logging
	TArray<FSpraySource> Sources;
	if (Room != nullptr)
	{
		// The tank ledger booked this step's leaked volume (sum of the solve's leak flows x Dt, capped at depletion):
		// that exact volume leaves the holes, split by their flows (rule 6, nothing recomputed).
		const FHydraulicNetwork& Net = Room->GetNetwork();
		const TArray<FCoolantInventory::FTank>& Tanks = Room->GetCoolantInventory().GetTanks();
		if (SprayTankLeakedM3.Num() != Tanks.Num())
		{
			SprayTankLeakedM3.SetNumUninitialized(Tanks.Num());
			for (int32 T = 0; T < Tanks.Num(); ++T) SprayTankLeakedM3[T] = Tanks[T].State.LeakedM3;
		}
		for (int32 T = 0; T < Tanks.Num(); ++T)
		{
			const double DeltaM3 = Tanks[T].State.LeakedM3 - SprayTankLeakedM3[T];
			SprayTankLeakedM3[T] = Tanks[T].State.LeakedM3;
			if (DeltaM3 <= 0.0) continue;
			TArray<TPair<int32, double>> Holes;   // network leak, weight
			double Sum = 0.0;
			for (const int32 L : Tanks[T].LeakIndices)
			{
				const double FlowM3s = FMath::Max(Net.GetLeakFlowM3s(L), 0.0);
				if (FlowM3s > 0.0) { Holes.Add({ L, FlowM3s }); Sum += FlowM3s; }
			}
			if (Sum <= 0.0)
			{
				// The step that emptied the tank closed its holes; split by the holes' areas as built.
				for (int32 J = 0; J < Tanks[T].LeakIndices.Num(); ++J)
				{
					const double D = Tanks[T].LeakDiametersM.IsValidIndex(J) ? Tanks[T].LeakDiametersM[J] : 0.0;
					if (D > 0.0) { Holes.Add({ Tanks[T].LeakIndices[J], D * D }); Sum += D * D; }
				}
			}
			for (const TPair<int32, double>& H : Holes)
			{
				if (Room->IsInternalLeak(H.Key))
				{
					InternalLeakedM3.FindOrAdd(H.Key) += DeltaM3 * H.Value / Sum;
					continue; // Keep it in the denominator: never redistribute internal water to W2.
				}
				if (!bLeakSpray) continue;
				int32 SiteIndex = INDEX_NONE;
				for (int32 S = 0; S < LiveLeakSites.Num() && SiteIndex == INDEX_NONE; ++S)
					if (!LiveLeakSites[S].bPlant && Room->GetLeakNetworkIndex(LiveLeakSites[S].RoomSpec) == H.Key) SiteIndex = S;
				if (SiteIndex == INDEX_NONE || Sum <= 0.0) continue;
				const FHydraulicLeak& Leak = Net.GetLeak(H.Key);
				FSpraySource& S = Sources.AddDefaulted_GetRef();
				S.Id = SiteIndex; S.OriginM = LiveLeakSites[SiteIndex].PositionM; S.DirectionM = LiveLeakSites[SiteIndex].SprayDirectionM;
				S.GaugePressurePa = Net.GetNodePressurePa(Leak.NodeIndex) - Leak.AmbientPressurePa;
				S.VolumeM3 = DeltaM3 * H.Value / Sum;
			}
		}
	}
	if (bLeakSpray && bPlantActive && Plant.IsThermalActive())
	{
		// Plant leaks: the make-up keeps the loop full, so the leaked volume is the solve's leak flow x Dt.
		for (int32 SiteIndex = 0; SiteIndex < LiveLeakSites.Num(); ++SiteIndex)
		{
			const FLiveLeakSite& Site = LiveLeakSites[SiteIndex];
			const FFacilityWaterLoop* L = Site.bPlant ? Plant.GetLoopFor(Site.Loop) : nullptr;
			const int32 K = L != nullptr ? L->FindLeak(Site.PlantSite, Site.Branch) : INDEX_NONE;
			const double Q = K != INDEX_NONE ? L->GetLeakFlowM3s(K) : 0.0;
			if (Q <= 0.0) continue;
			const FHydraulicLeak& Leak = L->GetNetwork().GetLeak(L->GetLeak(K).NetworkLeak);
			FSpraySource& S = Sources.AddDefaulted_GetRef();
			S.Id = SiteIndex; S.OriginM = Site.PositionM; S.DirectionM = Site.SprayDirectionM;
			S.GaugePressurePa = L->GetNetwork().GetNodePressurePa(Leak.NodeIndex) - Leak.AmbientPressurePa;
			S.VolumeM3 = Q * DtS;
		}
	}
	RackInteriorAfterStep(DtS); // Both Tick and RunSteps call this shared water path.
	const bool bStepSpray = bLeakSpray && (!Sources.IsEmpty() || Spray.HasWater());
	if (bStepSpray)
	{
		Spray.Step(Sources, DtS, Clock.GetSimTimeS());
		for (const TPair<FVector, double>& D : Spray.GetLastStepFloorDepositsM3())
			if (!FloorWater.AddWater(D.Key, D.Value)) FloorOffGridM3 += D.Value;
	}
	// W3 advances ONCE over the shared interval, including residual water after repair.
	if (FloorWater.HasWater()) FloorWater.Step(DtS, Clock.GetSimTimeS());
	if (!bStepSpray && !FloorWater.HasWater() && RackInterior.GetWater().ReceivedM3 == 0.0) return;
	const int32 Second = FMath::FloorToInt32((Clock.GetSimTimeS() + DtS) / 2.0);
	if (Second != LastSprayLogSecond) { LastSprayLogSecond = Second; LogSprayStatus(); }
}

void ADataCenterSimulationActor::LogSprayStatus() const
{
	const FSprayTotals& T = Spray.GetTotals();
	int32 Wet = 0;
	for (const FSprayWetting& W : Spray.GetWetting()) Wet += W.FirstWetS >= 0.0 ? 1 : 0;
	UE_LOG(LogTemp, Display, TEXT("[Water] t=%.2f s 누적 %.3f L = 공중 %.3f + 구멍 대기 %.3f + 물막 %.3f + 바닥 %.3f + 바깥 땅 %.3f L | 입자 %d (물줄기 %d), 분열 %d, 튐 %d, 젖은 장비 %d, 장비 안 누수 %.3f L | 잔차 %.2e"),
		Clock.GetSimTimeS(), T.EmittedM3 * 1e3, T.AirborneM3 * 1e3, T.WaitingM3 * 1e3, T.FilmM3 * 1e3, T.FloorM3 * 1e3, T.GroundM3 * 1e3,
		T.Parcels, T.JetParcels, T.Breakups, T.Splashes, Wet, T.ThroughEquipmentM3 * 1e3, T.Residual);
	const FFloorWaterTotals& F = FloorWater.GetTotals();
	const FRackInteriorWater& I = RackInterior.GetWater();
	if (I.ReceivedM3 > 0.0 || RackInterior.PendingInputM3(GetInternalLeakedM3()) > 0.0)
		UE_LOG(LogTemp, Display, TEXT("[RackWater] t=%.2f s input %.12g m3, residual %.3e m3, input pending %.12g m3, floor pending %.12g m3"),
			Clock.GetSimTimeS() + Clock.GetFixedStepS(), I.ReceivedM3, I.BalanceErrorM3(), RackInterior.PendingInputM3(GetInternalLeakedM3()), RackInterior.PendingFloorM3());
	int32 Flooded = 0;
	for (const FFloorFlood& Fl : FloorWater.GetFloods()) Flooded += Fl.FirstWetS >= 0.0 ? 1 : 0;
	UE_LOG(LogTemp, Display, TEXT("[Water] t=%.2f s 바닥 물 %.3f L (받음 %.3f L, 격자 밖 %.3f L), 젖은 칸 %d, 최대 수심 %.2f mm, 최대 속도 %.3f m/s, 이번 스텝 부분 스텝 %d(%s, GPU 스텝 누적 %d), 물 밑 장비 %d | 잔차 %.2e, 음수 수심 보정 %.2e L"),
		Clock.GetSimTimeS(), F.StoredM3 * 1e3, F.ReceivedM3 * 1e3, FloorOffGridM3 * 1e3, F.WetCells, F.MaxDepthM * 1e3, F.MaxSpeedMs, F.LastSubsteps,
		F.bLastStepGpu ? TEXT("GPU") : TEXT("CPU"), F.GpuSteps, Flooded, F.Residual, F.ClippedM3 * 1e3);
}

void ADataCenterSimulationActor::TickWaterDemo()
{
	if (WaterDemoHoleM < 0.0 || GetWorld() == nullptr || !GetWorld()->HasBegunPlay()) return;
	if (Mode == ESimMode::Layout)
	{
		// -ExecCmds runs on the first frame: wait for the layout tick's one preview build before entering.
		if (bPlantPreviewTried) EnterSimulationMode();
		return;
	}
	if (!bSimRunning || Room == nullptr || Clock.GetSimTimeS() < 1.0) return;
	if (LiveLeakSites.IsEmpty()) RebuildLiveLeakSites();
	const FBox DomainM = AirField != nullptr ? AirField->GetDomainBoundsM() : Room->GetRoomBoundsM();
	const FVector CentreM = DomainM.IsValid ? DomainM.GetCenter() : FVector::ZeroVector;
	int32 Best = INDEX_NONE;
	double BestD = TNumericLimits<double>::Max();
	for (int32 I = 0; I < LiveLeakSites.Num(); ++I)
	{
		const FLiveLeakSite& S = LiveLeakSites[I];
		if (S.bPlant || S.InternalSite != INDEX_NONE || Room->IsLeakSpecHeader(S.RoomSpec) || Room->IsLeakSpecReturnBranch(S.RoomSpec) || Room->GetLeakSpecRack(S.RoomSpec) == nullptr) continue;
		const double D = FVector::DistXY(S.PositionM, CentreM);
		if (D < BestD) { BestD = D; Best = I; }
	}
	if (Best == INDEX_NONE) return;   // rows not routed yet
	const double HoleM = WaterDemoHoleM;
	WaterDemoHoleM = -1.0;
	const FLiveLeakSite& Site = LiveLeakSites[Best];
	FVector Away = Site.PositionM - SimUnits::UnrealToMeters(Room->GetLeakSpecRack(Site.RoomSpec)->GetActorLocation());
	Away.Z = 0.0;
	Away = Away.IsNearlyZero() ? FVector(1, 0, 0) : Away.GetSafeNormal();
	FVector Along = FVector::CrossProduct(FVector::UpVector, Away);
	if (FVector::DotProduct(CentreM - Site.PositionM, Along) < 0.0) Along = -Along;
	SetLiveLeakDirection(Best, Away + FVector(0, 0, .25));
	if (!SetLiveLeak(Best, HoleM, TEXT("vrg.Water.Demo"))) return;
	const double FloorZ = DomainM.IsValid ? DomainM.Min.Z : 0.0;
	FVector EyeM = Site.PositionM + Along * 3.0 + Away * .3;
	EyeM.Z = FloorZ + 1.6;
	const FVector TargetM = Site.PositionM + Away * .6;
	for (TActorIterator<ADataCenterDebugController> It(GetWorld()); It; ++It) It->SetViewpointM(EyeM, (TargetM - EyeM).Rotation());
	const FVector D = Site.SprayDirectionM;
	UE_LOG(LogTemp, Display, TEXT("[Water] 화면 확인: 지점 %d %s, 구멍 %.1f mm, 물줄기 방향 (%.2f, %.2f, %.2f), 시점 %s m. 수리 vrg.Leak.Repair %d"),
		Best, *Site.Label, HoleM * 1e3, D.X, D.Y, D.Z, *EyeM.ToCompactString(), Best);
}

void ADataCenterSimulationActor::SetAirView(EAirView View)
{
	const EAirView Old = AirView;
	AirView = View;
	// The average is gathered only while a flow mode is on (it costs a pass over the faces each step): start it fresh.
	if (Old == EAirView::Volume && View != EAirView::Volume) { FlowAverager.Reset(); bFlowGridValid = false; }
	LastFlowViewWallS = -1.0;
	if (AirField != nullptr)
	{
		const bool bVolume = View == EAirView::Volume || View == EAirView::StreamSpeedVolume;
		AirField->bDrawVolume = bVolume;
		AirField->bDrawFlow = bVolume;
	}
	if ((View == EAirView::Volume || View == EAirView::Arrows) && FlowDisplay != nullptr) FlowDisplay->Clear();
	if (View == EAirView::Volume) { Streamlines.Reset(); PlanStreamlines.Reset(); }
	UE_LOG(LogTemp, Display, TEXT("[VrgAir] 공기 표시: %s"), AirFlowView::Name(View));
}

EAirView ADataCenterSimulationActor::CycleAirView()
{
	SetAirView(static_cast<EAirView>((uint8(AirView) + 1) % uint8(EAirView::Count)));
	return AirView;
}

double ADataCenterSimulationActor::GetPlanDrawZM() const
{
	const FBox B = GetAirDomainBoundsM();
	return B.IsValid ? B.Max.Z + PlanDrawOffsetM : PlanHeightM;
}

FBox ADataCenterSimulationActor::GetAirDomainBoundsM() const
{
	return AirField != nullptr ? AirField->GetDomainBoundsM() : FBox(ForceInit);
}

int32 ADataCenterSimulationActor::GetAirFlowInstanceCount() const
{
	return FlowDisplay != nullptr ? FlowDisplay->GetNumShown() : 0;
}

void ADataCenterSimulationActor::GetAirFlowLegends(TArray<FAirFlowLegend>& Out, FString& OutNote) const
{
	Out.Reset();
	OutNote.Reset();
	if (AirView == EAirView::Volume || Mode != ESimMode::Simulation) return;
	const FAirFlowLegend Speed{ TEXT("공기 속도"), 0.0, AirFlowView::SpeedMaxMs, TEXT("m/s") };
	const FAirFlowLegend Temp{ TEXT("공기 온도"), AirFlowView::TempMinC, AirFlowView::TempMaxC, TEXT("°C") };
	switch (AirView)
	{
	case EAirView::StreamTemperature: Out.Add(Temp); break;
	case EAirView::Plan: Out.Add({ FString::Printf(TEXT("공기 온도 (높이 %.2f m 단면, 방 위에 띄워 표시)"), PlanHeightM), Temp.Min, Temp.Max, Temp.Unit }); break;
	default: Out.Add(Speed); break;
	}
	const FString Avg = bFlowGridValid ? FString::Printf(TEXT("시간 평균 %.0f s (목표 %.0f s)"), FlowAverager.GetCoveredS(), FlowAverager.WindowS)
		: FString(TEXT("순간값 (평균 모으는 중)"));
	OutNote = FString::Printf(TEXT("%s · %s%s"), AirFlowView::Name(AirView), *Avg,
		AirView == EAirView::Plan ? TEXT(" · 흰 선 = 단면 안 흐름") : AirView == EAirView::StreamSpeedVolume ? TEXT(" · 볼륨 = 온도") : TEXT(""));
}

void ADataCenterSimulationActor::AirFlowAfterStep(double DtS)
{
	if (AirView == EAirView::Volume || AirField == nullptr) return;
	FlowAverager.Add(AirField->GetMutableSolver().GetGrid(), DtS);
}

void ADataCenterSimulationActor::RebuildAirFlowView()
{
	if (AirField == nullptr || AirView == EAirView::Volume) return;
	const FUniformAirGrid& Live = AirField->GetMutableSolver().GetGrid();
	bFlowGridValid = FlowAverager.Build(Live, FlowGrid);
	const FUniformAirGrid& G = bFlowGridValid ? FlowGrid : Live;
	StreamlineOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
	Streamlines.Reset();
	PlanStreamlines.Reset();
	TArray<FPlantInstance> List;
	UStaticMesh* Tube = PlantInstances::Cylinder();
	UStaticMesh* Box = PlantInstances::Cube();
	auto TubeAt = [&](const FVector& AM, const FVector& BM, double DiameterM, const FLinearColor& Color)
	{
		FPlantInstance& I = List.AddDefaulted_GetRef();
		I.Mesh = Tube; I.Color = Color;
		I.Transform = PlantVisual::TubeTransformCm(SimUnits::MetersToUnreal(StreamlineOriginM + AM), SimUnits::MetersToUnreal(StreamlineOriginM + BM),
			SimUnits::MetersToUnreal(FVector(DiameterM, 0, 0)).X);
	};
	const bool bStream = AirView == EAirView::StreamSpeed || AirView == EAirView::StreamTemperature || AirView == EAirView::StreamSpeedVolume;
	if (bStream)
	{
		// Seeds: a 3 x 5 grid on each CRAH supply face, 10 cm in front of it, and three heights behind each rack (its
		// exhaust). The rack body is solid in the air grid (its fans are ducts), so a line from a cooler ends at a rack
		// front and the hot side needs its own seeds.
		TArray<FVector> Seeds;
		for (const FCrahPlacement& P : CrahPlacements)
		{
			const FVector Right(-P.ForwardM.Y, P.ForwardM.X, 0.0);
			for (int32 A = 0; A < 3; ++A)
				for (int32 B = 0; B < 5; ++B)
					Seeds.Add(P.BaseM + P.ForwardM * (.5 * P.SizeM.X + .1) + Right * ((A - 1) * .3 * P.SizeM.Y) + FVector(0, 0, (.15 + .7 * B / 4.0) * P.SizeM.Z) - StreamlineOriginM);
		}
		for (const ARackThermalActor* Rack : Racks)
		{
			if (Rack == nullptr) continue;
			const FVector Base = SimUnits::UnrealToMeters(Rack->GetActorLocation());
			const FVector Fwd = Rack->GetActorForwardVector().GetSafeNormal2D();
			for (const double Z : { .5, 1.1, 1.7 })
				Seeds.Add(Base - Fwd * (.5 * Rack->RackSizeM.X + .1) + FVector(0, 0, Z) - StreamlineOriginM);
		}
		Streamlines = AirStreamlines::TraceAll(G, Seeds, FAirStreamlineSettings());
		// Tubes 3 cm across, 10 cm segments (every second 5 cm step), colour banded for the instanced components.
		const bool bTemp = AirView == EAirView::StreamTemperature;
		for (const FAirStreamline& L : Streamlines)
			for (int32 I = 2; I < L.PointsM.Num(); I += 2)
			{
				const double V = bTemp ? (SimUnits::KelvinToCelsius(L.TemperatureK[I]) - AirFlowView::TempMinC) / (AirFlowView::TempMaxC - AirFlowView::TempMinC)
					: L.SpeedMs[I] / AirFlowView::SpeedMaxMs;
				TubeAt(L.PointsM[I - 2], L.PointsM[I], .03, AirFlowView::Banded(V));
			}
	}
	else if (AirView == EAirView::Plan)
	{
		// One cell layer at the plan height, coloured by temperature, and the in-plane streamlines over it (white).
		// Drawn above the room (PlanDrawOffsetM over the domain top) like a map: at its own height it is hidden under the
		// containment roof, rack tops and pipes when seen from above (found by the 2026-10-27 screen check). The data is
		// the plan height's layer; only the drawing is lifted.
		const double H = G.GetCellSizeM();
		const int32 K = FMath::Clamp(FMath::FloorToInt32(PlanHeightM / H), 0, G.GetNumCellsZ() - 1);
		const double Zc = (K + .5) * H;
		const FVector Lift(0, 0, G.GetNumCellsZ() * H + PlanDrawOffsetM - Zc);
		for (int32 Y = 0; Y < G.GetNumCellsY(); ++Y)
			for (int32 X = 0; X < G.GetNumCellsX(); ++X)
			{
				if (!G.IsFluidCell(X, Y, K)) continue;
				const double C = SimUnits::KelvinToCelsius(G.TemperatureK[G.CellIndex(X, Y, K)]);
				FPlantInstance& I = List.AddDefaulted_GetRef();
				I.Mesh = Box; I.Color = AirFlowView::Banded((C - AirFlowView::TempMinC) / (AirFlowView::TempMaxC - AirFlowView::TempMinC));
				I.Transform = PlantVisual::BoxTransformCm(SimUnits::MetersToUnreal(StreamlineOriginM + FVector((X + .5) * H, (Y + .5) * H, Zc) + Lift),
					SimUnits::MetersToUnreal(FVector(H, H, .01)));
			}
		TArray<FVector> Seeds;
		for (int32 Y = 1; Y < G.GetNumCellsY(); Y += 3)
			for (int32 X = 1; X < G.GetNumCellsX(); X += 3)
				if (G.IsFluidCell(X, Y, K)) Seeds.Add(FVector((X + .5) * H, (Y + .5) * H, Zc));
		FAirStreamlineSettings S;
		S.bPlanar = true; S.MaxLengthM = 6.0;
		PlanStreamlines = AirStreamlines::TraceAll(G, Seeds, S);
		for (const FAirStreamline& L : PlanStreamlines)
			for (int32 I = 2; I < L.PointsM.Num(); I += 2)
				TubeAt(L.PointsM[I - 2] + Lift + FVector(0, 0, .03), L.PointsM[I] + Lift + FVector(0, 0, .03), .03, FLinearColor(.95f, .95f, .95f));
	}
	if (List.Num() > 0 && FlowDisplay == nullptr && GetWorld() != nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FlowDisplay = GetWorld()->SpawnActor<AAirFlowDisplayActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (FlowDisplay != nullptr) FlowDisplay->Show(List);
}

void ADataCenterSimulationActor::UpdateAirFlowView()
{
	if (AirView == EAirView::Volume || AirField == nullptr) return;
	// Wall time paces the redraw only; it never changes the simulation (the average follows the steps).
	const double NowS = FPlatformTime::Seconds();
	if (LastFlowViewWallS < 0.0 || NowS - LastFlowViewWallS > 1.0)
	{
		LastFlowViewWallS = NowS;
		RebuildAirFlowView();
	}
	if (AirView != EAirView::Arrows || GetWorld() == nullptr) return;
	// Arrows: the averaged velocity at cell centres on three heights, every second cell, length up to 0.4 m, speed colours.
	const FUniformAirGrid& G = bFlowGridValid ? FlowGrid : AirField->GetMutableSolver().GetGrid();
	const double H = G.GetCellSizeM();
	for (const double Zm : { .375, 1.125, 1.875 })
	{
		const int32 K = FMath::Clamp(FMath::FloorToInt32(Zm / H), 0, G.GetNumCellsZ() - 1);
		for (int32 Y = 0; Y < G.GetNumCellsY(); Y += 2)
			for (int32 X = 0; X < G.GetNumCellsX(); X += 2)
			{
				if (!G.IsFluidCell(X, Y, K)) continue;
				const FVector C((X + .5) * H, (Y + .5) * H, (K + .5) * H);
				const FVector U = G.SampleVelocityMs(C);
				const double Speed = U.Size();
				if (Speed < .02) continue;
				const FVector A = SimUnits::MetersToUnreal(StreamlineOriginM + C);
				const FVector B = SimUnits::MetersToUnreal(StreamlineOriginM + C + U / Speed * (.4 * FMath::Min(Speed / AirFlowView::SpeedMaxMs, 1.0) + .05));
				DrawDebugDirectionalArrow(GetWorld(), A, B, 8.0f, AirFlowView::Colormap(Speed / AirFlowView::SpeedMaxMs).ToFColor(true), false, -1.0f, SDPG_World, 1.5f);
			}
	}
}

void ADataCenterSimulationActor::DrawSpray() const
{
	UWorld* World = GetWorld();
	if (World == nullptr || !bShowSpray || !Spray.HasWater() || Mode != ESimMode::Simulation) return;
	// Debug drawing of the computed water (W2 check view; the real water surface is W6). Each point is one parcel.
	const TArray<FVector>& P = Spray.GetParcelPositionsM();
	const TArray<uint8>& J = Spray.GetParcelIsJet();
	for (int32 I = 0; I < P.Num(); ++I)
		DrawDebugPoint(World, SimUnits::MetersToUnreal(P[I]), J[I] ? 6.0f : 4.0f, J[I] ? FColor(40, 90, 255) : FColor(120, 200, 255), false, -1.0f, SDPG_World);
	// The last step's paths: a fast jet lands within one step, so the stream is shown as its path segments.
	const TArray<FVector>& From = Spray.GetLastStepTrailFromM();
	const TArray<FVector>& To = Spray.GetLastStepTrailToM();
	const TArray<uint8>& TJ = Spray.GetLastStepTrailIsJet();
	for (int32 I = 0; I < From.Num(); ++I)
		DrawDebugLine(World, SimUnits::MetersToUnreal(From[I]), SimUnits::MetersToUnreal(To[I]), TJ[I] ? FColor(40, 90, 255) : FColor(120, 200, 255), false, -1.0f, SDPG_World, TJ[I] ? 1.5f : 1.0f);
	for (const FSprayFilm& F : Spray.GetFilms())
		if (F.VolumeM3 > 0.0)
			DrawDebugLine(World, SimUnits::MetersToUnreal(F.FootM), SimUnits::MetersToUnreal(F.FootM + FVector(0, 0, F.WetHeightM)), FColor(60, 140, 255), false, -1.0f, SDPG_World, 2.0f);
	// W3: the floor water, one flat square per wet cell (a line as wide as the cell), darker where deeper (0 to 5 mm).
	const FFloorWater& Fw = FloorWater;
	const FIntRect Box = Fw.GetWetBox();
	const double Cell = Fw.GetCellM();
	const double Z = Fw.GetFloorZM() + 2e-3;
	int32 Drawn = 0;
	for (int32 Jc = Box.Min.Y; Jc <= Box.Max.Y && Drawn < 30000; ++Jc)
		for (int32 Ic = Box.Min.X; Ic <= Box.Max.X && Drawn < 30000; ++Ic)
		{
			const double D = Fw.GetDepthM()[Fw.Index(Ic, Jc)];
			if (D < 1e-5) continue;
			++Drawn;
			const double A = FMath::Clamp(D / 5e-3, 0.0, 1.0);
			const FColor C(uint8(140 - 120 * A), uint8(210 - 130 * A), 255);
			const double Y = Fw.GetOriginM().Y + (Jc + .5) * Cell;
			DrawDebugLine(World, SimUnits::MetersToUnreal(FVector(Fw.GetOriginM().X + Ic * Cell, Y, Z)), SimUnits::MetersToUnreal(FVector(Fw.GetOriginM().X + (Ic + 1) * Cell, Y, Z)),
				C, false, -1.0f, SDPG_World, float(SimUnits::MetersToUnreal(FVector(Cell, 0, 0)).X));
		}
}

void ADataCenterSimulationActor::UpdatePlantDisplay(bool bForce)
{
	// Shown whenever a plant is built (layout and simulation mode, vrg.Plant.Enable 0 or 1); flows and status only
	// while it is live (coupled and stepping).
	const bool bWant = bShowPlant && bPlantBuilt && Plant.IsBuilt() && GetWorld() != nullptr;
	if (!bWant)
	{
		if (PlantDisplay != nullptr) PlantDisplay->Destroy();
		PlantDisplay = nullptr;
		return;
	}
	if (PlantDisplay == nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PlantDisplay = GetWorld()->SpawnActor<AFacilityPlantDisplayActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		bForce = true;
	}
	// Wall time throttles the display upload only; it never advances or changes the simulation clock.
	const double NowS = FPlatformTime::Seconds();
	if (!bForce && NowS - LastPlantDisplayWallS < 0.25) return;
	VRG_SCOPE_COST(STAT_VrgPlantDisplay, PlantDisplayMs);
	LastPlantDisplayWallS = NowS;
	if (PlantDisplay == nullptr) return;
	FPlantDisplayInput In = MakePlantDisplayInput();
	In.bLive = Mode == ESimMode::Simulation && bPlantActive && Plant.IsThermalActive();
	const FPlantDisplaySnapshot S = PlantDisplay::Build(Plant, In);
	PlantDisplay->ShowSnapshot(S);
	// Placed units draw themselves: give them their status (live) or their kind colour back.
	TMap<const APlantEquipmentActor*, FLinearColor> Status;
	TArray<APlantEquipmentActor*> Kinds[NumPlantUnitKinds];
	for (int32 K = 0; K < NumPlantUnitKinds; ++K) Kinds[K] = VrgPlantEquipment::Gather(GetWorld(), static_cast<EPlantUnitKind>(K));
	if (In.bLive)
		for (const FPlantDisplayEquipment& E : S.Equipment)
		{
			EPlantUnitKind Kind;
			if (!E.bPlaced || !VrgPlantEquipment::KindOf(E, Kind)) continue;
			const TArray<APlantEquipmentActor*>& Placed = Kinds[int32(Kind)];
			if (Placed.IsValidIndex(E.Index)) Status.Add(Placed[E.Index], PlantDisplay::EquipmentColor(E));
		}
	for (const TArray<APlantEquipmentActor*>& Units : Kinds)
		for (APlantEquipmentActor* U : Units)
		{
			const FLinearColor* C = Status.Find(U);
			U->SetStatusColor(C ? *C : FLinearColor::Transparent);
		}
	if (bForce && S.Pipes.Num() != LastPlantDisplayPipes)
	{
		LastPlantDisplayPipes = S.Pipes.Num();
		const TArray<FPlantDisplayClash> Clashes = PlantDisplay::FindClashes(S);
		UE_LOG(LogTemp, Display, TEXT("[Plant] 설비 마당 표시: 배관 %d, 헤더 %d, 장비 %d, 표시 겹침 %d쌍 (%s)"), S.Pipes.Num(), S.Headers.Num(), S.Equipment.Num(),
			Clashes.Num(), In.bLive ? TEXT("계산값") : TEXT("구조만: 설비가 꺼졌거나 배치 모드"));
		// Which drawn items touch (display check, not a model fault): loop / role / edge of a pipe, loop / node of a header.
		auto Item = [&S](bool bHeader, int32 I)
		{
			return bHeader ? FString::Printf(TEXT("헤더(루프 %d 노드 %d)"), int32(S.Headers[I].Loop), S.Headers[I].Node)
				: FString::Printf(TEXT("배관(루프 %d 역할 %d 간선 %d 분기 %d)"), int32(S.Pipes[I].Loop), int32(S.Pipes[I].Role), S.Pipes[I].Edge, S.Pipes[I].Branch);
		};
		for (const FPlantDisplayClash& C : Clashes)
			UE_LOG(LogTemp, Display, TEXT("[Plant]   겹침: %s / %s, 간격 %.3f m"), *Item(C.bAHeader, C.A), *Item(C.bBHeader, C.B), C.GapM);
	}
}

const FCouplingSnapshot& ADataCenterSimulationActor::GetCachedDisplaySnapshot() const
{
	if (!bDisplaySnapshotValid || Clock.GetSimTimeS() != DisplaySnapshotSimTimeS)
	{
		DisplaySnapshot = GetSnapshot();
		DisplaySnapshotSimTimeS = Clock.GetSimTimeS();
		bDisplaySnapshotValid = true;
	}
	return DisplaySnapshot;
}

FVrgDashboardSummary ADataCenterSimulationActor::BuildDashboardSummary() const
{
	FVrgDashboardSummary S;
	S.SimTimeS = Clock.GetSimTimeS();
	S.bSimulating = Mode == ESimMode::Simulation;
	S.bEvalReady = bEvalReady && Eval.GetNumInlets() > 0;
	if (S.bEvalReady)
	{
		S.WindowS = Eval.GetWindowCoveredS();
		S.ItWindowW = Eval.GetWindowPower().ItW;
		S.bPartialPueValid = Eval.GetPartialPue(S.PartialPueWindow);
		S.InletCount = Eval.GetNumInlets();
		Eval.GetInletWindowSummary(S.InletWindowMeanMaxK, S.InletWindowMeanAvgK);
		const FCouplingSnapshot& Snap = GetCachedDisplaySnapshot();
		S.ItNowW = Snap.TotalItHeatW;
		S.CoolantRemovedNowW = Snap.TotalRadiatorRejectedW;
		S.AirRemovedNowW = Snap.TotalAirCoolingRemovedW;
	}
	return S;
}

FVrgDashboardLayout ADataCenterSimulationActor::BuildDashboardLayout() const
{
	FVrgDashboardLayout L;
	L.SimTimeS = Clock.GetSimTimeS();
	L.RecommendedInletK = EvalSettings.RecommendedInletK;
	if (AirField != nullptr)
	{
		const FBox B = AirField->GetDomainBoundsM();
		L.bHasDomain = true;
		L.DomainMinXM = B.Min.X; L.DomainMinYM = B.Min.Y; L.DomainMaxXM = B.Max.X; L.DomainMaxYM = B.Max.Y; L.DomainMinZM = B.Min.Z; L.DomainMaxZM = B.Max.Z;
	}
	// Inlet i is Racks[i], then Spines[j] at Racks.Num() + j (the evaluation's order, see EvalAfterStep).
	const bool bInletsMatch = bEvalReady && Eval.GetNumInlets() == Racks.Num() + Spines.Num();
	if (bEvalReady)
	{
		for (int32 I = 0; I < Eval.GetNumInlets(); ++I) L.InletWindowMeanK.Add(Eval.GetInlet(I).WindowMeanK);
	}
	// A plain walk over every actor: L_Test has ~100 actors and it costs ~0.007 ms; the class-wise walk was not faster
	// (vrg.Web.ProfileLayout, 2026-10-13). The publish cost was the JSON, which now happens per request.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			FVector SizeM;
			if (!ADataCenterDebugController::PlaceableBoxM(*It, SizeM)) continue;
			// S8 F8: the plant yard joins the dashboard in its last web stage; until then the room layout stays as it was.
			if (It->IsA<APlantEquipmentActor>()) continue;
			FVrgDashboardEquipment& E = L.Equipment.AddDefaulted_GetRef();
			if (const ARackThermalActor* Rack = Cast<ARackThermalActor>(*It))
			{
				E.Kind = TEXT("rack");
				if (bInletsMatch) E.InletIndex = Racks.IndexOfByPredicate([Rack](const TObjectPtr<ARackThermalActor>& R) { return R.Get() == Rack; });
			}
			else if (const ASpineCabinetActor* Spine = Cast<ASpineCabinetActor>(*It))
			{
				E.Kind = TEXT("spine");
				const int32 J = Spines.IndexOfByPredicate([Spine](const TObjectPtr<ASpineCabinetActor>& S) { return S.Get() == Spine; });
				if (bInletsMatch && J != INDEX_NONE) E.InletIndex = Racks.Num() + J;
			}
			else if (It->IsA<ACoolingDistributionUnitActor>()) E.Kind = TEXT("cdu");
			else if (It->IsA<ACrahUnitActor>()) E.Kind = TEXT("crah");
			else
			{
				E.Kind = TEXT("barrier");
				SizeM *= It->GetActorScale3D().GetAbs();   // a barrier's size is its scale (unit box)
			}
			const FVector M = SimUnits::UnrealToMeters(It->GetActorLocation());
			E.Name = It->GetName();
			E.CenterXM = M.X; E.CenterYM = M.Y;
			E.YawDeg = It->GetActorRotation().Yaw;
			E.DepthM = SizeM.X; E.WidthM = SizeM.Y; E.BaseZM = M.Z; E.HeightM = SizeM.Z;
		}
	}
	if (Mode == ESimMode::Simulation)
	{
		for (int32 I = 0; I < CrahPlacements.Num(); ++I)
		{
			const FCrahPlacement& P = CrahPlacements[I];
			if (P.bPlaced) continue;   // placed actors are already listed above
			FVrgDashboardEquipment& E = L.Equipment.AddDefaulted_GetRef();
			E.Kind = TEXT("crah");
			E.Name = FString::Printf(TEXT("자동 CRAH %d"), I + 1);
			E.CenterXM = P.BaseM.X; E.CenterYM = P.BaseM.Y;
			E.YawDeg = FMath::RadiansToDegrees(FMath::Atan2(P.ForwardM.Y, P.ForwardM.X));
			E.DepthM = P.SizeM.X; E.WidthM = P.SizeM.Y; E.BaseZM = P.BaseM.Z; E.HeightM = P.SizeM.Z;
			E.bAutoPlaced = true;
		}
	}
	// 3D stage 2: the drawn coolant pipes with this step's solved flow; temperature only where the model has one
	// (rack branches: rack inlet / outlet), never interpolated for headers (same rule as vrg.Pipes.Temperature).
	if (Room != nullptr)
	{
		const FHydraulicNetwork& Net = Room->GetNetwork();
		L.bHydraulicsConverged = Room->GetLastSolveResult().bConverged;
		for (const FRoomPipeRoute& R : Room->GetPipeRoutesM())
		{
			FVrgDashboardPipe& P = L.Pipes.AddDefaulted_GetRef();
			P.Edge = R.Edge; P.bSupply = R.bSupply; P.bBranch = R.bBranch; P.PointsM = R.PointsM;
			P.DiameterM = R.bBranch ? Room->BranchDiameterM : Room->HeaderDiameterM;
			if (R.Edge >= 0 && R.Edge < Net.GetNumPipes())
			{
				P.FlowM3s = Net.GetPipeFlowM3s(R.Edge);
				P.bTempValid = Room->TryGetPipeTemperatureK(R.Edge, P.TempK);
			}
			else if (R.Edge < 0 && -1 - R.Edge < Net.GetNumPumps()) P.FlowM3s = Net.GetPumpFlowM3s(-1 - R.Edge);
		}
	}
	return L;
}

FVrgDashboardCooling ADataCenterSimulationActor::BuildDashboardCooling() const
{
	FVrgDashboardCooling C;
	C.SimTimeS = Clock.GetSimTimeS();
	C.bSimulating = Mode == ESimMode::Simulation;
	C.bCoupled = C.bSimulating && bEvalReady;
	if (!C.bCoupled) return C;   // the plant values belong to the coupled run; nothing in layout mode

	// Power of the last step (D8), the same reads the evaluation sample uses (EvalAfterStep).
	C.ItW = Bridge.GetLastItHeatW();
	C.ServerFanW = Bridge.GetLastServerFanPowerW();
	C.PsuLossW = Bridge.GetLastConversionLossW();
	C.CrahFanW = Bridge.GetLastCrahFanPowerW();
	C.PumpW = Room != nullptr ? Room->GetTotalPumpElectricalW() : 0.0;
	C.bPartialPueValid = C.ItW > 0.0;
	C.PartialPue = C.bPartialPueValid ? (C.ItW + C.CrahFanW + C.PumpW) / C.ItW : 0.0;

	// Air side: CRAH units (one duct each) or the lumped ceiling coil.
	C.AirRemovedW = Bridge.GetLastAirCoolingRemovedW();
	const TArray<FCrahCouplingUnit>& Units = Bridge.GetCrahUnits();
	C.CrahMode = Units.Num() > 0 ? TEXT("units") : TEXT("ceilingCoil");
	TArray<double> RemovedW, MixK, SupplyK;
	C.bCrahPerUnit = Bridge.GetLastCrahUnitReadings(RemovedW, MixK, SupplyK);
	for (int32 U = 0; U < Units.Num(); ++U)
	{
		FVrgDashboardCrah& R = C.Crahs.AddDefaulted_GetRef();
		const bool bLabel = CrahUnitLabels.IsValidIndex(U) && CrahUnitLabels.Num() == Units.Num();
		R.Kind = bLabel && CrahUnitLabels[U].bTopIntake ? TEXT("perimeter") : TEXT("inrow");
		R.bPlaced = bLabel && CrahUnitLabels[U].bPlaced;
		if (bLabel) { R.XM = CrahUnitLabels[U].XYM.X; R.YM = CrahUnitLabels[U].XYM.Y; }
		R.FlowM3s = Units[U].HasBothSides() ? Units[U].VolumeFlowM3s : 0.0;
		R.UAWK = Units[U].CoilUAWK;
		R.FanW = Units[U].HasBothSides() ? Units[U].FanPowerW : 0.0;
		R.WaterSupplyK = Units[U].WaterSupplyTempK;
		R.bHasAir = C.bCrahPerUnit;
		if (R.bHasAir) { R.RemovedW = RemovedW[U]; R.IntakeMixK = MixK[U]; R.SupplyK = SupplyK[U]; C.CrahCoilRemovedW += RemovedW[U]; }
	}

	// Coolant side: CDU groups (same order as CduSummaries, BuildCduGroups) and racks on their own radiators.
	C.CoolantRemovedW = Bridge.GetTotalRadiatorRejectedW();
	const TArray<FCduCouplingGroup>& Groups = Bridge.GetCduGroups();
	double CduDutySumW = 0.0;
	int32 CduRacks = 0;
	for (int32 G = 0; G < Groups.Num(); ++G)
	{
		FVrgDashboardCdu& R = C.Cdus.AddDefaulted_GetRef();
		if (CduSummaries.IsValidIndex(G)) { R.Name = CduSummaries[G].Name; R.DesignDutyW = CduSummaries[G].DesignDutyW; }
		R.NumRacks = Groups[G].RackIndices.Num();
		R.UAWK = Groups[G].Hx.UAWK;
		R.FacilityFlowKgs = Groups[G].Hx.FacilityMassFlowKgs;
		R.FacilityCpJKgK = Groups[G].Hx.FacilityCpJKgK;
		R.FacilitySupplyK = Groups[G].Hx.FacilitySupplyTempK;
		auto At = [G](const TArray<double>& A) { return A.IsValidIndex(G) ? A[G] : 0.0; };
		R.DutyW = At(Bridge.GetLastCduDutyW()); R.ReturnK = At(Bridge.GetLastCduReturnK()); R.SupplyK = At(Bridge.GetLastCduSupplyK());
		R.FacilityReturnK = At(Bridge.GetLastCduFacilityReturnK());
		R.HotWK = At(Bridge.GetLastCduHotWK()); R.CoolantKgs = At(Bridge.GetLastCduCoolantKgs());
		CduDutySumW += R.DutyW;
		CduRacks += R.NumRacks;
	}
	C.RadiatorRemovedW = C.CoolantRemovedW - CduDutySumW;
	C.RadiatorRacks = FMath::Max(Racks.Num() - CduRacks, 0);   // spines carry no coolant

	// Hydraulics: one pump per row (InRowLoopBuilder), branch flows of that row's racks.
	if (Room != nullptr)
	{
		const FHydraulicNetwork& Net = Room->GetNetwork();
		const FHydraulicSolveResult& Res = Room->GetLastSolveResult();
		C.bHydraulics = Room->GetNumRows() > 0;
		C.bHydConverged = Res.bConverged; C.bHydSparse = Res.bUsedSparse;
		C.HydIterations = Res.IterationsUsed; C.HydResidual = Res.NormalizedResidual;
		const TArray<int32> RackRows = Room->GetRoutedRackRows();
		const TArray<int32>& Branches = Room->GetRackBranchPipeIndices();
		for (int32 Row = 0; Row < Room->GetNumRows(); ++Row)
		{
			const int32 Pump = Room->GetRowPumpIndex(Row);
			if (Pump == INDEX_NONE) continue;
			FVrgDashboardPump& P = C.Pumps.AddDefaulted_GetRef();
			P.Row = Row;
			if (const ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(Row)) P.Cdu = Cdu->GetName();
			P.FlowM3s = Net.GetPumpFlowM3s(Pump);
			P.HeadPa = Net.GetPumpDeltaPPa(Pump);
			P.ElectricW = Net.GetPumpElectricalInputPowerW(Pump);
			P.HydraulicW = Net.GetPumpHydraulicPowerW(Pump);
			double Sum = 0.0;
			for (int32 R = 0; R < RackRows.Num() && R < Branches.Num(); ++R)
			{
				if (RackRows[R] != Row || Branches[R] == INDEX_NONE) continue;
				const double F = Net.GetPipeFlowM3s(Branches[R]);
				P.BranchMinM3s = P.Branches ? FMath::Min(P.BranchMinM3s, F) : F;
				P.BranchMaxM3s = P.Branches ? FMath::Max(P.BranchMaxM3s, F) : F;
				Sum += F; ++P.Branches;
			}
			P.BranchMeanM3s = P.Branches ? Sum / P.Branches : 0.0;
		}
	}
	return C;
}

FVrgDashboardLeaks ADataCenterSimulationActor::BuildDashboardLeaks() const
{
	FVrgDashboardLeaks D;
	D.SimTimeS = Clock.GetSimTimeS();
	D.bSimulating = Mode == ESimMode::Simulation;
	D.FaultSettings = FaultMonitor.GetSettings();
	D.HistoryLengthS = EvalSettings.SeriesLengthS;
	if (Room == nullptr) return D;
	D.bHasRoom = true;
	const FHydraulicNetwork& Net = Room->GetNetwork();
	const FHydraulicSolveResult& Res = Room->GetLastSolveResult();
	D.bHydConverged = Res.bConverged; D.HydResidual = Res.NormalizedResidual; D.HydIterations = Res.IterationsUsed;
	D.ImpactThreshold = Room->ValveImpactThreshold;
	const bool bCoupled = D.bSimulating && bEvalReady;
	D.LeakHeatW = bCoupled ? Bridge.GetLastLeakHeatW() : 0.0;

	const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
	const TArray<int32> RackRows = Room->GetRoutedRackRows();
	const TArray<int32>& Branches = Room->GetRackBranchPipeIndices();
	const TArray<double>& Design = Room->GetRackDesignFlowM3s();
	const FCoolantInventory& Inv = Room->GetCoolantInventory();

	// Tanks: one per CDU loop; the row is the one whose pump the tank's loop holds.
	TSet<int32> ActiveTanks;   // tanks with a leak or a depletion (their history goes to the page)
	for (int32 T = 0; T < Inv.GetTanks().Num(); ++T)
	{
		const FCoolantInventory::FTank& Tank = Inv.GetTanks()[T];
		FVrgDashboardTank& R = D.Tanks.AddDefaulted_GetRef();
		R.Tank = T;
		for (int32 Row = 0; Row < Room->GetNumRows(); ++Row) if (Room->GetRowPumpIndex(Row) == Tank.PumpIndex) R.Row = Row;
		if (const ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(R.Row)) R.Cdu = Cdu->GetName();
		R.PositionM = Room->GetNodePositionM(Tank.CduNodeIndex);
		R.Leaks = Room->CountShownTankLeaks(Tank.LeakIndices);   // W1: closed live sites are not leaks
		R.bMakeup = Tank.Params.bMakeupWater;
		R.bDepleted = Tank.State.bDepleted;
		R.DepletedAtS = Tank.State.bDepleted ? Tank.State.DepletedAtS : -1.0;
		R.PressurePa = Inv.GetTankGaugePressurePa(T);
		R.FillPressurePa = Tank.FillGaugePa;
		R.TankVolumeM3 = Tank.Params.TankVolumeM3;
		R.WaterM3 = Tank.State.WaterVolumeM3; R.InitialWaterM3 = Tank.State.InitialWaterM3;
		R.LeakedM3 = Tank.State.LeakedM3; R.MakeupM3 = Tank.State.MakeupM3;
		R.LeakFlowM3s = Tank.LastLeakM3s; R.MakeupFlowM3s = Tank.LastMakeupM3s;
		R.PumpFlowM3s = Net.GetPumpFlowM3s(Tank.PumpIndex);
		if (R.Leaks > 0 || R.bDepleted) ActiveTanks.Add(T);
	}

	// Leaks: the orifices built into the network (a spec added after the last rebuild has no orifice yet).
	for (int32 S = 0; S < Room->GetNumLeakSpecs(); ++S)
	{
		const int32 NL = Room->GetLeakNetworkIndex(S);
		if (NL == INDEX_NONE || NL >= Net.GetNumLeaks() || !Room->IsLeakSpecShown(S)) continue;   // W1: unopened live sites stay off the page
		const FHydraulicLeak& Orifice = Net.GetLeak(NL);
		FVrgDashboardLeak& L = D.Leaks.AddDefaulted_GetRef();
		L.Spec = S; L.NetworkLeak = NL;
		const ARackThermalActor* Rack = Room->GetLeakSpecRack(S);
		L.Rack = Routed.IndexOfByPredicate([Rack](const ARackThermalActor* R) { return R == Rack; });
		L.Inlet = Racks.IndexOfByPredicate([Rack](const TObjectPtr<ARackThermalActor>& R) { return R.Get() == Rack; });
		L.bReturnBranch = Room->IsLeakSpecReturnBranch(S);
		for (int32 T = 0; T < Inv.GetTanks().Num(); ++T) if (Inv.GetTanks()[T].LeakIndices.Contains(NL)) L.Tank = T;
		L.HoleDiameterM = Orifice.HoleDiameterM; L.DischargeCoefficient = Orifice.DischargeCoefficient;
		L.PositionM = Room->GetNodePositionM(Orifice.NodeIndex);
		L.ElevationM = Net.GetNodeElevationM(Orifice.NodeIndex);
		L.NodePressurePa = Net.GetNodePressurePa(Orifice.NodeIndex);
		L.FlowM3s = Net.GetLeakFlowM3s(NL);
		L.bNodeIsolated = Net.IsNodeIsolated(Orifice.NodeIndex);
	}

	// Racks: the judgement's own input next to the hydraulic solution, and the two scopes kept apart (D4).
	const FVrgFaultInput In = MakeFaultInput();
	const TArray<double>& SupplyLeak = Bridge.GetLastSupplyLeakKgs();
	const TArray<double>& ReturnLeak = Bridge.GetLastReturnLeakKgs();
	for (int32 I = 0; I < Routed.Num(); ++I)
	{
		FVrgDashboardLeakRack& R = D.Racks.AddDefaulted_GetRef();
		R.Rack = I;
		const int32 J = Racks.IndexOfByKey(Routed[I]);
		R.Inlet = J;
		R.Row = RackRows.IsValidIndex(I) ? RackRows[I] : INDEX_NONE;
		R.Name = Routed[I] != nullptr ? Routed[I]->GetName() : FString();
		const int32 Pipe = Branches.IsValidIndex(I) ? Branches[I] : INDEX_NONE;
		R.DesignFlowM3s = Design.IsValidIndex(I) ? Design[I] : 0.0;
		R.FlowM3s = Pipe != INDEX_NONE ? Net.GetPipeFlowM3s(Pipe) : 0.0;
		if (bCoupled)
		{
			R.CoolantKgs = In.CoolantMassFlowKgs[I];
			R.SupplyLeakKgs = SupplyLeak.IsValidIndex(J) ? SupplyLeak[J] : 0.0;
			R.ReturnLeakKgs = ReturnLeak.IsValidIndex(J) ? ReturnLeak[J] : 0.0;
		}
		R.DesignKgs = In.DesignMassFlowKgs[I];
		R.MaxComponentK = In.MaxComponentTempK[I];
		R.bLoopDepleted = In.LoopDepleted[I];
		R.bValveClosed = Room->IsRackValveClosed(Routed[I]);
		const int32 RackNode = Pipe != INDEX_NONE ? Net.GetPipeToNodeIndex(Pipe) : INDEX_NONE;
		R.bIsolated = RackNode != INDEX_NONE && Net.IsNodeIsolated(RackNode);
		const double Change = FMath::Abs(FMath::Abs(R.FlowM3s) - R.DesignFlowM3s);
		R.bImpacted = !R.bIsolated && (R.DesignFlowM3s > 0.0 ? Change / R.DesignFlowM3s > D.ImpactThreshold : Change > 0.0);
	}

	const ADataCenterRoomActor::FValveReport& V = Room->GetLastValveReport();
	D.bHasValveReport = V.RackIndex != INDEX_NONE;
	if (D.bHasValveReport)
	{
		D.ValveReport.TimeS = -1.0; D.ValveReport.Rack = V.RackIndex; D.ValveReport.bClosed = V.bClosed;
		D.ValveReport.IsolatedRacks = V.IsolatedRacks; D.ValveReport.ImpactedRacks = V.ImpactedRacks;
		D.bValveConverged = V.bConverged;
		D.ValveFlowBeforeM3s = V.FlowBeforeM3s; D.ValveFlowAfterM3s = V.FlowAfterM3s;
	}

	D.Faults = FaultMonitor.GetFaults();
	D.LeakEvents = FaultMonitor.GetLeaks();
	D.ValveEvents = FaultMonitor.GetValves();
	for (const FVrgTankSample& S : FaultMonitor.GetTankSamples())
		if (ActiveTanks.Contains(S.Row) && S.TimeS >= D.SimTimeS - D.HistoryLengthS) D.TankHistory.Add(S);
	return D;
}

FVrgDashboardRacks ADataCenterSimulationActor::BuildDashboardRacks(int32 DetailInlet) const
{
	FVrgDashboardRacks Out;
	Out.SimTimeS = Clock.GetSimTimeS();
	Out.bSimulating = Mode == ESimMode::Simulation;
	if (!Out.bSimulating || !bEvalReady) return Out;   // the cabinets' state is the coupled run's; nothing to show in layout
	const TArray<double>& InletK = Bridge.GetLastAirInletTempK();
	const FCouplingSnapshot& Snap = GetCachedDisplaySnapshot();
	const int32 NumCabinets = Racks.Num() + Spines.Num();
	for (int32 I = 0; I < NumCabinets; ++I)
	{
		const bool bRack = I < Racks.Num();
		AActor* Cabinet = bRack ? static_cast<AActor*>(Racks[I].Get()) : static_cast<AActor*>(Spines[I - Racks.Num()].Get());
		if (Cabinet == nullptr) continue;
		const FRackThermalModel& Model = bRack ? Racks[I]->GetMutableModel() : Spines[I - Racks.Num()]->GetMutableModel();
		const FRackThermalInput& In = bRack ? Racks[I]->Input : Spines[I - Racks.Num()]->GetMutableInput();
		const FRackThermalParams& P = Model.GetParams();
		const FRackThermalState& St = Model.GetState();
		FVrgDashboardRackRow& R = Out.Rows.AddDefaulted_GetRef();
		R.Inlet = I;
		R.Kind = bRack ? TEXT("rack") : TEXT("spine");
		R.Name = Cabinet->GetName();
		const FVector M = SimUnits::UnrealToMeters(Cabinet->GetActorLocation());
		R.XM = M.X; R.YM = M.Y;
		R.Utilization = In.Utilization;
		R.HeatW = Model.GetLastTotalHeatW();
		R.ToCoolantW = Model.GetLastHeatRemovedByCoolantW();
		R.ToRoomAirW = Model.GetLastTotalHeatToRoomAirW();
		R.InletNowK = InletK.IsValidIndex(I) ? InletK[I] : In.AirInletTempK;
		R.CoolantInK = In.CoolantInletTempK;
		R.CoolantOutK = St.CoolantTempK;
		R.CoolantFlowM3s = Snap.RackCoolantMassFlowKgs.IsValidIndex(I) && CoolantDensityKgM3 > 0.0 ? Snap.RackCoolantMassFlowKgs[I] / CoolantDensityKgM3 : 0.0;
		R.FanPowerW = Model.GetLastTotalFanPowerW();
		R.NumSlots = Model.GetNumSleds();
		int32 FanSlots = 0;
		for (int32 S = 0; S < R.NumSlots; ++S)
		{
			const bool bDocked = !In.SledDocked.IsValidIndex(S) || In.SledDocked[S];
			R.DockedSlots += bDocked ? 1 : 0;
			if (bDocked && FRackThermalModel::FansOf(P, S) != nullptr && St.SlotFanSpeedRatio.IsValidIndex(S))
			{
				R.FanSpeedMean += St.SlotFanSpeedRatio[S];
				++FanSlots;
			}
			for (int32 N = Model.GetSlotNodeStart(S), End = N + Model.GetSlotNodeCount(S); N >= 0 && N < End; ++N)
			{
				if (bDocked && St.ComponentTempK.IsValidIndex(N) && St.ComponentTempK[N] > R.HottestPartK)
				{
					R.HottestPartK = St.ComponentTempK[N];
					R.HottestPart = FString::Printf(TEXT("슬롯 %d %s"), S + 1, *Model.GetNodeParams(N).Name.ToString());
				}
			}
		}
		if (FanSlots > 0) R.FanSpeedMean /= FanSlots;

		if (I != DetailInlet) continue;
		// The selected cabinet only: one snapshot gives every part's heat flows.
		const FRackThermalSnapshot RS = Model.MakeSnapshot(Out.SimTimeS);
		FVrgDashboardRackDetail& D = Out.Detail;
		Out.bHasDetail = true;
		D.Inlet = I;
		D.TotalHeatW = RS.TotalHeatW; D.ToCoolantW = RS.TotalHeatToCoolantW; D.ToAirW = RS.TotalHeatToAirW; D.ToAmbientW = RS.TotalHeatToAmbientW;
		D.MixedAirOutletK = RS.MixedAirOutletTempK; D.CoolantOutletK = RS.CoolantOutletTempK;
		const UEnum* KindEnum = StaticEnum<ERackPartKind>();
		for (int32 S = 0; S < R.NumSlots; ++S)
		{
			FVrgDashboardRackSlot& Slot = D.Slots.AddDefaulted_GetRef();
			Slot.Index = S;
			if (P.Slots.IsValidIndex(S))
			{
				const FRackSlot& Def = P.Slots[S];
				Slot.BottomZM = Def.BottomZM; Slot.HeightM = Def.HeightM;
				if (P.DeviceTypes.IsValidIndex(Def.DeviceType)) Slot.Device = P.DeviceTypes[Def.DeviceType].Name.ToString();
			}
			Slot.bDocked = !In.SledDocked.IsValidIndex(S) || In.SledDocked[S];
			Slot.InletK = In.SlotAirInletTempK.IsValidIndex(S) ? In.SlotAirInletTempK[S] : In.AirInletTempK;
			Slot.HeatToAirW = RS.SlotHeatToAirW.IsValidIndex(S) ? RS.SlotHeatToAirW[S] : 0.0;
			Slot.AirFlowKgs = RS.SlotAirMassFlowKgs.IsValidIndex(S) ? RS.SlotAirMassFlowKgs[S] : 0.0;
			Slot.OutletAirK = RS.SledAirOutletTempK.IsValidIndex(S) ? RS.SledAirOutletTempK[S] : 0.0;
			Slot.FanSpeed = RS.SlotFanSpeedRatio.IsValidIndex(S) ? RS.SlotFanSpeedRatio[S] : 0.0;
			for (int32 N = Model.GetSlotNodeStart(S), End = N + Model.GetSlotNodeCount(S); N >= 0 && N < End; ++N)
			{
				const FRackComponentParams& Part = Model.GetNodeParams(N);
				FVrgDashboardRackPart& Q = Slot.Parts.AddDefaulted_GetRef();
				Q.Name = Part.Name.ToString();
				Q.Kind = KindEnum != nullptr ? KindEnum->GetNameStringByValue(int64(Part.Kind)) : FString();
				Q.TempK = RS.ComponentTempK.IsValidIndex(N) ? RS.ComponentTempK[N] : 0.0;
				Q.HeatW = RS.ComponentHeatW.IsValidIndex(N) ? RS.ComponentHeatW[N] : 0.0;
				Q.ToCoolantW = RS.ComponentHeatToCoolantW.IsValidIndex(N) ? RS.ComponentHeatToCoolantW[N] : 0.0;
				Q.ToAirW = RS.ComponentHeatToAirW.IsValidIndex(N) ? RS.ComponentHeatToAirW[N] : 0.0;
				Slot.HeatW += Q.HeatW;
			}
		}
	}
	return Out;
}

FString ADataCenterSimulationActor::ProfileDashboardLayout(int32 Repeats) const
{
	UWorld* World = GetWorld();
	if (World == nullptr) return TEXT("월드 없음");
	Repeats = FMath::Clamp(Repeats, 1, 1000);
	auto TimeMs = [Repeats](auto&& Body)
	{
		const uint64 Start = FPlatformTime::Cycles64();
		for (int32 R = 0; R < Repeats; ++R) Body();
		return FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Start) / Repeats;
	};
	int32 AllActors = 0, FullHits = 0, ClassHits = 0;
	// (a) the stage-3 walk: every actor in the world through the PlaceableBoxM filter.
	const double FullMs = TimeMs([&]
	{
		AllActors = FullHits = 0;
		FVector SizeM;
		for (TActorIterator<AActor> It(World); It; ++It) { ++AllActors; FullHits += ADataCenterDebugController::PlaceableBoxM(*It, SizeM) ? 1 : 0; }
	});
	// (b) a class-wise walk over the five equipment classes, for comparison only.
	UClass* const EquipmentClasses[] = { ARackThermalActor::StaticClass(), ASpineCabinetActor::StaticClass(),
		ACoolingDistributionUnitActor::StaticClass(), ACrahUnitActor::StaticClass(), ALayoutBarrierActor::StaticClass() };
	const double ClassMs = TimeMs([&]
	{
		ClassHits = 0;
		FVector SizeM;
		for (UClass* C : EquipmentClasses) for (TActorIterator<AActor> It(World, C); It; ++It) ClassHits += ADataCenterDebugController::PlaceableBoxM(*It, SizeM) ? 1 : 0;
	});
	FVrgDashboardLayout L;
	const double BuildMs = TimeMs([&] { L = BuildDashboardLayout(); });
	FString Json;
	const double JsonMs = TimeMs([&] { Json = L.ToJson(); });
	const double SummaryMs = TimeMs([&] { Json = BuildDashboardSummary().ToJson(); });
	FVrgDashboardRacks Racks4;
	const double RackRowsMs = TimeMs([&] { Racks4 = BuildDashboardRacks(INDEX_NONE); });
	const double RackDetailMs = TimeMs([&] { Racks4 = BuildDashboardRacks(0); });
	// Rack tab JSON per request, split rows / detail, FJsonObject tree (before 2026-10-13) vs direct writer (now).
	const double RowsDomMs = TimeMs([&] { Json = Racks4.RowsToJsonDom(); });
	const double DetailDomMs = TimeMs([&] { Json = Racks4.DetailToJsonDom(); });
	const double RowsFastMs = TimeMs([&] { Json = Racks4.RowsToJson(); });
	const double DetailFastMs = TimeMs([&] { Json = Racks4.DetailToJson(); });
	return FString::Printf(TEXT("[VrgWeb] 평면도 비용 (%d회 평균, 게임 스레드): 전체 액터 순회 %.4f ms (액터 %d, 장비 %d) | 클래스별 순회 %.4f ms (장비 %d) | 평면도 생성 %.4f ms + JSON %.4f ms (항목 %d) | 요약 %.4f ms | 랙 탭 행 %.4f ms, 행+상세 1대 %.4f ms (행 %d) | 랙 JSON 이전(객체 트리) 행 %.4f + 상세 %.4f ms -> 지금(직접 쓰기) 행 %.4f + 상세 %.4f ms (%d자)"),
		Repeats, FullMs, AllActors, FullHits, ClassMs, ClassHits, BuildMs, JsonMs, L.Equipment.Num(), SummaryMs, RackRowsMs, RackDetailMs, Racks4.Rows.Num(),
		RowsDomMs, DetailDomMs, RowsFastMs, DetailFastMs, Json.Len());
}

bool ADataCenterSimulationActor::ProcessDashboardRequests()
{
	FString Name;
	if (!FVrgWebDashboard::Get().TakePendingReport(Name)) return false;
	FString Summary;
	const bool bOk = WriteEvaluationReport(Name, Summary);
	UE_LOG(LogTemp, Display, TEXT("[VrgWeb] 페이지 요청 리포트 %s: %s"), bOk ? TEXT("성공") : TEXT("실패"), *Summary);
	FVrgWebDashboard::Get().SetLastSave(Name, bOk, Summary);
	return true;
}

void ADataCenterSimulationActor::PublishDashboard()
{
	FVrgWebDashboard& Web = FVrgWebDashboard::Get();
	const double NowS = FPlatformTime::Seconds();
	if (!Web.IsRunning() || NowS - LastDashboardPublishS < DashboardPublishPeriodS) return;
	LastDashboardPublishS = NowS;
	ProcessDashboardRequests();   // a page-requested report write is a file write, kept out of the publish cost below
	// Summary JSON, series copy (<= SeriesLengthS points) and event markers all count as the dashboard's cost (D10).
	VRG_SCOPE_COST(STAT_VrgDashboardPublish, DashboardMs);
	Web.Publish(BuildDashboardSummary().ToJson());

	FVrgDashboardSeries Series;
	Series.SimTimeS = Clock.GetSimTimeS();
	if (bEvalReady)
	{
		Series.Generation = Eval.GetGeneration();
		Series.LengthS = Eval.GetSettings().SeriesLengthS;
		Eval.GetSeries(-1.0, Series.Points);
	}
	for (const FEvalEvent& E : EvalEvents)
	{
		FVrgDashboardEvent& D = Series.Events.AddDefaulted_GetRef();
		D.TimeS = E.TimeS; D.Kind = E.Kind; D.Index = E.Index; D.Value = E.Value; D.bApplied = E.bApplied;
	}
	Web.PublishSeries(MoveTemp(Series));
	Web.PublishLayout(BuildDashboardLayout());   // struct copy; JSON is built per request (it was most of the publish cost)
	Web.PublishRacks(BuildDashboardRacks(Web.GetRequestedRack()));
	Web.PublishCooling(BuildDashboardCooling());
	Web.PublishLeaks(BuildDashboardLeaks());   // S2b step 6: leak layer (struct copy, JSON per request)
	{
		// 3D sensor layer: the same evaluation sensors the report uses (D9 simulated measurements).
		FVrgDashboardSensors Sensors;
		if (bEvalReady) Sensors = FVrgDashboardSensors::FromEvaluation(Eval);
		Sensors.SimTimeS = Clock.GetSimTimeS();
		Sensors.bSimulating = Mode == ESimMode::Simulation;
		Sensors.bEvalReady = bEvalReady;
		Web.PublishSensors(MoveTemp(Sensors));
	}
	if (AirField != nullptr)
	{
		// 3D airflow layer sampling (display choice, not a physics setting): every 2nd cell in X and Y (0.5 m at the
		// default h 0.25 m) on the layers nearest 0.4 / 1.1 / 1.9 / 2.75 m above the floor (low, rack middle, rack top,
		// above the racks). The actual heights and spacing go into the JSON.
		static const TArray<double> HeightsM = { 0.4, 1.1, 1.9, 2.75 };
		FVrgDashboardAirflow Air = FVrgDashboardAirflow::Sample(AirField->GetSolver(), AirField->GetDomainBoundsM().Min, 2, HeightsM);
		Air.SimTimeS = Clock.GetSimTimeS();
		Air.bSimulating = Mode == ESimMode::Simulation;
		Web.PublishAirflow(MoveTemp(Air));
	}
}

void ADataCenterSimulationActor::DrawSummary() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());
	const FCouplingSnapshot& Snapshot = GetCachedDisplaySnapshot();

	if (!LastSetupMessage.IsEmpty())
	{
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 4, 0.0f, FColor::Orange,
			FString::Printf(TEXT("[%s] %s"), *GetName(), *LastSetupMessage));
	}

	const bool bAllConverged = Snapshot.bHydraulicsConverged && Snapshot.bAirPressureConverged;

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		bAllConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[통합] t=%.1f s | 랙/스파인 %d대 | 수력 %s | 공기 압력 %s"),
			Snapshot.SimTimeS, Snapshot.RackHeatToAirW.Num(),
			Snapshot.bHydraulicsConverged ? TEXT("수렴") : TEXT("수렴 실패"),
			Snapshot.bAirPressureConverged ? TEXT("수렴") : TEXT("수렴 실패 - 속도장 믿지 말 것")));

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("IT 발열 %.1f kW + CRAH 팬 %.1f kW = 냉각수 방출(%s) %.1f kW + 공기 코일 %.2f kW + 축적"),
			Snapshot.TotalItHeatW / 1000.0,
			Snapshot.CrahFanPowerW / 1000.0,
			CduSummaries.Num() > 0 ? TEXT("CDU") : TEXT("랙 라디에이터"),
			Snapshot.TotalRadiatorRejectedW / 1000.0,
			Snapshot.TotalAirCoolingRemovedW / 1000.0));

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f,
		FMath::Abs(Snapshot.NormalizedEnergyResidual) < 1.0e-9 ? FColor::Yellow : FColor::Red,
		FString::Printf(TEXT("전역 열수지: 유입 %.3e J - 유출 %.3e J - 축적 %.3e J | 정규화 잔차 %.2e"),
			Snapshot.EnergyInJ, Snapshot.EnergyOutJ, Snapshot.EnergyStoredJ,
			Snapshot.NormalizedEnergyResidual));

	if (Snapshot.RackAirMassFlowKgs.Num() > 0)
	{
		double AirKgs = 0.0, DuctKgs = 0.0;
		for (const double M : Snapshot.RackAirMassFlowKgs) AirKgs += M;
		for (const double M : Snapshot.RackDuctMassFlowKgs) DuctKgs += M;
		const bool bDuctOk = !Snapshot.bAirDuctsRejected && (AirKgs <= 0.0 || DuctKgs >= .999 * AirKgs);
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 5, 0.0f, bDuctOk ? FColor(180, 255, 200) : FColor::Orange,
			FString::Printf(TEXT("팬: 랙 풍량 %.2f kg/s 중 공기 격자 관통 %.2f kg/s | 팬 전력 %.2f kW%s"),
				AirKgs, DuctKgs, Snapshot.TotalFanPowerW / 1000.0,
				Snapshot.bAirDuctsRejected ? TEXT(" | 덕트 거부됨 - 팬 기류 없음 (옥트리 경로 또는 잘못된 면)") : TEXT("")));
	}

	// Coolant heat rejection: per CDU, duty and temperatures (the facility side is what leaves the building).
	if (CduSummaries.Num() > 0)
	{
		FString Line = TEXT("CDU 열교환기:");
		for (int32 G = 0; G < CduSummaries.Num(); ++G)
		{
			const FCduSummary& S = CduSummaries[G];
			const double Duty = Snapshot.CduDutyW.IsValidIndex(G) ? Snapshot.CduDutyW[G] : 0.0;
			const double Ret = Snapshot.CduReturnTempK.IsValidIndex(G) ? Snapshot.CduReturnTempK[G] : 0.0;
			const double Sup = Snapshot.CduSupplyTempK.IsValidIndex(G) ? Snapshot.CduSupplyTempK[G] : 0.0;
			const double Fac = Snapshot.CduFacilityReturnTempK.IsValidIndex(G) ? Snapshot.CduFacilityReturnTempK[G] : 0.0;
			Line += FString::Printf(TEXT(" [%d] 랙 %d대 %.0f kW (설계 %.0f) 회수 %.1f -> 공급 %.1f C, 설비수 %.1f -> %.1f C |"),
				G, S.NumRacks, Duty / 1000.0, S.DesignDutyW / 1000.0, Ret - 273.15, Sup - 273.15, S.FacilitySupplyTempK - 273.15, Fac - 273.15);
		}
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 7, 0.0f, FColor(160, 210, 255), Line);
	}
	else
	{
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 7, 0.0f, FColor(160, 210, 255),
			TEXT("냉각수 열 방출: 랙별 라디에이터 (CDU 용량 제한 없음)"));
	}

	// Air cooling: which device carries it. The CRAH figures are design sizing (assumptions), not a product.
	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 6, 0.0f, bCrahFellBackToCoil ? FColor::Orange : FColor(180, 220, 255),
		CrahUnits.Num() > 0
			? FString::Printf(TEXT("CRAH %s %d대 | 대당 평균 풍량 %.2f m3/s, 코일 UA %.0f W/K (설계 dT %.0f K, 접근 %.0f K 가정) | 코일 제거 %.1f kW | 팬 %.1f kW (추정 %.0f Pa, 효율 %.2f)"),
				bUsingPlacedCrahs ? TEXT("(레벨에 놓은 액터)") : CrahLayout == ECrahLayout::InRow ? TEXT("인로우") : TEXT("벽면"), CrahUnits.Num(), CrahUnitFlowM3s, CrahUnitUAWK,
				CrahDesignAirDeltaTK, CrahDesignApproachK, Snapshot.CrahCoilRemovedW / 1000.0, Snapshot.CrahFanPowerW / 1000.0,
				CrahFanPressurePa, CrahFanEfficiency)
			: FString::Printf(TEXT("공기 냉각: 천장 뭉친 코일 UA %.0f W/K (풍량 없음, 포화 없음)%s"), EffectiveCoilUAWK,
				bCrahFellBackToCoil ? TEXT(" - CRAH 유닛을 못 놓아 대신 씀") : TEXT("")));

	if (Snapshot.RackCoolantMassFlowKgs.Num() > 0)
	{
		double MinInletK = TNumericLimits<double>::Max();
		double MaxInletK = 0.0;
		for (const double InletK : Snapshot.RackAirInletTempK)
		{
			MinInletK = FMath::Min(MinInletK, InletK);
			MaxInletK = FMath::Max(MaxInletK, InletK);
		}

		// 온도 표시는 여기서만 °C로 바꾼다 (규약 2).
		if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f, FColor(200, 220, 255),
			FString::Printf(TEXT("랙 0 냉각수 %.3f kg/s | 랙 흡기 %.2f ~ %.2f C (공기 격자에서 옴)"),
				Snapshot.RackCoolantMassFlowKgs[0],
				MinInletK - 273.15, MaxInletK - 273.15));
	}
}

void ADataCenterSimulationActor::QueueWarmupSteps(int32 NumSteps)
{
	if (Mode == ESimMode::Layout)
	{
		EnterSimulationMode();
		if (Mode == ESimMode::Layout) return; // Rejected room must not enter through warm-up.
	}
	if (bNeedsRebuild)
	{
		BuildCoupling();
		bNeedsRebuild = (Racks.Num() == 0);
	}
	PendingWarmupSteps = FMath::Clamp(NumSteps, 1, 100000);
	WarmupStepsReported = PendingWarmupSteps;
	if (AirField != nullptr) AirField->GetMutableSolver().ResetStageTimes();
	WarmupCouplingMs = WarmupRackMs = 0.0;
	WarmupInletSumK.Reset(); WarmupInletMaxK.Reset();
	WarmupInletSamples = 0;
	VrgPerf::PendingWarmupSteps() = PendingWarmupSteps;
	bSimRunning = true;
	UE_LOG(LogTemp, Display, TEXT("[VrgSim] 예열 %d스텝 예약 (%.2f s 분량)"),
		PendingWarmupSteps, PendingWarmupSteps * Clock.GetFixedStepS());
}

void ADataCenterSimulationActor::DrainWarmupSteps()
{
	// 연결이 아직 안 됐으면(방 액터가 배치를 파악하기 전) 빈 다리를 돌리게 된다.
	// 스텝을 쓰지 말고 연결될 때까지 기다린다.
	if (Racks.Num() == 0)
	{
		return;
	}

	// 한 프레임에 다 돌리면 창이 얼어붙는다. 프레임마다 이만큼만 쓴다.
	constexpr double WarmupBudgetMs = 120.0;
	const uint64 StartCycles = FPlatformTime::Cycles64();

	while (PendingWarmupSteps > 0)
	{
		// 시계에도 같은 시간을 넣는다. CompleteStep만 부르면 누적기가 음수로 내려가
		// 이후 실시간 프레임이 한참 동안 스텝을 못 만든다 (규약 4).
		const bool bHydraulicsResolved = (Room != nullptr) && Room->ConsumeRebuiltFlag();
		Clock.BeginFrame(Clock.GetFixedStepS());
		const uint64 StepStart = FPlatformTime::Cycles64();
		EvalBeforeStep();
		if (Room != nullptr) Room->StepCoolantInventory(Clock.GetFixedStepS(), Clock.GetSimTimeS());
		PlantBeforeStep();
		Bridge.Step(Clock.GetFixedStepS(), bHydraulicsResolved, (Room == nullptr) || Room->GetLastSolveResult().bConverged);
		PlantAfterStep(Clock.GetFixedStepS());
		SprayAfterStep(Clock.GetFixedStepS()); AirFlowAfterStep(Clock.GetFixedStepS());   // W2: the same step path as Tick
		WarmupCouplingMs +=FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StepStart);
		WarmupRackMs += Bridge.GetLastRackModelsMs();
		VrgPerf::Accumulator().RackModelsMs += Bridge.GetLastRackModelsMs();
 for(int32 I=0;I<5;++I) VrgPerf::Accumulator().DetailMs[9+I]+=Bridge.GetLastCouplingStageMs(I);
 for(int32 I=0;I<7;++I) VrgPerf::Accumulator().DetailMs[14+I]+=Bridge.GetLastAirStageMs(I);
		Clock.CompleteStep();
		EvalAfterStep();
		--PendingWarmupSteps;

		if (PendingWarmupSteps < WarmupStepsReported / 4)
		{
			const FCouplingSnapshot Snap = GetSnapshot();
			if (WarmupInletSumK.Num() != Snap.RackAirInletTempK.Num())
			{
				WarmupInletSumK.Init(0.0, Snap.RackAirInletTempK.Num());
				WarmupInletMaxK.Init(0.0, Snap.RackAirInletTempK.Num());
				WarmupInletSamples = 0;
			}
			for (int32 I = 0; I < WarmupInletSumK.Num(); ++I)
			{
				WarmupInletSumK[I] += Snap.RackAirInletTempK[I];
				WarmupInletMaxK[I] = FMath::Max(WarmupInletMaxK[I], Snap.RackAirInletTempK[I]);
			}
			++WarmupInletSamples;
		}

		if (FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles) >= WarmupBudgetMs)
		{
			break;
		}
	}
	VrgPerf::PendingWarmupSteps() = PendingWarmupSteps;

	// 표시용 시각을 맞춰 준다. 이 액터의 시계가 유일한 시계다.
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->SetExternalSimTimeS(Clock.GetSimTimeS());
		}
	}
	if (AirField != nullptr)
	{
		AirField->SetExternalSimTimeS(Clock.GetSimTimeS());
	}

	if (PendingWarmupSteps == 0 && !PendingReportName.IsEmpty())
	{
		FString Summary;
		const bool bOk = WriteEvaluationReport(PendingReportName, Summary);
		UE_LOG(LogTemp, Display, TEXT("[VrgEval] 리포트 %s: %s"), bOk ? TEXT("성공") : TEXT("실패"), *Summary);
		PendingReportName.Reset();
	}
	if (PendingWarmupSteps == 0 && PendingProfileRepeats > 0)
	{
		// vrg.Web.ProfileLayout <N> wait: measured after the warm-up so the rack rows and detail exist.
		UE_LOG(LogTemp, Display, TEXT("%s"), *ProfileDashboardLayout(PendingProfileRepeats));
		PendingProfileRepeats = 0;
	}
	if (PendingWarmupSteps == 0)
	{
		const FCouplingSnapshot Snap = GetSnapshot();
		double MinInK = TNumericLimits<double>::Max(), MaxInK = 0.0;
		for (const double K : Snap.RackAirInletTempK) { MinInK = FMath::Min(MinInK, K); MaxInK = FMath::Max(MaxInK, K); }
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 예열 %d스텝 완료 (%.2f s) | t=%.1f s | IT %.1f kW = 냉각수 방출 %.1f kW + 공기 코일 %.1f kW + 축적 | ")
			TEXT("코일 UA %.0f W/K (공기 부하 %.1f kW) | 랙 흡기 %.2f ~ %.2f C | 열수지 잔차 %.2e"),
			WarmupStepsReported, WarmupStepsReported * Clock.GetFixedStepS(), Snap.SimTimeS, Snap.TotalItHeatW / 1000.0,
			Snap.TotalRadiatorRejectedW / 1000.0, Snap.TotalAirCoolingRemovedW / 1000.0, EffectiveCoilUAWK, DesignAirLoadW / 1000.0,
			MinInK - 273.15, MaxInK - 273.15, Snap.NormalizedEnergyResidual);
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] %s"), *Bridge.DescribeEnergyLedger());
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 스텝 평균 %.2f ms = 랙 모델 %.2f ms + 공기 + 나머지 | %s"),
			WarmupCouplingMs / FMath::Max(WarmupStepsReported, 1), WarmupRackMs / FMath::Max(WarmupStepsReported, 1),
			AirField ? *AirField->GetMutableSolver().DescribeStageTimes() : TEXT("-"));
		// Which cabinet breathes the hottest air (racks first, then spines, as in the bridge).
		int32 HottestIndex = 0;
		for (int32 I = 1; I < Snap.RackAirInletTempK.Num(); ++I)
			if (Snap.RackAirInletTempK[I] > Snap.RackAirInletTempK[HottestIndex]) HottestIndex = I;
		const AActor* Hottest = HottestIndex < Racks.Num() ? static_cast<const AActor*>(Racks[HottestIndex].Get())
			: Spines.IsValidIndex(HottestIndex - Racks.Num()) ? static_cast<const AActor*>(Spines[HottestIndex - Racks.Num()].Get()) : nullptr;
		const FVector HottestM = Hottest ? SimUnits::UnrealToMeters(Hottest->GetActorLocation()) : FVector::ZeroVector;
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 공기 차단 %d | 차단 셀 %d개 | 콜드 아일 %d개"), int32(Containment), PassiveSolidCells, ColdAisles.Num());
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 냉각수 열 방출 %d | CDU %d개"), int32(CoolantRejection), CduSummaries.Num());
		for (int32 G = 0; G < CduSummaries.Num(); ++G)
		{
			const FCduSummary& S = CduSummaries[G];
			UE_LOG(LogTemp, Display, TEXT("[VrgSim]   CDU %d: 랙 %d대 | 부하 %.1f kW (설계 %.1f) | UA %.0f W/K, 설비수 %.2f kg/s | 회수 %.2f -> 공급 %.2f C | 설비수 %.2f -> %.2f C"),
				G, S.NumRacks, Snap.CduDutyW.IsValidIndex(G) ? Snap.CduDutyW[G] / 1000.0 : 0.0, S.DesignDutyW / 1000.0, S.UAWK, S.FacilityMassFlowKgs,
				Snap.CduReturnTempK.IsValidIndex(G) ? Snap.CduReturnTempK[G] - 273.15 : 0.0, Snap.CduSupplyTempK.IsValidIndex(G) ? Snap.CduSupplyTempK[G] - 273.15 : 0.0,
				S.FacilitySupplyTempK - 273.15, Snap.CduFacilityReturnTempK.IsValidIndex(G) ? Snap.CduFacilityReturnTempK[G] - 273.15 : 0.0);
		}
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 공기 냉각 방식 %d | CRAH %d대 (빠짐 %d, 코일 대체 %s) | 대당 %.3f m3/s, UA %.0f W/K | CRAH 코일 %.1f kW / 풍량 %.2f m3/s / 팬 %.2f kW | 랙 흡기 최고 %.2f C (%s, %.2f, %.2f m)"),
			int32(CrahLayout), CrahUnits.Num(), CrahUnitsDropped, bCrahFellBackToCoil ? TEXT("예") : TEXT("아니오"),
			CrahUnitFlowM3s, CrahUnitUAWK, Snap.CrahCoilRemovedW / 1000.0, Snap.CrahFlowM3s, Snap.CrahFanPowerW / 1000.0, MaxInK - 273.15,
			Hottest ? *Hottest->GetName() : TEXT("-"), HottestM.X, HottestM.Y);
		if (WarmupInletSamples > 0)
		{
			double AvgMinK = TNumericLimits<double>::Max(), AvgMaxK = 0.0, AvgMeanK = 0.0, PeakK = 0.0;
			for (int32 I = 0; I < WarmupInletSumK.Num(); ++I)
			{
				const double AvgK = WarmupInletSumK[I] / WarmupInletSamples;
				AvgMinK = FMath::Min(AvgMinK, AvgK); AvgMaxK = FMath::Max(AvgMaxK, AvgK); AvgMeanK += AvgK / WarmupInletSumK.Num();
				PeakK = FMath::Max(PeakK, WarmupInletMaxK[I]);
				const AActor* Cabinet = I < Racks.Num() ? static_cast<const AActor*>(Racks[I].Get())
					: Spines.IsValidIndex(I - Racks.Num()) ? static_cast<const AActor*>(Spines[I - Racks.Num()].Get()) : nullptr;
				const FVector M = Cabinet ? SimUnits::UnrealToMeters(Cabinet->GetActorLocation()) : FVector::ZeroVector;
				UE_LOG(LogTemp, Display, TEXT("[VrgSim]   흡기 (%.2f, %.2f) m: 평균 %.2f C / 최고 %.2f C / 마지막 %.2f C"), M.X, M.Y,
					AvgK - 273.15, WarmupInletMaxK[I] - 273.15, Snap.RackAirInletTempK[I] - 273.15);
			}
			UE_LOG(LogTemp, Display, TEXT("[VrgSim] 마지막 %.0f s 시간 평균 흡기: 최저 %.2f / 평균 %.2f / 최고 %.2f C | 순간 최고 %.2f C (%d 표본)"),
				WarmupInletSamples * Clock.GetFixedStepS(), AvgMinK - 273.15, AvgMeanK - 273.15, AvgMaxK - 273.15, PeakK - 273.15, WarmupInletSamples);
		}
	}
}

// ---- S6 evaluation: events, per-step recording, reports (2026-09-27) ----

void ADataCenterSimulationActor::EvalBeforeStep()
{
	// Events fire on the integrated clock (rule 4), in the order given, once per simulation run.
	const double NowS = Clock.GetSimTimeS();
	for (FEvalEvent& E : EvalEvents)
	{
		if (E.bApplied || NowS + 1e-9 < E.TimeS) continue;
		E.bApplied = true;
		if (E.Kind == TEXT("util"))
		{
			for (ARackThermalActor* Rack : Racks) if (Rack != nullptr) Rack->Input.Utilization = FMath::Clamp(E.Value, 0.0, 1.0);
		}
		else if (E.Kind == TEXT("rackutil") && Racks.IsValidIndex(E.Index) && Racks[E.Index] != nullptr)
		{
			Racks[E.Index]->Input.Utilization = FMath::Clamp(E.Value, 0.0, 1.0);
		}
		else if (E.Kind == TEXT("ambient"))
		{
			// S8 F2: outdoor dry bulb [K] for the dry coolers from this step on (rule 4: at this step's start time).
			PlantThermalSpec.AmbientK = E.Value;
			Plant.SetAmbientK(E.Value);
		}
		else if (E.Kind == TEXT("valve") && Room != nullptr)
		{
			// S2b step 4: Index = rack in the room's routed order, Value 1 = close, 0 = open. Re-solves now, before
			// this step's coupling reads the flows (rule 4: the change happens at this step's start time).
			const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
			if (Routed.IsValidIndex(E.Index) && Room->SetRackValveClosed(Routed[E.Index], E.Value > 0.5))
			{
				const ADataCenterRoomActor::FValveReport& V = Room->GetLastValveReport();
				FVrgValveRecord R;
				R.TimeS = NowS; R.Rack = E.Index; R.bClosed = V.bClosed; R.IsolatedRacks = V.IsolatedRacks; R.ImpactedRacks = V.ImpactedRacks;
				FaultMonitor.AddValve(R);
			}
		}
		else if (E.Kind == TEXT("leak") && Room != nullptr)
		{
			// S2b step 5: opens the hole prepared at entry, at this step's start (rule 4).
			const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
			// The hole was prepared at entry; while running EnsureLeakSpec cannot add one (topology locked), so an
			// unprepared event is reported instead of rebuilding the room mid-run.
			if (Routed.IsValidIndex(E.Index) && Room->SetLeakHoleDiameterM(Room->EnsureLeakSpec(Routed[E.Index], E.bFlag), E.Value))
			{
				FVrgLeakRecord R;
				R.TimeS = NowS; R.Rack = E.Index; R.bReturnBranch = E.bFlag; R.HoleDiameterM = E.Value;
				FaultMonitor.AddLeak(R);
			}
			else UE_LOG(LogTemp, Warning, TEXT("[VrgEval] t=%.2f s 누수 이벤트(랙 %d)를 적용하지 못함: 진입 때 준비된 구멍이 없다(랙 번호 확인)"), NowS, E.Index);
		}
		else if (E.Kind == TEXT("plant"))
		{
			// S8 F6b: facility leak / trip at this step's start (rule 4), before PlantBeforeStep hands the flows over.
			ApplyPlantFault(E.Args);
		}
		else if (E.Kind == TEXT("leakat") && E.Args.Num() >= 1)
		{
			// W1: a live leak at the site nearest to a point [m], at this step's start (rule 4).
			const int32 Site = E.Args.Num() == 1 ? ResolveLiveLeakSite(E.Args[0]) : E.Args.Num() >= 3
				? FindLiveLeakSite(FVector(FCString::Atod(*E.Args[0]), FCString::Atod(*E.Args[1]), FCString::Atod(*E.Args[2]))) : INDEX_NONE;
			if (Site == INDEX_NONE || !SetLiveLeak(Site, E.Value, TEXT("이벤트")))
				UE_LOG(LogTemp, Warning, TEXT("[VrgEval] t=%.2f s leakat: 지점 이름/좌표를 찾지 못했거나 적용 실패"), NowS);
		}
		UE_LOG(LogTemp, Display, TEXT("[VrgEval] t=%.2f s 이벤트 %s %d %.3f"), NowS, *E.Kind, E.Index, E.Value);
	}
}

bool ADataCenterSimulationActor::ApplyPlantFault(const TArray<FString>& Args)
{
	if (!bPlantActive || !Plant.IsThermalActive())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Plant] 설비 장애는 설비 열 모델이 도는 시뮬레이션 모드에서만 넣는다(vrg.Plant.Enable 1 후 진입)"));
		return false;
	}
	auto LoopOf = [](const FString& Text, EPlantLoopId& Out)
	{
		const FString L = Text.ToLower();
		if (L == TEXT("fws")) Out = EPlantLoopId::Fws;
		else if (L == TEXT("chw")) Out = EPlantLoopId::Chw;
		else if (L == TEXT("cw")) Out = EPlantLoopId::Cw;
		else return false;
		return true;
	};
	const FString Cmd = Args.Num() > 0 ? Args[0].ToLower() : FString();
	const double NowS = Clock.GetSimTimeS();
	const int32 Before = Plant.GetFaultLog().Num();
	EPlantLoopId Id = EPlantLoopId::Fws;
	bool bOk = false;
	if (Cmd == TEXT("leak") && Args.Num() >= 5 && LoopOf(Args[1], Id))
	{
		const FString Site = Args[2].ToLower();
		const EPlantLeakSite S = Site == TEXT("return") ? EPlantLeakSite::ReturnHeader : Site == TEXT("branchsupply") ? EPlantLeakSite::BranchSupply
			: Site == TEXT("branchreturn") ? EPlantLeakSite::BranchReturn : EPlantLeakSite::SupplyHeader;
		bOk = (Site == TEXT("supply") || Site == TEXT("return") || Site == TEXT("branchsupply") || Site == TEXT("branchreturn"))
			&& Plant.AddLeak(Id, S, FCString::Atoi(*Args[3]), FCString::Atod(*Args[4]) * 1e-3, NowS) != INDEX_NONE;
	}
	else if (Cmd == TEXT("leakclear")) { Plant.ClearLeaks(NowS); bOk = true; }
	else if (Cmd == TEXT("pump") && Args.Num() >= 3 && LoopOf(Args[1], Id)) bOk = Plant.TripPump(Id, FCString::Atoi(*Args[2]), NowS);
	else if ((Cmd == TEXT("chiller") || Cmd == TEXT("trim")) && Args.Num() >= 2) { Plant.SetTrippedChillers(FCString::Atoi(*Args[1]), Cmd == TEXT("trim"), NowS); bOk = true; }
	else if (Cmd == TEXT("drycooler") && Args.Num() >= 3 && LoopOf(Args[1], Id)) bOk = Plant.SetTrippedDryCoolers(Id, FCString::Atoi(*Args[2]), NowS);
	if (!bOk)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Plant] 장애를 적용하지 못함(%s): 구성 %s에 그 루프·분기·펌프가 없거나 인자가 틀렸다. 사용법: leak <fws|chw|cw> <supply|return|branchsupply|branchreturn> <분기> <mm> | leakclear | pump <루프> <번호> | chiller <대수> | trim <대수> | drycooler <fws|cw> <대수>"),
			*FString::Join(Args, TEXT(" ")), FFacilityPlant::TopologyName(Plant.GetTopology()));
		return false;
	}
	UpdatePlantDisplay(true);   // F7: leak markers and failed equipment show at once
	for (int32 I = Before; I < Plant.GetFaultLog().Num(); ++I)
	{
		const FPlantFaultReport& R = Plant.GetFaultLog()[I];
		FString Rows;
		for (int32 B = 0; B < R.BranchAfterM3s.Num(); ++B)
		{
			const double A = R.BranchBeforeM3s.IsValidIndex(B) ? R.BranchBeforeM3s[B] : 0.0;
			if (FMath::Abs(R.BranchAfterM3s[B] - A) > 1e-6 * FMath::Max(FMath::Abs(A), 1e-9))
				Rows += FString::Printf(TEXT(" [분기 %d: %.3f -> %.3f L/s]"), B, A * 1e3, R.BranchAfterM3s[B] * 1e3);
		}
		UE_LOG(LogTemp, Display, TEXT("[Plant] 장애 t=%.2f s %s: 누수 %.3f L/s, 펌프 %d -> %d대(속도 %.4f -> %.4f)%s%s, 유량이 바뀐 분기 %d/%d%s"),
			R.TimeS, *R.What, R.LeakM3s * 1e3, R.PumpsRunningBefore, R.PumpsRunningAfter, R.PumpSpeedBefore, R.PumpSpeedAfter,
			R.bPumpsSaturated ? TEXT(" | 차압 설정 미달(펌프 포화)") : TEXT(""), (R.BranchAfterM3s.Num() > 0 && !R.bControlConverged) ? TEXT(" | 제어 수렴 실패") : TEXT(""),
			R.AffectedBranches, R.BranchAfterM3s.Num(), *Rows);
	}
	return true;
}

FVrgFaultInput ADataCenterSimulationActor::MakeFaultInput() const
{
	FVrgFaultInput In;
	In.TimeS = Clock.GetSimTimeS();
	if (Room == nullptr) return In;
	const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
	const TArray<double>& Design = Room->GetRackDesignFlowM3s();
	const TArray<double>& Flow = Bridge.GetLastCoolantMassFlowKgs();
	In.bHydraulicsConverged = Room->GetLastSolveResult().bConverged;
	In.HydraulicResidual = Room->GetLastSolveResult().NormalizedResidual;
	for (int32 I = 0; I < Routed.Num(); ++I)
	{
		const int32 J = Racks.IndexOfByKey(Routed[I]);
		In.CoolantMassFlowKgs.Add(Flow.IsValidIndex(J) ? Flow[J] : 0.0);
		In.DesignMassFlowKgs.Add(Design.IsValidIndex(I) ? Design[I] * CoolantDensityKgM3 : 0.0);
		double MaxK = 0.0;
		if (Routed[I] != nullptr) for (const double K : Routed[I]->GetMutableModel().GetState().ComponentTempK) MaxK = FMath::Max(MaxK, K);
		In.MaxComponentTempK.Add(MaxK);
		In.LoopDepleted.Add(Room->IsRackLoopDepleted(I));
	}
	return In;
}

void ADataCenterSimulationActor::FaultAfterStep()
{
	if (Room == nullptr) return;
	const FVrgFaultInput In = MakeFaultInput();
	const int32 Before = FaultMonitor.GetFaults().Num();
	if (FaultMonitor.Update(In) > 0)
		for (int32 F = Before; F < FaultMonitor.GetFaults().Num(); ++F)
		{
			const FVrgFaultRecord& R = FaultMonitor.GetFaults()[F];
			UE_LOG(LogTemp, Display, TEXT("[Fault] t=%.2f s 랙 %d %s (값 %.4g) — 계산된 유량·온도로 판정(D4)"), R.TimeS, R.Rack, FVrgFaultMonitor::KindName(R.Kind), R.Value);
		}
	// Tank history: one sample per simulated second per row.
	const int32 Second = FMath::FloorToInt32(In.TimeS + 1e-9);
	if (Second != LastTankSampleSecond)
	{
		LastTankSampleSecond = Second;
		const FCoolantInventory& Inv = Room->GetCoolantInventory();
		for (int32 T = 0; T < Inv.GetTanks().Num(); ++T)
		{
			const FCoolantInventory::FTank& Tank = Inv.GetTanks()[T];
			FVrgTankSample S;
			S.TimeS = In.TimeS; S.Row = T; S.PressurePa = Inv.GetTankGaugePressurePa(T);
			S.WaterM3 = Tank.State.WaterVolumeM3; S.LeakedM3 = Tank.State.LeakedM3; S.bDepleted = Tank.State.bDepleted;
			FaultMonitor.AddTankSample(S);
		}
	}
}

void ADataCenterSimulationActor::EvalAfterStep()
{
	FaultAfterStep();
	const TArray<double>& Inlets = Bridge.GetLastAirInletTempK();
	if (!bEvalReady || Eval.GetNumInlets() != Inlets.Num())
	{
		Eval.Reset(EvalSettings, Inlets.Num(), Inlets);
		bEvalReady = true;
		RebuildInstruments();   // the run's pipes, units and CDUs exist by its first step
	}
	FVrgEvaluationSample Power;
	Power.ItW = Bridge.GetLastItHeatW();
	Power.ServerFanW = Bridge.GetLastServerFanPowerW();
	Power.CrahFanW = Bridge.GetLastCrahFanPowerW();
	Power.PumpW = Room != nullptr ? Room->GetTotalPumpElectricalW() : 0.0;
	Power.CoolantRemovedW = Bridge.GetTotalRadiatorRejectedW();
	Power.AirRemovedW = Bridge.GetLastAirCoolingRemovedW();
	// S8 F5: plant pumps, dry cooler fans, compressors (only with the plant model; the fixed boundary has none).
	Power.bPlant = bPlantActive && Plant.IsThermalActive();
	Power.PlantW = Power.bPlant ? Plant.GetPlantElectricW() : 0.0;
	Eval.AfterStep(Clock.GetSimTimeS(), Clock.GetFixedStepS(), Inlets, Power);
	InstrumentsAfterStep();
}

double ADataCenterSimulationActor::GetSiteEnergyInJ() const
{
	return Bridge.GetEnergyInJ() + (bPlantActive && Plant.IsThermalActive() ? Plant.GetExternalInJ() : 0.0);
}

double ADataCenterSimulationActor::GetSiteEnergyResidual() const
{
	if (!bPlantActive || !Plant.IsThermalActive()) return 0.0;
	const double InJ = GetSiteEnergyInJ();
	return FMath::Abs(InJ) > UE_DOUBLE_SMALL_NUMBER ? (Bridge.GetEnergyResidualJ() + Plant.GetLedgerResidualJ()) / InJ : 0.0;
}

bool ADataCenterSimulationActor::WriteEvaluationReport(const FString& Name, FString& OutSummary) const
{
	if (!bEvalReady || Eval.GetNumInlets() == 0)
	{
		OutSummary = TEXT("시뮬레이션 스텝이 아직 없다(배치 모드에서는 리포트가 없다)");
		return false;
	}
	const FVrgEvaluationSettings& S = Eval.GetSettings();
	const FCouplingSnapshot Snap = GetSnapshot();
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("name"), Name);
	Root->SetNumberField(TEXT("simTimeS"), Eval.GetSimTimeS());
	Root->SetNumberField(TEXT("windowS"), Eval.GetWindowCoveredS());

	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	if (AirField != nullptr)
	{
		const FAirSolver& Air = AirField->GetMutableSolver();
		Config->SetNumberField(TEXT("cells"), Air.GetGrid().GetNumCells());
		Config->SetNumberField(TEXT("cellSizeM"), Air.GetGrid().GetCellSizeM());
		Config->SetBoolField(TEXT("turbulence"), Air.GetTurbulenceSettings().bEnabled);
		Config->SetBoolField(TEXT("gpuStep"), Air.DidLastStepRunOnGpu());
		Config->SetBoolField(TEXT("octree"), Air.IsUsingOctree());
	}
	Config->SetNumberField(TEXT("crahLayout"), int32(CrahLayout));
	Config->SetNumberField(TEXT("containment"), int32(Containment));
	Config->SetNumberField(TEXT("coolantRejection"), int32(CoolantRejection));
	Config->SetNumberField(TEXT("sensorSeed"), S.Seed);
	Root->SetObjectField(TEXT("config"), Config);

	const FVrgEvaluationSample P = Eval.GetWindowPower();
	TSharedRef<FJsonObject> Power = MakeShared<FJsonObject>();
	Power->SetNumberField(TEXT("itW"), P.ItW);
	Power->SetNumberField(TEXT("serverFanW"), P.ServerFanW);
	Power->SetNumberField(TEXT("crahFanW"), P.CrahFanW);
	Power->SetNumberField(TEXT("pumpW"), P.PumpW);
	// Web dashboard stage 5 (optional fields; older reports lack them): window-average heat removal [W].
	Power->SetNumberField(TEXT("coolantRemovedW"), P.CoolantRemovedW);
	Power->SetNumberField(TEXT("airRemovedW"), P.AirRemovedW);
	double Pue = 0.0;
	const bool bPue = Eval.GetPartialPue(Pue);
	if (bPue) Power->SetNumberField(TEXT("partialPue"), Pue); else Power->SetField(TEXT("partialPue"), MakeShared<FJsonValueNull>());
	// S8 F6: the plant topology and its design point, so an A/B comparison can show whether both runs were sized alike.
	if (P.bPlant && bPlantActive)
	{
		TSharedRef<FJsonObject> PlantJ = MakeShared<FJsonObject>();
		PlantJ->SetStringField(TEXT("topology"), FFacilityPlant::TopologyName(Plant.GetTopology()));
		PlantJ->SetNumberField(TEXT("designAmbientK"), PlantThermalSpec.DesignAmbientK);
		PlantJ->SetNumberField(TEXT("ambientK"), Plant.GetAmbientK());
		PlantJ->SetNumberField(TEXT("designCduHeatW"), Plant.GetDesignHeatW());
		PlantJ->SetNumberField(TEXT("designChwLoadW"), Plant.GetDesignChwLoadW());
		PlantJ->SetNumberField(TEXT("designCondenserW"), Plant.GetDesignCondenserW());
		PlantJ->SetNumberField(TEXT("chillerRefCapacityW"), Plant.GetChillerRefCapacityW());
		PlantJ->SetNumberField(TEXT("trimRefCapacityW"), Plant.GetTrimRefCapacityW());
		PlantJ->SetNumberField(TEXT("fwsDryCoolers"), Plant.GetNumDryCoolers());
		PlantJ->SetNumberField(TEXT("cwDryCoolers"), Plant.GetNumCwDryCoolers());
		PlantJ->SetNumberField(TEXT("economizerOnS"), Plant.GetState().EconomizerOnS);
		PlantJ->SetNumberField(TEXT("compressorJ"), Plant.GetState().CompressorInJ);
		PlantJ->SetNumberField(TEXT("siteLedgerResidual"), GetSiteEnergyResidual());
		// S8 F6b: facility leaks and faults of this run (optional fields).
		PlantJ->SetNumberField(TEXT("leakMassKg"), Plant.GetState().LeakMassKg);
		PlantJ->SetNumberField(TEXT("leakOutJ"), Plant.GetState().LeakOutJ);
		// F6 comparison items (plan): end-of-run CDU secondary supply (TCS, mean over CDUs), hottest component, and the
		// time the plant missed a setpoint (FWS supply after the trim chiller, chiller capacity).
		double TcsK = 0.0;
		for (const double K : Snap.CduSupplyTempK) TcsK += K / FMath::Max(Snap.CduSupplyTempK.Num(), 1);
		PlantJ->SetNumberField(TEXT("cduSecondarySupplyK"), TcsK);
		double MaxCompK = 0.0;
		for (const double K : MakeFaultInput().MaxComponentTempK) MaxCompK = FMath::Max(MaxCompK, K);
		PlantJ->SetNumberField(TEXT("maxComponentK"), MaxCompK);
		PlantJ->SetNumberField(TEXT("fwsSupplyNotMetS"), Plant.GetState().FwsSupplyNotMetS);
		PlantJ->SetNumberField(TEXT("chillerLimitedS"), Plant.GetState().ChillerLimitedS);
		TArray<TSharedPtr<FJsonValue>> Faults;
		for (const FPlantFaultReport& F : Plant.GetFaultLog())
		{
			TSharedRef<FJsonObject> FJ = MakeShared<FJsonObject>();
			FJ->SetNumberField(TEXT("t"), F.TimeS);
			FJ->SetStringField(TEXT("what"), F.What);
			FJ->SetStringField(TEXT("loop"), FFacilityPlant::LoopName(F.Loop));
			FJ->SetNumberField(TEXT("leakM3s"), F.LeakM3s);
			FJ->SetNumberField(TEXT("affectedBranches"), F.AffectedBranches);
			FJ->SetNumberField(TEXT("branches"), F.BranchAfterM3s.Num());
			FJ->SetBoolField(TEXT("pumpsSaturated"), F.bPumpsSaturated);
			Faults.Add(MakeShared<FJsonValueObject>(FJ));
		}
		PlantJ->SetArrayField(TEXT("faults"), Faults);
		Root->SetObjectField(TEXT("plant"), PlantJ);
	}
	// S8 F5 (optional fields; older reports and fixed-boundary runs lack them): plant electric input and cooling PUE.
	double CoolingPue = 0.0;
	if (P.bPlant) Power->SetNumberField(TEXT("plantW"), P.PlantW);
	if (Eval.GetCoolingPue(CoolingPue)) Power->SetNumberField(TEXT("coolingPue"), CoolingPue);
	Power->SetStringField(TEXT("partialPueScope"), TEXT("(IT + CRAH fans + pumps) / IT; chillers, cooling towers, UPS/distribution losses and lighting are not modelled"));
	if (P.bPlant) Power->SetStringField(TEXT("coolingPueScope"), TEXT("(IT + CRAH fans + CDU pumps + plant pumps + dry cooler fans + chiller compressors) / IT; UPS/distribution losses and lighting are not modelled, so not a facility PUE"));
	Root->SetObjectField(TEXT("power"), Power);

	TArray<TSharedPtr<FJsonValue>> InletItems;
	int32 AboveRecommended = 0, AboveAllowable = 0, SensorAlarms = 0;
	double MaxMeanK = 0.0, FirstDetectionS = -1.0;
	for (int32 I = 0; I < Eval.GetNumInlets(); ++I)
	{
		const FVrgEvaluation::FInletStats St = Eval.GetInlet(I);
		const AActor* Cabinet = I < Racks.Num() ? static_cast<const AActor*>(Racks[I].Get())
			: Spines.IsValidIndex(I - Racks.Num()) ? static_cast<const AActor*>(Spines[I - Racks.Num()].Get()) : nullptr;
		const FVector M = Cabinet ? SimUnits::UnrealToMeters(Cabinet->GetActorLocation()) : FVector::ZeroVector;
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), I);
		Item->SetStringField(TEXT("name"), Cabinet ? Cabinet->GetName() : FString());
		Item->SetNumberField(TEXT("x_m"), M.X); Item->SetNumberField(TEXT("y_m"), M.Y);
		Item->SetNumberField(TEXT("windowMeanC"), St.WindowMeanK - 273.15);
		Item->SetNumberField(TEXT("runMeanC"), St.RunMeanK - 273.15);
		Item->SetNumberField(TEXT("peakC"), St.PeakK - 273.15);
		Item->SetNumberField(TEXT("secondsAboveRecommended"), St.SecondsAboveRecommended);
		Item->SetNumberField(TEXT("secondsAboveAllowable"), St.SecondsAboveAllowable);
		Item->SetNumberField(TEXT("sensorLastC"), St.SensorLastK - 273.15);
		Item->SetNumberField(TEXT("sensorFirstAlarmS"), St.SensorFirstAlarmS);
		Item->SetNumberField(TEXT("sensorAlarms"), St.SensorAlarms);
		InletItems.Add(MakeShared<FJsonValueObject>(Item));
		AboveRecommended += St.WindowMeanK > S.RecommendedInletK ? 1 : 0;
		AboveAllowable += St.WindowMeanK > S.AllowableInletK ? 1 : 0;
		SensorAlarms += St.SensorAlarms > 0 ? 1 : 0;
		if (St.SensorFirstAlarmS >= 0.0) FirstDetectionS = FirstDetectionS < 0.0 ? St.SensorFirstAlarmS : FMath::Min(FirstDetectionS, St.SensorFirstAlarmS);
	}
	Root->SetArrayField(TEXT("inlets"), InletItems);
	double AvgMeanK = 0.0;
	Eval.GetInletWindowSummary(MaxMeanK, AvgMeanK);
	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("inletWindowMeanMaxC"), MaxMeanK - 273.15);
	Summary->SetNumberField(TEXT("inletWindowMeanAvgC"), AvgMeanK - 273.15);
	Summary->SetNumberField(TEXT("inletsAboveRecommended"), AboveRecommended);
	Summary->SetNumberField(TEXT("inletsAboveAllowable"), AboveAllowable);
	Summary->SetNumberField(TEXT("inletsWithSensorAlarm"), SensorAlarms);
	Summary->SetNumberField(TEXT("firstSensorAlarmS"), FirstDetectionS);
	Root->SetObjectField(TEXT("summary"), Summary);

	TSharedRef<FJsonObject> Checks = MakeShared<FJsonObject>();
	Checks->SetNumberField(TEXT("globalNormalizedEnergyResidual"), Snap.NormalizedEnergyResidual);
	Checks->SetStringField(TEXT("ledger"), Bridge.DescribeEnergyLedger());
	if (AirField != nullptr)
	{
		Checks->SetNumberField(TEXT("pressureSolves"), double(AirField->GetMutableSolver().GetNumPressureSolves()));
		Checks->SetNumberField(TEXT("pressureFailures"), double(AirField->GetMutableSolver().GetNumPressureFailures()));
	}
	Checks->SetBoolField(TEXT("hydraulicsConverged"), Snap.bHydraulicsConverged);
	Root->SetObjectField(TEXT("checks"), Checks);

	TArray<TSharedPtr<FJsonValue>> EventItems;
	for (const FEvalEvent& E : EvalEvents)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("timeS"), E.TimeS); Item->SetStringField(TEXT("kind"), E.Kind);
		Item->SetNumberField(TEXT("index"), E.Index); Item->SetNumberField(TEXT("value"), E.Value); Item->SetBoolField(TEXT("applied"), E.bApplied);
		if (E.Kind == TEXT("leak")) Item->SetBoolField(TEXT("returnBranch"), E.bFlag);
		if (E.Kind == TEXT("leakat") && E.Args.Num() >= 3)
			Item->SetArrayField(TEXT("pointM"), { MakeShared<FJsonValueNumber>(FCString::Atod(*E.Args[0])), MakeShared<FJsonValueNumber>(FCString::Atod(*E.Args[1])),
				MakeShared<FJsonValueNumber>(FCString::Atod(*E.Args[2])) });
		if (E.Kind == TEXT("leakat") && E.Args.Num() == 1) Item->SetStringField(TEXT("site"), E.Args[0]);
		EventItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("events"), EventItems);
	// Stage 5: the plan view at this SimTime (equipment footprints in metres), so an A/B comparison can show what moved.
	Root->SetObjectField(TEXT("layout"), BuildDashboardLayout().ToJsonObject());
	// S2b step 5 (optional field; older reports lack it): leaks, valves, tank history, first fault times.
	Root->SetObjectField(TEXT("s2b"), FaultMonitor.ToJsonObject());
	// 2026-10-23 (optional field): the instrument points and the leak alarms of their detection cables (D9 / D4).
	TSharedRef<FJsonObject> InstrumentsJson = MakeShared<FJsonObject>();
	InstrumentsJson->SetNumberField(TEXT("points"), Instruments.Num());
	InstrumentsJson->SetNumberField(TEXT("leakCables"), Instruments.GetNumLeakCables());
	InstrumentsJson->SetBoolField(TEXT("simulatedMeasurement"), true);
	TArray<TSharedPtr<FJsonValue>> RackWaterSensors;
	auto ContactCoords = [](const FVector& V) -> TArray<TSharedPtr<FJsonValue>>
	{ return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)}; };
	for (int32 I = 0; I < Instruments.Num(); ++I)
	{
		const auto& Pt = Instruments.GetPoint(I);
		if (!Pt.bWaterContact) continue;
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("sensorId"), Pt.Id); Item->SetStringField(TEXT("rackId"), Pt.RackId);
		Item->SetStringField(TEXT("type"), InstrumentSpecs::SiteCode(Pt.Site)); Item->SetBoolField(TEXT("estimatedSpec"), true);
		Item->SetArrayField(TEXT("positionM"), ContactCoords(Pt.PositionM));
		Item->SetArrayField(TEXT("coverageStartM"), ContactCoords(Pt.CoverageStartM)); Item->SetArrayField(TEXT("coverageEndM"), ContactCoords(Pt.CoverageEndM));
		Item->SetNumberField(TEXT("thresholdM3"), Pt.Contact.WettingVolumeM3); Item->SetNumberField(TEXT("samplePeriodS"), Pt.Contact.SamplePeriodS);
		Item->SetNumberField(TEXT("responseDelayS"), Pt.Contact.ResponseDelayS); Item->SetNumberField(TEXT("contactWaterM3"), Instruments.GetContactWaterM3(I));
		Item->SetBoolField(TEXT("alarm"), Instruments.IsLeakAlarm(I)); Item->SetNumberField(TEXT("alarmTimeS"), Instruments.GetLeakAlarmTimeS(I));
		RackWaterSensors.Add(MakeShared<FJsonValueObject>(Item));
	}
	InstrumentsJson->SetArrayField(TEXT("rackWaterSensors"), RackWaterSensors);
	TArray<TSharedPtr<FJsonValue>> LeakAlarmItems;
	for (const FLeakAlarm& A : Instruments.GetAlarmLog())
	{
		const FInstrumentPoint& Pt = Instruments.GetPoint(A.Point);
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("timeS"), A.TimeS); Item->SetStringField(TEXT("label"), Pt.Label);
		Item->SetNumberField(TEXT("x_m"), Pt.PositionM.X); Item->SetNumberField(TEXT("y_m"), Pt.PositionM.Y); Item->SetNumberField(TEXT("z_m"), Pt.PositionM.Z);
		Item->SetNumberField(TEXT("coveredLeakLps"), A.FlowM3s * 1e3);
		Item->SetStringField(TEXT("sensorId"), Pt.Id.IsEmpty() ? FString::Printf(TEXT("instrument:%d"), A.Point) : Pt.Id);
		Item->SetStringField(TEXT("rackId"), Pt.RackId); Item->SetStringField(TEXT("type"), InstrumentSpecs::SiteCode(Pt.Site));
		Item->SetBoolField(TEXT("flowBased"), !Pt.bWaterContact);
		if (Pt.bWaterContact)
		{
			Item->SetNumberField(TEXT("contactWaterM3"), A.WaterM3); Item->SetNumberField(TEXT("firstContactS"), A.FirstContactS);
			Item->SetNumberField(TEXT("thresholdReachedS"), A.ThresholdS);
		}
		LeakAlarmItems.Add(MakeShared<FJsonValueObject>(Item));
	}
	InstrumentsJson->SetArrayField(TEXT("leakAlarms"), LeakAlarmItems);
	Root->SetObjectField(TEXT("instruments"), InstrumentsJson);

	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"));
	const FString JsonPath = FPaths::Combine(Dir, Name + TEXT(".json"));
	FString Md = FString::Printf(TEXT("# 평가 리포트 %s\n\n시뮬 시각 %.1f s, 평균 창 %.0f s. 모의 계측값과 보정되지 않은 모델의 결과이며 실제 설비 값이 아니다(D9).\n\n"),
		*Name, Eval.GetSimTimeS(), Eval.GetWindowCoveredS());
	Md += FString::Printf(TEXT("## 전력 (창 평균)\n\n| IT | 서버 팬(IT에 포함) | CRAH 팬 | 펌프 | 부분 PUE |\n|---|---|---|---|---|\n| %.1f kW | %.1f kW | %.1f kW | %.2f kW | %s |\n\n"),
		P.ItW / 1e3, P.ServerFanW / 1e3, P.CrahFanW / 1e3, P.PumpW / 1e3, bPue ? *FString::Printf(TEXT("%.4f"), Pue) : TEXT("계산 불가(IT 0)"));
	Md += TEXT("부분 PUE = (IT + CRAH 팬 + 펌프) / IT. 칠러·냉각탑·UPS·배전 손실·조명은 모델에 없어 시설 PUE가 아니다(D8).\n\n");
	Md += FString::Printf(TEXT("## 흡기\n\n창 평균 최고 %.2f C / 평균 %.2f C, 권장 27 C 초과 %d, 허용 32 C 초과 %d, 센서 경보 %d (첫 감지 %.1f s)\n\n| # | 위치 (m) | 창 평균 C | 최고 C | 27 C 초과 s | 센서 마지막 C | 첫 경보 s |\n|---|---|---|---|---|---|---|\n"),
		MaxMeanK - 273.15, AvgMeanK - 273.15, AboveRecommended, AboveAllowable, SensorAlarms, FirstDetectionS);
	for (int32 I = 0; I < Eval.GetNumInlets(); ++I)
	{
		const FVrgEvaluation::FInletStats St = Eval.GetInlet(I);
		const TSharedPtr<FJsonObject> Item = InletItems[I]->AsObject();
		Md += FString::Printf(TEXT("| %d | (%.2f, %.2f) | %.2f | %.2f | %.1f | %.1f | %s |\n"), I, Item->GetNumberField(TEXT("x_m")), Item->GetNumberField(TEXT("y_m")),
			St.WindowMeanK - 273.15, St.PeakK - 273.15, St.SecondsAboveRecommended, St.SensorLastK - 273.15,
			St.SensorFirstAlarmS >= 0.0 ? *FString::Printf(TEXT("%.1f"), St.SensorFirstAlarmS) : TEXT("-"));
	}
	Md += FString::Printf(TEXT("\n## 누수 감지 센서\n\n계측 지점 %d개, 누수 감지 케이블 %d개, 경보 %d건 (모의 계측값, 덮는 반경은 게임용 추상화, D9).\n\n"),
		Instruments.Num(), Instruments.GetNumLeakCables(), Instruments.GetAlarmLog().Num());
	if (!Instruments.GetAlarmLog().IsEmpty())
	{
		Md += TEXT("접촉 센서는 도착/노출 누적량과 추정 문턱·지연을 사용한다. 배관 센서의 덮는 반경은 별도 게임 추상화다.\n\n");
		Md += TEXT("| 시각 s | 센서 ID | 랙 ID | 종류 | 지점 | 위치 (m) | 덮는 누수 L/s | 접촉량 m³ |\n|---|---|---|---|---|---|---|---|\n");
		for (const FLeakAlarm& A : Instruments.GetAlarmLog())
		{
			const FInstrumentPoint& Pt = Instruments.GetPoint(A.Point);
			const FString Id = Pt.Id.IsEmpty() ? FString::Printf(TEXT("instrument:%d"), A.Point) : Pt.Id;
			Md += FString::Printf(TEXT("| %.2f | %s | %s | %s | %s | (%.2f, %.2f, %.2f) | %s | %s |\n"), A.TimeS, *Id, *Pt.RackId,
				InstrumentSpecs::SiteCode(Pt.Site), *Pt.Label, Pt.PositionM.X, Pt.PositionM.Y, Pt.PositionM.Z,
				Pt.bWaterContact ? TEXT("-") : *FString::Printf(TEXT("%.3f"), A.FlowM3s * 1e3),
				Pt.bWaterContact ? *FString::Printf(TEXT("%.12g"), A.WaterM3) : TEXT("-"));
		}
	}
	Md += FString::Printf(TEXT("\n## 점검\n\n전역 정규화 열수지 잔차 %.3e. %s\n"), Snap.NormalizedEnergyResidual, *Bridge.DescribeEnergyLedger());
	const bool bOk = FFileHelper::SaveStringToFile(Json, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& FFileHelper::SaveStringToFile(Md, *FPaths::Combine(Dir, Name + TEXT(".md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	OutSummary = FString::Printf(TEXT("%s | 흡기 창 평균 최고 %.2f C, 권장 초과 %d, 허용 초과 %d, 센서 경보 %d | IT %.1f kW, 부분 PUE %s | 잔차 %.2e"),
		*JsonPath, MaxMeanK - 273.15, AboveRecommended, AboveAllowable, SensorAlarms, P.ItW / 1e3,
		bPue ? *FString::Printf(TEXT("%.4f"), Pue) : TEXT("계산 불가"), Snap.NormalizedEnergyResidual);
	return bOk;
}

namespace
{
	TArray<ADataCenterSimulationActor*> EvalSimActors()
	{
		TArray<ADataCenterSimulationActor*> Out;
		if (GEngine == nullptr) return Out;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld()) continue;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) Out.Add(*It);
		}
		return Out;
	}

	// A/B: the scalar results of two reports side by side and the per-inlet difference (matched by index).
	bool CompareReports(const FString& NameA, const FString& NameB, FString& OutSummary)
	{
		auto Read = [](const FString& Name, TSharedPtr<FJsonObject>& Out)
		{
			FString Text;
			return FFileHelper::LoadFileToString(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), Name + TEXT(".json")))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
		};
		TSharedPtr<FJsonObject> A, B;
		if (!Read(NameA, A) || !Read(NameB, B)) { OutSummary = TEXT("리포트를 읽지 못했다"); return false; }
		FString Md = FString::Printf(TEXT("# A/B: %s 대 %s\n\n같은 IT 부하·같은 시각 창에서 비교할 것(D8). 모의 계측값·보정되지 않은 모델의 결과다.\n\n| 항목 | A | B | B - A |\n|---|---|---|---|\n"), *NameA, *NameB);
		auto Row = [&](const TCHAR* Label, const TSharedPtr<FJsonObject>& ObjA, const TSharedPtr<FJsonObject>& ObjB, const TCHAR* Field, double Scale, const TCHAR* Unit)
		{
			double VA = 0.0, VB = 0.0;
			const bool bA = ObjA->TryGetNumberField(Field, VA), bB = ObjB->TryGetNumberField(Field, VB);
			Md += FString::Printf(TEXT("| %s | %s | %s | %s |\n"), Label,
				bA ? *FString::Printf(TEXT("%.4g %s"), VA * Scale, Unit) : TEXT("-"), bB ? *FString::Printf(TEXT("%.4g %s"), VB * Scale, Unit) : TEXT("-"),
				bA && bB ? *FString::Printf(TEXT("%+.4g %s"), (VB - VA) * Scale, Unit) : TEXT("-"));
		};
		const TSharedPtr<FJsonObject> SA = A->GetObjectField(TEXT("summary")), SB = B->GetObjectField(TEXT("summary"));
		const TSharedPtr<FJsonObject> PA = A->GetObjectField(TEXT("power")), PB = B->GetObjectField(TEXT("power"));
		Row(TEXT("흡기 창 평균 최고"), SA, SB, TEXT("inletWindowMeanMaxC"), 1.0, TEXT("C"));
		Row(TEXT("흡기 창 평균의 평균"), SA, SB, TEXT("inletWindowMeanAvgC"), 1.0, TEXT("C"));
		Row(TEXT("권장 27 C 초과 흡기"), SA, SB, TEXT("inletsAboveRecommended"), 1.0, TEXT(""));
		Row(TEXT("허용 32 C 초과 흡기"), SA, SB, TEXT("inletsAboveAllowable"), 1.0, TEXT(""));
		Row(TEXT("센서 경보 흡기"), SA, SB, TEXT("inletsWithSensorAlarm"), 1.0, TEXT(""));
		Row(TEXT("IT"), PA, PB, TEXT("itW"), 1e-3, TEXT("kW"));
		Row(TEXT("CRAH 팬"), PA, PB, TEXT("crahFanW"), 1e-3, TEXT("kW"));
		Row(TEXT("펌프"), PA, PB, TEXT("pumpW"), 1e-3, TEXT("kW"));
		Row(TEXT("부분 PUE"), PA, PB, TEXT("partialPue"), 1.0, TEXT(""));
		// S8 F5/F6 (optional: fixed-boundary reports lack them): plant power, cooling PUE and the topology / design point,
		// so a topology comparison shows whether both sides were sized by the same rule (D8).
		Row(TEXT("설비 전력(펌프·팬·압축기)"), PA, PB, TEXT("plantW"), 1e-3, TEXT("kW"));
		Row(TEXT("냉각 PUE"), PA, PB, TEXT("coolingPue"), 1.0, TEXT(""));
		const TSharedPtr<FJsonObject>* PlantA = nullptr; const TSharedPtr<FJsonObject>* PlantB = nullptr;
		const bool bPlantA = A->TryGetObjectField(TEXT("plant"), PlantA), bPlantB = B->TryGetObjectField(TEXT("plant"), PlantB);
		if (bPlantA || bPlantB)
		{
			const TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>& XA = bPlantA ? *PlantA : Empty;
			const TSharedPtr<FJsonObject>& XB = bPlantB ? *PlantB : Empty;
			FString TA = TEXT("-"), TB = TEXT("-");
			XA->TryGetStringField(TEXT("topology"), TA); XB->TryGetStringField(TEXT("topology"), TB);
			Md += FString::Printf(TEXT("| 설비 구성 | %s | %s | %s |\n"), *TA, *TB, TA == TB ? TEXT("같음") : TEXT("다름"));
			Row(TEXT("설계 외기"), XA, XB, TEXT("designAmbientK"), 1.0, TEXT("K"));
			Row(TEXT("외기(끝)"), XA, XB, TEXT("ambientK"), 1.0, TEXT("K"));
			Row(TEXT("설계 CDU 열"), XA, XB, TEXT("designCduHeatW"), 1e-3, TEXT("kW"));
			Row(TEXT("설계 냉수 부하(CRAH)"), XA, XB, TEXT("designChwLoadW"), 1e-3, TEXT("kW"));
			Row(TEXT("칠러 기준 용량(대당)"), XA, XB, TEXT("chillerRefCapacityW"), 1e-3, TEXT("kW"));
			Row(TEXT("트림 칠러 기준 용량"), XA, XB, TEXT("trimRefCapacityW"), 1e-3, TEXT("kW"));
			Row(TEXT("이코노마이저 운전"), XA, XB, TEXT("economizerOnS"), 1.0, TEXT("s"));
			Row(TEXT("압축기 누적 일"), XA, XB, TEXT("compressorJ"), 1e-6, TEXT("MJ"));
			Row(TEXT("CDU 2차측 공급(끝, 평균)"), XA, XB, TEXT("cduSecondarySupplyK"), 1.0, TEXT("K"));
			Row(TEXT("부품 최고 온도(끝)"), XA, XB, TEXT("maxComponentK"), 1.0, TEXT("K"));
			Row(TEXT("FWS 공급 설정 미달"), XA, XB, TEXT("fwsSupplyNotMetS"), 1.0, TEXT("s"));
			Row(TEXT("칠러 용량 부족"), XA, XB, TEXT("chillerLimitedS"), 1.0, TEXT("s"));
		}
		double ItA = 0.0, ItB = 0.0;
		PA->TryGetNumberField(TEXT("itW"), ItA); PB->TryGetNumberField(TEXT("itW"), ItB);
		if (FMath::Abs(ItA - ItB) > 1e-3 * FMath::Max(FMath::Abs(ItA), 1.0))
			Md += FString::Printf(TEXT("\n**IT 부하가 다르다(%.1f 대 %.1f kW).** D8: 전력·PUE는 같은 IT 부하에서만 비교한다.\n"), ItA / 1e3, ItB / 1e3);
		const TArray<TSharedPtr<FJsonValue>>& IA = A->GetArrayField(TEXT("inlets"));
		const TArray<TSharedPtr<FJsonValue>>& IB = B->GetArrayField(TEXT("inlets"));
		Md += TEXT("\n| 흡기 # | A 창 평균 C | B 창 평균 C | B - A |\n|---|---|---|---|\n");
		double MaxAbs = 0.0;
		for (int32 I = 0; I < FMath::Min(IA.Num(), IB.Num()); ++I)
		{
			const double VA = IA[I]->AsObject()->GetNumberField(TEXT("windowMeanC")), VB = IB[I]->AsObject()->GetNumberField(TEXT("windowMeanC"));
			MaxAbs = FMath::Max(MaxAbs, FMath::Abs(VB - VA));
			Md += FString::Printf(TEXT("| %d | %.2f | %.2f | %+.2f |\n"), I, VA, VB, VB - VA);
		}
		if (IA.Num() != IB.Num()) Md += FString::Printf(TEXT("\n흡기 수가 다르다(%d 대 %d) - 앞쪽만 짝지었다.\n"), IA.Num(), IB.Num());
		const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), FString::Printf(TEXT("AB_%s_vs_%s.md"), *NameA, *NameB));
		FFileHelper::SaveStringToFile(Md, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		OutSummary = FString::Printf(TEXT("%s | 흡기별 창 평균 최대 차 %.2f K"), *Path, MaxAbs);
		return true;
	}
}

static FAutoConsoleCommand GVrgWebProfileLayoutCommand(
	TEXT("vrg.Web.ProfileLayout"),
	TEXT("웹 대시보드 평면도 비용 진단: 전체 액터 순회와 클래스별 순회, 평면도 생성·JSON, 랙 탭 생성 시간을 로그에 쓴다. vrg.Web.ProfileLayout [반복 수=50] [wait = 예약된 예열이 끝난 뒤]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const int32 Repeats = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 50;
		const bool bWait = Args.Num() > 1 && Args[1] == TEXT("wait");   // after the queued warm-up (rack rows exist then)
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			if (bWait) Sim->PendingProfileRepeats = FMath::Max(1, Repeats);
			else UE_LOG(LogTemp, Display, TEXT("%s"), *Sim->ProfileDashboardLayout(Repeats));
		}
	}));

static FAutoConsoleCommand GVrgEvalReportCommand(
	TEXT("vrg.Eval.Report"),
	TEXT("평가 리포트를 Saved/Reports/<이름>.json/.md로 쓴다. vrg.Eval.Report <이름>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			FString Summary;
			const bool bOk = Sim->WriteEvaluationReport(Args.Num() > 0 ? Args[0] : FString(TEXT("report")), Summary);
			UE_LOG(LogTemp, Display, TEXT("[VrgEval] 리포트 %s: %s"), bOk ? TEXT("성공") : TEXT("실패"), *Summary);
		}
	}));

static FAutoConsoleCommand GVrgEvalEventCommand(
	TEXT("vrg.Eval.Event"),
	TEXT("시각 이벤트를 넣는다(시뮬레이션 진입마다 다시 적용). vrg.Eval.Event <t s> util <0~1> | <t s> rackutil <랙 번호> <0~1> | <t s> valve <랙 번호> close|open | <t s> leak <랙 번호> <supply|return> <mm> | <t s> ambient <외기 C> | <t s> plant <vrg.Plant.Fault 인자> | <t s> leakat <x m> <y m> <z m> <mm>(실시간 누수, 가장 가까운 지점, 0 = 수리) | clear"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			if (Args.Num() >= 1 && Args[0] == TEXT("clear")) { Sim->EvalEvents.Reset(); UE_LOG(LogTemp, Display, TEXT("[VrgEval] 이벤트 모두 지움")); continue; }
			if (Args.Num() < 3) { UE_LOG(LogTemp, Warning, TEXT("[VrgEval] 사용법: vrg.Eval.Event <t> util <v> | <t> rackutil <i> <v> | clear")); continue; }
			ADataCenterSimulationActor::FEvalEvent E;
			E.TimeS = FCString::Atod(*Args[0]);
			E.Kind = Args[1].ToLower();
			if (E.Kind == TEXT("rackutil") && Args.Num() >= 4) { E.Index = FCString::Atoi(*Args[2]); E.Value = FCString::Atod(*Args[3]); }
			else if (E.Kind == TEXT("leak") && Args.Num() >= 5) { E.Index = FCString::Atoi(*Args[2]); E.bFlag = Args[3].ToLower() == TEXT("return"); E.Value = FCString::Atod(*Args[4]) * 1e-3; }
			else if (E.Kind == TEXT("valve") && Args.Num() >= 4) { E.Index = FCString::Atoi(*Args[2]); E.Value = Args[3].ToLower() == TEXT("close") ? 1.0 : 0.0; }
			else if (E.Kind == TEXT("util")) { E.Value = FCString::Atod(*Args[2]); }
			else if (E.Kind == TEXT("ambient")) { E.Value = FCString::Atod(*Args[2]) + 273.15; }   // S8 F2: outdoor dry bulb [C] -> K
			else if (E.Kind == TEXT("plant")) { for (int32 I = 2; I < Args.Num(); ++I) E.Args.Add(Args[I]); }   // S8 F6b: vrg.Plant.Fault arguments
			else if (E.Kind == TEXT("leakat") && Args.Num() >= 6) { E.Args = { Args[2], Args[3], Args[4] }; E.Value = FCString::Atod(*Args[5]) * 1e-3; }   // W1: x y z [m], hole [mm]
			else if (E.Kind == TEXT("leakat") && Args.Num() == 4) { E.Args = { Args[2] }; E.Value = FCString::Atod(*Args[3]) * 1e-3; } // W3.5: exact site key/quoted label, hole mm
			else { UE_LOG(LogTemp, Warning, TEXT("[VrgEval] 모르는 이벤트: %s"), *E.Kind); continue; }
			Sim->EvalEvents.Add(E);
			Sim->EvalEvents.StableSort([](const ADataCenterSimulationActor::FEvalEvent& L, const ADataCenterSimulationActor::FEvalEvent& R) { return L.TimeS < R.TimeS; });
			UE_LOG(LogTemp, Display, TEXT("[VrgEval] 이벤트 추가 t=%.2f s %s %d %.3f (총 %d개)"), E.TimeS, *E.Kind, E.Index, E.Value, Sim->EvalEvents.Num());
		}
	}));

static FAutoConsoleCommand GVrgLeakSitesCommand(
	TEXT("vrg.Leak.Sites"),
	TEXT("실시간 누수 지점. 기본=외부만. vrg.Leak.Sites rack <0부터 랙 번호> = 해당 랙 내부 지점만. At/Repair/leakat은 출력된 key나 따옴표로 감싼 label도 받는다."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			Sim->RebuildLiveLeakSites();
			const auto& Sites = Sim->GetLiveLeakSites();
			const bool bRack = Args.Num() == 2 && Args[0].Equals(TEXT("rack"), ESearchCase::IgnoreCase) && Args[1].IsNumeric();
			if (!Args.IsEmpty() && !bRack) { UE_LOG(LogTemp, Warning, TEXT("[Leak] vrg.Leak.Sites [rack <번호>]")); continue; }
			int32 PlantN = 0;
			for (const auto& S : Sites) PlantN += S.bPlant ? 1 : 0;
			UE_LOG(LogTemp, Display, TEXT("[Leak] 실시간 누수 지점 %d개 (방 %d, 설비 %d)"), Sites.Num(), Sites.Num() - PlantN, PlantN);
			for (int32 I = 0; I < Sites.Num(); ++I)
				if (bRack ? Sites[I].InternalSite != INDEX_NONE && Sites[I].RackIndex == FCString::Atoi(*Args[1]) : Sites[I].InternalSite == INDEX_NONE)
					UE_LOG(LogTemp, Display, TEXT("[Leak]   지점 %d %s [%s] (%.2f, %.2f, %.2f) m, 구멍 %.2f mm"), I, *Sites[I].Label, *Sites[I].Key,
						Sites[I].PositionM.X, Sites[I].PositionM.Y, Sites[I].PositionM.Z, Sim->GetLiveLeakHoleDiameterM(I) * 1e3);
		}
	}));

static FAutoConsoleCommand GVrgLeakAtCommand(
	TEXT("vrg.Leak.At"),
	TEXT("시뮬레이션 중 구멍 변경(0=수리). vrg.Leak.At <x m> <y m> <z m> <mm> = W1 외부 지점; 또는 <지점key|번호|따옴표label> <mm> = 내부 포함."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() != 2 && Args.Num() != 4) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 사용법: vrg.Leak.At <key> <mm> 또는 <x m> <y m> <z m> <mm>")); return; }
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			const int32 Site = Args.Num() == 2 ? Sim->ResolveLiveLeakSite(Args[0]) : Sim->FindLiveLeakSite(FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2])));
			if (Site == INDEX_NONE) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 지점 이름/좌표 없음(vrg.Leak.Sites [rack <번호>])")); continue; }
			Sim->SetLiveLeak(Site, FCString::Atod(*Args.Last()) * 1e-3, TEXT("콘솔"));
		}
	}));

static FAutoConsoleCommand GVrgLeakRepairCommand(
	TEXT("vrg.Leak.Repair"),
	TEXT("실시간 누수 수리(구멍 0). vrg.Leak.Repair <지점 key|번호|따옴표label|all>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			const bool bAll = Args.Num() == 0 || Args[0].ToLower() == TEXT("all");
			for (int32 I = 0; I < Sim->GetLiveLeakSites().Num(); ++I)
				if ((bAll && Sim->GetLiveLeakHoleDiameterM(I) > 0.0) || (!bAll && I == Sim->ResolveLiveLeakSite(Args[0]))) Sim->SetLiveLeak(I, 0.0, TEXT("수리"));
		}
	}));

static FAutoConsoleCommand GVrgLeakDirectionCommand(
	TEXT("vrg.Leak.Direction"),
	TEXT("W2: 누수 지점의 물줄기 방향(단위 벡터로 맞춤, 0 0 0 = 기본 아래쪽 원뿔). vrg.Leak.Direction <지점 번호> <x> <y> <z>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 4) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 사용법: vrg.Leak.Direction <지점 번호> <x> <y> <z>")); return; }
		const FVector Dir(FCString::Atod(*Args[1]), FCString::Atod(*Args[2]), FCString::Atod(*Args[3]));
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			const int32 Site = FCString::Atoi(*Args[0]);
			if (!Sim->SetLiveLeakDirection(Site, Dir)) { UE_LOG(LogTemp, Warning, TEXT("[Leak] 지점 %d 없음(vrg.Leak.Sites)"), Site); continue; }
			const FVector D = Sim->GetLiveLeakSites()[Site].SprayDirectionM;
			UE_LOG(LogTemp, Display, TEXT("[Leak] 지점 %d %s 물줄기 방향 (%.3f, %.3f, %.3f)"), Site, *Sim->GetLiveLeakSites()[Site].Label, D.X, D.Y, D.Z);
		}
	}));

static FAutoConsoleCommand GVrgWaterStatusCommand(
	TEXT("vrg.Water.Status"),
	TEXT("W2: 새어 나온 물의 장부(공중·구멍 대기·물막·바닥·바깥 땅)와 입자 수, 젖은 장비를 로그에 쓴다."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors())
		{
			Sim->LogSprayStatus();
			const FLeakSpray& S = Sim->GetLeakSpray();
			for (int32 I = 0; I < S.GetWetting().Num(); ++I)
				if (S.GetWetting()[I].FirstWetS >= 0.0)
					UE_LOG(LogTemp, Display, TEXT("[Water]   젖음 %s: 처음 t=%.2f s, 가장 높은 곳 %.2f m, 닿은 물 %.3f L"), *S.GetGeometry().Obstacles[I].Name,
						S.GetWetting()[I].FirstWetS, S.GetWetting()[I].TopWetZM, S.GetWetting()[I].WaterM3 * 1e3);
			for (const FSprayFilm& F : S.GetFilms())
				UE_LOG(LogTemp, Display, TEXT("[Water]   물막 %s 면 %d: 폭 %.2f m, 젖은 높이 %.2f m, 두께 %.3f mm, 흘러내림 %.4f L/s"),
					F.Obstacle >= 0 ? *S.GetGeometry().Obstacles[F.Obstacle].Name : F.Obstacle == INDEX_NONE ? TEXT("방 벽") : TEXT("방 외벽"),
					F.Face, F.WidthM, F.WetHeightM, F.LastThicknessM * 1e3, F.LastOutflowM3s * 1e3);
			for (const FFloorFlood& Fl : Sim->GetFloorWater().GetFloods())
				if (Fl.FirstWetS >= 0.0)
					UE_LOG(LogTemp, Display, TEXT("[Water]   물 밑 %s: 처음 t=%.2f s, 지금 평균 %.2f mm·최대 %.2f mm, 최고 %.2f mm (발판 칸 %d)"), *Fl.Name,
						Fl.FirstWetS, Fl.MeanDepthM * 1e3, Fl.MaxDepthM * 1e3, Fl.PeakDepthM * 1e3, Fl.Cells.Num());
		}
	}));

static FAutoConsoleCommand GVrgWaterCellCommand(
	TEXT("vrg.Water.CellM"),
	TEXT("W3: 바닥 물 격자 칸 크기 [m] (기본 0.025, 다음 시뮬레이션 진입부터). vrg.Water.CellM <m>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1) return;
		const double Cell = FCString::Atod(*Args[0]);
		if (!(Cell >= .005 && Cell <= .5)) { UE_LOG(LogTemp, Warning, TEXT("[Water] 칸 크기는 0.005~0.5 m")); return; }
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->FloorWaterSettings.CellM = Cell;
		UE_LOG(LogTemp, Display, TEXT("[Water] 바닥 물 칸 %.3f m (다음 진입부터)"), Cell);
	}));

static FAutoConsoleCommand GVrgAirViewCommand(
	TEXT("vrg.Air.View"),
	TEXT("공기 표시(V로 순환): 0 온도 볼륨, 1 유선·속도, 2 유선·온도, 3 유선(속도)+볼륨, 4 평면 보기, 5 화살표. 유선·평면·화살표는 최근 30 s 평균 흐름. vrg.Air.View <0~5>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1) return;
		const int32 V = FMath::Clamp(FCString::Atoi(*Args[0]), 0, int32(EAirView::Count) - 1);
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->SetAirView(static_cast<EAirView>(V));
	}));

static FAutoConsoleCommand GVrgAirPlanHeightCommand(
	TEXT("vrg.Air.PlanHeight"),
	TEXT("평면 보기 단면 높이 [m] (바닥에서). vrg.Air.PlanHeight <m>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1) return;
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) { Sim->PlanHeightM = FMath::Max(0.0, FCString::Atod(*Args[0])); Sim->RebuildAirFlowView(); }
	}));

static FAutoConsoleCommand GVrgWaterGpuCommand(
	TEXT("vrg.Water.Gpu"),
	TEXT("W3: 바닥 물 계산 경로. 0 = CPU(기본), 1 = GPU(있으면), -1 = 자동(젖은 범위가 크면 GPU). 다음 시뮬레이션 진입부터. vrg.Water.Gpu <-1|0|1>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1) return;
		const int32 Mode = FMath::Clamp(FCString::Atoi(*Args[0]), -1, 1);
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->FloorWaterSettings.GpuMode = Mode;
		UE_LOG(LogTemp, Display, TEXT("[Water] 바닥 물 경로 %s (다음 진입부터, GPU %s)"), Mode == 0 ? TEXT("CPU") : Mode == 1 ? TEXT("GPU") : TEXT("자동"),
			FVrgFloorWaterGpu::IsAvailable() ? TEXT("있음") : TEXT("없음"));
	}));

static FAutoConsoleCommand GVrgWaterShowCommand(
	TEXT("vrg.Water.Show"),
	TEXT("W2: 계산한 물 입자·물막·바닥 물 디버그 표시. vrg.Water.Show <0|1>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->bShowSpray = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;
	}));

static FAutoConsoleCommand GVrgWaterDemoCommand(
	TEXT("vrg.Water.Demo"),
	TEXT("W2 화면 확인: (배치 모드면 시뮬레이션 진입) t=1 s에 방 가운데 랙의 공급 분기를 열고 물줄기를 통로 건너편으로 향하게 한 뒤 시점을 옆에 둔다. vrg.Water.Demo [mm, 기본 3]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		const double Mm = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 3.0;
		if (!(Mm > 0.0)) { UE_LOG(LogTemp, Warning, TEXT("[Water] 사용법: vrg.Water.Demo [mm > 0]")); return; }
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->RequestWaterDemo(Mm * 1e-3);
	}));

static FAutoConsoleCommand GVrgEvalReportWhenDoneCommand(
	TEXT("vrg.Eval.ReportWhenDone"),
	TEXT("예약된 스텝(vrg.Air.RunSteps)이 끝나면 리포트를 쓴다. vrg.Eval.ReportWhenDone <이름>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) Sim->PendingReportName = Args.Num() > 0 ? Args[0] : FString(TEXT("report"));
	}));

static FAutoConsoleCommand GVrgEvalSeedCommand(
	TEXT("vrg.Eval.Seed"),
	TEXT("센서 잡음 시드(다음 시뮬레이션 진입부터). vrg.Eval.Seed <n>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		for (ADataCenterSimulationActor* Sim : EvalSimActors()) if (Args.Num() > 0) Sim->EvalSettings.Seed = FCString::Atoi(*Args[0]);
	}));

static FAutoConsoleCommand GVrgEvalCompareCommand(
	TEXT("vrg.Eval.Compare"),
	TEXT("두 리포트를 비교해 Saved/Reports/AB_<A>_vs_<B>.md로 쓴다. vrg.Eval.Compare <A> <B>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2) return;
		FString Summary;
		const bool bOk = CompareReports(Args[0], Args[1], Summary);
		UE_LOG(LogTemp, Display, TEXT("[VrgEval] 비교 %s: %s"), bOk ? TEXT("성공") : TEXT("실패"), *Summary);
	}));


bool ADataCenterSimulationActor::CaptureAutomaticCrahLayout(TArray<FCrahLayoutRecord>& Out)
{
 Out.Reset();
 if(!GetWorld() || CrahLayout==ECrahLayout::CeilingCoil) return false;
 for(TActorIterator<ACrahUnitActor> It(GetWorld());It;++It) return true; // Already placed: no conversion.
 AAirFieldDebugActor* Field=nullptr; ADataCenterRoomActor* CurrentRoom=nullptr;
 for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Field=*It; break; }
 for(TActorIterator<ADataCenterRoomActor> It(GetWorld());It;++It) { CurrentRoom=*It; break; }
 if(!Field) return false;
 // The planner is shared with simulation/editor conversion. Restore every planning cache on return.
 TGuardValue<TArray<TObjectPtr<ARackThermalActor>>> RackGuard(Racks,{});
 TGuardValue<TArray<TObjectPtr<ASpineCabinetActor>>> SpineGuard(Spines,{});
 TGuardValue<TArray<FCabinetRow>> RowGuard(CabinetRows,{});
 TGuardValue<TArray<FColdAisle>> AisleGuard(ColdAisles,{});
 TGuardValue<TArray<FCrahPlacement>> PlacementGuard(CrahPlacements,{});
 TGuardValue<int32> UnpairedGuard(NumUnpairedRows,0), DroppedGuard(CrahUnitsDropped,0);
 if(CurrentRoom) for(auto* R:CurrentRoom->GetRoutedRacks()) Racks.Add(R);
 else for(TActorIterator<ARackThermalActor> It(GetWorld());It;++It) Racks.Add(*It);
 for(TActorIterator<ASpineCabinetActor> It(GetWorld());It;++It) Spines.Add(*It);
 GroupCabinetRows(); PlanAutoCrahUnits(); FilterCrahPlacementsToGrid(Field);
 for(const auto& P:CrahPlacements) {
  auto Settings=GetDefault<ACrahUnitActor>()->CaptureLayoutSettings();
  Settings.Kind=P.bTopIntake?ECrahUnitKind::Perimeter:ECrahUnitKind::InRow; Settings.SizeM=P.SizeM; Settings.VolumeFlowM3s=P.FlowM3s;
  Out.Add({FGuid::NewGuid().ToString(),NAME_None,FTransform(P.ForwardM.Rotation(),SimUnits::MetersToUnreal(P.BaseM)),Settings,true,true});
 }
 return true;
}

void ADataCenterSimulationActor::RefreshCrahLayoutDisplay()
{
 if(Mode!=ESimMode::Layout) return;
 // Geometry only after a successful edit: remove virtual mesh duplicates, or show automatic boxes after Undo.
 FindTargetActors(); GroupCabinetRows(); PlanCrahUnits(); UpdateCrahMeshes();
}
