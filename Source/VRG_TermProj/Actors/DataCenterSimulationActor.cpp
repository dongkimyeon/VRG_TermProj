// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/DataCenterSimulationActor.h"
#include "Simulation/Core/VrgPerfCounters.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Visualization/GjRackLayout.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Thermal/RackThermalModel.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"

ADataCenterSimulationActor::ADataCenterSimulationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// Referenced from the CDO so the cooker packages it (a runtime LoadObject would not be found in a build).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	CrahMesh = Cube.Object;
}

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
	PendingWarmupSteps = 0;
	VrgPerf::PendingWarmupSteps() = 0;
	HoldModelsForLayout();
	UE_LOG(LogTemp, Display, TEXT("[VrgSim] 배치 모드: 물리 정지. 배치를 바꾼 뒤 시뮬레이션 모드로 들어가면 새로 구성한다."));
}

void ADataCenterSimulationActor::EnterSimulationMode()
{
	bDisplaySnapshotValid = false;
	Mode = ESimMode::Simulation;
	FindTargetActors();
	// Rows and hydraulics first: the coupling reads the routed racks and branch pipes.
	if (Room != nullptr) Room->RebuildRoomNow();
	FindTargetActors();
	if (AirField != nullptr)
	{
		AirField->SetLayoutMode(false);
		if (AirField->bAutoFitToRacks) AirField->FitToRacks();
		AirField->ResetSim();
		AirField->StartSim();
	}
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr) Rack->ResetSim();
	}
	BuildCoupling();
	bNeedsRebuild = (Racks.Num() == 0);
	bSimRunning = true;
	UE_LOG(LogTemp, Display, TEXT("[VrgSim] 시뮬레이션 모드: 랙 %d대 / 스파인 %d대 / CRAH %d대로 구성하고 t=0부터 시작."),
		Racks.Num(), Spines.Num(), CrahPlacements.Num());
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
	if (AirField == nullptr || !bMarkRacksAsSolid)
	{
		return;
	}

	FAirSolver& Solver = AirField->GetMutableSolver();
	FUniformAirGrid& Grid = Solver.GetMutableGrid();
	Grid.ClearSolidCells();
	SolidOwner.Init(INDEX_NONE, Grid.GetNumCells());

	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());

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

	Solver.OnSolidCellsChanged();
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
		Bridge.Initialize({}, {});
		return;
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

	LastSetupMessage = Missing.Num() > 0
		? FString::Printf(TEXT("안 붙은 도메인: %s"), *FString::Join(Missing, TEXT(", ")))
		: FString();
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
	CduSummaries.Reset();
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
			CduSummaries[G].NumRacks += 1;
			CduSummaries[G].DesignDutyW += FRackThermalModel::DesignHeatToCoolantW(Rack->Params, Rack->Input);
			HotWK[G] += Rack->Input.CoolantMassFlowKgs * Rack->Params.CoolantCpJKgK;
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
	}
	Bridge.SetCduGroups(Groups);
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
	// Units must sit inside the air grid, otherwise their faces are missing and they would carry no air.
	const FUniformAirGrid& Grid = AirField->GetMutableSolver().GetGrid();
	const FVector GridMinM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
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
	CrahUnitFlowM3s = CrahUnitUAWK = CrahUnitFanW = 0.0;
	bCrahFellBackToCoil = false;
	if (CrahLayout == ECrahLayout::CeilingCoil || AirField == nullptr)
	{
		return;
	}
	const FUniformAirGrid& Grid = AirField->GetMutableSolver().GetGrid();
	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());
	const int32 NumCabinets = Racks.Num() + Spines.Num();
	// The octree air path rejects ducts, so CRAH units there would leave the room with no cooling at all.
	if (SolidOwner.Num() != Grid.GetNumCells() || AirField->GetMutableSolver().IsUsingOctree())
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
		if (Unit.HasBothSides()) CrahUnits.Add(MoveTemp(Unit));
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
		Unit->RerunConstructionScripts();
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
	// The engine cube is 100 cm on a side; scale 1 = 1 m. Every placed unit is drawn: one without faces carries
	// no air but still blocks it as a solid, and the summary reports it (what is shown is what is computed).
	for (const FCrahPlacement& P : CrahPlacements)
	{
		if (P.bPlaced) continue;  // an ACrahUnitActor draws itself
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this);
		Mesh->SetStaticMesh(CrahMesh);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetupAttachment(RootComponent);
		Mesh->RegisterComponent();
		Mesh->SetWorldLocationAndRotation(SimUnits::MetersToUnreal(P.BaseM + FVector(0.0, 0.0, .5 * P.SizeM.Z)), P.ForwardM.Rotation());
		Mesh->SetWorldScale3D(P.SizeM);
		CrahMeshes.Add(Mesh);
	}
}

void ADataCenterSimulationActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (Mode == ESimMode::Layout)
	{
		// Racks placed or spawned while in layout must not start integrating on their own.
		HoldModelsForLayout();
		if (bShowSummary && GEngine != nullptr)
		{
			GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.0f, FColor(120, 220, 255),
				TEXT("[배치 모드] 물리 정지 | Enter: 시뮬레이션 시작 | G: 바라보는 장비 잡기/놓기 | T: 90도 회전"));
		}
		return;
	}

	if (bNeedsRebuild)
	{
		BuildCoupling();

		// 방 액터는 RebuildRoom()이 플래그만 세우고 실제 배치 파악은 자기 Tick에서 한다.
		// 이 액터가 먼저 돌면 GetRoutedRacks()가 비어 있어 랙 0대로 나온다.
		// 한 번 실패하고 포기하면 영원히 연결되지 않으므로 될 때까지 다시 시도한다.
		// 실제로 겪었다 - 통합 액터를 배치했는데 화면에 "랙 0대 / IT 발열 0.0 kW"가 뜨고
		// 공기 액터가 자기 더미 열원으로 계속 돌았다 (시계도 둘로 갈렸다).
		bNeedsRebuild = (Racks.Num() == 0);
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
		const bool bHydraulicsConverged =
			(Room == nullptr) || Room->GetLastSolveResult().bConverged;

		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			{
				// S7-0 계측. 통합 스텝 안에 랙·수력·공기가 모두 들어 있다.
				VRG_SCOPE_COST(STAT_VrgCouplingStep, CouplingMs);
				Bridge.Step(Clock.GetFixedStepS(), bHydraulicsResolved && StepIndex == 0, bHydraulicsConverged);
			}
			Clock.CompleteStep();
			VrgPerf::Accumulator().SolverSteps += 1;
			VrgPerf::Accumulator().SolverMs += Bridge.GetLastAirSolverMs();
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
}

void ADataCenterSimulationActor::DrawSummary() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());
	if (!bDisplaySnapshotValid || Clock.GetSimTimeS() != DisplaySnapshotSimTimeS)
	{
		DisplaySnapshot = GetSnapshot();
		DisplaySnapshotSimTimeS = Clock.GetSimTimeS();
		bDisplaySnapshotValid = true;
	}
	const FCouplingSnapshot& Snapshot = DisplaySnapshot;

	if (!LastSetupMessage.IsEmpty())
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + 4, 0.0f, FColor::Orange,
			FString::Printf(TEXT("[%s] %s"), *GetName(), *LastSetupMessage));
	}

	const bool bAllConverged = Snapshot.bHydraulicsConverged && Snapshot.bAirPressureConverged;

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		bAllConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[통합] t=%.1f s | 랙/스파인 %d대 | 수력 %s | 공기 압력 %s"),
			Snapshot.SimTimeS, Snapshot.RackHeatToAirW.Num(),
			Snapshot.bHydraulicsConverged ? TEXT("수렴") : TEXT("수렴 실패"),
			Snapshot.bAirPressureConverged ? TEXT("수렴") : TEXT("수렴 실패 - 속도장 믿지 말 것")));

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("IT 발열 %.1f kW + CRAH 팬 %.1f kW = 냉각수 방출(%s) %.1f kW + 공기 코일 %.2f kW + 축적"),
			Snapshot.TotalItHeatW / 1000.0,
			Snapshot.CrahFanPowerW / 1000.0,
			CduSummaries.Num() > 0 ? TEXT("CDU") : TEXT("랙 라디에이터"),
			Snapshot.TotalRadiatorRejectedW / 1000.0,
			Snapshot.TotalAirCoolingRemovedW / 1000.0));

	GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f,
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
		GEngine->AddOnScreenDebugMessage(BaseKey + 5, 0.0f, bDuctOk ? FColor(180, 255, 200) : FColor::Orange,
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
		GEngine->AddOnScreenDebugMessage(BaseKey + 7, 0.0f, FColor(160, 210, 255), Line);
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + 7, 0.0f, FColor(160, 210, 255),
			TEXT("냉각수 열 방출: 랙별 라디에이터 (CDU 용량 제한 없음)"));
	}

	// Air cooling: which device carries it. The CRAH figures are design sizing (assumptions), not a product.
	GEngine->AddOnScreenDebugMessage(BaseKey + 6, 0.0f, bCrahFellBackToCoil ? FColor::Orange : FColor(180, 220, 255),
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
		GEngine->AddOnScreenDebugMessage(BaseKey + 3, 0.0f, FColor(200, 220, 255),
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

	const bool bHydraulicsConverged =
		(Room == nullptr) || Room->GetLastSolveResult().bConverged;

	while (PendingWarmupSteps > 0)
	{
		// 시계에도 같은 시간을 넣는다. CompleteStep만 부르면 누적기가 음수로 내려가
		// 이후 실시간 프레임이 한참 동안 스텝을 못 만든다 (규약 4).
		const bool bHydraulicsResolved = (Room != nullptr) && Room->ConsumeRebuiltFlag();
		Clock.BeginFrame(Clock.GetFixedStepS());
		const uint64 StepStart = FPlatformTime::Cycles64();
		Bridge.Step(Clock.GetFixedStepS(), bHydraulicsResolved, bHydraulicsConverged);
		WarmupCouplingMs += FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StepStart);
		WarmupRackMs += Bridge.GetLastRackModelsMs();
		Clock.CompleteStep();
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
