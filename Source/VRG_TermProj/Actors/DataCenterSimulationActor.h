// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "DataCenterSimulationActor.generated.h"

class AAirFieldDebugActor;
class ADataCenterRoomActor;
class ARackThermalActor;

// S4: 세 도메인을 하나의 시계로 묶는 통합 액터 (MVP).
//
// 레벨에서 찾는 것:
//   ADataCenterRoomActor   -> 수력망 (랙 분기 유량)
//   AAirFieldDebugActor    -> 공기 격자 (랙 흡기 온도 / 배기 열원)
//   ARackThermalActor 여럿 -> 랙 열모델
//
// --- 규약 4: 시계는 하나다 ---
// 이 액터가 SimClock을 쥐고, 랙과 공기 액터는 bDrivenExternally로 자기 적분을 멈춘다.
// 액터마다 따로 돌리면 같은 시간 구간을 두 번 적분하거나 서로 어긋난 시각을 본다.
// 수력은 유량이 바뀔 때만 다시 풀지만, 그 사이에도 열과 공기는 같은 dt로 돈다.
//
// --- 규약 6: 주고받는 값은 한 번만 계산한다 ---
// 실제 교환은 FCouplingBridge가 한다. 이 액터는 기하(어느 셀이 랙 흡기인지,
// 어느 파이프가 이 랙의 분기인지)를 풀어서 다리에 넘기는 일만 한다.
//
// --- 랙은 공기 격자에서 고체다 ---
// 랙이 차지하는 셀을 고체로 표시해 기류가 랙을 통과하지 못하게 한다.
// 앞뒤는 뚫려 있어야 하므로 랙 몸통 안쪽만 막고 앞뒤 한 겹은 남긴다.
UCLASS()
class VRG_TERMPROJ_API ADataCenterSimulationActor : public AActor
{
	GENERATED_BODY()

public:
	ADataCenterSimulationActor();

	virtual void Tick(float DeltaTime) override;

	// 배치가 바뀐 뒤 누른다. 연결을 다시 풀고 시계를 0으로 되돌린다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void RebuildCoupling();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void PauseSim();

	UFUNCTION(BlueprintCallable, Category = "Simulation")
	FCouplingSnapshot GetSnapshot() const;

	// 고정 시뮬레이션 스텝 [s]. 모든 도메인이 이 값을 쓴다 (규약 4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	double FixedStepS = 0.02;

	// 한 프레임에 돌릴 스텝 수 상한. 공기 압력 풀이가 무거워서 필요하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxStepsPerFrame = 4;

	// 물의 밀도 [kg/m^3]. 수력의 체적유량을 질량유량으로 바꿀 때만 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	double CoolantDensityKgM3 = 997.0;

	// ---- 공기 결합 (m) ----
	// 랙 앞면에서 흡기 셀을 잡을 거리. 이 안쪽 셀들의 평균 온도가 랙 흡기 온도가 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	double InletSampleDepthM = 0.4;

	// 랙 뒷면에서 배기 셀을 잡을 거리. 랙이 공기로 내보낸 열이 여기 들어간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	double ExhaustDepthM = 0.5;

	// 랙을 공기 격자에서 고체로 막을지. 끄면 기류가 랙을 통과한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	bool bMarkRacksAsSolid = true;

	// ---- CRAH 냉각 코일 ----
	// 코일을 둘 높이 범위 [m]. 천장 쪽 한 층을 코일로 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilBottomZM = 2.6;

	// 코일 전체 전도도 [W/K]. 운전점에서 역산해 넣어야 한다.
	// 랙 N대의 공랭 부하를 접근온도 약 10 K로 빼려면 UA = 부하 / 10이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilTotalUAWK = 3000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilSupplyTempK = 291.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FSimClock Clock;
	FCouplingBridge Bridge;

	UPROPERTY(Transient)
	TObjectPtr<ADataCenterRoomActor> Room;

	UPROPERTY(Transient)
	TObjectPtr<AAirFieldDebugActor> AirField;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ARackThermalActor>> Racks;

	bool bSimRunning = false;
	bool bNeedsRebuild = true;

	// 연결이 성립했는지. 안 되면 화면에 이유를 띄운다.
	FString LastSetupMessage;

	void FindTargetActors();

	// 랙 기하를 공기 격자 인덱스로 풀어 다리에 넘길 연결 정보를 만든다.
	void BuildCoupling();

	// 랙이 차지하는 셀을 고체로 표시한다.
	void MarkRackSolidCells();

	// 천장 쪽 한 층을 CRAH 코일로 잡는다.
	void BuildCoolingCoil();

	void DrawSummary() const;
};
