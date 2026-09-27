// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RackThermalTypes.generated.h"

// 규약 5: 계산 입력 / 계산 상태 / 렌더 스냅샷을 구분한다.
// 여기 있는 구조체는 전부 POD다. UObject 참조나 포인터를 넣지 않는다.
// 워커 스레드가 그대로 복사해 들고 갈 수 있어야 한다.
//
// 규약 7: 부품 온도는 평탄 배열 + 선형 인덱스로 들고 있다. 슬롯 s의 노드는
// FRackThermalModel::GetSlotNodeStart(s)부터 그 장비의 부품 수만큼이다.
//
// 모든 온도는 K, 열량은 W, 질량유량은 kg/s (규약 1·2·3).
//
// D1 준수: 랙 내부를 부품 노드로 쪼개는 것은 D1 위반이 아니다. D1은 *공기 도메인에
// 주입할 때* 체적 열원으로 넣으라는 뜻이다. 슬롯마다 자기 높이의 격자 셀에 넣는다.
//
// 2026-09-27: 랙은 공개 사양 기반 장비(GB200 NVL72, Quantum 스파인 스위치)만 쓴다.
// 예전의 15 kW 하이브리드 균일 랙(유닛 12대 x CPU/GPU/RAM/VRM)과 랙 단위 팬 경로는 없앴다.

// What a component is, independent of where it sits in a device's list. Debug knobs and the UI find
// "every GPU" by kind.
UENUM(BlueprintType)
enum class ERackPartKind : uint8
{
	Cpu, Gpu, Memory, Nic, Dpu, Storage, Power, Fan, Switch, Management, Other
};

// 부품 하나의 고정 물성. 같은 장비 종류의 슬롯끼리 공유한다.
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	ERackPartKind Kind = ERackPartKind::Other;

	// Nominal heat [W] used to fill FRackThermalInput::ComponentHeatW (MakeDefaultHeatW).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double DefaultHeatW = 0.0;

	// Conversion-loss part (e.g. a power shelf): its heat is not an input but this fraction of the rack's
	// DC load, i.e. the heat of every other docked component this step. 0 = ordinary part.
	// A shelf of N shelves sharing a load at efficiency eta carries (1/eta - 1) / N.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	double LossFractionOfRackLoad = 0.0;

	// Air path topology inside a device. Consecutive components with the same AirStage >= 0 sit side by
	// side (parallel): each sees the stage inlet air and the air-cooled ones split the slot flow equally.
	// -1 = a stage of its own in list order (series), which is the S1 behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component")
	int32 AirStage = -1;

	// Load dependence: heat = ComponentHeatW * (f + (1 - f) * utilization), f = this fraction.
	// The input ComponentHeatW is the full-load heat. 1 = load independent (the S1 behaviour).
	// Linear idle-to-busy interpolation is the standard server power model (Fan, Weber, Barroso,
	// "Power Provisioning for a Warehouse-sized Computer", ISCA 2007). Per-part values and sources are in
	// RackThermalModel.cpp; most are estimates.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Component", meta = (ClampMin = "0", ClampMax = "1"))
	double IdleHeatFraction = 1.0;
};

// Fans of one device (all identical, in parallel, blowing front -> rear).
// NumFans <= 0: the device has no fans and gets no forced air (e.g. the fully liquid-cooled NVLink switch tray).
// Every slot's air flow is its own fans' operating point.
//
// Fan curve (per fan, quadratic approximation of the datasheet P-Q curve between its two end points):
//   dP = r^2 * ShutoffPressurePa * (1 - (FlowM3s / (r * MaxFlowM3s))^2)
// Affinity laws for a speed change of the same fan: flow ~ r, pressure ~ r^2, electric input ~ r^3 (D8: a
// speed-change approximation of one device, not a comparison between devices). RatedInputW is the
// ELECTRIC input at full speed (D8: not divided by an efficiency again).
// Device impedance: dP = DockedResistancePaS2Kg2 * MassFlowKgs^2.
USTRUCT(BlueprintType)
struct FRackFanSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	int32 NumFans = 0;

	// Free-delivery flow of one fan at full speed [m^3/s].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double MaxFlowM3s = 0.0;

	// Shut-off (zero flow) static pressure of one fan at full speed [Pa].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double ShutoffPressurePa = 0.0;

	// Electric input of one fan at full speed [W] (datasheet rated input).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double RatedInputW = 0.0;

	// Speed control (BMC-like proportional law on the hottest air-cooled part of the device):
	//   r_target = MinSpeedRatio + (1 - MinSpeedRatio) * clamp((T - ControlStartK) / (ControlFullK - ControlStartK), 0, 1)
	// r follows r_target at most MaxSlewPerS per second. A manual override in the input bypasses this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan", meta = (ClampMin = "0", ClampMax = "1"))
	double MinSpeedRatio = 0.3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double ControlStartK = 328.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double ControlFullK = 348.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fan")
	double MaxSlewPerS = 0.2;

	bool HasFans() const { return NumFans > 0 && MaxFlowM3s > 0.0 && ShutoffPressurePa > 0.0; }
};

// One kind of rack-mounted device (compute tray, switch tray, power shelf, ...). Components are listed
// in air-path order: air passes through them one after another (front to rear).
USTRUCT(BlueprintType)
struct FRackDeviceType
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Device")
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Device")
	TArray<FRackComponentParams> Components;

	// Air-path resistance when docked [Pa*s^2/kg^2].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Device")
	double DockedResistancePaS2Kg2 = 0.0;

	// True when the device has cold-plate parts on the rack coolant loop (counts for the branch resistance).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Device")
	bool bLiquidCooled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Device")
	FRackFanSpec Fans;
};

// A mounted device: which type and where (rack-local, from the rack floor).
USTRUCT(BlueprintType)
struct FRackSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	int32 DeviceType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	double BottomZM = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slot")
	double HeightM = 0.04445;
};

// 랙(캐비닛) 1대의 고정 설정값. 시뮬레이션 중에는 바뀌지 않는다.
USTRUCT(BlueprintType)
struct FRackThermalParams
{
	GENERATED_BODY()

	// 슬롯 수. Initialize가 Slots.Num()으로 맞춘다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	int32 NumSleds = 0;

	// 랙 내부 냉각수 노드 열용량 [J/K]. 약 9.6 kg의 물.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantHeatCapacityJK = 4.0e4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantCpJKgK = 4182.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirCpJKgK = 1005.0;

	// Air density for the fan curves (volume flow <-> mass flow) [kg/m^3]. Same value as the air grid.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirDensityKgM3 = 1.184;

	// ---- Layout ----
	// Every slot uses DeviceTypes[Slot.DeviceType].Components. A rack with no slots has no nodes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<FRackDeviceType> DeviceTypes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<FRackSlot> Slots;
};

// 매 스텝 바깥에서 주어지는 경계 조건.
USTRUCT(BlueprintType)
struct FRackThermalInput
{
	GENERATED_BODY()

	// 노드별 전부하 발열 [W] (노드 순서는 모델의 슬롯 배치). 비어 있으면 모델이 0으로 본다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> ComponentHeatW;

	// 길이 NumSleds. false면 슬롯이 비어 있다 (유닛을 뽑은 상태).
	// 비어 있으면 모델이 전부 도킹된 것으로 본다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<bool> SledDocked;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantInletTempK = 300.15;

	// GB200 NVL72 설계점 (랙당 약 2.3 kg/s, 냉각수 ΔT 약 10 K). 통합 구동에서는 수력 해가 준다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantMassFlowKgs = 2.3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double AirInletTempK = 297.15;

	// Optional inlet temperature per slot [K] (the room air in front of that slot's height).
	// Length != number of slots -> every slot uses AirInletTempK.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> SlotAirInletTempK;

	// Workload utilization 0..1 for every slot (full load = 1: ComponentHeatW as given).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack", meta = (ClampMin = "0", ClampMax = "1"))
	double Utilization = 1.0;

	// Optional per-slot utilization; length != number of slots -> every slot uses Utilization.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> SlotUtilization;

	// Fan speed ratio override 0..1 for every fan-equipped slot. < 0: automatic control (default).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double FanSpeedOverride = -1.0;
};

// 적분되는 상태. 이것만 있으면 시뮬레이션을 이어서 돌릴 수 있다.
USTRUCT(BlueprintType)
struct FRackThermalState
{
	GENERATED_BODY()

	// 노드별 온도 [K]. Input.ComponentHeatW와 같은 순서.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> ComponentTempK;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double CoolantTempK = 300.15;

	// Fan speed ratio per slot (0..1). Integrated state of the fan controller (fan mode only).
	// Length != number of slots -> Initialize sets it to each device's controller target at the initial state.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	TArray<double> SlotFanSpeedRatio;
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

	// Heat each part actually produced in the last step [W] (utilization, fan operating point and PSU loss
	// applied; 0 when undocked). Same index rule as ComponentTempK.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> ComponentHeatW;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> ComponentHeatToAirW;

	// 슬롯별 공기 질량유량 [kg/s]. 빈 슬롯도 포함한다 (그쪽으로 새는 양).
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotAirMassFlowKgs;

	// 유닛을 빠져나온 공기 온도 [K]. 빈 슬롯은 입구 온도 그대로다.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SledAirOutletTempK;

	// Heat each slot hands to the room air [W]: forced air + case natural convection of its parts.
	// The coupling injects this at the slot's own height behind the rack.
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotHeatToAirW;

	// Fan mode: speed ratio, device pressure rise [Pa] and fan electric input [W] per slot (0 without fans).
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotFanSpeedRatio;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotPressureRisePa;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	TArray<double> SlotFanPowerW;

	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	double TotalFanPowerW = 0.0;

	// True when slot flows come from the devices' own fans (see FRackFanSpec).
	UPROPERTY(BlueprintReadOnly, Category = "Rack")
	bool bFanMode = false;

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
