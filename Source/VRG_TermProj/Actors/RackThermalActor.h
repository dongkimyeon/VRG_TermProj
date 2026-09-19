// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Thermal/RadiatorModel.h"
#include "RackThermalActor.generated.h"

// 랙(캐비닛) 1대의 열모델을 레벨에서 돌리는 액터.
// 액터는 입력 전달과 결과 표현만 한다. 물리 계산은 전부 FRackThermalModel 안에 있다.
// 길이는 m로 들고 있다가 그리는 순간에만 cm로 바꾼다 (규약 1).
UCLASS()
class VRG_TERMPROJ_API ARackThermalActor : public AActor
{
	GENERATED_BODY()

public:
	ARackThermalActor();

	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void PauseSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void ResetSim();

	UFUNCTION(BlueprintCallable, Category = "Rack")
	FRackThermalSnapshot GetSnapshot() const;

	// 유닛을 뽑거나 꽂는다. 공랭 유로 배분과 콜드플레이트 결합이 함께 바뀐다.
	UFUNCTION(BlueprintCallable, Category = "Rack")
	void SetSledDocked(int32 SledIndex, bool bDocked);

	UFUNCTION(BlueprintCallable, Category = "Rack")
	bool IsSledDocked(int32 SledIndex) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalParams Params;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalInput Input;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalState InitialState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator")
	FRadiatorParams Radiator;

	// 켜면 랙에서 나온 냉각수를 라디에이터에 통과시켜 다시 랙 입구로 돌린다.
	// 열 도메인 안에서만 닫는 루프다. 유량은 여전히 이 액터의 입력이며
	// 수력 솔버에서 받아오지 않는다 (그 연결은 S4에서 한다).
	// 끄면 CoolantInletTempK를 고정 경계로 쓰는 S1 개루프 구성이 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator")
	bool bCloseCoolantLoopThroughRadiator = true;

	// 고정 시뮬레이션 스텝 [s]. 모든 도메인이 같은 시계를 쓴다 (규약 4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double FixedStepS = 0.01;

	// 랙 외형 크기 [m]. 표시용이며 계산에는 쓰이지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	FVector RackSizeM = FVector(1.2, 0.6, 2.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowReadout = true;

protected:
	virtual void BeginPlay() override;

private:
	FSimClock Clock;
	FRackThermalModel Model;
	bool bSimRunning = false;

	// Input의 TArray 길이를 Params.NumSleds에 맞춘다. 비어 있으면 기본값으로 채운다.
	void ConformInputArrays();

	void DrawReadout() const;
};
