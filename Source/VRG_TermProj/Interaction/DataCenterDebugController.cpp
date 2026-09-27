// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/DataCenterDebugController.h"

#include "Actors/DataCenterRoomActor.h"
#include "Actors/HydraulicNetworkDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Thermal/RackThermalModel.h"

#include "DrawDebugHelpers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"

ADataCenterDebugController::ADataCenterDebugController()
{
	PrimaryActorTick.bCanEverTick = true;

	// 여기 값은 레벨에 랙이 하나도 없을 때만 쓰이는 자리 표시다.
	// 랙을 찾으면 InitializeKnobsFromTargets()가 그 랙의 현재 값으로 다시 잡는다.
	// 랙 종류마다 발열 규모가 달라서(15 kW 하이브리드 vs GB200 NVL72) 숫자를
	// 여기 박아 두면 다른 랙을 놓는 순간 범위가 어긋난다.
	CpuHeatKnob.Value = 350.0;
	CpuHeatKnob.MinValue = 0.0;
	CpuHeatKnob.MaxValue = 700.0;
	CpuHeatKnob.StepSize = 25.0;

	GpuHeatKnob.Value = 700.0;
	GpuHeatKnob.MinValue = 0.0;
	GpuHeatKnob.MaxValue = 1400.0;
	GpuHeatKnob.StepSize = 50.0;

	// 회전수비는 무차원이라 랙 종류와 무관하다. 그대로 둔다.
	PumpSpeedKnob.Value = 1.0;
	PumpSpeedKnob.MinValue = 0.0;
	PumpSpeedKnob.MaxValue = 1.5;
	PumpSpeedKnob.StepSize = 0.05;

	// UA는 자릿수를 훑어야 포화가 눈에 보인다. 선형 증분으로는 키를 수백 번 눌러야 한다.
	RadiatorUAKnob.Value = 1.6e3;
	RadiatorUAKnob.MinValue = 1.0e2;
	RadiatorUAKnob.MaxValue = 2.0e4;
	RadiatorUAKnob.StepSize = 1.25;
	RadiatorUAKnob.bMultiplicative = true;

	// Utilization is dimensionless and the same for every rack type.
	UtilizationKnob.Value = 1.0;
	UtilizationKnob.MinValue = 0.0;
	UtilizationKnob.MaxValue = 1.0;
	UtilizationKnob.StepSize = 0.1;
}

void ADataCenterDebugController::BeginPlay()
{
	Super::BeginPlay();

	FindTargetActors();

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (MappingContext != nullptr)
			{
				Subsystem->AddMappingContext(MappingContext, 0);
			}
		}
	}

	if (Racks.Num() > 0)
	{
		InitializeKnobsFromTargets();
		bKnobsInitialized = true;
	}

	// 손잡이 값을 한 번 밀어 넣어 화면 값과 실제 값을 맞춘다.
	// 방 액터가 나중에 띄우는 랙은 RefreshTargetsIfStale()이 챙긴다.
	ApplyHeatKnobs();
	ApplyPumpSpeedKnob();
	ApplyRadiatorKnob();
}

void ADataCenterDebugController::FindTargetActors()
{
	Racks.Reset();
	HydraulicNetwork = nullptr;
	Room = nullptr;

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	for (TActorIterator<ARackThermalActor> It(World); It; ++It)
	{
		Racks.Add(*It);
	}
	for (TActorIterator<AHydraulicNetworkDebugActor> It(World); It; ++It)
	{
		HydraulicNetwork = *It;
		break;
	}
	for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It)
	{
		Room = *It;
		break;
	}
}

void ADataCenterDebugController::RefreshTargetsIfStale(float DeltaTime)
{
	TimeSinceRefreshS += DeltaTime;
	if (TimeSinceRefreshS < TargetRefreshIntervalS)
	{
		return;
	}
	TimeSinceRefreshS = 0.0f;

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// 방 액터는 첫 틱에 랙을 띄우므로 BeginPlay 시점에는 아직 없다.
	// 개수가 달라졌을 때만 다시 훑어 매 틱 순회를 피한다.
	int32 RackCount = 0;
	for (TActorIterator<ARackThermalActor> It(World); It; ++It)
	{
		++RackCount;
	}

	if (RackCount == Racks.Num() && (Racks.Num() > 0 || Room != nullptr || HydraulicNetwork != nullptr))
	{
		return;
	}

	FindTargetActors();

	if (Racks.Num() > 0 && !bKnobsInitialized)
	{
		InitializeKnobsFromTargets();
		bKnobsInitialized = true;
	}

	// 새로 뜬 랙에도 지금 손잡이 값을 밀어 넣는다.
	ApplyHeatKnobs();
	ApplyRadiatorKnob();
	ApplyPumpSpeedKnob();
}

void ADataCenterDebugController::InitializeKnobsFromTargets()
{
	const ARackThermalActor* Rack = Racks.Num() > 0 ? Racks[0].Get() : nullptr;
	if (Rack == nullptr)
	{
		return;
	}

	// 랙이 지금 쓰고 있는 값을 가운데 두고 0 ~ 2배 범위를 잡는다.
	// 증분은 5%라 20번 누르면 0까지 내려간다.
	auto SetupHeatKnob = [](FDebugKnob& Knob, double CurrentW)
	{
		if (CurrentW <= 0.0)
		{
			return;
		}
		Knob.Value = CurrentW;
		Knob.MinValue = 0.0;
		Knob.MaxValue = CurrentW * 2.0;
		Knob.StepSize = CurrentW * 0.05;
		Knob.bMultiplicative = false;
	};

	// Knobs are per chip: the first CPU / GPU node of the rack sets the range, every node of that kind follows.
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	FRackThermalModel::ResolveLayout(Rack->Params, Nodes, Starts);
	const int32 CpuIndex = Nodes.IndexOfByPredicate([](const FRackComponentParams& P) { return P.Kind == ERackPartKind::Cpu; });
	const int32 GpuIndex = Nodes.IndexOfByPredicate([](const FRackComponentParams& P) { return P.Kind == ERackPartKind::Gpu; });
	if (Rack->Input.ComponentHeatW.IsValidIndex(CpuIndex)) SetupHeatKnob(CpuHeatKnob, Rack->Input.ComponentHeatW[CpuIndex]);
	if (Rack->Input.ComponentHeatW.IsValidIndex(GpuIndex)) SetupHeatKnob(GpuHeatKnob, Rack->Input.ComponentHeatW[GpuIndex]);

	// 라디에이터는 현재 UA를 가운데 두고 위아래로 16배씩 훑는다.
	// 곱셈 증분 1.25배이므로 한쪽 끝까지 약 13번이다.
	if (Rack->Radiator.UAWK > 0.0)
	{
		RadiatorUAKnob.Value = Rack->Radiator.UAWK;
		RadiatorUAKnob.MinValue = Rack->Radiator.UAWK / 16.0;
		RadiatorUAKnob.MaxValue = Rack->Radiator.UAWK * 16.0;
		RadiatorUAKnob.StepSize = 1.25;
		RadiatorUAKnob.bMultiplicative = true;
	}

	UE_LOG(LogTemp, Log,
		TEXT("DataCenterDebugController: 랙 %d대에서 손잡이를 잡았습니다. CPU %.0f W, GPU %.0f W, 라디에이터 UA %.0f W/K."),
		Racks.Num(), CpuHeatKnob.Value, GpuHeatKnob.Value, RadiatorUAKnob.Value);
}

UInputAction* ADataCenterDebugController::AddKeyAction(FName ActionName, const FKey& Key)
{
	if (MappingContext == nullptr)
	{
		return nullptr;
	}

	UInputAction* Action = NewObject<UInputAction>(this, ActionName);
	Action->ValueType = EInputActionValueType::Boolean;
	Actions.Add(Action);

	MappingContext->MapKey(Action, Key);
	return Action;
}

void ADataCenterDebugController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	if (EnhancedInput == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterDebugController: EnhancedInputComponent가 아닙니다. DefaultInput.ini의 DefaultInputComponentClass를 확인하세요."));
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("DebugMappingContext"));

	// 전부 누르는 순간 한 번만 반응한다 (Started). 누르고 있으면 계속 도는 것보다
	// 몇 번 눌렀는지가 분명해서 값을 되짚기 쉽다.
	struct FBinding
	{
		const TCHAR* Name;
		FKey Key;
		void (ADataCenterDebugController::*Handler)();
	};

	const FBinding Bindings[] = {
		{ TEXT("CpuHeatDown"),   EKeys::One,        &ADataCenterDebugController::OnCpuHeatDown },
		{ TEXT("CpuHeatUp"),     EKeys::Two,        &ADataCenterDebugController::OnCpuHeatUp },
		{ TEXT("GpuHeatDown"),   EKeys::Three,      &ADataCenterDebugController::OnGpuHeatDown },
		{ TEXT("GpuHeatUp"),     EKeys::Four,       &ADataCenterDebugController::OnGpuHeatUp },
		{ TEXT("PumpSpeedDown"), EKeys::Five,       &ADataCenterDebugController::OnPumpSpeedDown },
		{ TEXT("PumpSpeedUp"),   EKeys::Six,        &ADataCenterDebugController::OnPumpSpeedUp },
		{ TEXT("RadiatorDown"),  EKeys::Seven,      &ADataCenterDebugController::OnRadiatorDown },
		{ TEXT("RadiatorUp"),    EKeys::Eight,      &ADataCenterDebugController::OnRadiatorUp },
		{ TEXT("UtilDown"),      EKeys::Nine,       &ADataCenterDebugController::OnUtilizationDown },
		{ TEXT("UtilUp"),        EKeys::Zero,       &ADataCenterDebugController::OnUtilizationUp },
		{ TEXT("PrevSled"),      EKeys::Left,       &ADataCenterDebugController::OnSelectPreviousSled },
		{ TEXT("NextSled"),      EKeys::Right,      &ADataCenterDebugController::OnSelectNextSled },
		{ TEXT("ToggleSled"),    EKeys::E,          &ADataCenterDebugController::OnToggleSelectedSled },
		{ TEXT("ResetSim"),      EKeys::R,          &ADataCenterDebugController::OnResetSim },
		{ TEXT("ToggleHelp"),    EKeys::H,          &ADataCenterDebugController::OnToggleHelp },
	};

	for (const FBinding& Binding : Bindings)
	{
		if (UInputAction* Action = AddKeyAction(Binding.Name, Binding.Key))
		{
			EnhancedInput->BindAction(Action, ETriggerEvent::Started, this, Binding.Handler);
		}
	}
}

void ADataCenterDebugController::ApplyHeatKnobs()
{
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack == nullptr)
		{
			continue;
		}

		TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
		FRackThermalModel::ResolveLayout(Rack->Params, Nodes, Starts);
		if (Rack->Input.ComponentHeatW.Num() != Nodes.Num())
		{
			continue;
		}

		for (int32 Node = 0; Node < Nodes.Num(); ++Node)
		{
			if (Nodes[Node].Kind == ERackPartKind::Cpu) Rack->Input.ComponentHeatW[Node] = CpuHeatKnob.Value;
			if (Nodes[Node].Kind == ERackPartKind::Gpu) Rack->Input.ComponentHeatW[Node] = GpuHeatKnob.Value;
		}
	}
}

void ADataCenterDebugController::ApplyPumpSpeedKnob()
{
	if (HydraulicNetwork != nullptr)
	{
		HydraulicNetwork->SetPumpSpeedRatio(PumpSpeedKnob.Value);
	}
	if (Room != nullptr)
	{
		Room->SetPumpSpeedRatio(PumpSpeedKnob.Value);
	}
}

void ADataCenterDebugController::ApplyRadiatorKnob()
{
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->Radiator.UAWK = RadiatorUAKnob.Value;
		}
	}
}

void ADataCenterDebugController::OnCpuHeatDown()
{
	CpuHeatKnob.Nudge(-1);
	ApplyHeatKnobs();
}

void ADataCenterDebugController::OnCpuHeatUp()
{
	CpuHeatKnob.Nudge(1);
	ApplyHeatKnobs();
}

void ADataCenterDebugController::OnGpuHeatDown()
{
	GpuHeatKnob.Nudge(-1);
	ApplyHeatKnobs();
}

void ADataCenterDebugController::OnGpuHeatUp()
{
	GpuHeatKnob.Nudge(1);
	ApplyHeatKnobs();
}

void ADataCenterDebugController::OnPumpSpeedDown()
{
	PumpSpeedKnob.Nudge(-1);
	ApplyPumpSpeedKnob();
}

void ADataCenterDebugController::OnPumpSpeedUp()
{
	PumpSpeedKnob.Nudge(1);
	ApplyPumpSpeedKnob();
}

void ADataCenterDebugController::OnRadiatorDown()
{
	RadiatorUAKnob.Nudge(-1);
	ApplyRadiatorKnob();
}

void ADataCenterDebugController::OnRadiatorUp()
{
	RadiatorUAKnob.Nudge(1);
	ApplyRadiatorKnob();
}

void ADataCenterDebugController::ApplyUtilizationKnob()
{
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->Input.Utilization = UtilizationKnob.Value;
			Rack->Input.SlotUtilization.Reset();
		}
	}
}

void ADataCenterDebugController::OnUtilizationDown()
{
	UtilizationKnob.Nudge(-1);
	ApplyUtilizationKnob();
}

void ADataCenterDebugController::OnUtilizationUp()
{
	UtilizationKnob.Nudge(+1);
	ApplyUtilizationKnob();
}

ARackThermalActor* ADataCenterDebugController::FindLookedAtRack() const
{
	FVector EyeCm;
	FRotator ViewRotation;
	GetPlayerViewPoint(EyeCm, ViewRotation);
	return PickRackAlongRay(Racks, EyeCm, ViewRotation.Vector(), MaxSelectDistanceM * 100.0);
}

ARackThermalActor* ADataCenterDebugController::PickRackAlongRay(const TArray<TObjectPtr<ARackThermalActor>>& Racks,
	const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm)
{
	// Slab test against each rack's box in its own frame (forward = depth, right = width, up = height from the base).
	ARackThermalActor* Best = nullptr;
	double BestT = MaxDistanceCm;
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack == nullptr) continue;
		const FVector Axes[3] = { Rack->GetActorForwardVector(), Rack->GetActorRightVector(), Rack->GetActorUpVector() };
		const FVector SizeCm = Rack->RackSizeM * 100.0;
		const double Lo[3] = { -.5 * SizeCm.X, -.5 * SizeCm.Y, 0.0 }, Hi[3] = { .5 * SizeCm.X, .5 * SizeCm.Y, SizeCm.Z };
		const FVector Rel = EyeCm - Rack->GetActorLocation();
		double TMin = 0.0, TMax = BestT;
		bool bHit = true;
		for (int32 A = 0; A < 3 && bHit; ++A)
		{
			const double O = FVector::DotProduct(Rel, Axes[A]), D = FVector::DotProduct(Dir, Axes[A]);
			if (FMath::Abs(D) < 1e-9)
			{
				bHit = O >= Lo[A] && O <= Hi[A];
				continue;
			}
			double T0 = (Lo[A] - O) / D, T1 = (Hi[A] - O) / D;
			if (T0 > T1) Swap(T0, T1);
			TMin = FMath::Max(TMin, T0); TMax = FMath::Min(TMax, T1);
			bHit = TMin <= TMax;
		}
		if (bHit && TMin < BestT)
		{
			BestT = TMin;
			Best = Rack;
		}
	}
	return Best;
}

void ADataCenterDebugController::DrawTargetHighlight() const
{
	const ARackThermalActor* Rack = TargetRack.Get();
	UWorld* World = GetWorld();
	if (Rack == nullptr || World == nullptr) return;
	const FQuat Rotation = Rack->GetActorQuat();
	const FVector SizeCm = Rack->RackSizeM * 100.0;
	DrawDebugBox(World, Rack->GetActorLocation() + Rack->GetActorUpVector() * .5 * SizeCm.Z, .5 * SizeCm, Rotation, FColor::Green, false, 0.0f, 0, 1.5f);
	double BottomM = 0.0, HeightM = 0.0;
	Rack->GetSlotHeightRangeM(SelectedSledIndex, BottomM, HeightM);
	if (HeightM > 0.0)
	{
		const FVector SlotCentre = Rack->GetActorLocation() + Rack->GetActorUpVector() * (BottomM + .5 * HeightM) * 100.0;
		DrawDebugBox(World, SlotCentre, FVector(.5 * SizeCm.X + 2.0, .5 * SizeCm.Y + 2.0, .5 * HeightM * 100.0), Rotation, FColor::Yellow, false, 0.0f, 0, 2.0f);
	}
}

void ADataCenterDebugController::OnSelectPreviousSled()
{
	if (const ARackThermalActor* Rack = TargetRack.Get())
	{
		const int32 NumSleds = FMath::Max(1, Rack->Params.NumSleds);
		SelectedSledIndex = (SelectedSledIndex - 1 + NumSleds) % NumSleds;
	}
}

void ADataCenterDebugController::OnSelectNextSled()
{
	if (const ARackThermalActor* Rack = TargetRack.Get())
	{
		const int32 NumSleds = FMath::Max(1, Rack->Params.NumSleds);
		SelectedSledIndex = (SelectedSledIndex + 1) % NumSleds;
	}
}

void ADataCenterDebugController::OnToggleSelectedSled()
{
	// Only the rack being looked at. Nothing happens when the view ray hits no rack.
	ARackThermalActor* Rack = TargetRack.Get();
	if (Rack == nullptr)
	{
		return;
	}
	Rack->ToggleSledDocked(SelectedSledIndex);

	// The S2 hydraulic debug actor has its own abstract racks and no link to rack actors; its rack with the
	// same index as the target in this controller's list follows the target's docked count.
	const int32 TargetIndex = Racks.IndexOfByKey(Rack);
	if (HydraulicNetwork != nullptr && TargetIndex != INDEX_NONE && TargetIndex < HydraulicNetwork->NumRacks)
	{
		HydraulicNetwork->SetRackDockedSleds(TargetIndex, Rack->GetNumDockedSleds());
	}

	// 방 액터는 랙 액터에서 도킹 상태를 직접 읽으므로 다시 풀라고만 알리면 된다.
	if (Room != nullptr)
	{
		Room->RebuildRoom();
	}
}

void ADataCenterDebugController::OnResetSim()
{
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->ResetSim();
			Rack->StartSim();
		}
	}
	ApplyHeatKnobs();
	ApplyRadiatorKnob();
}

void ADataCenterDebugController::OnToggleHelp()
{
	bShowHelp = !bShowHelp;
}

void ADataCenterDebugController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	RefreshTargetsIfStale(DeltaTime);

	ARackThermalActor* Looked = FindLookedAtRack();
	if (Looked != TargetRack.Get())
	{
		TargetRack = Looked;
		// Racks can differ in slot count (NVL72 37 slots vs hybrid 12); keep the index valid for the new rack.
		if (Looked != nullptr) SelectedSledIndex = FMath::Clamp(SelectedSledIndex, 0, FMath::Max(1, Looked->Params.NumSleds) - 1);
	}
	DrawTargetHighlight();

	if (bShowHelp)
	{
		DrawHelp();
	}
}

void ADataCenterDebugController::DrawHelp() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f, FColor(180, 220, 255),
		TEXT("[디버그 키] 1/2 CPU 발열  3/4 GPU 발열  5/6 펌프 회전수  7/8 라디에이터 UA  9/0 부하율  ←/→ 유닛 선택  E 뽑기/꽂기 (바라보는 랙만)  R 리셋  H 도움말"));

	const ARackThermalActor* Rack = TargetRack.Get();
	FString Selection = TEXT("바라보는 랙 없음 - 랙을 조준하면 ←/→, E가 그 랙에만 적용된다");
	if (Rack != nullptr)
	{
		const FVector M = SimUnits::UnrealToMeters(Rack->GetActorLocation());
		Selection = FString::Printf(TEXT("대상 랙 (%.2f, %.2f) m | 선택 유닛 U%d/%d %s | 꽂힌 유닛 %d"),
			M.X, M.Y, SelectedSledIndex, Rack->Params.NumSleds,
			Rack->IsSledDocked(SelectedSledIndex) ? TEXT("[꽂힘]") : TEXT("[뽑힘]"), Rack->GetNumDockedSleds());
	}

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor(255, 220, 160),
		FString::Printf(
			TEXT("CPU %.0f W/유닛 | GPU %.0f W/유닛 (전부하) | 부하율 %.0f%% | 펌프 r=%.2f | 라디에이터 UA %.0f W/K | %s"),
			CpuHeatKnob.Value, GpuHeatKnob.Value, UtilizationKnob.Value * 100.0, PumpSpeedKnob.Value, RadiatorUAKnob.Value,
			*Selection));
}
