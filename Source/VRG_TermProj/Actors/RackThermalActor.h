// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "RackThermalActor.generated.h"

// S1 랙 1대 열모델을 레벨에서 돌려보기 위한 액터.
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalParams Params;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalInput Input;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalState InitialState;

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

	void DrawReadout() const;
};
