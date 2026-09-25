// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/DataCenterDebugController.h"

#include "Actors/DataCenterRoomActor.h"
#include "Actors/HydraulicNetworkDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Thermal/RackThermalModel.h"

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

void ADataCenterDebugController::OnSelectPreviousSled()
{
	if (Racks.Num() > 0 && Racks[0] != nullptr)
	{
		const int32 NumSleds = FMath::Max(1, Racks[0]->Params.NumSleds);
		SelectedSledIndex = (SelectedSledIndex - 1 + NumSleds) % NumSleds;
	}
}

void ADataCenterDebugController::OnSelectNextSled()
{
	if (Racks.Num() > 0 && Racks[0] != nullptr)
	{
		const int32 NumSleds = FMath::Max(1, Racks[0]->Params.NumSleds);
		SelectedSledIndex = (SelectedSledIndex + 1) % NumSleds;
	}
}

void ADataCenterDebugController::OnToggleSelectedSled()
{
	for (ARackThermalActor* Rack : Racks)
	{
		if (Rack != nullptr)
		{
			Rack->ToggleSledDocked(SelectedSledIndex);
		}
	}

	// 수력 쪽 분기 저항도 같이 바뀌어야 한다.
	if (HydraulicNetwork != nullptr && Racks.Num() > 0 && Racks[0] != nullptr)
	{
		const int32 NumDocked = Racks[0]->GetNumDockedSleds();
		for (int32 RackIndex = 0; RackIndex < HydraulicNetwork->NumRacks; ++RackIndex)
		{
			HydraulicNetwork->SetRackDockedSleds(RackIndex, NumDocked);
		}
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
		TEXT("[디버그 키] 1/2 CPU 발열  3/4 GPU 발열  5/6 펌프 회전수  7/8 라디에이터 UA  9/0 부하율  ←/→ 유닛 선택  E 뽑기/꽂기  R 리셋  H 도움말"));

	const int32 NumSleds = (Racks.Num() > 0 && Racks[0] != nullptr) ? Racks[0]->Params.NumSleds : 0;
	const bool bSelectedDocked = (Racks.Num() > 0 && Racks[0] != nullptr) && Racks[0]->IsSledDocked(SelectedSledIndex);

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor(255, 220, 160),
		FString::Printf(
			TEXT("CPU %.0f W/유닛 | GPU %.0f W/유닛 (전부하) | 부하율 %.0f%% | 펌프 r=%.2f | 라디에이터 UA %.0f W/K | 선택 유닛 U%d/%d %s"),
			CpuHeatKnob.Value, GpuHeatKnob.Value, UtilizationKnob.Value * 100.0, PumpSpeedKnob.Value, RadiatorUAKnob.Value,
			SelectedSledIndex, NumSleds,
			bSelectedDocked ? TEXT("[꽂힘]") : TEXT("[뽑힘]")));
}
