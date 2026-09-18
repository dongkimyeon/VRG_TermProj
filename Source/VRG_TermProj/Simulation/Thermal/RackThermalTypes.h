// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RackThermalTypes.generated.h"

// 규약 5: 계산 입력 / 계산 상태 / 렌더 스냅샷을 구분한다.
// 여기 있는 구조체는 전부 POD다. UObject 참조나 포인터를 넣지 않는다.
// 워커 스레드가 그대로 복사해 들고 갈 수 있어야 한다.
//
// 모든 온도는 K, 열량은 W, 질량유량은 kg/s (규약 1·2·3).

// 랙의 고정 설정값. 시뮬레이션 중에는 바뀌지 않는다.
USTRUCT(BlueprintType)
struct FRackThermalParams
{
	GENERATED_BODY()

	// 칩(IT 장비) 노드 열용량 [J/K]
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double ChipHeatCapacityJK = 5.0e4;

	// 랙 내부 냉각수 노드 열용량 [J/K]
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantHeatCapacityJK = 4.0e4;

	// 칩 -> 냉각수 열전도도 [W/K]
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double ChipToCoolantUAWK = 800.0;

	// 칩 -> 공기 열전도도 [W/K]. epsilon-NTU의 UA로 쓰인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double ChipToAirUAWK = 150.0;

	// 냉각수 비열 [J/(kg*K)]. 기본값은 상온 물.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantCpJKgK = 4182.0;

	// 공기 비열 [J/(kg*K)]. 기본값은 해수면 표준 공기.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirCpJKgK = 1005.0;
};

// 매 스텝 바깥에서 주어지는 경계 조건.
USTRUCT(BlueprintType)
struct FRackThermalInput
{
	GENERATED_BODY()

	// D1: 랙 발열은 체적 열원 하나로 통일한다. 배기 온도를 따로 덮어쓰지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double HeatW = 1.5e4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantInletTempK = 300.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantMassFlowKgs = 0.5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirInletTempK = 297.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirMassFlowKgs = 1.2;
};

// 적분되는 상태. 이것만 있으면 시뮬레이션을 이어서 돌릴 수 있다.
USTRUCT(BlueprintType)
struct FRackThermalState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double ChipTempK = 300.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantTempK = 300.15;
};

// 표시용 읽기 전용 사본. 렌더/UI는 이것만 본다.
USTRUCT(BlueprintType)
struct FRackThermalSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double SimTimeS = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double ChipTempK = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double CoolantTempK = 0.0;

	// 잘 섞인 단일 냉각수 노드를 가정하므로 출구 온도 = 노드 온도다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double CoolantOutletTempK = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double HeatChipToCoolantW = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double HeatChipToAirW = 0.0;

	// 냉각수가 이류로 랙 밖으로 실어 나른 열.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double HeatRemovedByCoolantW = 0.0;

	// 열수지 잔차 [J]: 유입 - 유출 - 축적. 정규화 값은 아래 필드를 쓴다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double EnergyResidualJ = 0.0;

	// 잔차를 총 유입 에너지로 정규화한 무차원 값. 보존 검사 기준.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double NormalizedEnergyResidual = 0.0;

	// 명시적 오일러 단조 조건을 만족했는지. false면 결과를 믿으면 안 된다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	bool bStepStable = true;
};
