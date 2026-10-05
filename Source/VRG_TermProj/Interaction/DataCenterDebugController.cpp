// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgSpineLayoutState.h"
#include "Interaction/LayoutRackPalette.h"
#include "Simulation/Core/VrgHud.h"

#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Visualization/GjRackLayout.h"
#include "Simulation/Core/SimUnits.h"
#include "Simulation/Thermal/RackThermalModel.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "SceneView.h"
#include "Engine/Console.h"
#include "InputKeyEventArgs.h"
#include "UnrealClient.h"
#include "GameFramework/PawnMovementComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "HAL/IConsoleManager.h"
#include "Widgets/SViewport.h"

static TAutoConsoleVariable<int32> CVarLayoutInputDiagnostics(TEXT("vrg.Layout.InputDiagnostics"), 0,
	TEXT("Log placement input gates at duplicate/confirm/patch/capture events; 0 disables diagnostics."));

ADataCenterDebugController::ADataCenterDebugController()
{
	PrimaryActorTick.bCanEverTick = true;

	// 여기 값은 레벨에 랙이 하나도 없을 때만 쓰이는 자리 표시다.
	// 랙을 찾으면 InitializeKnobsFromTargets()가 그 랙의 현재 값(칩 한 개의 전부하 발열)으로 다시 잡는다.
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

	if (RackCount == Racks.Num() && (Racks.Num() > 0 || Room != nullptr))
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
		{ TEXT("ToggleSled"),    EKeys::E,          &ADataCenterDebugController::OnToggleSelectedSled },
		{ TEXT("ResetSim"),      EKeys::R,          &ADataCenterDebugController::OnResetSim },
		{ TEXT("ToggleSimMode"), EKeys::Enter,      &ADataCenterDebugController::OnToggleSimMode },
		{ TEXT("GrabOrDrop"),    EKeys::G,          &ADataCenterDebugController::OnGrabOrDrop },
		{ TEXT("RotatePlace"),   EKeys::T,          &ADataCenterDebugController::OnRotatePlaceable },
		{ TEXT("Containment"),   EKeys::C,          &ADataCenterDebugController::OnCycleContainment },
		{ TEXT("ToggleHelp"),    EKeys::H,          &ADataCenterDebugController::OnToggleHelp },
		{ TEXT("CameraWheelUp"), EKeys::MouseScrollUp, &ADataCenterDebugController::OnCameraWheelUp },
		{ TEXT("CameraWheelDown"), EKeys::MouseScrollDown, &ADataCenterDebugController::OnCameraWheelDown },
		{ TEXT("FocusEquipment"), EKeys::F, &ADataCenterDebugController::OnFocusEquipment },
		{ TEXT("SelectLayoutEquipment"), EKeys::LeftMouseButton, &ADataCenterDebugController::OnSelectLayoutEquipment },
		{ TEXT("CancelLayoutDrag"), EKeys::Escape, &ADataCenterDebugController::OnCancelLayoutDrag },
		{ TEXT("DeleteLayoutEquipment"), EKeys::Delete, &ADataCenterDebugController::OnDeleteLayoutEquipment },
	};

	for (const FBinding& Binding : Bindings)
	{
		if (UInputAction* Action = AddKeyAction(Binding.Name, Binding.Key))
		{
			EnhancedInput->BindAction(Action, ETriggerEvent::Started, this, Binding.Handler);
			if (Binding.Key == EKeys::LeftMouseButton)
			{
				EnhancedInput->BindAction(Action, ETriggerEvent::Completed, this, &ADataCenterDebugController::OnReleaseLayoutEquipment);
				EnhancedInput->BindAction(Action, ETriggerEvent::Canceled, this, &ADataCenterDebugController::OnCancelLayoutDrag);
			}
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

ARackThermalActor* ADataCenterDebugController::FindLookedAtRack(double& OutHitZM) const
{
	FVector EyeCm;
	FRotator ViewRotation;
	GetPlayerViewPoint(EyeCm, ViewRotation);
	return PickRackAlongRay(Racks, EyeCm, ViewRotation.Vector(), MaxSelectDistanceM * 100.0, &OutHitZM);
}

ARackThermalActor* ADataCenterDebugController::PickRackAlongRay(const TArray<TObjectPtr<ARackThermalActor>>& Racks,
	const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm, double* OutHitZM)
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
			// Where the ray enters the box, as height above the rack base [m].
			if (OutHitZM) *OutHitZM = FVector::DotProduct(Rel + Dir * TMin, Axes[2]) / 100.0;
		}
	}
	return Best;
}

int32 ADataCenterDebugController::SlotAtHeight(const ARackThermalActor& Rack, double HeightM)
{
	for (int32 Slot = 0; Slot < Rack.Params.NumSleds; ++Slot)
	{
		double BottomM = 0.0, SlotHeightM = 0.0;
		Rack.GetSlotHeightRangeM(Slot, BottomM, SlotHeightM);
		if (SlotHeightM > 0.0 && HeightM >= BottomM && HeightM < BottomM + SlotHeightM) return Slot;
	}
	return INDEX_NONE;  // blank panel, frame or top of the rack
}

bool ADataCenterDebugController::PlaceableBoxM(const AActor* Actor, FVector& OutSizeM)
{
	if (const ARackThermalActor* Rack = Cast<ARackThermalActor>(Actor)) { OutSizeM = Rack->RackSizeM; return true; }
	if (const ACrahUnitActor* Crah = Cast<ACrahUnitActor>(Actor)) { OutSizeM = Crah->SizeM; return true; }
	if (const ACoolingDistributionUnitActor* Cdu = Cast<ACoolingDistributionUnitActor>(Actor)) { OutSizeM = Cdu->SizeM; return true; }
	if (Cast<ASpineCabinetActor>(Actor)) { OutSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM); return true; }
	return false;
}

AActor* ADataCenterDebugController::PickPlaceableAlongRay(UWorld* World, const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm)
{
	if (World == nullptr) return nullptr;
	AActor* Best = nullptr;
	double BestT = MaxDistanceCm;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FVector SizeM;
		if (!PlaceableBoxM(*It, SizeM)) continue;
		// Same slab test as the rack pick, in the actor's own (unscaled) frame.
		const FVector Axes[3] = { It->GetActorForwardVector(), It->GetActorRightVector(), It->GetActorUpVector() };
		const FVector SizeCm = SizeM * 100.0;
		const double Lo[3] = { -.5 * SizeCm.X, -.5 * SizeCm.Y, 0.0 }, Hi[3] = { .5 * SizeCm.X, .5 * SizeCm.Y, SizeCm.Z };
		const FVector Rel = EyeCm - It->GetActorLocation();
		double TMin = 0.0, TMax = BestT;
		bool bHit = true;
		for (int32 A = 0; A < 3 && bHit; ++A)
		{
			const double O = FVector::DotProduct(Rel, Axes[A]), D = FVector::DotProduct(Dir, Axes[A]);
			if (FMath::Abs(D) < 1e-9) { bHit = O >= Lo[A] && O <= Hi[A]; continue; }
			double T0 = (Lo[A] - O) / D, T1 = (Hi[A] - O) / D;
			if (T0 > T1) Swap(T0, T1);
			TMin = FMath::Max(TMin, T0); TMax = FMath::Min(TMax, T1);
			bHit = TMin <= TMax;
		}
		if (bHit && TMin < BestT) { BestT = TMin; Best = *It; }
	}
	return Best;
}

bool ADataCenterDebugController::FloorPointSnapped(const FVector& EyeCm, const FVector& Dir, double PlaneZCm, double SnapM, FVector& OutCm)
{
	if (Dir.Z > -1e-3) return false;  // looking level or up: the ray never meets the floor in front
	const double T = (PlaneZCm - EyeCm.Z) / Dir.Z;
	if (T < 0.0) return false;
	OutCm = EyeCm + Dir * T;
	if (SnapM > 0.0)
	{
		const double S = SnapM * 100.0;
		OutCm.X = FMath::RoundToDouble(OutCm.X / S) * S;
		OutCm.Y = FMath::RoundToDouble(OutCm.Y / S) * S;
	}
	OutCm.Z = PlaneZCm;
	return true;
}

bool ADataCenterDebugController::EquipmentViewpoint(UWorld* World, double HorizontalFovDeg, double Aspect, FVector& OutEyeCm, FRotator& OutLook, const AActor* OnlyActor, const TArray<AActor*>* OnlyActors)
{
	if (World == nullptr) return false;
	TArray<FVector> Corners;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (OnlyActor != nullptr && *It != OnlyActor) continue;
		if (OnlyActors != nullptr && !OnlyActors->Contains(*It)) continue;
		FVector SizeM;
		if (!PlaceableBoxM(*It, SizeM)) continue;
		const FVector HalfCm = SizeM * 50.0;
		for (int32 C = 0; C < 8; ++C)
		{
			const FVector Local((C & 1 ? 1 : -1) * HalfCm.X, (C & 2 ? 1 : -1) * HalfCm.Y, C & 4 ? 2.0 * HalfCm.Z : 0.0);
			Corners.Add(It->GetActorTransform().TransformPosition(Local));
		}
	}
	if (Corners.Num() == 0) return false;
	const FBox Bounds(Corners);
	const FVector Center = Bounds.GetCenter();
	// Oblique view from above (the direction vrg.Air.FrameDomain uses), looking at the centre. The distance is the
	// smallest at which every corner is inside the frustum: with offsets x, y across the view and s along Dir,
	// the corner's depth is D - s, so it needs D >= s + |x| / tan(half H) and D >= s + |y| / tan(half V).
	const FVector Dir = FVector(-0.75, -0.55, 0.45).GetSafeNormal();
	OutLook = (-Dir).Rotation();
	const FRotationMatrix View(OutLook);
	const FVector Right = View.GetScaledAxis(EAxis::Y), Up = View.GetScaledAxis(EAxis::Z);
	const double TanH = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(HorizontalFovDeg, 10.0, 170.0)) * 0.5);
	const double TanV = TanH / FMath::Max(Aspect, 0.1);
	double Distance = 0.0;
	for (const FVector& P : Corners)
	{
		const FVector D = P - Center;
		const double S = FVector::DotProduct(D, Dir);
		Distance = FMath::Max3(Distance, S + FMath::Abs(FVector::DotProduct(D, Right)) / TanH, S + FMath::Abs(FVector::DotProduct(D, Up)) / TanV);
	}
	Distance *= 1.08;  // a small margin so the outermost cabinets are not cut by the screen edge
	OutEyeCm = Center + Dir * Distance;
	return true;
}

bool ADataCenterDebugController::TryFrameEquipment(const AActor* OnlyActor, const TArray<AActor*>* OnlyActors)
{
	APawn* ViewPawn = GetPawn();
	if (ViewPawn == nullptr) return false;
	double Aspect = 16.0 / 9.0;
	if (GEngine != nullptr && GEngine->GameViewport != nullptr)
	{
		FVector2D Size;
		GEngine->GameViewport->GetViewportSize(Size);
		if (Size.X > 0.0 && Size.Y > 0.0) Aspect = Size.X / Size.Y;
	}
	const double Fov = PlayerCameraManager != nullptr ? PlayerCameraManager->GetFOVAngle() : 90.0;
	FVector Eye;
	FRotator Look;
	if (!EquipmentViewpoint(GetWorld(), Fov, Aspect, Eye, Look, OnlyActor, OnlyActors)) return false;
	ViewPawn->ConsumeMovementInputVector();
	if (UPawnMovementComponent* Movement = ViewPawn->GetMovementComponent()) Movement->StopMovementImmediately();
	ViewPawn->SetActorLocation(Eye, false, nullptr, ETeleportType::TeleportPhysics);
	FramedEyeCm = ViewPawn->GetActorLocation();
	ViewPawn->SetActorRotation(Look);
	SetControlRotation(Look);
	UE_LOG(LogTemp, Display, TEXT("[VrgDebug] 장비 보기 (%s): %s (FOV %.0f, 화면비 %.2f)"),
		OnlyActor ? *OnlyActor->GetName() : OnlyActors ? TEXT("선택 그룹") : TEXT("전체"), *Eye.ToCompactString(), Fov, Aspect);
	return true;
}

namespace
{
	// XY projection of a transformed box. Three generators include tilted/scaled height as well as depth/width.
	struct FLayoutFootprint
	{
		FVector2D Centre;
		FVector2D HalfEdges[3];
	};
	bool MakeLayoutFootprint(const FVector& SizeM, const FTransform& Transform, FLayoutFootprint& Out)
	{
		if (Transform.ContainsNaN() || SizeM.ContainsNaN()
			|| SizeM.X <= 0 || SizeM.Y <= 0 || SizeM.Z <= 0) return false;
		const FVector HalfCm = SizeM * 50.0; // Actor/view boundary only; solver units remain m.
		const FVector CentreCm = Transform.TransformPosition(FVector(0, 0, HalfCm.Z));
		Out.Centre = FVector2D(CentreCm.X, CentreCm.Y);
		const FVector LocalEdges[] = { FVector(HalfCm.X, 0, 0), FVector(0, HalfCm.Y, 0), FVector(0, 0, HalfCm.Z) };
		for (int32 I = 0; I < 3; ++I)
		{
			const FVector Edge = Transform.TransformVector(LocalEdges[I]);
			Out.HalfEdges[I] = FVector2D(Edge.X, Edge.Y);
		}
		return true;
	}
	bool MakeLayoutFootprint(const AActor* Actor, const FTransform& Transform, FLayoutFootprint& Out)
	{
		FVector SizeM;
		return ADataCenterDebugController::PlaceableBoxM(Actor, SizeM) && MakeLayoutFootprint(SizeM, Transform, Out);
	}
	bool FootprintsConflict(const FLayoutFootprint& A, const FLayoutFootprint& B)
	{
		const FVector2D Delta = B.Centre - A.Centre;
		// Team placement plan: 1 cm total clearance, not 1 cm expansion on each box and not a physical constant.
		constexpr double ClearanceCm = 1.0;
		for (const FLayoutFootprint* Footprint : { &A, &B })
			for (const FVector2D& Edge : Footprint->HalfEdges)
			{
				if (Edge.IsNearlyZero()) continue;
				const FVector2D Axis = FVector2D(-Edge.Y, Edge.X).GetSafeNormal();
				double Radius = 0;
				for (int32 I = 0; I < 3; ++I)
					Radius += FMath::Abs(FVector2D::DotProduct(A.HalfEdges[I], Axis)) + FMath::Abs(FVector2D::DotProduct(B.HalfEdges[I], Axis));
				if (FMath::Abs(FVector2D::DotProduct(Delta, Axis)) >= Radius + ClearanceCm) return false;
			}
		return true;
	}
}

bool ADataCenterDebugController::IsLayoutPlacementBlocked(const AActor* Actor, const FTransform& Proposed, const TArray<AActor*>* Ignored)
{
	if (!IsValid(Actor)) return true;
	FVector SizeM;
	if (!PlaceableBoxM(Actor, SizeM)) return true;
	TArray<AActor*> Excluded;
	if (Ignored) Excluded = *Ignored;
	Excluded.Add(const_cast<AActor*>(Actor));
	return IsLayoutBoxBlocked(Actor->GetWorld(), SizeM, Proposed, &Excluded);
}

bool ADataCenterDebugController::IsLayoutBoxBlocked(UWorld* World, const FVector& SizeM, const FTransform& Proposed, const TArray<AActor*>* Ignored)
{
	if (!World) return true;
	FLayoutFootprint Candidate;
	if (!MakeLayoutFootprint(SizeM, Proposed, Candidate)) return true;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!IsValid(*It) || (Ignored && Ignored->Contains(*It))) continue;
		FVector OtherSizeM;
		if (!PlaceableBoxM(*It, OtherSizeM)) continue;
		FLayoutFootprint Other;
		if (!MakeLayoutFootprint(OtherSizeM, It->GetActorTransform(), Other) || FootprintsConflict(Candidate, Other)) return true;
	}
	return false;
}

void ADataCenterDebugController::BeginRackAddition()
{
	if (!IsLayoutMode() || !IsCameraInputAvailable()) return;
	EndLayoutDrag(false);
	EndLayoutMarquee(false);
	Grabbed.Reset();
	RackDuplicationMembers.Reset();
	SpineDuplicationMembers.Reset();
	bAddingSpine = false;
	bAddingCdu = false;
	bAddingRack = true;
	bRackAdditionHasFloor = false;
	RackAdditionTransform = FTransform::Identity;
	UpdateRackAdditionFromCursor();
}

void ADataCenterDebugController::BeginSpineAddition()
{
 if (!IsLayoutMode() || !IsCameraInputAvailable()) return;
 BeginRackAddition();
 if (bAddingRack) bAddingSpine = true;
}

void ADataCenterDebugController::BeginCduAddition()
{
 if (!IsLayoutMode() || !IsCameraInputAvailable() || IsLayoutDragging() || bMarqueeActive || bAddingRack
  || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return;
 BeginRackAddition();
 if (bAddingRack) bAddingCdu = true;
}

bool ADataCenterDebugController::BeginRackDuplication()
{
	UWorld* CurrentWorld = GetWorld();
	if (!CurrentWorld || !CurrentWorld->IsGameWorld() || !IsLayoutMode() || !IsCameraInputAvailable()
		|| bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	if (Selected.IsEmpty()) return false;
	TArray<FLayoutRackMember> Members;
	for (AActor* Actor : Selected)
	{
		auto* Rack = Cast<ARackThermalActor>(Actor);
		if (!IsValid(Rack) || Rack->GetWorld() != CurrentWorld) return false;
		Members.Append(CaptureRackPresence(Rack, false).Racks);
		// Preserve single duplication: native rack, authored settings only, no Blueprint state.
		Members.Last().Class = ARackThermalActor::StaticClass();
	}
	RackAdditionTransform = Members[0].Transform;
	RackDuplicationMembers = MoveTemp(Members);
	bAddingRack = true;
	bRackAdditionHasFloor = false;
	UpdateRackAdditionFromCursor();
	return true;
}

void ADataCenterDebugController::OnDuplicateLayoutRack()
{
	if (!BeginRackDuplication() && !BeginSpineDuplication())
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 복제 거절: 랙 선택 전체 또는 네이티브 스파인 선택 전체 또는 네이티브 CDU 한 대와 배치 대기 상태가 필요합니다."));
	LogLayoutInputState(TEXT("Ctrl+D"));
}

bool ADataCenterDebugController::BeginSpineDuplication()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !IsLayoutMode() || !IsCameraInputAvailable()
		|| bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const auto Selected = GetSelectedLayoutActors();
	if (Selected.IsEmpty()) return false;
	TArray<FLayoutSpineMember> Members;
	for (auto* Actor : Selected)
	{
		const auto* Spine = Cast<ASpineCabinetActor>(Actor);
		if (!IsValid(Spine) || Spine->GetWorld() != World || Spine->GetClass() != ASpineCabinetActor::StaticClass()) return false;
		const auto Settings = Spine->CaptureLayoutSettings();
		if (!Settings.IsValid()) return false;
		Members.Add({VrgSpineLifecycle::Identity(Spine), Spine->GetFName(), Spine->GetActorTransform(), Settings});
	}
	RackDuplicationMembers.Reset();
	RackAdditionTransform = Members[0].Transform;
	SpineDuplicationMembers = MoveTemp(Members);
	bAddingRack = true; bAddingSpine = true; bRackAdditionHasFloor = false;
	UpdateRackAdditionFromCursor();
	return true;
}

bool ADataCenterDebugController::InputKey(const FInputKeyEventArgs& Params)
{
	if (Params.Key == EKeys::LeftMouseButton)
	{
		if (Params.Event == IE_Pressed) LogLayoutInputState(TEXT("RawLmbPressBefore"));
		else if (Params.Event == IE_Released) LogLayoutInputState(TEXT("RawLmbReleaseBefore"));
		else if (Params.Event == IE_DoubleClick) LogLayoutInputState(TEXT("RawLmbDoubleClickBefore"));
	}
	if ((Params.Key == EKeys::LeftAlt || Params.Key == EKeys::RightAlt) && Params.Event == IE_Released)
		LogLayoutInputState(TEXT("AltRelease")); // Also observes the end of Alt+Print Screen when routed to the game.
	if (Params.Key == EKeys::LeftControl) bLayoutLeftControl = Params.Event != IE_Released;
	if (Params.Key == EKeys::RightControl) bLayoutRightControl = Params.Event != IE_Released;
	if ((Params.Key == EKeys::Z || Params.Key == EKeys::Y) && (bLayoutLeftControl || bLayoutRightControl))
	{
		if (Params.Event == IE_Pressed)
		{
			const bool bRedo = Params.Key == EKeys::Y;
			const bool bOk = bRedo ? TryRedoLayout() : TryUndoLayout();
			UE_LOG(LogTemp, Display, TEXT("[VrgLayout] %s %s (Undo %d / Redo %d)"),
				bRedo ? TEXT("Redo") : TEXT("Undo"), bOk ? TEXT("확정") : TEXT("거절"), LayoutUndo.Num(), LayoutRedo.Num());
		}
		return true; // Event-time chords, including short presses; never repeat or move the camera.
	}
	if (Params.Key == EKeys::D && Params.Event == IE_Pressed && (bLayoutLeftControl || bLayoutRightControl))
	{
		// A press/release chord may fit into one frame. Polling Ctrl in an Enhanced Input Started callback
		// would then see the already released key. Handle this chord at event time, and never repeat it.
		OnDuplicateLayoutRack();
		return true;
	}
	return Super::InputKey(Params); // D without Ctrl retains normal camera input.
}

void ADataCenterDebugController::CancelRackAddition()
{
	bAddingRack = false;
	bAddingSpine = false;
	bAddingCdu = false;
	bRackAdditionHasFloor = false;
	RackDuplicationMembers.Reset();
	SpineDuplicationMembers.Reset();
}

void ADataCenterDebugController::UpdateRackAdditionAlongRay(const FVector& EyeCm, const FVector& Direction)
{
	if (!bAddingRack) return;
	FVector FloorCm;
	bRackAdditionHasFloor = IsLayoutMode() && IsCameraInputAvailable()
		&& FloorPointSnapped(EyeCm, Direction, (!RackDuplicationMembers.IsEmpty() || !SpineDuplicationMembers.IsEmpty()) ? RackAdditionTransform.GetLocation().Z : 0.0, PlaceSnapM, FloorCm);
	if (bRackAdditionHasFloor) RackAdditionTransform.SetLocation(FloorCm);
}

void ADataCenterDebugController::UpdateRackAdditionFromCursor()
{
	if (!bAddingRack) return;
	if (!IsLayoutMode() || OpenPatchMaps > 0) { CancelRackAddition(); return; }
	FVector EyeCm, Direction;
	float X = 0, Y = 0;
	int32 Width = 0, Height = 0;
	GetViewportSize(Width, Height);
	const UGameViewportClient* Client = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	if (!IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !Client || !Client->Viewport || !Client->Viewport->HasFocus()
		|| !GetMousePosition(X, Y) || X < 0 || Y < 0 || X >= Width || Y >= Height
		|| !DeprojectMousePositionToWorld(EyeCm, Direction))
	{
		bRackAdditionHasFloor = false;
		return;
	}
	UpdateRackAdditionAlongRay(EyeCm, Direction);
}

void ADataCenterDebugController::LogLayoutInputState(const TCHAR* Context) const
{
	if (CVarLayoutInputDiagnostics.GetValueOnGameThread() == 0) return;
	const UGameViewportClient* Client = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	float X = 0, Y = 0;
	int32 Width = 0, Height = 0;
	GetViewportSize(Width, Height);
	const bool bMouse = GetMousePosition(X, Y);
	FVector EyeCm, Direction, FloorCm;
	const bool bRay = DeprojectMousePositionToWorld(EyeCm, Direction);
	const bool bFloor = bRay && FloorPointSnapped(EyeCm, Direction,
		(!RackDuplicationMembers.IsEmpty() || !SpineDuplicationMembers.IsEmpty()) ? RackAdditionTransform.GetLocation().Z : 0.0, PlaceSnapM, FloorCm);
	FTransform Proposed; FVector SizeM;
	const bool bPreview = GetRackAdditionPreview(Proposed, SizeM);
	// Opt-in, event-only diagnostics. Observing a ray does not update placement or weaken any guard.
	UE_LOG(LogTemp, Display, TEXT("[VrgLayoutInput] %s layout=%d enabled=%d paused=%d patch=%d console=%s focus=%d RMB=%d selected=%d adding=%d floorStored=%d mouse=%d XY=(%.1f,%.1f)/(%d,%d) ray=%d floorRay=%d preview=%d blocked=%d"),
		Context, IsLayoutMode(), InputEnabled(), IsPaused(), OpenPatchMaps,
		Client && Client->ViewportConsole ? *Client->ViewportConsole->ConsoleState.ToString() : TEXT("NoConsole"),
		Client && Client->Viewport && Client->Viewport->HasFocus(), IsInputKeyDown(EKeys::RightMouseButton),
		GetSelectedLayoutActors().Num(), bAddingRack, bRackAdditionHasFloor, bMouse, X, Y, Width, Height,
		bRay, bFloor, bPreview, bPreview && IsRackAdditionBlocked());
	const AActor* Selected = GetSelectedLayoutActor();
	UE_LOG(LogTemp, Display, TEXT("[VrgLayoutGesture] %s LMB=%d drag=%d moved=%d threshold=%d row=%d marquee=%d grabbed=%d undo=%d redo=%d target=%s liveCm=%s yaw=%.3f startCm=%s previewCm=%s"),
		Context, IsInputKeyDown(EKeys::LeftMouseButton), DragMembers.Num(), bDragMoved, bDragMouseThresholdPassed,
		bDragRowSnapped, bMarqueeActive, Grabbed.IsValid(), LayoutUndo.Num(), LayoutRedo.Num(),
		Selected ? *Selected->GetName() : TEXT("None"), Selected ? *Selected->GetActorLocation().ToCompactString() : TEXT("None"),
		Selected ? Selected->GetActorRotation().Yaw : 0.0, *DragStartTransform.GetLocation().ToCompactString(), *DragPreviewCm.ToCompactString());
}

bool ADataCenterDebugController::GetRackAdditionPreview(FTransform& Out, FVector& SizeM, int32 MemberIndex) const
{
	if (!bAddingRack || !bRackAdditionHasFloor || !IsLayoutMode() || !IsCameraInputAvailable()
		|| MemberIndex < 0 || MemberIndex >= GetRackAdditionPreviewCount()) return false;
	Out = RackAdditionTransform;
	// Reuse the authored GJ geometry; no new equipment dimensions or physical defaults.
	if (bAddingCdu) SizeM = GetDefault<ACoolingDistributionUnitActor>()->SizeM;
	else if (!RackDuplicationMembers.IsEmpty())
	{
		SizeM = RackDuplicationMembers[MemberIndex].Settings.RackSizeM;
		if (RackDuplicationMembers.Num() > 1)
		{
			Out = RackDuplicationMembers[MemberIndex].Transform;
			Out.AddToTranslation(RackAdditionTransform.GetLocation() - RackDuplicationMembers[0].Transform.GetLocation());
		}
	}
	else
	{
		SizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		if (SpineDuplicationMembers.Num() > 1)
		{
			Out = SpineDuplicationMembers[MemberIndex].Transform;
			Out.AddToTranslation(RackAdditionTransform.GetLocation() - SpineDuplicationMembers[0].Transform.GetLocation());
		}
	}
	return true;
}

bool ADataCenterDebugController::TryRotateRackAddition()
{
	if (!bAddingRack || !IsLayoutMode() || !IsCameraInputAvailable() || GetRackAdditionPreviewCount() > 1
		|| IsInputKeyDown(EKeys::RightMouseButton)) return false;
	RackAdditionTransform.SetRotation(FRotator(0, 90, 0).Quaternion() * RackAdditionTransform.GetRotation());
	return true;
}

bool ADataCenterDebugController::IsRackAdditionBlocked() const
{
	if (GetRackAdditionPreviewCount() == 0) return true;
	TArray<FLayoutFootprint> Footprints;
	for (int32 I = 0; I < GetRackAdditionPreviewCount(); ++I)
	{
		FTransform Proposed; FVector SizeM; FLayoutFootprint Candidate;
		if (!GetRackAdditionPreview(Proposed, SizeM, I) || IsLayoutBoxBlocked(GetWorld(), SizeM, Proposed)
			|| !MakeLayoutFootprint(SizeM, Proposed, Candidate)) return true;
		for (const auto& Other : Footprints) if (FootprintsConflict(Candidate, Other)) return true;
		Footprints.Add(Candidate);
	}
	return false;
}

ARackThermalActor* ADataCenterDebugController::ConfirmRackAddition()
{
	if (bAddingCdu) { ConfirmCduAddition(); return nullptr; }
	if (bAddingSpine) { ConfirmSpineAddition(); return nullptr; }
	LogLayoutInputState(TEXT("Confirm"));
	FTransform Proposed; FVector SizeM;
	if (!GetWorld() || !GetWorld()->IsGameWorld() || !GetRackAdditionPreview(Proposed, SizeM) || IsRackAdditionBlocked())
	{
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 랙 추가 거절: 목적지 없음 또는 장비 겹침"));
		return nullptr;
	}
	if (!RackDuplicationMembers.IsEmpty())
	{
		FLayoutTransformCommand Command;
		Command.bRackPresence = true; // bBeforePresent=false: Undo removes the whole creation.
		for (int32 I = 0; I < RackDuplicationMembers.Num(); ++I)
		{
			FLayoutRackMember Member = RackDuplicationMembers[I];
			GetRackAdditionPreview(Member.Transform, SizeM, I);
			Member.Id = FGuid::NewGuid().ToString();
			Member.Name = MakeUniqueObjectName(GetWorld()->PersistentLevel, Member.Class.Get(), FName(*(TEXT("DuplicatedRack_") + Member.Id)));
			Command.Racks.Add(MoveTemp(Member));
		}
		// Reuse the batch validation/staging/rollback/rebuild used by Undo and Redo.
		if (!RestoreRackPresence(Command, true)) return nullptr;
		PushLayoutCommand(Command);
		CancelRackAddition();
		for (const auto& Member : Command.Racks) LayoutSelection.Add(VrgRackLifecycle::Find(GetWorld(), Member.Id));
		auto* First = VrgRackLifecycle::Find(GetWorld(), Command.Racks[0].Id);
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] GB200 NVL72 복제 확정: %d대 | 랙 %d대"), Command.Racks.Num(), Racks.Num());
		return First;
	}
	// Configure before BeginPlay: a runtime rack must never integrate on a second clock in layout.
	auto* AddedRack = GetWorld()->SpawnActorDeferred<ARackThermalActor>(ARackThermalActor::StaticClass(), Proposed,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!AddedRack) return nullptr;
	AddedRack->bDrivenExternally = true;
	{
		AddedRack->ApplyGb200Nvl72Preset();
		// Placement plan: inherit the current rack heat/load controls, never a serialized copy of its physical params.
		for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It)
		{
			if (*It == AddedRack || It->Input.ComponentHeatW.Num() != AddedRack->Input.ComponentHeatW.Num()
				|| It->Params.Slots.Num() != AddedRack->Params.Slots.Num()) continue;
			AddedRack->Input.ComponentHeatW = It->Input.ComponentHeatW;
			AddedRack->Input.Utilization = It->Input.Utilization;
			AddedRack->Input.SlotUtilization = It->Input.SlotUtilization;
			break;
		}
		AddedRack->RackSizeM = SizeM;
		AddedRack->bUseGjModels = true;
	}
	AddedRack->LayoutId = FGuid::NewGuid().ToString();
	AddedRack->FinishSpawning(Proposed);
	PushLayoutCommand(CaptureRackPresence(AddedRack, false));
	AddedRack->PauseSim();
	AddedRack->RefreshVisuals();
	CancelRackAddition(); // One click creates exactly one rack, not a repeat-placement tool.
	LayoutSelection.Reset();
	LayoutSelection.Add(AddedRack);
	FindTargetActors();
	// Existing discovery owns rows, branch pipes and CDU assignment. Simulation entry reuses these
	// rows, resets the air domain and binds every rack's heat/slot-air targets at t=0.
	for (TActorIterator<ADataCenterRoomActor> It(GetWorld()); It; ++It) It->RebuildRoomNow();
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] GB200 NVL72 추가: %s %s yaw %.0f deg"),
		*AddedRack->GetName(), *AddedRack->GetActorLocation().ToCompactString(), AddedRack->GetActorRotation().Yaw);
	return AddedRack;
}

ACoolingDistributionUnitActor* ADataCenterDebugController::ConfirmCduAddition()
{
 FTransform Proposed; FVector SizeM;
 if (!bAddingCdu || !GetWorld() || !GetWorld()->IsGameWorld()
  || !GetRackAdditionPreview(Proposed, SizeM) || IsRackAdditionBlocked()) return nullptr;
 const FString Id = FGuid::NewGuid().ToString();
 const FName Name = MakeUniqueObjectName(GetWorld()->PersistentLevel, ACoolingDistributionUnitActor::StaticClass(),
  FName(*(TEXT("AddedCdu_") + Id)));
 FLayoutTransformCommand Command;
 Command.Cdu = FLayoutCduMember{Id, Name, Proposed, GetDefault<ACoolingDistributionUnitActor>()->CaptureLayoutSettings()};
 if (!RestoreCduPresence(Command, true)) return nullptr;
 PushLayoutCommand(Command); CancelRackAddition();
 auto* Cdu = VrgCduLifecycle::Find(GetWorld(), Id); LayoutSelection.Add(Cdu);
 UE_LOG(LogTemp, Display, TEXT("[VrgLayout] CDU 추가: %s %s yaw %.0f deg"), *Cdu->GetName(),
  *Cdu->GetActorLocation().ToCompactString(), Cdu->GetActorRotation().Yaw);
 return Cdu;
}

ASpineCabinetActor* ADataCenterDebugController::ConfirmSpineAddition()
{
 FTransform Proposed; FVector SizeM;
 if (!bAddingSpine || !GetWorld() || !GetWorld()->IsGameWorld()
  || !GetRackAdditionPreview(Proposed, SizeM) || IsRackAdditionBlocked()) return nullptr;
 FLayoutTransformCommand Command;
 const bool bDuplicating = !SpineDuplicationMembers.IsEmpty();
 for (int32 I = 0; I < GetRackAdditionPreviewCount(); ++I)
 {
  GetRackAdditionPreview(Proposed, SizeM, I);
  const FString Id = FGuid::NewGuid().ToString();
  const FName Name = MakeUniqueObjectName(GetWorld()->PersistentLevel, ASpineCabinetActor::StaticClass(),
   FName(*((bDuplicating ? TEXT("DuplicatedSpine_") : TEXT("AddedSpine_")) + Id)));
  // Defaults remain owned by the actor; duplicates carry authored settings only.
  Command.Spines.Add({Id, Name, Proposed, bDuplicating ? SpineDuplicationMembers[I].Settings
   : GetDefault<ASpineCabinetActor>()->CaptureLayoutSettings()});
 }
 if (!RestoreSpinePresence(Command, true)) return nullptr;
 PushLayoutCommand(Command);
 CancelRackAddition();
 for (const auto& Member : Command.Spines) LayoutSelection.Add(VrgSpineLifecycle::Find(GetWorld(), Member.Id));
 auto* Spine = VrgSpineLifecycle::Find(GetWorld(), Command.Spines[0].Id);
 UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 스파인 %s: %d대 | 앵커 %s %s yaw %.0f deg"),
  bDuplicating ? TEXT("복제") : TEXT("추가"), Command.Spines.Num(), *Spine->GetName(), *Spine->GetActorLocation().ToCompactString(), Spine->GetActorRotation().Yaw);
 return Spine;
}

bool ADataCenterDebugController::TryDeleteLayoutRack()
{
	UWorld* CurrentWorld = GetWorld();
	if (!CurrentWorld || !CurrentWorld->IsGameWorld() || !IsLayoutMode() || !IsCameraInputAvailable()
		|| bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	if (Selected.IsEmpty()) return false;
	TArray<ARackThermalActor*> SelectedRacks;
	for (AActor* Actor : Selected)
	{
		auto* Rack = Cast<ARackThermalActor>(Actor);
		if (!IsValid(Rack) || Rack->GetWorld() != CurrentWorld) return false;
		SelectedRacks.Add(Rack);
	}
	FLayoutTransformCommand Command;
	Command.bRackPresence = true; Command.bBeforePresent = true;
	for (auto* Rack : SelectedRacks) Command.Racks.Append(CaptureRackPresence(Rack, true).Racks);
	if (!VrgRackLifecycle::RemoveBatch(SelectedRacks)) return false;
	PushLayoutCommand(Command);
	VrgRackLifecycle::Rebuild(CurrentWorld);
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 랙 삭제: %d대 | 남은 랙 %d대"), SelectedRacks.Num(), Racks.Num());
	return true;
}

void ADataCenterDebugController::OnDeleteLayoutEquipment()
{
	if (!TryDeleteLayoutRack() && !TryDeleteLayoutSpine() && !TryDeleteLayoutCdu())
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 삭제 거절: 랙 선택 전체 또는 네이티브 스파인 선택 전체 또는 네이티브 CDU 한 대와 배치 대기 상태가 필요합니다."));
}

bool ADataCenterDebugController::TryDeleteLayoutCdu()
{
 UWorld* World = GetWorld();
 if (!World || !World->IsGameWorld() || !IsLayoutMode() || !IsCameraInputAvailable()
  || bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
 const auto Selected = GetSelectedLayoutActors();
 if (Selected.Num() != 1) return false;
 auto* Cdu = Cast<ACoolingDistributionUnitActor>(Selected[0]);
 if (!IsValid(Cdu) || Cdu->GetWorld() != World || Cdu->GetClass() != ACoolingDistributionUnitActor::StaticClass()) return false;
 FLayoutTransformCommand Command; Command.bBeforePresent = true;
 Command.Cdu = FLayoutCduMember{VrgCduLifecycle::Identity(Cdu), Cdu->GetFName(), Cdu->GetActorTransform(),
  Cdu->CaptureLayoutSettings(), Cdu->bRuntimeLayoutCdu};
 if (!RestoreCduPresence(Command, true)) return false;
 PushLayoutCommand(MoveTemp(Command));
 UE_LOG(LogTemp, Display, TEXT("[VrgLayout] CDU 삭제: 1대"));
 return true;
}

bool ADataCenterDebugController::TryDeleteLayoutSpine()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !IsLayoutMode() || !IsCameraInputAvailable()
		|| bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const auto Selected = GetSelectedLayoutActors();
	if (Selected.IsEmpty()) return false;
	FLayoutTransformCommand Command;
	Command.bBeforePresent = true;
	for (auto* Actor : Selected)
	{
		auto* Spine = Cast<ASpineCabinetActor>(Actor);
		if (!IsValid(Spine) || Spine->GetWorld() != World || Spine->GetClass() != ASpineCabinetActor::StaticClass()) return false;
		Command.Spines.Add({VrgSpineLifecycle::Identity(Spine), Spine->GetFName(), Spine->GetActorTransform(),
			Spine->CaptureLayoutSettings(), Spine->bRuntimeLayoutSpine});
	}
	if (!RestoreSpinePresence(Command, true)) return false;
	PushLayoutCommand(MoveTemp(Command));
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 스파인 삭제: %d대"), Selected.Num());
	return true;
}

bool ADataCenterDebugController::TryLayoutMove(AActor* Actor, const FVector& TargetCm)
{
	if (bAddingRack || bMarqueeActive) return false;
	if (GetSelectedLayoutActors().Num() > 1) return false;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || !IsValid(Actor) || Actor->GetWorld() != GetWorld() || TargetCm.ContainsNaN()) return false;
	FTransform Proposed = Actor->GetActorTransform();
	Proposed.SetLocation(TargetCm);
	if (IsLayoutPlacementBlocked(Actor, Proposed)) return false;
	const FTransform Before = Actor->GetActorTransform();
	const bool bMoved = Actor->SetActorLocation(TargetCm, false);
	if (bMoved && !Actor->GetActorTransform().Equals(Before, 0.0)) ClearLayoutHistory(); // Legacy G is not transactional.
	return bMoved;
}

void ADataCenterDebugController::ClearLayoutHistory()
{
	LayoutUndo.Reset();
	LayoutRedo.Reset();
}

void ADataCenterDebugController::ResetLayoutEditsForLoad()
{
	CancelRackAddition();
	EndLayoutDrag(false);
	EndLayoutMarquee(false);
	Grabbed.Reset();
	ClearLayoutHistory();
}

void ADataCenterDebugController::RecordLayoutTransform(AActor* Actor, const FTransform& Before)
{
	RecordLayoutTransforms({Actor}, {Before});
}

void ADataCenterDebugController::RecordLayoutTransforms(const TArray<AActor*>& Actors, const TArray<FTransform>& Before)
{
	FLayoutTransformCommand Command;
	bool bChanged = false;
	for (int32 I = 0; I < Actors.Num(); ++I)
	{
		const FTransform After = Actors[I]->GetActorTransform();
		bChanged |= !After.Equals(Before[I], 0.0);
		auto* Rack = Cast<ARackThermalActor>(Actors[I]);
		if (Rack && Rack->LayoutId.IsEmpty()) Rack->LayoutId = VrgRackLifecycle::Identity(Rack);
		auto* Spine = Cast<ASpineCabinetActor>(Actors[I]);
  Command.Members.Add({Actors[I], Before[I], After, Rack ? Rack->LayoutId : FString(),
   Spine ? VrgSpineLifecycle::Identity(Spine) : FString(),
   Cast<ACoolingDistributionUnitActor>(Actors[I]) ? VrgCduLifecycle::Identity(CastChecked<ACoolingDistributionUnitActor>(Actors[I])) : FString()});
	}
	if (!bChanged) return;
	PushLayoutCommand(MoveTemp(Command));
}

void ADataCenterDebugController::PushLayoutCommand(FLayoutTransformCommand Command)
{
	LayoutRedo.Reset();
	if (LayoutUndo.Num() == 100) LayoutUndo.RemoveAt(0); // Placement plan: commands, not individual members.
	LayoutUndo.Add(MoveTemp(Command));
}

bool ADataCenterDebugController::RestoreLayoutTransform(bool bRedo)
{
	if (!IsLayoutMode() || !IsCameraInputAvailable() || bAddingRack || IsLayoutDragging() || bMarqueeActive
		|| Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	TArray<FLayoutTransformCommand>& From = bRedo ? LayoutRedo : LayoutUndo;
	TArray<FLayoutTransformCommand>& To = bRedo ? LayoutUndo : LayoutRedo;
	if (From.IsEmpty()) return false;
	const FLayoutTransformCommand Command = From.Last();
	if (Command.Cdu.IsSet())
	{
		if (!RestoreCduPresence(Command, bRedo)) return false;
		From.Pop(); To.Add(Command); return true;
	}
	if (!Command.Spines.IsEmpty())
	{
		if (!RestoreSpinePresence(Command, bRedo)) return false;
		From.Pop(); To.Add(Command);
		return true;
	}
	if (Command.bRackPresence)
	{
		if (!RestoreRackPresence(Command, bRedo)) return false;
		From.Pop(); To.Add(Command);
		return true;
	}
	TArray<AActor*> Actors;
	TArray<FTransform> Expected, Targets;
	for (const FLayoutTransformMember& Member : Command.Members)
	{
		AActor* Actor = Member.Actor.Get();
		const FTransform& Current = bRedo ? Member.Before : Member.After;
		if (!IsValid(Actor) || Actor->GetWorld() != GetWorld() || !Actor->GetActorTransform().Equals(Current)
			|| !Actor->GetRootComponent() || Actor->GetRootComponent()->Mobility != EComponentMobility::Movable) return false;
		Actors.Add(Actor); Expected.Add(Current); Targets.Add(bRedo ? Member.After : Member.Before);
	}
	TArray<FLayoutFootprint> Footprints;
	for (int32 I = 0; I < Actors.Num(); ++I)
	{
		if (IsLayoutPlacementBlocked(Actors[I], Targets[I], &Actors)) return false;
		FLayoutFootprint Candidate;
		if (!MakeLayoutFootprint(Actors[I], Targets[I], Candidate)) return false;
		for (const FLayoutFootprint& Other : Footprints) if (FootprintsConflict(Candidate, Other)) return false;
		Footprints.Add(Candidate);
	}
	for (int32 I = 0; I < Actors.Num(); ++I)
	{
		if (!Actors[I]->SetActorTransform(Targets[I], false))
		{
			for (int32 J = 0; J <= I; ++J) Actors[J]->SetActorTransform(Expected[J], false);
			return false;
		}
	}
	From.Pop(); To.Add(Command);
	for (TActorIterator<ADataCenterRoomActor> It(GetWorld()); It; ++It) It->RebuildRoomNow();
	if (ADataCenterSimulationActor* Sim = FindSimActor()) Sim->RebuildCoupling();
	return true;
}

void ADataCenterDebugController::RefreshLayoutEquipmentReferences(bool bKeepSelection)
{
	if (!bKeepSelection)
	{
		LayoutSelection.Reset(); TargetRack.Reset(); HoveredPlaceable.Reset(); Grabbed.Reset();
		SelectedSledIndex = INDEX_NONE;
	}
	FindTargetActors();
}

ADataCenterDebugController::FLayoutTransformCommand ADataCenterDebugController::CaptureRackPresence(ARackThermalActor* Rack, bool bBeforePresent)
{
	FLayoutTransformCommand Command;
	Command.bRackPresence = true; Command.bBeforePresent = bBeforePresent;
	Command.Racks.Add({VrgRackLifecycle::Identity(Rack), Rack->GetFName(), Rack->GetClass(),
		Rack->GetActorTransform(), Rack->CaptureLayoutSettings()});
	return Command;
}

void ADataCenterDebugController::RebindLayoutRack(ARackThermalActor* Rack, ARackThermalActor* Previous)
{
	// Only sanctioned recreation rebinds old weak references; arbitrary external replacement remains rejected.
	for (auto* Stack : {&LayoutUndo, &LayoutRedo})
		for (FLayoutTransformCommand& Command : *Stack)
			for (FLayoutTransformMember& Member : Command.Members)
				if (Member.RackId == VrgRackLifecycle::Identity(Rack)) Member.Actor = Rack;
	// Failed deletion rollback must preserve selection/hover, including stale weak pointers.
	if (Previous)
	{
		const TWeakObjectPtr<AActor> Old(Previous);
		for (auto& Selected : LayoutSelection) if (Selected == Old) Selected = Rack;
		if (TargetRack == TWeakObjectPtr<ARackThermalActor>(Previous)) TargetRack = Rack;
		if (HoveredPlaceable == Old) HoveredPlaceable = Rack;
	}
}

bool ADataCenterDebugController::RestoreRackPresence(const FLayoutTransformCommand& Command, bool bRedo)
{
	if (!GetWorld()->IsGameWorld() || Command.Racks.IsEmpty()) return false;
	const bool bTargetPresent = bRedo ? !Command.bBeforePresent : Command.bBeforePresent;
	TArray<ARackThermalActor*> Present;
	TArray<FLayoutFootprint> Footprints;
	TSet<FString> Ids; TSet<FName> Names;
	// Validate every member before changing the world. Both external and group-internal
	// overlaps use the same total 1 cm placement clearance as drag/rotation.
	for (const FLayoutRackMember& Member : Command.Racks)
	{
		if (!Member.Class.IsValid() || Ids.Contains(Member.Id) || Names.Contains(Member.Name)) return false;
		Ids.Add(Member.Id); Names.Add(Member.Name);
		auto* Rack = VrgRackLifecycle::Find(GetWorld(), Member.Id);
		if (bTargetPresent)
		{
			if (Rack || FindObjectFast<UObject>(GetWorld()->PersistentLevel, Member.Name)
				|| IsLayoutBoxBlocked(GetWorld(), Member.Settings.RackSizeM, Member.Transform)) return false;
			FLayoutFootprint Candidate;
			if (!MakeLayoutFootprint(Member.Settings.RackSizeM, Member.Transform, Candidate)) return false;
			for (const auto& Other : Footprints) if (FootprintsConflict(Candidate, Other)) return false;
			Footprints.Add(Candidate);
		}
		else
		{
			if (!Rack || Rack->GetClass() != Member.Class.Get() || Rack->GetFName() != Member.Name
				|| !Rack->GetActorTransform().Equals(Member.Transform)) return false;
			const FRackLayoutSettings Current = Rack->CaptureLayoutSettings();
			if (!FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Current, &Member.Settings, 0)) return false;
			Present.Add(Rack);
		}
	}
	if (bTargetPresent)
	{
		for (const FLayoutRackMember& Member : Command.Racks)
		{
			auto* Rack = VrgRackLifecycle::Spawn(GetWorld(), Member.Name, Member.Id, Member.Class.Get(), Member.Transform, Member.Settings);
			if (!Rack)
			{
				if (!Present.IsEmpty()) verify(VrgRackLifecycle::RemoveBatch(Present));
				return false;
			}
			Present.Add(Rack);
		}
		for (auto* Rack : Present) RebindLayoutRack(Rack);
	}
	else if (!VrgRackLifecycle::RemoveBatch(Present)) return false;
	VrgRackLifecycle::Rebuild(GetWorld());
	return true;
}

bool ADataCenterDebugController::RestoreCduPresence(const FLayoutTransformCommand& Command, bool bRedo)
{
 if (!GetWorld()->IsGameWorld() || !Command.Cdu.IsSet()) return false;
 const auto& M = Command.Cdu.GetValue();
 if (M.Id.IsEmpty() || !M.Settings.IsValid()) return false;
 auto* Cdu = VrgCduLifecycle::Find(GetWorld(), M.Id);
 const bool bTargetPresent = bRedo ? !Command.bBeforePresent : Command.bBeforePresent;
 if (bTargetPresent)
 {
  if (Cdu || FindObjectFast<UObject>(GetWorld()->PersistentLevel, M.Name)
   || IsLayoutBoxBlocked(GetWorld(), M.Settings.SizeM, M.Transform)) return false;
  Cdu = VrgCduLifecycle::Spawn(GetWorld(), M.Name, M.Id, M.Transform, M.Settings, M.bRuntime);
  if (!Cdu) return false;
  NotifyLayoutCduRecreated(Cdu);
 }
 else
 {
  if (!Cdu || Cdu->GetFName() != M.Name || Cdu->bRuntimeLayoutCdu != M.bRuntime || !Cdu->GetActorTransform().Equals(M.Transform)
   || !(Cdu->CaptureLayoutSettings() == M.Settings) || !VrgCduLifecycle::Remove(Cdu)) return false;
 }
 VrgRackLifecycle::Rebuild(GetWorld()); return true;
}

void ADataCenterDebugController::NotifyLayoutCduRecreated(ACoolingDistributionUnitActor* Cdu)
{
 for (auto* Stack : {&LayoutUndo, &LayoutRedo}) for (auto& Command : *Stack) for (auto& M : Command.Members)
  if (M.CduId == VrgCduLifecycle::Identity(Cdu)) M.Actor = Cdu;
}

bool ADataCenterDebugController::RestoreSpinePresence(const FLayoutTransformCommand& Command, bool bRedo)
{
	if (!GetWorld()->IsGameWorld() || Command.Spines.IsEmpty()) return false;
	const bool bTargetPresent = bRedo ? !Command.bBeforePresent : Command.bBeforePresent;
	const FVector SizeM(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	TArray<ASpineCabinetActor*> Present; TArray<FLayoutFootprint> Footprints;
	TSet<FString> Ids; TSet<FName> Names;
	// Preflight all identities, configurations, names and external/internal overlaps.
	for (const auto& Member : Command.Spines)
	{
		if (Member.Id.IsEmpty() || !Member.Settings.IsValid() || Ids.Contains(Member.Id) || Names.Contains(Member.Name)) return false;
		Ids.Add(Member.Id); Names.Add(Member.Name);
		auto* Spine = VrgSpineLifecycle::Find(GetWorld(), Member.Id);
		if (bTargetPresent)
		{
			if (Spine || FindObjectFast<UObject>(GetWorld()->PersistentLevel, Member.Name)
				|| IsLayoutBoxBlocked(GetWorld(), SizeM, Member.Transform)) return false;
			FLayoutFootprint Candidate;
			if (!MakeLayoutFootprint(SizeM, Member.Transform, Candidate)) return false;
			for (const auto& Other : Footprints) if (FootprintsConflict(Candidate, Other)) return false;
			Footprints.Add(Candidate);
		}
		else
		{
			if (!Spine || Spine->GetClass() != ASpineCabinetActor::StaticClass() || Spine->GetFName() != Member.Name
				|| !Spine->GetActorTransform().Equals(Member.Transform) || Spine->bRuntimeLayoutSpine != Member.bRuntime
				|| !(Spine->CaptureLayoutSettings() == Member.Settings)) return false;
			Present.Add(Spine);
		}
	}
	if (bTargetPresent)
	{
		const auto DeletedBefore = GetWorld()->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced;
		for (const auto& Member : Command.Spines)
		{
			auto* Spine = VrgSpineLifecycle::Spawn(GetWorld(), Member.Name, Member.Id, Member.Transform, Member.Settings, Member.bRuntime);
			if (!Spine)
			{
				if (!Present.IsEmpty()) verify(VrgSpineLifecycle::RemoveBatch(Present));
				GetWorld()->GetSubsystem<UVrgSpineLayoutState>()->DeletedPlaced = DeletedBefore;
				return false;
			}
			Present.Add(Spine);
		}
		for (auto* Spine : Present) NotifyLayoutSpineRecreated(Spine);
	}
	else if (!VrgSpineLifecycle::RemoveBatch(Present)) return false;
	VrgRackLifecycle::Rebuild(GetWorld());
	return true;
}

void ADataCenterDebugController::NotifyLayoutSpineRecreated(ASpineCabinetActor* Spine, ASpineCabinetActor* Previous)
{
	// Only sanctioned recreation reconnects previous transform records, never an arbitrary replacement.
	for (auto* Stack : {&LayoutUndo, &LayoutRedo})
		for (auto& Command : *Stack) for (auto& Member : Command.Members)
			if (Member.SpineId == VrgSpineLifecycle::Identity(Spine)) Member.Actor = Spine;
	if (Previous)
	{
		const TWeakObjectPtr<AActor> Old(Previous);
		for (auto& Selected : LayoutSelection) if (Selected == Old) Selected = Spine;
		if (HoveredPlaceable == Old) HoveredPlaceable = Spine;
	}
}

bool ADataCenterDebugController::TryUndoLayout() { return RestoreLayoutTransform(false); }
bool ADataCenterDebugController::TryRedoLayout() { return RestoreLayoutTransform(true); }

bool ADataCenterDebugController::IsLayoutDragPreviewBlocked() const
{
	if (!IsLayoutDragging() || !bDragMoved) return false;
	TArray<AActor*> Members;
	for (const FLayoutDragMember& Member : DragMembers)
	{
		if (!Member.Actor.IsValid() || Member.Actor->GetWorld() != GetWorld()
			|| !Member.Actor->GetActorTransform().Equals(Member.Start, 0.0)) return true;
		Members.Add(Member.Actor.Get());
	}
	TArray<FLayoutFootprint> Footprints;
	for (const FLayoutDragMember& Member : DragMembers)
	{
		FTransform Proposed;
		GetLayoutDragPreviewTransform(Member.Actor.Get(), Proposed);
		if (IsLayoutPlacementBlocked(Member.Actor.Get(), Proposed, &Members)) return true;
		FLayoutFootprint Candidate;
		if (!MakeLayoutFootprint(Member.Actor.Get(), Proposed, Candidate)) return true;
		// Ignore members' old locations, but still check their proposed footprints against each other.
		for (const FLayoutFootprint& Other : Footprints)
			if (FootprintsConflict(Candidate, Other)) return true;
		Footprints.Add(Candidate);
	}
	return false;
}

bool ADataCenterDebugController::GetLayoutDragPreviewTransform(const AActor* Actor, FTransform& Out) const
{
	for (const FLayoutDragMember& Member : DragMembers)
		if (Member.Actor.Get() == Actor && IsValid(Actor))
		{
			Out = Member.Start;
			Out.SetLocation(Member.Start.GetLocation() + (DragPreviewCm - DragStartTransform.GetLocation()));
			if (bDragRowSnapped && DragMembers.Num() == 1) Out.SetRotation(DragPreviewRotation);
			return true;
		}
	return false;
}

bool ADataCenterDebugController::TryLayoutRotation(AActor* Actor, const FRotator& Rotation, bool bRecordHistory)
{
	if (bAddingRack || bMarqueeActive) return false;
	if (GetSelectedLayoutActors().Num() > 1) return false;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsLayoutDragging() || !IsValid(Actor)
		|| Actor->GetWorld() != GetWorld() || Rotation.ContainsNaN()) return false;
	FTransform Proposed = Actor->GetActorTransform();
	Proposed.SetRotation(Rotation.Quaternion());
	if (IsLayoutPlacementBlocked(Actor, Proposed))
	{
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 회전 거절: %s (장비 상자/1 cm 여유)"), *Actor->GetName());
		return false;
	}
	const FTransform Before = Actor->GetActorTransform();
	const bool bRotated = Actor->SetActorRotation(Rotation);
	if (bRotated)
	{
		if (bRecordHistory) RecordLayoutTransform(Actor, Before);
		else if (!Actor->GetActorTransform().Equals(Before, 0.0)) ClearLayoutHistory(); // Legacy T is not recorded.
	}
	return bRotated;
}

bool ADataCenterDebugController::TryRotateLayoutSelection()
{
	if (!IsLayoutMode() || !IsCameraInputAvailable() || bAddingRack || IsLayoutDragging() || bMarqueeActive
		|| IsInputKeyDown(EKeys::RightMouseButton) || Grabbed.IsValid()) return false;
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	if (Selected.Num() < 2) return false;
	TArray<FTransform> Starts, Proposed;
	FBox2D Bounds(ForceInit);
	for (AActor* Actor : Selected)
	{
		FVector SizeM;
		if (!PlaceableBoxM(Actor, SizeM) || !Actor->GetRootComponent()
			|| Actor->GetRootComponent()->Mobility != EComponentMobility::Movable) return false;
		Starts.Add(Actor->GetActorTransform());
		FLayoutFootprint Footprint;
		if (!MakeLayoutFootprint(Actor, Starts.Last(), Footprint)) return false;
		for (int32 C = 0; C < 8; ++C)
		{
			FVector2D Point = Footprint.Centre;
			for (int32 Axis = 0; Axis < 3; ++Axis) Point += Footprint.HalfEdges[Axis] * (C & (1 << Axis) ? 1.0 : -1.0);
			Bounds += Point;
		}
	}
	const FVector2D Pivot = Bounds.GetCenter();
	// User-requested quarter turn about world Z, with the projected group AABB centre as pivot.
	const FQuat QuarterTurn(FVector::UpVector, UE_DOUBLE_PI * .5);
	TArray<FLayoutFootprint> Footprints;
	for (int32 I = 0; I < Selected.Num(); ++I)
	{
		FTransform Transform = Starts[I];
		const FVector Position = Transform.GetLocation();
		Transform.SetLocation(FVector(Pivot.X - (Position.Y - Pivot.Y), Pivot.Y + (Position.X - Pivot.X), Position.Z));
		Transform.SetRotation(QuarterTurn * Transform.GetRotation());
		if (IsLayoutPlacementBlocked(Selected[I], Transform, &Selected)) return false;
		FLayoutFootprint Candidate;
		if (!MakeLayoutFootprint(Selected[I], Transform, Candidate)) return false;
		for (const FLayoutFootprint& Other : Footprints) if (FootprintsConflict(Candidate, Other)) return false;
		Footprints.Add(Candidate);
		Proposed.Add(Transform);
	}
	for (int32 I = 0; I < Selected.Num(); ++I)
	{
		if (!Selected[I]->SetActorTransform(Proposed[I], false))
		{
			for (int32 J = 0; J <= I; ++J) Selected[J]->SetActorTransform(Starts[J], false);
			return false;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 그룹 90도 회전 확정: %d대, 중심 (%.3f, %.3f) cm"), Selected.Num(), Pivot.X, Pivot.Y);
	RecordLayoutTransforms(Selected, Starts);
	return true;
}

void ADataCenterDebugController::OnFocusEquipment()
{
	CancelRackAddition();
	if (!IsLayoutMode() || !IsCameraInputAvailable()) return;
	// Click selection persists when the view changes. Release legacy G movement before framing.
	EndLayoutMarquee(false);
	EndLayoutDrag(false);
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	Grabbed.Reset();
	if (TryFrameEquipment(nullptr, Selected.IsEmpty() ? nullptr : &Selected)) bFramedMoveLogged = false;
}

ADataCenterSimulationActor* ADataCenterDebugController::FindSimActor() const
{
	UWorld* World = GetWorld();
	if (World == nullptr) return nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) return *It;
	return nullptr;
}

void ADataCenterDebugController::OnSelectLayoutEquipment()
{
	LogLayoutInputState(TEXT("EnhancedSelectBegin"));
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) return;
	FVector EyeCm, Direction;
	// Failed deprojection (e.g. no viewport) is not an intentional click on empty space.
	if (DeprojectMousePositionToWorld(EyeCm, Direction))
	{
		if (bAddingRack)
		{
			UpdateRackAdditionFromCursor();
			ConfirmRackAddition();
			return;
		}
		const bool bShift = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
		if (!PickPlaceableAlongRay(GetWorld(), EyeCm, Direction, TNumericLimits<double>::Max()))
		{
			float X = 0, Y = 0;
			if (GetMousePosition(X, Y)) BeginLayoutMarquee(FVector2D(X, Y), bShift);
			return;
		}
		if (bShift)
		{
			SelectLayoutAlongRay(EyeCm, Direction, true);
			return; // Shift changes selection only, never arms a drag.
		}
		BeginLayoutDragAlongRay(EyeCm, Direction);
		float MouseX = 0.0f, MouseY = 0.0f;
		GetMousePosition(MouseX, MouseY);
		DragStartMouse = FVector2D(MouseX, MouseY);
		LogLayoutInputState(TEXT("EnhancedSelectArmed"));
	}
}

void ADataCenterDebugController::BeginLayoutDragAlongRay(const FVector& EyeCm, const FVector& Direction)
{
	if (bAddingRack) return;
	EndLayoutMarquee(false);
	EndLayoutDrag(false);
	SelectLayoutAlongRay(EyeCm, Direction, false, true);
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return;
	AActor* Selected = PickPlaceableAlongRay(GetWorld(), EyeCm, Direction.GetSafeNormal(), TNumericLimits<double>::Max());
	if (!Selected || !FloorPointSnapped(EyeCm, Direction.GetSafeNormal(), Selected->GetActorLocation().Z, 0.0, DragStartFloorCm)) return;
	DragStartTransform = Selected->GetActorTransform();
	DragAnchor = Selected;
	for (AActor* Member : GetSelectedLayoutActors()) DragMembers.Add({ Member, Member->GetActorTransform() });
	DragPreviewCm = Selected->GetActorLocation();
	DragRawPreviewCm = DragPreviewCm;
	DragPreviewRotation = DragStartTransform.GetRotation();
	bDragRowSnapped = bDragDisableRowSnap = false;
	bDragMoved = false;
	bDragMouseThresholdPassed = false;
}

void ADataCenterDebugController::UpdateLayoutDragAlongRay(const FVector& EyeCm, const FVector& Direction, bool bDisableRowSnap)
{
	if (!IsLayoutDragging()) return;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton))
	{
		EndLayoutDrag(false);
		return;
	}
	if (EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return;
	FVector FloorCm;
	if (!FloorPointSnapped(EyeCm, Direction.GetSafeNormal(), DragStartTransform.GetLocation().Z, 0.0, FloorCm)) return;
	const FVector DeltaCm = FloorCm - DragStartFloorCm;
	if (!bDragMoved && DeltaCm.IsNearlyZero()) return; // A click must not snap an off-grid actor.
	DragRawPreviewCm = DragStartTransform.GetLocation() + DeltaCm;
	bDragDisableRowSnap = bDisableRowSnap;
	bDragMoved = true;
	RefreshLayoutRowSnap();
}

void ADataCenterDebugController::RefreshLayoutRowSnap()
{
	DragPreviewCm = DragRawPreviewCm;
	DragPreviewRotation = DragStartTransform.GetRotation();
	bDragRowSnapped = false;
	// User-selected placement grid, not a measured physical constant. Convert m only at the actor boundary.
	if (FMath::IsFinite(PlaceSnapM) && PlaceSnapM > 0.0)
	{
		const double StepCm = PlaceSnapM * 100.0;
		DragPreviewCm.X = FMath::RoundToDouble(DragPreviewCm.X / StepCm) * StepCm;
		DragPreviewCm.Y = FMath::RoundToDouble(DragPreviewCm.Y / StepCm) * StepCm;
	}
	DragPreviewCm.Z = DragStartTransform.GetLocation().Z;
	if (DragMembers.IsEmpty() || bDragDisableRowSnap
		|| IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt)) return;
	const ARackThermalActor* Moving = Cast<ARackThermalActor>(DragAnchor.Get());
	if (!Moving || DragRawPreviewCm.ContainsNaN()) return;
	const bool bGroup = DragMembers.Num() > 1;
	TArray<ARackThermalActor*> MovingRacks;
	for (const FLayoutDragMember& Member : DragMembers)
	{
		auto* Rack = Cast<ARackThermalActor>(Member.Actor.Get());
		if (!IsValid(Rack) || Member.Start.ContainsNaN()) return; // Mixed selections keep the original group drag.
		MovingRacks.Add(Rack);
	}
	double BestDistanceSq = TNumericLimits<double>::Max();
	for (TActorIterator<ADataCenterRoomActor> It(GetWorld()); It; ++It)
	{
		const auto* CurrentRoom = *It;
		for (const FRackPlacementRow& Row : CurrentRoom->GetRackRowsForPlacement(Moving, &MovingRacks))
		{
			if (Row.Racks.Num() < 2) continue; // No inferred pitch from a lone rack or configured spawn pitch.
			TArray<double> Gaps;
			for (int32 I = 1; I < Row.Racks.Num(); ++I)
			{
				const double Gap = FVector::DotProduct(Row.Racks[I]->GetActorLocation() - Row.Racks[I - 1]->GetActorLocation(), Row.AlongDir);
				if (FMath::IsFinite(Gap) && Gap > UE_DOUBLE_SMALL_NUMBER) Gaps.Add(Gap);
			}
			if (Gaps.IsEmpty()) continue;
			Gaps.Sort();
			// UI interpretation of irregular rows: median of observed positive neighbour gaps (not a physical input).
			const int32 Middle = Gaps.Num() / 2;
			const double PitchCm = Gaps.Num() % 2 ? Gaps[Middle] : .5 * (Gaps[Middle - 1] + Gaps[Middle]);
			const ARackThermalActor* Reference = Row.Racks[0];
			const double First = FVector::DotProduct(Reference->GetActorLocation(), Row.AlongDir);
			const double Last = FVector::DotProduct(Row.Racks.Last()->GetActorLocation(), Row.AlongDir);
			const double Along = FVector::DotProduct(DragRawPreviewCm, Row.AlongDir);
			bool bNearRow = false;
			for (const FLayoutDragMember& Member : DragMembers)
			{
				const double MemberAlong = Along + FVector::DotProduct(Member.Start.GetLocation() - DragStartTransform.GetLocation(), Row.AlongDir);
				if (MemberAlong >= First - PitchCm && MemberAlong <= Last + PitchCm) bNearRow = true;
			}
			if (!bNearRow) continue;
			FRotator Rotation = DragStartTransform.Rotator();
			if (!bGroup) Rotation.Yaw = (-Row.RearDir).Rotation().Yaw;
			FTransform Proposed = DragStartTransform;
			if (!bGroup) Proposed.SetRotation(Rotation.Quaternion());
			Proposed.SetLocation(DragRawPreviewCm);
			const FVector ReferenceRear = Reference->GetActorTransform().TransformPosition(FVector(-Reference->RackSizeM.X * 50.0, 0, 0));
			const FVector ProposedRear = Proposed.TransformPosition(FVector(-Moving->RackSizeM.X * 50.0, 0, 0));
			const double RearCorrection = FVector::DotProduct(ReferenceRear - ProposedRear, Row.RearDir);
			// Same effective minimum as DiscoverRows; metres convert only here at the actor/UI boundary.
			if (FMath::Abs(RearCorrection) > FMath::Max(CurrentRoom->RowGroupToleranceM, 0.05) * 100.0) continue;
			const double Slot = First + FMath::RoundToDouble((Along - First) / PitchCm) * PitchCm;
			Proposed.SetLocation(DragRawPreviewCm + Row.AlongDir * (Slot - Along) + Row.RearDir * RearCorrection);
			// Physical rear-face alignment alone may land in another origin bucket (e.g. very different depths).
			if (!CurrentRoom->AreTransformsInSameRackRow(Proposed, Reference->GetActorTransform())) continue;
			double GuideFirst = FMath::Min(First, Slot), GuideLast = FMath::Max(Last, Slot);
			if (bGroup)
			{
				// Rigid translation only: accept an existing slot lattice and common rear line.
				// UE floating-point comparison epsilon is a UI compatibility check, not a physical tolerance or fitted value.
				const double Epsilon = UE_DOUBLE_KINDA_SMALL_NUMBER;
				const FVector Facing = Reference->GetActorForwardVector().GetSafeNormal2D();
				const double Rear = FVector::DotProduct(ReferenceRear, Row.RearDir);
				auto Compatible = [&](const FTransform& Transform, const FVector& RackSizeM)
				{
					const double SlotIndex = (FVector::DotProduct(Transform.GetLocation(), Row.AlongDir) - First) / PitchCm;
					return Transform.GetRotation().GetForwardVector().GetSafeNormal2D().Equals(Facing, Epsilon)
						&& FMath::IsNearlyEqual(FVector::DotProduct(Transform.TransformPosition(FVector(-RackSizeM.X * 50.0, 0, 0)), Row.RearDir), Rear, Epsilon)
						&& FMath::IsNearlyEqual(SlotIndex, FMath::RoundToDouble(SlotIndex), Epsilon)
						&& CurrentRoom->AreTransformsInSameRackRow(Transform, Reference->GetActorTransform());
				};
				bool bCompatible = true;
				for (const auto* Rack : Row.Racks)
					if (!Compatible(Rack->GetActorTransform(), Rack->RackSizeM)) bCompatible = false;
				const FVector Translation = Proposed.GetLocation() - DragStartTransform.GetLocation();
				for (const FLayoutDragMember& Member : DragMembers)
				{
					FTransform MemberTarget = Member.Start;
					MemberTarget.AddToTranslation(Translation);
					if (!Compatible(MemberTarget, CastChecked<ARackThermalActor>(Member.Actor.Get())->RackSizeM)) bCompatible = false;
					const double MemberSlot = FVector::DotProduct(MemberTarget.GetLocation(), Row.AlongDir);
					GuideFirst = FMath::Min(GuideFirst, MemberSlot); GuideLast = FMath::Max(GuideLast, MemberSlot);
				}
				if (!bCompatible) continue;
			}
			const double DistanceSq = FVector::DistSquared2D(Proposed.GetLocation(), DragRawPreviewCm);
			if (DistanceSq >= BestDistanceSq) continue;
			BestDistanceSq = DistanceSq;
			DragPreviewCm = Proposed.GetLocation(); DragPreviewRotation = Proposed.GetRotation(); bDragRowSnapped = true;
			const double Rear = FVector::DotProduct(ReferenceRear, Row.RearDir);
			RowGuideStartCm = Row.RearDir * Rear + Row.AlongDir * GuideFirst;
			RowGuideEndCm = Row.RearDir * Rear + Row.AlongDir * GuideLast;
			RowGuideStartCm.Z = RowGuideEndCm.Z = DragStartTransform.GetLocation().Z;
		}
	}
}

bool ADataCenterDebugController::GetLayoutRowSnapGuide(FVector& StartCm, FVector& EndCm) const
{
	if (!IsLayoutDragging() || !bDragMoved || !bDragRowSnapped) return false;
	StartCm = RowGuideStartCm; EndCm = RowGuideEndCm;
	return true;
}

void ADataCenterDebugController::EndLayoutDrag(bool bCommit)
{
	const bool bHadDrag = IsLayoutDragging();
	if (bHadDrag) LogLayoutInputState(bCommit ? TEXT("DragEndCommitBegin") : TEXT("DragEndCancelBegin"));
	if (bCommit && bDragMoved && IsLayoutDragging() && IsLayoutMode() && IsCameraInputAvailable()
		&& !IsInputKeyDown(EKeys::RightMouseButton))
	{
		RefreshLayoutRowSnap(); // Re-read row positions/Alt at release without rebuilding any routing or meshes.
		bool bAllowed = !IsLayoutDragPreviewBlocked();
		// Preflight every member before changing any actor. No solver or routing tick can interleave on the game thread.
		for (const FLayoutDragMember& Member : DragMembers)
			if (!Member.Actor.IsValid() || !Member.Actor->GetRootComponent()
				|| Member.Actor->GetRootComponent()->Mobility != EComponentMobility::Movable) bAllowed = false;
		int32 Applied = 0;
		if (bAllowed)
		{
			for (const FLayoutDragMember& Member : DragMembers)
			{
				FTransform Proposed;
				if (!GetLayoutDragPreviewTransform(Member.Actor.Get(), Proposed)
					|| !(bDragRowSnapped ? Member.Actor->SetActorTransform(Proposed, false) : Member.Actor->SetActorLocation(Proposed.GetLocation(), false)))
				{
					bAllowed = false;
					break;
				}
				++Applied;
			}
		}
		if (bAllowed)
		{
			TArray<AActor*> Actors;
			TArray<FTransform> Starts;
			for (const FLayoutDragMember& Member : DragMembers) { Actors.Add(Member.Actor.Get()); Starts.Add(Member.Start); }
			RecordLayoutTransforms(Actors, Starts);
		}
		if (!bAllowed && Applied > 0)
		{
			for (int32 I = 0; I < Applied; ++I)
				if (DragMembers[I].Actor.IsValid()) DragMembers[I].Actor->SetActorTransform(DragMembers[I].Start, false);
		}
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 그룹 이동 %s: %d대"), bAllowed ? TEXT("확정") : TEXT("거절"), DragMembers.Num());
	}
	// Cancel needs no restoration: the live actor never left its original location.
	DragMembers.Reset();
	DragAnchor.Reset();
	bDragMoved = false;
	bDragRowSnapped = false;
	if (bHadDrag) LogLayoutInputState(bCommit ? TEXT("DragEndCommitDone") : TEXT("DragEndCancelDone"));
}

void ADataCenterDebugController::OnReleaseLayoutEquipment()
{
	LogLayoutInputState(TEXT("EnhancedReleaseBegin"));
	if (bMarqueeActive)
	{
		UpdateLayoutMarqueeFromCursor();
		EndLayoutMarquee(true);
		return;
	}
	UpdateLayoutDragFromCursor(); // Include the final cursor position, even between two ticks.
	EndLayoutDrag(true);
	LogLayoutInputState(TEXT("EnhancedReleaseDone"));
}

void ADataCenterDebugController::OnCancelLayoutDrag()
{
	LogLayoutInputState(TEXT("EnhancedCancelBegin"));
	CancelRackAddition();
	EndLayoutMarquee(false);
	EndLayoutDrag(false);
}

void ADataCenterDebugController::UpdateLayoutDragFromCursor()
{
	if (!IsLayoutDragging()) return;
	const UGameViewportClient* Client = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	float X = 0.0f, Y = 0.0f;
	int32 Width = 0, Height = 0;
	GetViewportSize(Width, Height);
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !Client || !Client->Viewport || !Client->Viewport->HasFocus()
		|| !GetMousePosition(X, Y) || X < 0 || Y < 0 || X >= Width || Y >= Height)
	{
		LogLayoutInputState(TEXT("DragFocusCancel"));
		EndLayoutDrag(false); // Losing focus / leaving the viewport never commits a stray move.
		return;
	}
	// UI gesture threshold (4 screen pixels), not a physical tolerance. Click jitter does not snap equipment.
	const bool bHadThreshold = bDragMouseThresholdPassed;
	bDragMouseThresholdPassed |= FVector2D::Distance(DragStartMouse, FVector2D(X, Y)) >= 4.0;
	if (!bHadThreshold && bDragMouseThresholdPassed) LogLayoutInputState(TEXT("DragMoveThreshold"));
	if (!bDragMouseThresholdPassed) return;
	FVector EyeCm, Direction;
	if (DeprojectMousePositionToWorld(EyeCm, Direction)) UpdateLayoutDragAlongRay(EyeCm, Direction);
}

TArray<AActor*> ADataCenterDebugController::GetSelectedLayoutActors() const
{
	TArray<AActor*> Selected;
	for (const TWeakObjectPtr<AActor>& Actor : LayoutSelection)
		if (Actor.IsValid()) Selected.Add(Actor.Get());
	return Selected;
}

AActor* ADataCenterDebugController::GetSelectedLayoutActor() const
{
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	return Selected.IsEmpty() ? nullptr : Selected[0];
}

bool ADataCenterDebugController::ProjectLayoutBoxToScreen(const AActor* Actor, const FMatrix& ViewProjection, const FIntRect& ViewRect, FBox2D& Out)
{
	Out = FBox2D(ForceInit);
	FVector SizeM;
	if (!IsValid(Actor) || !PlaceableBoxM(Actor, SizeM) || SizeM.ContainsNaN() || ViewProjection.ContainsNaN()
		|| ViewRect.Width() <= 0 || ViewRect.Height() <= 0 || Actor->GetActorTransform().ContainsNaN()) return false;
	FVector4 Clip[8];
	for (int32 C = 0; C < 8; ++C)
	{
		const FVector Local((C & 1 ? .5 : -.5) * SizeM.X * 100.0,
			(C & 2 ? .5 : -.5) * SizeM.Y * 100.0, C & 4 ? SizeM.Z * 100.0 : 0.0);
		Clip[C] = ViewProjection.TransformFVector4(FVector4(Actor->GetActorTransform().TransformPosition(Local), 1.0));
	}
	auto AddPoint = [&](const FVector4& P) {
		if (P.W <= 0 || P.ContainsNaN()) return;
		const FVector2D Pixel(ViewRect.Min.X + (.5 + .5 * P.X / P.W) * ViewRect.Width(),
			ViewRect.Min.Y + (.5 - .5 * P.Y / P.W) * ViewRect.Height());
		if (!Pixel.ContainsNaN()) Out += Pixel;
	};
	// UE reversed-Z uses near plane Z=W. Clip the 12 box edges before dividing by W, including boxes crossing the eye.
	for (int32 C = 0; C < 8; ++C)
	{
		const double A = Clip[C].W - Clip[C].Z;
		if (A >= 0) AddPoint(Clip[C]);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (C & (1 << Axis)) continue;
			const int32 Other = C | (1 << Axis);
			const double B = Clip[Other].W - Clip[Other].Z;
			if ((A < 0) != (B < 0)) AddPoint(Clip[C] + (Clip[Other] - Clip[C]) * (A / (A - B)));
		}
	}
	if (!Out.bIsValid || Out.Max.X < ViewRect.Min.X || Out.Min.X > ViewRect.Max.X
		|| Out.Max.Y < ViewRect.Min.Y || Out.Min.Y > ViewRect.Max.Y) return false;
	Out.Min.X = FMath::Max(Out.Min.X, double(ViewRect.Min.X)); Out.Max.X = FMath::Min(Out.Max.X, double(ViewRect.Max.X));
	Out.Min.Y = FMath::Max(Out.Min.Y, double(ViewRect.Min.Y)); Out.Max.Y = FMath::Min(Out.Max.Y, double(ViewRect.Max.Y));
	return true;
}

bool ADataCenterDebugController::SelectLayoutInScreenRect(const FBox2D& Rect, const FMatrix& ViewProjection, const FIntRect& ViewRect, bool bAdd)
{
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsLayoutDragging() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !Rect.bIsValid || Rect.Min.ContainsNaN() || Rect.Max.ContainsNaN() || ViewProjection.ContainsNaN()
		|| ViewRect.Width() <= 0 || ViewRect.Height() <= 0) return false;
	TArray<TWeakObjectPtr<AActor>> Selected;
	if (bAdd) for (AActor* Actor : GetSelectedLayoutActors()) Selected.Add(Actor);
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		FBox2D Bounds;
		if (ProjectLayoutBoxToScreen(*It, ViewProjection, ViewRect, Bounds) && Bounds.Intersect(Rect)) Selected.AddUnique(*It);
	}
	LayoutSelection = MoveTemp(Selected);
	Grabbed.Reset();
	return true;
}

void ADataCenterDebugController::BeginLayoutMarquee(const FVector2D& Start, bool bAdd)
{
	if (bAddingRack) return;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton) || Start.ContainsNaN()) return;
	EndLayoutDrag(false);
	bMarqueeActive = true; bMarqueeMoved = false; bMarqueeAdd = bAdd;
	MarqueeStart = MarqueeEnd = Start; // Selection is unchanged until release, so cancel requires no restoration.
	Grabbed.Reset();
}

void ADataCenterDebugController::UpdateLayoutMarquee(const FVector2D& Current)
{
	if (!bMarqueeActive) return;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) { EndLayoutMarquee(false); return; }
	if (Current.ContainsNaN()) return;
	MarqueeEnd = Current;
	bMarqueeMoved |= FVector2D::Distance(MarqueeStart, Current) >= 4.0; // Existing UI drag threshold, pixels.
}

bool ADataCenterDebugController::GetLayoutMarqueeRect(FBox2D& Out) const
{
	Out = FBox2D(ForceInit);
	if (!bMarqueeActive || !bMarqueeMoved) return false;
	Out += MarqueeStart; Out += MarqueeEnd;
	return true;
}

void ADataCenterDebugController::EndLayoutMarquee(bool bCommit)
{
	if (!bMarqueeActive) return;
	FBox2D Rect; GetLayoutMarqueeRect(Rect);
	const bool bMoved = bMarqueeMoved, bAdd = bMarqueeAdd;
	bMarqueeActive = false;
	if (!bCommit || !IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) return;
	if (!bMoved) { LayoutSelection.Reset(); return; } // Empty click clears even when Shift was held.
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	const UGameViewportClient* Client = GetWorld()->GetGameViewport();
	FSceneViewProjectionData Data;
	if (LocalPlayer && Client && Client->Viewport && LocalPlayer->GetProjectionData(Client->Viewport, Data))
		SelectLayoutInScreenRect(Rect, Data.ComputeViewProjectionMatrix(), Data.GetConstrainedViewRect(), bAdd);
}

void ADataCenterDebugController::UpdateLayoutMarqueeFromCursor()
{
	if (!bMarqueeActive) return;
	const UGameViewportClient* Client = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	float X = 0, Y = 0; int32 Width = 0, Height = 0; GetViewportSize(Width, Height);
	if (!Client || !Client->Viewport || !Client->Viewport->HasFocus() || !GetMousePosition(X, Y)
		|| X < 0 || Y < 0 || X >= Width || Y >= Height) { EndLayoutMarquee(false); return; }
	UpdateLayoutMarquee(FVector2D(X, Y));
}

void ADataCenterDebugController::SelectLayoutAlongRay(const FVector& EyeCm, const FVector& Direction, bool bToggle, bool bKeepSelected)
{
	if (bAddingRack) return;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) return;
	if (EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return;
	EndLayoutMarquee(false);
	// Layout can be viewed from well beyond the 15 m unit-undocking reach. Select the closest box
	EndLayoutDrag(false);
	// on the ray without that interaction-distance limit; this is UI geometry, not a physical parameter.
	AActor* Picked = PickPlaceableAlongRay(GetWorld(), EyeCm, Direction.GetSafeNormal(), TNumericLimits<double>::Max());
	Grabbed.Reset(); // Clicking is selection, never a continuation of the legacy G movement.
	LayoutSelection.RemoveAll([](const TWeakObjectPtr<AActor>& Actor) { return !Actor.IsValid(); });
	const TWeakObjectPtr<AActor> PickedRef(Picked);
	if (!Picked) LayoutSelection.Reset();
	else if (bToggle)
	{
		if (LayoutSelection.Contains(PickedRef)) LayoutSelection.Remove(PickedRef);
		else LayoutSelection.Add(PickedRef);
	}
	else if (!bKeepSelected || !LayoutSelection.Contains(PickedRef))
	{
		LayoutSelection.Reset();
		LayoutSelection.Add(PickedRef);
	}
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 선택 %d대: %s"), LayoutSelection.Num(), Picked ? *Picked->GetName() : TEXT("없음"));
}

bool ADataCenterDebugController::IsLayoutMode() const
{
	const ADataCenterSimulationActor* Sim = FindSimActor();
	return Sim != nullptr && Sim->GetSimMode() == ESimMode::Layout;
}

void ADataCenterDebugController::OnToggleSimMode()
{
	CancelRackAddition();
	EndLayoutMarquee(false);
	EndLayoutDrag(false);
	ADataCenterSimulationActor* Sim = FindSimActor();
	if (Sim == nullptr) return;
	if (Sim->GetSimMode() == ESimMode::Layout)
	{
		Grabbed.Reset();  // drop whatever is held where it is
		Sim->EnterSimulationMode();
	}
	else
	{
		Sim->EnterLayoutMode();
	}
}

void ADataCenterDebugController::OnGrabOrDrop()
{
	if (bAddingRack || bMarqueeActive) return;
	if (GetSelectedLayoutActors().Num() > 1) { Grabbed.Reset(); return; }
	if (IsLayoutDragging()) return;
	if (Grabbed.IsValid()) { Grabbed.Reset(); return; }
	if (!IsLayoutMode()) return;  // moving equipment is a layout-mode action
	FVector EyeCm; FRotator ViewRotation;
	GetPlayerViewPoint(EyeCm, ViewRotation);
	AActor* Picked = PickPlaceableAlongRay(GetWorld(), EyeCm, ViewRotation.Vector(), MaxSelectDistanceM * 100.0);
	FVector FloorCm;
	if (Picked == nullptr || !FloorPointSnapped(EyeCm, ViewRotation.Vector(), Picked->GetActorLocation().Z, 0.0, FloorCm)) return;
	Grabbed = Picked;
	GrabOffsetCm = Picked->GetActorLocation() - FloorCm;
	GrabOffsetCm.Z = 0.0;
}

void ADataCenterDebugController::OnCycleContainment()
{
	// S6: containment is part of the layout (none -> blanking -> blanking + cold-aisle roof); the next simulation entry builds it.
	ADataCenterSimulationActor* Sim = FindSimActor();
	if (Sim == nullptr || Sim->GetSimMode() != ESimMode::Layout) return;
	Sim->Containment = static_cast<EAisleContainment>((int32(Sim->Containment) + 1) % 3);
	UE_LOG(LogTemp, Display, TEXT("[VrgDebug] 컨테인먼트 %d (0 없음, 1 블랭킹, 2 블랭킹 + 콜드 아일 지붕) - 시뮬레이션 진입 때 적용"), int32(Sim->Containment));
}

void ADataCenterDebugController::OnRotatePlaceable()
{
	if (bAddingRack || bMarqueeActive) return;
	if (GetSelectedLayoutActors().Num() > 1) return;
	if (IsLayoutDragging()) return;
	if (!IsLayoutMode()) return;
	AActor* Target = Grabbed.IsValid() ? Grabbed.Get() : HoveredPlaceable.Get();
	if (Target == nullptr) return;
	FRotator R = Target->GetActorRotation();
	R.Yaw = FRotator::NormalizeAxis(FMath::RoundToDouble((R.Yaw + 90.0) / 90.0) * 90.0);
	TryLayoutRotation(Target, R);
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
	if (SelectedSledIndex != INDEX_NONE) Rack->GetSlotHeightRangeM(SelectedSledIndex, BottomM, HeightM);
	if (HeightM > 0.0)
	{
		const FVector SlotCentre = Rack->GetActorLocation() + Rack->GetActorUpVector() * (BottomM + .5 * HeightM) * 100.0;
		DrawDebugBox(World, SlotCentre, FVector(.5 * SizeCm.X + 2.0, .5 * SizeCm.Y + 2.0, .5 * HeightM * 100.0), Rotation, FColor::Yellow, false, 0.0f, 0, 2.0f);
	}
}

void ADataCenterDebugController::OnToggleSelectedSled()
{
	if (IsLayoutMode() || OpenPatchMaps > 0) return; // E flies up in layout; it must not undock hardware.
	// Only the unit being looked at. Nothing happens when the view ray hits no rack or no unit (blank, frame).
	ARackThermalActor* Rack = TargetRack.Get();
	if (Rack == nullptr || SelectedSledIndex == INDEX_NONE)
	{
		return;
	}
	Rack->ToggleSledDocked(SelectedSledIndex);

	// 방 액터는 랙 액터에서 도킹 상태를 직접 읽으므로 다시 풀라고만 알리면 된다.
	if (Room != nullptr)
	{
		Room->RebuildRoom();
	}
}

void ADataCenterDebugController::OnResetSim()
{
	if (IsLayoutMode())
	{
		if (bAddingRack)
		{
			TryRotateRackAddition();
			return;
		}
		if (!IsCameraInputAvailable() || IsLayoutDragging() || bMarqueeActive || IsInputKeyDown(EKeys::RightMouseButton) || Grabbed.IsValid()) return;
		if (GetSelectedLayoutActors().Num() > 1)
		{
			if (!TryRotateLayoutSelection()) { UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 그룹 회전 거절")); }
			return;
		}
		AActor* Selected = GetSelectedLayoutActor();
		if (!Selected) return;
		FRotator Rotation = Selected->GetActorRotation();
		// User-requested UI quarter turn; preserve a non-cardinal starting heading instead of snapping it.
		Rotation.Yaw = FRotator::NormalizeAxis(Rotation.Yaw + 90.0);
		if (TryLayoutRotation(Selected, Rotation, true))
		{
			UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 선택 회전: %s yaw %.2f deg"), *Selected->GetName(), Rotation.Yaw);
		}
		return;
	}
	// Simulation still restarts from the current layout; standalone racks retain their existing reset path.
	if (ADataCenterSimulationActor* Sim = FindSimActor())
	{
		if (Sim->GetSimMode() == ESimMode::Simulation) Sim->EnterSimulationMode();
		return;
	}
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
	RefreshInteractionInputMode();
	Super::Tick(DeltaTime);

	RefreshTargetsIfStale(DeltaTime);

	// Start view: retried for a few frames (the pawn is possessed after BeginPlay), then given up.
	if (bFrameEquipmentOnStart && StartFrameAttempts >= 0 && StartFrameAttempts < 30)
	{
		StartFrameAttempts = TryFrameEquipment() ? -1 : StartFrameAttempts + 1;
	}
	// Diagnostic (log only): the start view moved without input.
	if (StartFrameAttempts < 0 && !bFramedMoveLogged && GetPawn() != nullptr && FVector::Dist(GetPawn()->GetActorLocation(), FramedEyeCm) > 100.0)
	{
		bFramedMoveLogged = true;
		UE_LOG(LogTemp, Warning, TEXT("[VrgDebug] 시작 시점에서 카메라가 움직였다: %s -> %s (폰 %s)"), *FramedEyeCm.ToCompactString(),
			*GetPawn()->GetActorLocation().ToCompactString(), *GetPawn()->GetClass()->GetName());
	}

	// Layout mode: hover / move the equipment under the crosshair. The room re-routes pipes once it settles.
	if (IsLayoutMode())
	{
		FVector EyeCm; FRotator ViewRotation;
		GetPlayerViewPoint(EyeCm, ViewRotation);
		if (AActor* Held = Grabbed.Get())
		{
			FVector FloorCm;
			if (FloorPointSnapped(EyeCm, ViewRotation.Vector(), Held->GetActorLocation().Z, PlaceSnapM, FloorCm))
			{
				FVector Target = FloorCm + GrabOffsetCm;
				if (PlaceSnapM > 0.0)
				{
					const double S = PlaceSnapM * 100.0;
					Target.X = FMath::RoundToDouble(Target.X / S) * S;
					Target.Y = FMath::RoundToDouble(Target.Y / S) * S;
				}
				Target.Z = Held->GetActorLocation().Z;
				TryLayoutMove(Held, Target);
			}
		}
		HoveredPlaceable = Grabbed.IsValid() ? Grabbed : TWeakObjectPtr<AActor>(PickPlaceableAlongRay(GetWorld(), EyeCm, ViewRotation.Vector(), MaxSelectDistanceM * 100.0));
		FVector SizeM;
		if (const AActor* H = HoveredPlaceable.Get(); H != nullptr && PlaceableBoxM(H, SizeM))
		{
			const FVector HalfCm = SizeM * 50.0;
			DrawDebugBox(GetWorld(), H->GetActorLocation() + H->GetActorUpVector() * HalfCm.Z, HalfCm, H->GetActorQuat(),
				Grabbed.IsValid() ? FColor::Orange : FColor::Cyan, false, 0.0f, 0, 2.0f);
		}
		const bool bPreviewBlocked = IsLayoutDragPreviewBlocked();
		FVector GuideStart, GuideEnd;
		if (GetLayoutRowSnapGuide(GuideStart, GuideEnd))
		{
			const FColor GuideColor = bPreviewBlocked ? FColor::Red : FColor::Cyan;
			DrawDebugLine(GetWorld(), GuideStart, GuideEnd, GuideColor, false, 0.0f, 0, 2.0f);
			// Existing destination box shows orientation; this tether marks its origin on the rear guide.
			const FVector Along = (GuideEnd - GuideStart).GetSafeNormal2D();
			const FVector RearPoint = GuideStart + Along * FVector::DotProduct(DragPreviewCm - GuideStart, Along);
			DrawDebugLine(GetWorld(), RearPoint, DragPreviewCm, GuideColor, false, 0.0f, 0, 2.0f);
		}
		for (const AActor* Selected : GetSelectedLayoutActors())
		{
			if (!PlaceableBoxM(Selected, SizeM)) continue;
			const FVector HalfCm = SizeM * 50.0;
			DrawDebugBox(GetWorld(), Selected->GetActorTransform().TransformPosition(FVector(0, 0, HalfCm.Z)),
				HalfCm * Selected->GetActorScale3D().GetAbs(), Selected->GetActorQuat(), FColor::Green, false, 0.0f, 0, 3.0f);
			if (IsLayoutDragging() && bDragMoved)
			{
				FTransform Proposed;
				if (GetLayoutDragPreviewTransform(Selected, Proposed))
					DrawDebugBox(GetWorld(), Proposed.TransformPosition(FVector(0, 0, SizeM.Z * 50.0)),
						HalfCm * Proposed.GetScale3D().GetAbs(), Proposed.GetRotation(), bPreviewBlocked ? FColor::Red : FColor::Orange, false, 0.0f, 0, 3.0f);
			}
		}

		// Destination color takes precedence when its edges coincide with the green selection.
		// Only submission order changes; geometry and collision queries stay the same.
		const FColor AdditionColor = IsRackAdditionBlocked() ? FColor::Red : FColor::Orange;
		for (int32 I = 0; I < GetRackAdditionPreviewCount(); ++I)
		{
			FTransform AdditionPreview;
			if (GetRackAdditionPreview(AdditionPreview, SizeM, I))
				DrawDebugBox(GetWorld(), AdditionPreview.TransformPosition(FVector(0, 0, SizeM.Z * 50.0)),
					SizeM * 50.0 * AdditionPreview.GetScale3D().GetAbs(), AdditionPreview.GetRotation(), AdditionColor, false, 0.0f, 0, 3.0f);
		}
	}
	else
	{
		Grabbed.Reset();
		HoveredPlaceable.Reset();
	}

	// Unit crosshair is simulation-only; E in layout is camera ascent.
	double HitZM = 0.0;
	ARackThermalActor* Looked = IsLayoutMode() ? nullptr : FindLookedAtRack(HitZM);
	TargetRack = Looked;
	SelectedSledIndex = Looked != nullptr ? SlotAtHeight(*Looked, HitZM) : INDEX_NONE;
	DrawTargetHighlight();

	if (bShowHelp)
	{
		DrawHelp();
	}
}

void ADataCenterDebugController::SetPatchMapOpen(bool bOpen)
{
	if (bOpen) CancelRackAddition();
	if (bOpen && RackPalette) RackPalette->SetVisibility(ESlateVisibility::Collapsed);
	if (bOpen) EndLayoutMarquee(false);
	if (bOpen) EndLayoutDrag(false);
	OpenPatchMaps = FMath::Max(0, OpenPatchMaps + (bOpen ? 1 : -1));
	bInputModeInitialized = false;
	if (OpenPatchMaps == 0) RefreshInteractionInputMode();
	LogLayoutInputState(bOpen ? TEXT("PatchOpen") : TEXT("PatchClose"));
}

void ADataCenterDebugController::UpdateRotation(float DeltaTime)
{
	// DefaultPawn supplies mouse deltas. Only layout mode gates them behind RMB;
	// simulation retains its existing free-look bindings. No simulation clock is advanced here.
	if (!IsCameraInputAvailable() || bMarqueeActive || (IsLayoutMode() && !IsInputKeyDown(EKeys::RightMouseButton)))
		RotationInput = FRotator::ZeroRotator;
	Super::UpdateRotation(DeltaTime);
}

bool ADataCenterDebugController::IsMoveInputIgnored() const
{
	// Prevent DefaultPawn's WASD, arrows, C and E bindings from moving the layout camera a second time.
	return IsLayoutMode() || OpenPatchMaps > 0 || Super::IsMoveInputIgnored();
}

bool ADataCenterDebugController::IsCameraInputAvailable() const
{
	if (OpenPatchMaps > 0 || !InputEnabled() || IsPaused()) return false;
	const UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	return !Viewport || !Viewport->ViewportConsole || Viewport->ViewportConsole->ConsoleState == NAME_None;
}

void ADataCenterDebugController::PlayerTick(float DeltaTime)
{
	RefreshInteractionInputMode();
	Super::PlayerTick(DeltaTime);
	// A focus/input-mode change can flush pressed keys without routing their release events here.
	if (!IsInputKeyDown(EKeys::LeftControl)) bLayoutLeftControl = false;
	if (!IsInputKeyDown(EKeys::RightControl)) bLayoutRightControl = false;
	UpdateRackAdditionFromCursor();
	if (bMarqueeActive)
	{
		if (!IsInputKeyDown(EKeys::LeftMouseButton)) EndLayoutMarquee(false);
		else UpdateLayoutMarqueeFromCursor();
	}
	if (IsLayoutDragging())
	{
		if (!IsInputKeyDown(EKeys::LeftMouseButton))
		{
			LogLayoutInputState(TEXT("TickNoLmbCancel"));
			EndLayoutDrag(false);
		}
		else UpdateLayoutDragFromCursor();
	}
	if (IsLayoutMode() || OpenPatchMaps > 0)
	{
		if (APawn* ViewPawn = GetPawn())
		{
			ViewPawn->ConsumeMovementInputVector();
			if (UPawnMovementComponent* Movement = ViewPawn->GetMovementComponent()) Movement->StopMovementImmediately();
		}
	}
	const FVector Axes(
		int32(IsInputKeyDown(EKeys::W)) - int32(IsInputKeyDown(EKeys::S)),
		int32(IsInputKeyDown(EKeys::D)) - int32(IsInputKeyDown(EKeys::A)),
		int32(IsInputKeyDown(EKeys::E)) - int32(IsInputKeyDown(EKeys::Q)));
	ApplyLayoutCameraInput(DeltaTime, Axes, 0.0f, IsInputKeyDown(EKeys::RightMouseButton));
}

void ADataCenterDebugController::ApplyLayoutCameraInput(float DeltaTime, const FVector& Axes, float Wheel, bool bFly)
{
	APawn* ViewPawn = GetPawn();
	if (!ViewPawn || !IsLayoutMode() || !IsCameraInputAvailable() || Super::IsMoveInputIgnored() || IsLayoutDragging() || bMarqueeActive) return;
	if (bFly && Wheel != 0.0f)
		LayoutCameraSpeedMs = FMath::Clamp(LayoutCameraSpeedMs * FMath::Pow(1.25, FMath::Clamp(double(Wheel), -32.0, 32.0)), 0.25, 24.0);
	const FRotationMatrix View(GetControlRotation());
	const FVector Forward = View.GetScaledAxis(EAxis::X);
	FVector OffsetCm = FVector::ZeroVector;
	if (bFly && DeltaTime > 0.0f)
	{
		const FVector Direction = (Forward * Axes.X + View.GetScaledAxis(EAxis::Y) * Axes.Y + FVector::UpVector * Axes.Z).GetClampedToMaxSize(1.0);
		OffsetCm = Direction * LayoutCameraSpeedMs * DeltaTime * 100.0;
	}
	// Wheel without RMB dollies by 0.5 s of the current view speed per notch (UI choice, no solver time).
	else if (!bFly) OffsetCm = Forward * Wheel * LayoutCameraSpeedMs * 0.5 * 100.0;
	if (!OffsetCm.IsNearlyZero())
	{
		ViewPawn->SetActorLocation(ViewPawn->GetActorLocation() + OffsetCm, false);
		bFramedMoveLogged = true; // Intentional navigation is not the startup drift diagnostic.
	}
}

void ADataCenterDebugController::OnCameraWheelUp()
{
	ApplyLayoutCameraInput(0.0f, FVector::ZeroVector, 1.0f, IsInputKeyDown(EKeys::RightMouseButton));
}

void ADataCenterDebugController::OnCameraWheelDown()
{
	ApplyLayoutCameraInput(0.0f, FVector::ZeroVector, -1.0f, IsInputKeyDown(EKeys::RightMouseButton));
}

void ADataCenterDebugController::RefreshInteractionInputMode()
{
	if (!IsLayoutMode()) { CancelRackAddition(); EndLayoutMarquee(false); EndLayoutDrag(false); LayoutSelection.Reset(); }
	if (OpenPatchMaps > 0) return;
	const bool bLayout = IsLayoutMode();
	if (bInputModeInitialized && bLastLayoutInputMode == bLayout) return;
	bInputModeInitialized = true;
	bLastLayoutInputMode = bLayout;
	bShowMouseCursor = bLayout;
	if (GetLocalPlayer() == nullptr) return; // Automation worlds have no viewport.
	if (!RackPalette && bLayout)
	{
		RackPalette = CreateWidget<ULayoutRackPalette>(this);
		if (RackPalette) RackPalette->AddToViewport();
	}
	if (RackPalette) RackPalette->SetVisibility(bLayout ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (bLayout)
	{
		FInputModeGameAndUI Mode;
		// An empty WidgetToFocus leaves focus on the removed patch widget. Cursor deprojection
		// still succeeds, but HasFocus rejects placement until another viewport click.
		if (UGameViewportClient* Client = GetWorld()->GetGameViewport())
			Mode.SetWidgetToFocus(Client->GetGameViewportWidget());
		Mode.SetHideCursorDuringCapture(false);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(Mode);
	}
	else SetInputMode(FInputModeGameOnly());
}

void ADataCenterDebugController::DrawHelp() const
{
	if (GEngine == nullptr)
	{
		return;
	}

	const uint64 BaseKey = static_cast<uint64>(GetUniqueID());

	GEngine->AddOnScreenDebugMessage(BaseKey, 0.0f, FColor(180, 220, 255),
		IsLayoutMode()
		? TEXT("[배치 키] Shift+클릭 추가/제거 | 빈 곳 드래그 사각형 선택·Shift 추가 | 장비 드래그 그룹 이동/빨강 거절/Esc 취소 | 단일 랙 줄 붙이기/Alt 해제 | Ctrl+D 선택 랙 복제 | Ctrl+Z/Y 배치 실행 취소/다시 실행 | 추가/복제: 클릭 확정/R 회전/Esc 취소 | R 선택 90도 회전 | Delete 선택 랙 전체 삭제 | F 선택/없으면 전체 | 우클릭+마우스/WASD/QE 비행·휠 속도 | 휠 전후 | G/T 단일만 C 컨테인먼트 Enter 시뮬레이션 H 도움말")
		: TEXT("[디버그 키] Enter 배치 모드 | 1/2 CPU 발열  3/4 GPU 발열  5/6 펌프 회전수  7/8 라디에이터 UA(랙별 모드)  9/0 부하율  E 바라보는 유닛 뽑기/꽂기  R 리셋  H 도움말"));

	const ARackThermalActor* Rack = TargetRack.Get();
	FString Selection = TEXT("바라보는 유닛 없음 - 유닛을 조준하고 E");
	if (Rack != nullptr && SelectedSledIndex == INDEX_NONE)
	{
		Selection = TEXT("랙의 빈 칸·프레임을 보고 있음 - 유닛을 조준하고 E");
	}
	else if (Rack != nullptr)
	{
		const FVector M = SimUnits::UnrealToMeters(Rack->GetActorLocation());
		Selection = FString::Printf(TEXT("바라보는 유닛: 랙 (%.2f, %.2f) m U%d/%d %s | 이 랙 꽂힌 유닛 %d"),
			M.X, M.Y, SelectedSledIndex, Rack->Params.NumSleds,
			Rack->IsSledDocked(SelectedSledIndex) ? TEXT("[꽂힘]") : TEXT("[뽑힘]"), Rack->GetNumDockedSleds());
	}

	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(BaseKey + 1, 0.0f, FColor(255, 220, 160),
		FString::Printf(
			TEXT("CPU %.0f W/유닛 | GPU %.0f W/유닛 (전부하) | 부하율 %.0f%% | 펌프 r=%.2f | 라디에이터 UA %.0f W/K | %s"),
			CpuHeatKnob.Value, GpuHeatKnob.Value, UtilizationKnob.Value * 100.0, PumpSpeedKnob.Value, RadiatorUAKnob.Value,
			*Selection));
}
