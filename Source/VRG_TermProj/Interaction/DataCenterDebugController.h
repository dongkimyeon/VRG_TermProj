// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DebugKnob.h"
#include "DataCenterDebugController.generated.h"

class ARackThermalActor;
class AHydraulicNetworkDebugActor;
class ADataCenterRoomActor;
class UInputAction;
class UInputMappingContext;

// 런타임에 시뮬레이션 파라미터를 돌려 보기 위한 디버그 입력.
//
// EnhancedInput을 쓰지만 InputAction/MappingContext 에셋을 만들지 않고 코드에서
// 만든다. 에셋을 만들면 물리 파라미터의 기본값이 코드와 에셋 두 곳에 흩어진다.
//
// 이 컨트롤러는 값을 정하지 않는다. 손잡이(FDebugKnob)가 범위와 증분을 들고 있고,
// 여기서는 키를 손잡이에 잇고 결과를 액터에 밀어 넣기만 한다.
//
// 손잡이의 시작값과 범위는 레벨에서 찾은 첫 랙의 현재 값에서 잡는다.
// 랙 종류(15 kW 하이브리드 / GB200 NVL72)마다 발열 규모가 달라서, 여기에
// 숫자를 박아 두면 다른 랙을 놓는 순간 범위가 어긋난다.
//
// 대상은 한 번만 찾지 않는다. ADataCenterRoomActor는 첫 틱에 랙을 띄우므로
// BeginPlay 시점에는 아직 없다. 주기적으로 다시 훑는다.
//
// 키 배치:
//   1 / 2        CPU 발열 내리기 / 올리기 (유닛당)
//   3 / 4        GPU 발열 내리기 / 올리기 (유닛당)
//   5 / 6        펌프 회전수비 내리기 / 올리기
//   7 / 8        라디에이터 UA 내리기 / 올리기
//   E            바라보는 유닛 하나 뽑기 / 꽂기 (다른 유닛·다른 랙은 건드리지 않는다)
//   R            시뮬레이션 리셋
//   H            도움말 표시 토글
//
// "바라보는 유닛"은 카메라 시선이 처음 닿는 랙 상자(RackSizeM, 공기 격자에서 고체로 막는 것과 같은 상자)에서
// 시선이 들어간 높이의 슬롯이다. 메시는 충돌이 꺼져 있어 라인 트레이스 대신 이 상자와 직접 교차시킨다.
// 뽑힌 유닛은 앞으로 나와 있지만 같은 높이에서 랙 앞면을 보면 다시 잡힌다. 랙은 초록, 유닛은 노란 상자로 그린다.
UCLASS()
class VRG_TERMPROJ_API ADataCenterDebugController : public APlayerController
{
	GENERATED_BODY()

public:
	ADataCenterDebugController();

	virtual void Tick(float DeltaTime) override;

	// 유닛당 CPU 발열 [W].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob CpuHeatKnob;

	// 유닛당 GPU 발열 [W].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob GpuHeatKnob;

	// 펌프 회전수비 [무차원]. 상사식으로 체절양정이 r^2로 스케일된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob PumpSpeedKnob;

	// 라디에이터 크기 [W/K]. 자릿수를 훑어야 하므로 곱셈으로 움직인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob RadiatorUAKnob;

	// Workload utilization of every rack [dimensionless, 0..1]. Part heat follows its idle fraction.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob UtilizationKnob;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowHelp = true;

	// How far the view ray looks for a rack to select [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.5"))
	double MaxSelectDistanceM = 15.0;

	// Nearest rack whose box (RackSizeM, base at the actor origin) the ray hits within MaxDistanceCm, or null.
	// Unreal units here (cm): this is view picking, not a solver.
	static ARackThermalActor* PickRackAlongRay(const TArray<TObjectPtr<ARackThermalActor>>& Racks,
		const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm, double* OutHitZM = nullptr);

	// Slot whose height range (rack-local, m) contains HeightM, or INDEX_NONE (blank panel, frame).
	static int32 SlotAtHeight(const ARackThermalActor& Rack, double HeightM);

	// 레벨에서 대상을 다시 훑는 주기 [s]. 방 액터가 랙을 늦게 띄우기 때문에 필요하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float TargetRefreshIntervalS = 0.5f;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> Actions;

	// 레벨에서 찾은 대상. 랙은 여러 대일 수 있고 손잡이는 전부에 같이 적용한다.
	UPROPERTY(Transient)
	TArray<TObjectPtr<ARackThermalActor>> Racks;

	UPROPERTY(Transient)
	TObjectPtr<AHydraulicNetworkDebugActor> HydraulicNetwork;

	UPROPERTY(Transient)
	TObjectPtr<ADataCenterRoomActor> Room;

	int32 SelectedSledIndex = INDEX_NONE;  // the unit under the crosshair
	float TimeSinceRefreshS = 0.0f;

	// The rack the camera looks at (updated every tick). Unit selection and E act on it only.
	TWeakObjectPtr<ARackThermalActor> TargetRack;
	ARackThermalActor* FindLookedAtRack(double& OutHitZM) const;
	void DrawTargetHighlight() const;
	bool bKnobsInitialized = false;

	// 키 하나를 InputAction으로 만들고 매핑에 넣는다.
	UInputAction* AddKeyAction(FName ActionName, const FKey& Key);

	void FindTargetActors();

	// 랙 수가 달라졌으면 대상을 다시 훑고 손잡이 범위를 다시 잡는다.
	void RefreshTargetsIfStale(float DeltaTime);

	// 첫 랙의 현재 값에서 손잡이의 시작값과 범위를 잡는다.
	void InitializeKnobsFromTargets();

	// 손잡이 값을 대상 액터에 반영한다. 손잡이를 돌린 직후에만 부른다.
	void ApplyHeatKnobs();
	void ApplyPumpSpeedKnob();
	void ApplyRadiatorKnob();
	void ApplyUtilizationKnob();

	void OnCpuHeatDown();
	void OnCpuHeatUp();
	void OnGpuHeatDown();
	void OnGpuHeatUp();
	void OnPumpSpeedDown();
	void OnPumpSpeedUp();
	void OnRadiatorDown();
	void OnRadiatorUp();
	void OnUtilizationDown();
	void OnUtilizationUp();
	void OnToggleSelectedSled();
	void OnResetSim();
	void OnToggleHelp();

	void DrawHelp() const;
};
