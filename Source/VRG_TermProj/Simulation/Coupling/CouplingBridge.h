// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Thermal/RackThermalTypes.h"
#include "Simulation/Thermal/RadiatorModel.h"
#include "CouplingBridge.generated.h"

class FHydraulicNetwork;
class FRackThermalModel;

// S4: 도메인 연결과 전역 열수지.
//
// --- 규약 6: 하나의 전달량은 한 번만 계산한다 ---
// 이 클래스가 도메인 사이에 오가는 값을 한 번 계산해 양쪽에 나눠 준다.
// 각 모델이 같은 값을 따로 다시 계산하지 않는다. 그래서 장부가 어긋나지 않는다.
//
//   수력 -> 열:   랙 분기 체적유량 -> 랙 냉각수 질량유량
//   공기 -> 열:   랙 앞 공기 온도  -> 랙 흡기 온도
//   열  -> 공기:  랙이 공기로 내보낸 열 -> 공기 격자의 체적 열원
//                 (강제 공랭 + 케이스 자연대류를 합친 값이다. 자연대류도 결국
//                  방 공기를 데우므로 빼면 전역 장부에서 그만큼 새어 나간다)
//
// --- D1: 공기에 넣을 때는 합산한다 ---
// 랙은 유닛 18장 x 부품 4개로 쪼개져 있지만, 공기 격자에 넘길 때는 반드시
// **하나의 체적 열원**으로 합산한다. 부품별로 쪼개 넣거나 배기 온도를
// T_in + HeatW/(mdot*cp)로 덮어쓰지 않는다. 두 방식을 함께 쓰면 열이 중복 계상된다.
//
// --- 규약 4: 하나의 시계 ---
// 도메인마다 갱신 주기가 달라도 같은 시간 구간을 적분한다.
// 수력은 유량이 바뀌었을 때만 다시 풀지만, 그 사이에도 열과 공기는 같은 dt로 돈다.
//
// --- 전역 열수지 ---
// 계 전체로 보면 IT 발열이 들어오고, 라디에이터(냉각수)와 CRAH 코일(공기)로 나간다.
// 랙 노드·냉각수 노드·공기 격자에 쌓인 양이 축적이다.
//   유입 - 유출 - 축적 = 0
// 랙에서 공기로 넘어간 열은 계 안에서의 이동이라 유입에도 유출에도 넣지 않는다.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않으므로 워커 스레드에서 돌릴 수 있다.

// 다리가 빌려 쓰는 랙 하나의 모델·입력·라디에이터.
// 수명은 바깥(액터)이 쥔다. 여기서는 포인터로 빌리기만 한다.
struct FRackCouplingTarget
{
	FRackThermalModel* Model = nullptr;
	FRackThermalInput* Input = nullptr;

	// 랙마다 붙은 라디에이터. 냉각수 루프를 닫고 계 밖으로 열을 버린다.
	const FRadiatorParams* Radiator = nullptr;
};

// 랙 하나가 다른 도메인과 주고받는 값.
// 액터가 아니라 인덱스로만 랙을 가리킨다 (규약 5: 워커가 액터를 조회하지 않는다).
USTRUCT(BlueprintType)
struct FRackCouplingLink
{
	GENERATED_BODY()

	// 이 랙의 공급 분기 파이프 인덱스. INDEX_NONE이면 수력에 안 붙어 있다.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	int32 BranchPipeIndex = INDEX_NONE;

	// 랙 흡기 쪽 공기 셀들. 여기 온도를 평균해 랙 흡기 온도로 준다.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	TArray<int32> InletCellIndices;

	// 랙 배기 쪽 공기 셀들. 랙이 공기로 내보낸 열을 여기에 고르게 나눠 넣는다.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	TArray<int32> ExhaustCellIndices;

	// 수력에 안 붙어 있을 때 쓸 냉각수 질량유량 [kg/s].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	double FallbackCoolantMassFlowKgs = 2.3;

	// Per-slot air coupling (optional). Slot s uses cells [Start[s], Start[s+1]) of the flat lists: the
	// inlet / exhaust cells whose height overlaps that slot. When these are sized NumSlots + 1 the bridge
	// gives every slot its own inlet temperature and injects every slot's air-side heat at its own height
	// instead of lumping the rack. A slot with no cells falls back to the rack-wide lists above.
	TArray<int32> SlotInletStart, SlotInletCells, SlotExhaustStart, SlotExhaustCells;
};

// 한 스텝에 실제로 오간 값. 화면에 그대로 띄워 어디서 끊겼는지 볼 수 있게 한다.
USTRUCT(BlueprintType)
struct FCouplingSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double SimTimeS = 0.0;

	// 랙별로 수력에서 받은 냉각수 질량유량 [kg/s].
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	TArray<double> RackCoolantMassFlowKgs;

	// 랙별로 공기에서 받은 흡기 온도 [K].
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	TArray<double> RackAirInletTempK;

	// 랙별로 공기에 넘긴 열 [W]. D1대로 부품별 합산값 하나다.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	TArray<double> RackHeatToAirW;

	// ---- 전역 열수지 ----
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double TotalItHeatW = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double TotalRadiatorRejectedW = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double TotalAirCoolingRemovedW = 0.0;

	// 누적값 [J].
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double EnergyInJ = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double EnergyOutJ = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double EnergyStoredJ = 0.0;

	// 유입 기준 정규화 잔차. 전역 보존 검사 기준이다.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	double NormalizedEnergyResidual = 0.0;

	// 수력을 이번 스텝에 다시 풀었는지, 그 결과가 수렴했는지.
	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	bool bHydraulicsResolvedThisStep = false;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	bool bHydraulicsConverged = true;

	UPROPERTY(BlueprintReadOnly, Category = "Coupling")
	bool bAirPressureConverged = true;
};

// 랙 여러 대 + 수력망 하나 + 공기 격자 하나를 한 시계로 돌린다.
// 모델의 수명은 바깥(액터)이 쥐고, 여기서는 포인터로 빌려 쓴다.
class VRG_TERMPROJ_API FCouplingBridge
{
public:
	// 랙 대상과 연결 정보를 짝지어 넘긴다. 둘의 길이가 같아야 한다.
	void Initialize(const TArray<FRackCouplingTarget>& InTargets,
		const TArray<FRackCouplingLink>& InLinks);

	// 수력망과 공기 격자를 붙인다. nullptr이면 그 도메인은 쓰지 않는다.
	void SetHydraulicNetwork(FHydraulicNetwork* InNetwork) { HydraulicNetwork = InNetwork; }
	void SetAirSolver(FAirSolver* InAirSolver) { if (AirSolver != InAirSolver) bLedgerBaselineTaken = false; AirSolver = InAirSolver; }

	// 물의 밀도 [kg/m^3]. 체적유량을 질량유량으로 바꿀 때만 쓴다.
	void SetCoolantDensityKgM3(double InDensityKgM3) { CoolantDensityKgM3 = InDensityKgM3; }

	// 공기 솔버에 넘길 입력. 냉각 코일(CellCoolingUAWK, CoolingSupplyTempK)과
	// 중력·부력 설정은 바깥에서 채운다.
	// CellHeatW는 Step()이 매번 덮어쓴다. 랙에서 계산해 넘기는 값이라 손으로 넣으면
	// 그 스텝에 지워진다.
	FAirSolverInput AirSolverInput;

	// 한 스텝. 안에서 랙 열모델과 공기 솔버를 같은 dt로 돌린다 (규약 4).
	// bHydraulicsResolved는 이번 스텝에 수력이 다시 풀렸는지를 바깥에서 알려 주는 값이다.
	void Step(double DeltaTimeS, bool bHydraulicsResolved, bool bHydraulicsConverged);

	FCouplingSnapshot MakeSnapshot(double SimTimeS) const;

	// ---- 전역 열수지 (규약: 축적량을 포함한다) ----
	double GetEnergyInJ() const { return EnergyInJ; }
	double GetEnergyOutJ() const { return EnergyOutJ; }
	double GetEnergyStoredJ() const;
	double GetEnergyResidualJ() const;
	double GetNormalizedEnergyResidual() const;
	// Where a global residual comes from [J]: sum of rack-model residuals, air-solver residual, heat handed
	// to the air but dropped because the target cell was solid or outside the grid, and the rest (coupling).
	FString DescribeEnergyLedger() const;

	// 랙마다 붙은 라디에이터가 지금 버리는 열의 합 [W].
	double GetTotalRadiatorRejectedW() const { return LastRadiatorRejectedW; }

	// S7-0 계측. 직전 스텝의 공기 솔버 시간 [ms]. 게임 스레드 쪽이 읽어 누산한다.
	double GetLastAirSolverMs() const { return LastAirSolverMs; }

private:
	TArray<FRackCouplingTarget> Targets;
	TArray<FRackCouplingLink> Links;

	FHydraulicNetwork* HydraulicNetwork = nullptr;
	FAirSolver* AirSolver = nullptr;

	double CoolantDensityKgM3 = 997.0;

	// 전역 장부 [J]. Step()이 매번 다시 합산한다.
	double EnergyInJ = 0.0;
	double EnergyOutJ = 0.0;

	// 라디에이터가 버린 누적 열 [J]. 공기 쪽 유출은 공기 솔버가 따로 센다.
	double RadiatorRejectedJ = 0.0;

	// 마지막 스텝에 오간 값. 스냅샷에 그대로 싣는다.
	TArray<double> LastCoolantMassFlowKgs;
	TArray<double> LastAirInletTempK;
	TArray<double> LastHeatToAirW;
	double LastItHeatW = 0.0;
	double LastRadiatorRejectedW = 0.0;
	double LastAirCoolingRemovedW = 0.0;

	// 규약 5대로 전역 누산기를 건드리지 않고 멤버에 담는다.
	double LastAirSolverMs = 0.0;
	bool bLastHydraulicsResolved = false;
	bool bLastHydraulicsConverged = true;

	// 공기 격자에 넣을 셀별 열원 [W]. 매 스텝 재할당하지 않으려고 멤버로 들고 있다.
	TArray<double> ScratchCellHeatW;
	double DroppedAirHeatJ = 0.0;
	double RackCoolantOutJ = 0.0, RackAirOutJ = 0.0;
	// The rack models and the air solver may have run before this bridge took over (the integrated actor
	// retries until the room has found its racks). The global ledger counts only what happened since the
	// first coupled step, so those earlier standalone steps cannot show up as a constant residual.
	bool bLedgerBaselineTaken = false;
	double BaselineModelInJ = 0.0, BaselineStoredJ = 0.0, BaselineAirOutJ = 0.0;
	double GetRawEnergyStoredJ() const;

	// 수력에서 랙별 냉각수 질량유량을 읽는다. 없으면 예비값을 쓴다.
	void PullCoolantFlow();

	// 공기 격자에서 랙별 흡기 온도를 읽는다. 없으면 랙이 들고 있던 값을 그대로 둔다.
	void PullAirInletTemperature();

	// 라디에이터로 냉각수 루프를 닫는다. 버린 열을 **한 번만** 계산해
	// 랙 입구 온도와 전역 장부에 같은 값으로 쓴다 (규약 6).
	void CloseCoolantLoop(double DeltaTimeS);

	// 랙이 공기로 내보낸 열을 공기 격자의 체적 열원으로 민다 (D1: 합산해 하나로).
	void PushHeatToAir();
};
