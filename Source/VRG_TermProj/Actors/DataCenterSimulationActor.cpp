// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/DataCenterSimulationActor.h"
#include "Simulation/Core/VrgPerfCounters.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/SimUnits.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

ADataCenterSimulationActor::ADataCenterSimulationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ADataCenterSimulationActor::BeginPlay()
{
	Super::BeginPlay();
	RebuildCoupling();
	StartSim();
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
	if (AirField == nullptr || !bMarkRacksAsSolid)
	{
		return;
	}

	FAirSolver& Solver = AirField->GetMutableSolver();
	FUniformAirGrid& Grid = Solver.GetMutableGrid();
	Grid.ClearSolidCells();

	const FVector GridOriginM = SimUnits::UnrealToMeters(AirField->GetActorLocation());

	for (const ARackThermalActor* Rack : Racks)
	{
		if (Rack == nullptr)
		{
			continue;
		}

		const FVector RackM = SimUnits::UnrealToMeters(Rack->GetActorLocation());
		const FVector Forward = Rack->GetActorForwardVector().GetSafeNormal2D();
		const FVector Right = Rack->GetActorRightVector().GetSafeNormal2D();

		// 앞뒤 한 겹은 남긴다. 랙은 공기가 앞으로 들어와 뒤로 나가는 통로라
		// 통째로 막으면 흡기·배기 셀이 격자에서 끊긴다.
		const double SolidDepthM = FMath::Max(Rack->RackSizeM.X - 2.0 * Grid.GetCellSizeM(), 0.0);
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
						&& AlongWidthM <= Rack->RackSizeM.Y * 0.5
						&& OffsetM.Z >= 0.0 && OffsetM.Z <= Rack->RackSizeM.Z)
					{
						Grid.bSolidCell[Grid.CellIndex(X, Y, Z)] = true;
					}
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

	// 전체 UA를 코일 셀에 고르게 나눈다.
	const double PerCellUAWK = CoilTotalUAWK / CoilCells.Num();
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

	// 고체 표시가 먼저다. 코일 셀을 고를 때 고체를 빼야 한다.
	MarkRackSolidCells();
	BuildCoolingCoil();

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

	for (int32 RackIndex = 0; RackIndex < Racks.Num(); ++RackIndex)
	{
		ARackThermalActor* Rack = Racks[RackIndex];
		if (Rack == nullptr)
		{
			continue;
		}

		FRackCouplingTarget Target;
		Target.Model = &Rack->GetMutableModel();
		Target.Input = &Rack->GetMutableInput();
		Target.Radiator = &Rack->GetRadiatorParams();
		Targets.Add(Target);

		FRackCouplingLink Link;
		Link.BranchPipeIndex = BranchIndices.IsValidIndex(RackIndex) ? BranchIndices[RackIndex] : INDEX_NONE;
		Link.FallbackCoolantMassFlowKgs = Rack->GetMutableInput().CoolantMassFlowKgs;

		if (bHasAir)
		{
			// 랙 앞뒤로 한 덩어리씩 셀을 잡는다.
			// 앞(+X 로컬)이 흡기, 뒤가 배기다.
			const FVector RackM = SimUnits::UnrealToMeters(Rack->GetActorLocation());
			const FVector Forward = Rack->GetActorForwardVector().GetSafeNormal2D();
			const FVector Right = Rack->GetActorRightVector().GetSafeNormal2D();
			const double HalfDepthM = Rack->RackSizeM.X * 0.5;

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
						if (FMath::Abs(FVector::DotProduct(OffsetM, Right)) > Rack->RackSizeM.Y * 0.5
							|| OffsetM.Z < 0.0 || OffsetM.Z > Rack->RackSizeM.Z)
						{
							continue;
						}

						const double AlongM = FVector::DotProduct(OffsetM, Forward);

						// 앞쪽: 랙 앞면 바깥 InletSampleDepthM 안.
						if (AlongM > HalfDepthM && AlongM <= HalfDepthM + InletSampleDepthM)
						{
							Link.InletCellIndices.Add(Grid->CellIndex(X, Y, Z));
						}
						// 뒤쪽: 랙 뒷면 바깥 ExhaustDepthM 안.
						else if (AlongM < -HalfDepthM && AlongM >= -HalfDepthM - ExhaustDepthM)
						{
							Link.ExhaustCellIndices.Add(Grid->CellIndex(X, Y, Z));
						}
					}
				}
			}

			if (Link.ExhaustCellIndices.Num() > 0)
			{
				++NumRacksWithAir;
			}
		}

		Links.Add(Link);
	}

	Bridge.Initialize(Targets, Links);
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

	LastSetupMessage = Missing.Num() > 0
		? FString::Printf(TEXT("안 붙은 도메인: %s"), *FString::Join(Missing, TEXT(", ")))
		: FString();
}

void ADataCenterSimulationActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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
	const FCouplingSnapshot Snapshot = GetSnapshot();

	if (!LastSetupMessage.IsEmpty())
	{
		GEngine->AddOnScreenDebugMessage(BaseKey + 4, 0.0f, FColor::Orange,
			FString::Printf(TEXT("[%s] %s"), *GetName(), *LastSetupMessage));
	}

	const bool bAllConverged = Snapshot.bHydraulicsConverged && Snapshot.bAirPressureConverged;

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f,
		bAllConverged ? FColor::Green : FColor::Red,
		FString::Printf(TEXT("[통합] t=%.1f s | 랙 %d대 | 수력 %s | 공기 압력 %s"),
			Snapshot.SimTimeS, Snapshot.RackHeatToAirW.Num(),
			Snapshot.bHydraulicsConverged ? TEXT("수렴") : TEXT("수렴 실패"),
			Snapshot.bAirPressureConverged ? TEXT("수렴") : TEXT("수렴 실패 - 속도장 믿지 말 것")));

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor::Cyan,
		FString::Printf(TEXT("IT 발열 %.1f kW = 라디에이터 %.1f kW + CRAH %.2f kW + 축적"),
			Snapshot.TotalItHeatW / 1000.0,
			Snapshot.TotalRadiatorRejectedW / 1000.0,
			Snapshot.TotalAirCoolingRemovedW / 1000.0));

	GEngine->AddOnScreenDebugMessage(BaseKey + 2, 0.0f,
		FMath::Abs(Snapshot.NormalizedEnergyResidual) < 1.0e-9 ? FColor::Yellow : FColor::Red,
		FString::Printf(TEXT("전역 열수지: 유입 %.3e J - 유출 %.3e J - 축적 %.3e J | 정규화 잔차 %.2e"),
			Snapshot.EnergyInJ, Snapshot.EnergyOutJ, Snapshot.EnergyStoredJ,
			Snapshot.NormalizedEnergyResidual));

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
	if (bNeedsRebuild)
	{
		BuildCoupling();
		bNeedsRebuild = (Racks.Num() == 0);
	}
	PendingWarmupSteps = FMath::Clamp(NumSteps, 1, 100000);
	WarmupStepsReported = PendingWarmupSteps;
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
		Bridge.Step(Clock.GetFixedStepS(), bHydraulicsResolved, bHydraulicsConverged);
		Clock.CompleteStep();
		--PendingWarmupSteps;

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
		UE_LOG(LogTemp, Display, TEXT("[VrgSim] 예열 %d스텝 완료 (%.2f s)"),
			WarmupStepsReported, WarmupStepsReported * Clock.GetFixedStepS());
	}
}
