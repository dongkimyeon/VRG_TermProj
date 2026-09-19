// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/DataCenterDebugController.h"

#include "Actors/HydraulicNetworkDebugActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"

ADataCenterDebugController::ADataCenterDebugController()
{
	PrimaryActorTick.bCanEverTick = true;

	// 기본값은 FRackThermalModel::DefaultSledHeatW()의 운전점을 가운데 두고 잡았다.
	CpuHeatKnob.Value = 350.0;
	CpuHeatKnob.MinValue = 0.0;
	CpuHeatKnob.MaxValue = 700.0;
	CpuHeatKnob.StepSize = 25.0;

	GpuHeatKnob.Value = 700.0;
	GpuHeatKnob.MinValue = 0.0;
	GpuHeatKnob.MaxValue = 1400.0;
	GpuHeatKnob.StepSize = 50.0;

	PumpSpeedKnob.Value = 1.0;
	PumpSpeedKnob.MinValue = 0.0;
	PumpSpeedKnob.MaxValue = 1.5;
	PumpSpeedKnob.StepSize = 0.05;

	// UA는 100 W/K에서 20000 W/K까지 훑어야 포화가 눈에 보인다.
	// 선형 증분으로는 키를 수백 번 눌러야 하므로 곱셈으로 움직인다.
	RadiatorUAKnob.Value = 1.6e3;
	RadiatorUAKnob.MinValue = 1.0e2;
	RadiatorUAKnob.MaxValue = 2.0e4;
	RadiatorUAKnob.StepSize = 1.25;
	RadiatorUAKnob.bMultiplicative = true;
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

	// 손잡이 초기값을 한 번 밀어 넣어 화면 값과 실제 값을 맞춘다.
	ApplyHeatKnobs();
	ApplyPumpSpeedKnob();
	ApplyRadiatorKnob();
}

void ADataCenterDebugController::FindTargetActors()
{
	Racks.Reset();
	HydraulicNetwork = nullptr;

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

	if (Racks.Num() == 0 && HydraulicNetwork == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DataCenterDebugController: 레벨에서 랙 액터도 배관 액터도 찾지 못했습니다. 디버그 키가 할 일이 없습니다."));
	}
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

		const int32 NumSleds = FMath::Max(1, Rack->Params.NumSleds);
		if (Rack->Input.ComponentHeatW.Num() != NumSleds * NumRackComponents)
		{
			continue;
		}

		for (int32 SledIndex = 0; SledIndex < NumSleds; ++SledIndex)
		{
			Rack->Input.ComponentHeatW[FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Cpu)] = CpuHeatKnob.Value;
			Rack->Input.ComponentHeatW[FRackThermalModel::NodeIndex(SledIndex, ERackComponent::Gpu)] = GpuHeatKnob.Value;
		}
	}
}

void ADataCenterDebugController::ApplyPumpSpeedKnob()
{
	if (HydraulicNetwork != nullptr)
	{
		HydraulicNetwork->SetPumpSpeedRatio(PumpSpeedKnob.Value);
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

	// 수력 쪽 분기 저항도 같이 바뀌어야 한다. 랙 1대의 꽂힌 유닛 수를 그대로 넘긴다.
	if (HydraulicNetwork != nullptr && Racks.Num() > 0 && Racks[0] != nullptr)
	{
		const int32 NumDocked = Racks[0]->GetNumDockedSleds();
		for (int32 RackIndex = 0; RackIndex < HydraulicNetwork->NumRacks; ++RackIndex)
		{
			HydraulicNetwork->SetRackDockedSleds(RackIndex, NumDocked);
		}
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
		TEXT("[디버그 키] 1/2 CPU 발열  3/4 GPU 발열  5/6 펌프 회전수  7/8 라디에이터 UA  ←/→ 유닛 선택  E 뽑기/꽂기  R 리셋  H 도움말"));

	const int32 NumSleds = (Racks.Num() > 0 && Racks[0] != nullptr) ? Racks[0]->Params.NumSleds : 0;
	const bool bSelectedDocked = (Racks.Num() > 0 && Racks[0] != nullptr) && Racks[0]->IsSledDocked(SelectedSledIndex);

	GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor(255, 220, 160),
		FString::Printf(
			TEXT("CPU %.0f W/유닛 | GPU %.0f W/유닛 | 펌프 r=%.2f | 라디에이터 UA %.0f W/K | 선택 유닛 U%d/%d %s"),
			CpuHeatKnob.Value, GpuHeatKnob.Value, PumpSpeedKnob.Value, RadiatorUAKnob.Value,
			SelectedSledIndex, NumSleds,
			bSelectedDocked ? TEXT("[꽂힘]") : TEXT("[뽑힘]")));
}
