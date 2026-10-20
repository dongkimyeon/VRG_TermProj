// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HydraulicTypes.generated.h"

// 규약 1: 압력 Pa, 체적유량 m^3/s, 질량유량 kg/s.
// S2b (2026-10-20): every hydraulic pressure is GAUGE pressure [Pa(g)], ambient air = 0. Before the leak model only
// pressure differences were used, so this definition changes no earlier result.
// 규약 3: 체적유량 이름은 FlowM3s. 'Q'를 쓰지 않는다.
// 규약 7: 그래프는 인덱스로만 연결한다. 포인터 그래프를 만들지 않는다.

// 배관 그래프의 노드. 합류점이거나 압력 경계다.
USTRUCT(BlueprintType)
struct FHydraulicNode
{
	GENERATED_BODY()

	// true면 압력이 고정된 경계 노드(팽창 탱크, CDU 공급 헤더 등).
	// 네트워크에 고정 압력 노드가 최소 하나는 있어야 압력이 결정된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	bool bFixedPressure = false;

	// 고정 노드면 경계 압력, 자유 노드면 초기 추정값 [Pa(g), gauge].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PressurePa = 1.0e5;

	// 이 노드로 외부에서 직접 들어오는 체적유량 [m^3/s]. 빠져나가면 음수.
	// 랙이 뽑아 쓰는 유량을 여기에 음수로 넣는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ExternalInflowM3s = 0.0;

	// S2b (2026-10-20): node height above the floor [m] (option B of Tools/S2b_누수_계획.md). Pipes and pumps act on
	// the piezometric pressure P + rho*g*z; PressurePa stays the static pressure at this height. 0 everywhere keeps
	// every result bit-identical to the model without heights.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ElevationM = 0.0;
};

// 수동 저항 요소. Δp = Resistance * Flow * |Flow|.
// Δp is the piezometric drop (P_from + rho*g*z_from) - (P_to + rho*g*z_to) (S2b height term; equal to the static
// drop when both ends are at the same height).
USTRUCT(BlueprintType)
struct FHydraulicPipe
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 FromNodeIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 ToNodeIndex = INDEX_NONE;

	// 저항 계수 [Pa*s^2/m^6]. 완전 난류 영역의 2차 손실을 가정한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ResistancePaS2M6 = 2.0e6;
};

// 능동 요소. 유량이 보조 미지수로 들어간다 (D3).
// 펌프 곡선: Δp_pump = ShutoffHeadPa * r^2 - CurveCoeff * Flow * |Flow|   (r = SpeedRatio)
// Δp_pump is the piezometric rise between suction and discharge (the pump's total head as pressure).
USTRUCT(BlueprintType)
struct FHydraulicPump
{
	GENERATED_BODY()

	// 흡입 노드
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 FromNodeIndex = INDEX_NONE;

	// 토출 노드
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 ToNodeIndex = INDEX_NONE;

	// 유량 0일 때의 양정 [Pa].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ShutoffHeadPa = 3.0e5;

	// 곡선 기울기 [Pa*s^2/m^6]. 유량이 늘수록 양정이 떨어지는 정도.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double CurveCoeffPaS2M6 = 1.0e6;

	// 회전수비 r = N / N_rated. 디버그 키로 조절하는 "펌프 속도"가 이것이다.
	//
	// D8이 경고한 "P ∝ Q^3를 쓰지 말라"는 다른 장비끼리 비교할 때의 이야기고,
	// 같은 펌프의 회전수 변경은 상사식 적용 대상이다. 상사칙은
	//   Flow ∝ r,   Head ∝ r^2
	// 이므로 곡선을 (Flow/r, Head/r^2) 좌표로 옮겨 놓으면 계수는 그대로 두고
	// 체절양정만 r^2으로 스케일하면 상사 관계가 성립한다:
	//   Δp = ShutoffHeadPa * r^2 - CurveCoeff * Flow * |Flow|
	//
	// 전력은 여전히 운전점에서 구한다. P_rated * r^3 으로 만들지 않는다
	// (애초에 이 모델에는 P_rated가 없다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	double SpeedRatio = 1.0;

	// D8: 유체 동력 -> 전기 입력 환산용 종합 효율 (펌프 + 모터).
	// 이 모델에는 P_rated가 아예 없다. 전력은 항상 운전점에서 계산한다:
	//   유체 동력 = Δp * FlowM3s,  전기 입력 = 유체 동력 / Efficiency.
	// P ∝ Flow^3 같은 상사식으로 전력을 만들지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double Efficiency = 0.7;
};

// S2b step 1 (2026-10-20): a leak orifice at a node, discharging to the ambient at the node's height.
//   Flow = Cd * A * sqrt(2 * (P - P_amb) / rho)   for P > P_amb, else 0 (no inflow from the ambient),
// i.e. P - P_amb = R_leak * Flow^2 with R_leak = rho / (2 Cd^2 A^2), the same quadratic form as a pipe.
// P is the static gauge pressure at the node (the jet leaves at the node's own height, so no height term).
// The flow is an outflow of that node in the mass balance. Diameter 0 = no leak (results bit-identical).
USTRUCT(BlueprintType)
struct FHydraulicLeak
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 NodeIndex = INDEX_NONE;

	// Equivalent round hole diameter [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.0"))
	double HoleDiameterM = 0.0;

	// Discharge coefficient. 0.61 is the textbook value of a sharp-edged orifice; a real crack or fitting gap is
	// not that shape, so this is an ESTIMATE (cracks and loose fittings are commonly quoted between about 0.6 and 1).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	double DischargeCoefficient = 0.61;

	// Pressure the jet discharges into [Pa(g)]; 0 = room air.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double AmbientPressurePa = 0.0;
};

// D5: 정규화 잔차 목표와 최대 반복 수를 함께 쓴다.
USTRUCT(BlueprintType)
struct FHydraulicSolveSettings
{
	GENERATED_BODY()

	// 무차원 정규화 잔차 목표. 정규화 기준은 결과 구조체에 함께 실린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double NormalizedResidualTarget = 1.0e-10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 MaxIterations = 60;

	// Jacobian에서만 쓰는 정규화 하한. 잔차에는 적용하지 않으므로
	// 수렴한 해는 영향을 받지 않는다 (수렴 경로만 안정화된다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double JacobianDeltaPFloorPa = 1.0e-3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double JacobianFlowFloorM3s = 1.0e-6;

	// 잔차가 커지면 스텝을 절반씩 줄여 다시 시도하는 최대 횟수.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	int32 MaxBacktrackSteps = 12;
};

// D5: 수렴 실패를 결과에 표시한다. 호출자가 bConverged를 보지 않고 쓰면 안 된다.
USTRUCT(BlueprintType)
struct FHydraulicSolveResult
{
	GENERATED_BODY()

	// D3: same Newton equations, sparse LU for networks exceeding 200 nodes.
	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	bool bUsedSparse = false;

	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	bool bConverged = false;

	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	int32 IterationsUsed = 0;

	// 무차원. 질량 블록은 ReferenceFlowM3s로, 펌프 블록은 ReferencePressurePa로
	// 각각 나눈 뒤 둘 중 큰 값을 취한다.
	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	double NormalizedResidual = 0.0;

	// 정규화 기준을 함께 노출한다 (검증 규칙: 정규화 기준과 단위를 함께 적는다).
	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	double ReferenceFlowM3s = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	double ReferencePressurePa = 0.0;

	// 선형 풀이가 특이해져 중단됐는지.
	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	bool bLinearSolveFailed = false;

	// 고정 압력 노드가 없는 등 구성 자체가 잘못됐는지.
	UPROPERTY(BlueprintReadOnly, Category = "Hydraulics")
	bool bIllPosed = false;
};
