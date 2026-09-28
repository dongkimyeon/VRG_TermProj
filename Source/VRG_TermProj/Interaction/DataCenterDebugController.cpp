// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/DataCenterDebugController.h"
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
#include "DrawDebugHelpers.h"
#include "Engine/GameViewportClient.h"
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

bool ADataCenterDebugController::EquipmentViewpoint(UWorld* World, double HorizontalFovDeg, double Aspect, FVector& OutEyeCm, FRotator& OutLook)
{
	if (World == nullptr) return false;
	TArray<FVector> Corners;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		FVector SizeM;
		if (!PlaceableBoxM(*It, SizeM)) continue;
		const FVector HalfCm = SizeM * 50.0;
		for (int32 C = 0; C < 8; ++C)
		{
			const FVector Local((C & 1 ? 1 : -1) * HalfCm.X, (C & 2 ? 1 : -1) * HalfCm.Y, C & 4 ? 2.0 * HalfCm.Z : 0.0);
			Corners.Add(It->GetActorTransform().TransformPositionNoScale(Local));
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

bool ADataCenterDebugController::TryFrameEquipment()
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
	if (!EquipmentViewpoint(GetWorld(), Fov, Aspect, Eye, Look)) return false;
	ViewPawn->SetActorLocation(Eye, false, nullptr, ETeleportType::TeleportPhysics);
	FramedEyeCm = ViewPawn->GetActorLocation();
	ViewPawn->SetActorRotation(Look);
	SetControlRotation(Look);
	UE_LOG(LogTemp, Display, TEXT("[VrgDebug] 시작 시점: 장비 전체가 보이게 %s (FOV %.0f, 화면비 %.2f)"), *Eye.ToCompactString(), Fov, Aspect);
	return true;
}

ADataCenterSimulationActor* ADataCenterDebugController::FindSimActor() const
{
	UWorld* World = GetWorld();
	if (World == nullptr) return nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) return *It;
	return nullptr;
}

bool ADataCenterDebugController::IsLayoutMode() const
{
	const ADataCenterSimulationActor* Sim = FindSimActor();
	return Sim != nullptr && Sim->GetSimMode() == ESimMode::Layout;
}

void ADataCenterDebugController::OnToggleSimMode()
{
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
	if (!IsLayoutMode()) return;
	AActor* Target = Grabbed.IsValid() ? Grabbed.Get() : HoveredPlaceable.Get();
	if (Target == nullptr) return;
	FRotator R = Target->GetActorRotation();
	R.Yaw = FRotator::NormalizeAxis(FMath::RoundToDouble((R.Yaw + 90.0) / 90.0) * 90.0);
	Target->SetActorRotation(R);
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
	// With the integrated actor: restart the simulation from the current layout, and do nothing in layout mode
	// (resetting racks there would start their own standalone integration).
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
				Held->SetActorLocation(Target);
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
	}
	else
	{
		Grabbed.Reset();
		HoveredPlaceable.Reset();
	}

	// The unit under the crosshair: the rack the view ray enters first, the slot at the height where it enters.
	double HitZM = 0.0;
	ARackThermalActor* Looked = FindLookedAtRack(HitZM);
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
	OpenPatchMaps = FMath::Max(0, OpenPatchMaps + (bOpen ? 1 : -1));
	bInputModeInitialized = false;
	if (OpenPatchMaps == 0) RefreshInteractionInputMode();
}

void ADataCenterDebugController::RefreshInteractionInputMode()
{
	if (OpenPatchMaps > 0) return;
	const bool bLayout = IsLayoutMode();
	if (bInputModeInitialized && bLastLayoutInputMode == bLayout) return;
	bInputModeInitialized = true;
	bLastLayoutInputMode = bLayout;
	bShowMouseCursor = bLayout;
	if (GetLocalPlayer() == nullptr) return; // Automation worlds have no viewport.
	if (bLayout)
	{
		FInputModeGameAndUI Mode;
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
		TEXT("[디버그 키] Enter 배치/시뮬레이션 전환  G 장비 잡기/놓기  T 90도 회전  C 컨테인먼트 (배치 모드) | 1/2 CPU 발열  3/4 GPU 발열  5/6 펌프 회전수  7/8 라디에이터 UA(랙별 모드)  9/0 부하율  E 바라보는 유닛 뽑기/꽂기  R 리셋  H 도움말"));

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
