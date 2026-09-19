// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DebugKnob.h"
#include "DataCenterDebugController.generated.h"

class ARackThermalActor;
class AHydraulicNetworkDebugActor;
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
// 키 배치:
//   1 / 2        CPU 발열 내리기 / 올리기 (유닛당)
//   3 / 4        GPU 발열 내리기 / 올리기 (유닛당)
//   5 / 6        펌프 회전수비 내리기 / 올리기
//   7 / 8        라디에이터 UA 내리기 / 올리기
//   Left / Right 선택 유닛 바꾸기
//   E            선택 유닛 뽑기 / 꽂기
//   R            시뮬레이션 리셋
//   H            도움말 표시 토글
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowHelp = true;

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

	int32 SelectedSledIndex = 0;

	// 키 하나를 InputAction으로 만들고 매핑에 넣는다.
	UInputAction* AddKeyAction(FName ActionName, const FKey& Key);

	void FindTargetActors();

	// 손잡이 값을 대상 액터에 반영한다. 손잡이를 돌린 직후에만 부른다.
	void ApplyHeatKnobs();
	void ApplyPumpSpeedKnob();
	void ApplyRadiatorKnob();

	void OnCpuHeatDown();
	void OnCpuHeatUp();
	void OnGpuHeatDown();
	void OnGpuHeatUp();
	void OnPumpSpeedDown();
	void OnPumpSpeedUp();
	void OnRadiatorDown();
	void OnRadiatorUp();
	void OnSelectPreviousSled();
	void OnSelectNextSled();
	void OnToggleSelectedSled();
	void OnResetSim();
	void OnToggleHelp();

	void DrawHelp() const;
};
