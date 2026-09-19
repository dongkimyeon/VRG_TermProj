// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RackThermalTypes.generated.h"

// 규약 5: 계산 입력 / 계산 상태 / 렌더 스냅샷을 구분한다.
// 여기 있는 구조체는 전부 POD다. UObject 참조나 포인터를 넣지 않는다.
// 워커 스레드가 그대로 복사해 들고 갈 수 있어야 한다.
//
// 규약 7: 유닛(슬레드) x 부품 온도는 평탄 배열 + 선형 인덱스로 들고 있다.
// 인덱스는 SledIndex * NumRackComponents + (int32)ERackComponent.
//
// 모든 온도는 K, 열량은 W, 질량유량은 kg/s (규약 1·2·3).
//
// D1 준수: 랙 내부를 부품 노드로 쪼개는 것은 D1 위반이 아니다. D1은 *공기 도메인에
// 주입할 때* 하나의 체적 열원으로 합치라는 뜻이다. S3/S4에서 격자에 넘길 때는
// FRackThermalSnapshot::TotalHeatToAirW + TotalHeatToAmbientW 를 합산해 하나로 준다.

// 서버 유닛 1대 안의 부품. 배열 순서가 그대로 공랭 직렬 체인 순서다.
//
// 액랭/공랭 분담은 하이브리드 서버 실기 구성을 따른다:
//   콜드플레이트(액랭) = Cpu, Gpu      -> ToCoolantUAWK > 0, ToAirUAWK = 0
//   공기               = Ram, Vrm      -> ToAirUAWK > 0,     ToCoolantUAWK = 0
// 공랭 체인 순서는 허용 온도가 낮은 부품을 앞에 둔다 (DDR5 RDIMM 약 85 C <
// VRM 파워스테이지 약 125 C). 앞쪽 부품이 더 찬 공기를 받는다.
// Cpu/Gpu는 ToAirUAWK가 0이라 체인에서 공기 온도를 바꾸지 않고 통과만 한다.
UENUM(BlueprintType)
enum class ERackComponent : uint8
{
	Cpu   UMETA(DisplayName = "CPU"),
	Gpu   UMETA(DisplayName = "GPU"),
	Ram   UMETA(DisplayName = "RAM"),
	Vrm   UMETA(DisplayName = "VRM"),
};

static constexpr int32 NumRackComponents = 4;

// 부품 하나의 고정 물성. 모든 유닛이 같은 구성이라고 보고 유닛 간에 공유한다.
USTRUCT(BlueprintType)
struct FRackComponentParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	FName Name = NAME_None;

	// 열용량 [J/K]. 부품 + 히트스프레더 + 콜드플레이트 금속 질량까지 묶은 값.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double HeatCapacityJK = 400.0;

	// 콜드플레이트를 통한 냉각수 열전도도 [W/K]. 공랭 전용 부품은 0.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double ToCoolantUAWK = 0.0;

	// 통과 공기에 대한 열전도도 [W/K]. epsilon-NTU의 UA로 쓰인다. 액랭 전용 부품은 0.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double ToAirUAWK = 0.0;

	// 케이스를 통한 실내 공기로의 자연대류·복사 [W/K]. 강제 공랭과 별개로 항상 있다.
	// 유닛을 뽑아 강제 공랭이 끊겨도 이 경로로 식는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double ToAmbientUAWK = 1.0;
};

// 랙(캐비닛) 1대의 고정 설정값. 시뮬레이션 중에는 바뀌지 않는다.
USTRUCT(BlueprintType)
struct FRackThermalParams
{
	GENERATED_BODY()

	// 캐비닛에 꽂히는 서버 유닛 슬롯 수.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack", meta = (ClampMin = "1", ClampMax = "64"))
	int32 NumSleds = 12;

	// 길이 NumRackComponents. 비어 있으면 Initialize가 기본 구성으로 채운다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<FRackComponentParams> Components;

	// 랙 내부 냉각수 노드 열용량 [J/K]. 약 9.6 kg의 물.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantHeatCapacityJK = 4.0e4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantCpJKgK = 4182.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirCpJKgK = 1005.0;

	// ---- 슬롯 병렬 유로 저항 [Pa*s^2/kg^2] ----
	// 공기는 캐비닛 앞뒤 압력차 하나를 공유하는 병렬 유로로 슬롯에 나뉜다.
	// 2차 손실을 가정하므로 MassFlow_i = sqrt(dP / R_i), 즉 배분 가중치는 1/sqrt(R_i)다.
	// 총 유량은 팬이 정하므로 입력으로 고정하고, 여기서는 비율만 결정한다.
	//
	// 유닛이 꽂힌 슬롯: 서버 내부(히트싱크·DIMM·백플레인)를 지나므로 저항이 크다.
	// dP 약 100 Pa에서 0.1 kg/s -> R = 100/0.1^2 = 1.0e4.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double DockedSlotResistancePaS2Kg2 = 1.0e4;

	// 빈 슬롯: 뻥 뚫린 구멍이라 저항이 훨씬 낮다. dP 약 100 Pa에서 0.5 kg/s -> R = 400.
	// 저항비 25배 = 유량비 5배. 빈 슬롯이 공기를 훔쳐가므로 남은 유닛이 뜨거워진다.
	// 실제로 blanking panel을 끼우는 이유가 이것이다 (패널을 끼우면 이 값이 커진다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double EmptySlotResistancePaS2Kg2 = 4.0e2;
};

// 매 스텝 바깥에서 주어지는 경계 조건.
USTRUCT(BlueprintType)
struct FRackThermalInput
{
	GENERATED_BODY()

	// 평탄 배열 [SledIndex * NumRackComponents + Component], 단위 W.
	// 비어 있으면 모델이 0으로 본다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> ComponentHeatW;

	// 길이 NumSleds. false면 슬롯이 비어 있다 (유닛을 뽑은 상태).
	// 비어 있으면 모델이 전부 도킹된 것으로 본다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<bool> SledDocked;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantInletTempK = 300.15;

	// S2 인로우 해석이 낸 랙당 분기 유량 0.82 L/s에 맞춘 값.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantMassFlowKgs = 0.82;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirInletTempK = 297.15;

	// 캐비닛 전체를 지나는 공기 질량유량 [kg/s]. 팬이 정하며 슬롯 사이에 배분된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double TotalAirMassFlowKgs = 1.2;
};

// 적분되는 상태. 이것만 있으면 시뮬레이션을 이어서 돌릴 수 있다.
USTRUCT(BlueprintType)
struct FRackThermalState
{
	GENERATED_BODY()

	// 평탄 배열 [SledIndex * NumRackComponents + Component], 단위 K.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> ComponentTempK;

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

	// 평탄 배열. State와 같은 인덱스 규칙.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> ComponentTempK;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> ComponentHeatToCoolantW;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> ComponentHeatToAirW;

	// 슬롯별 공기 질량유량 [kg/s]. 빈 슬롯도 포함한다 (그쪽으로 새는 양).
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotAirMassFlowKgs;

	// 유닛을 빠져나온 공기 온도 [K]. 빈 슬롯은 입구 온도 그대로다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SledAirOutletTempK;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double CoolantTempK = 0.0;

	// 잘 섞인 단일 냉각수 노드를 가정하므로 출구 온도 = 노드 온도다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double CoolantOutletTempK = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double TotalHeatW = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double TotalHeatToCoolantW = 0.0;

	// 강제 공랭이 실어 나간 열.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double TotalHeatToAirW = 0.0;

	// 케이스 자연대류·복사로 실내 공기에 나간 열.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double TotalHeatToAmbientW = 0.0;

	// 냉각수가 이류로 랙 밖으로 실어 나른 열.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double HeatRemovedByCoolantW = 0.0;

	// 캐비닛 전체 배기 온도 [K]. 슬롯 배기를 질량유량 가중 혼합한 값.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double MixedAirOutletTempK = 0.0;

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
