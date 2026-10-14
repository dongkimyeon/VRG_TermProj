// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Thermal/RackThermalTypes.h"

// S1(확장): 랙 1대 과도 열모델. 캐비닛 = 슬롯 N개, 슬롯마다 장비 한 대(FRackDeviceType), 장비 = 부품 여러 개.
// 2026-09-27부터 장비는 공개 사양 기반 프리셋(GB200 NVL72, Quantum 스파인)만 쓴다.
//
// 노드는 모든 슬롯 장비의 부품 노드 + 냉각수 노드 1개다.
// 전부 열용량을 가진 과도 노드다 (D2).
//   부품 i:   C_i * dT_i/dt = P_i - Heat_to_coolant_i - Heat_to_air_i - Heat_to_ambient_i
//   냉각수:   C_c * dT_c/dt = sum(Heat_to_coolant_i) - Heat_removed_by_coolant
//
// 룸 공기는 준정상(D2)이라 열용량 노드를 두지 않고 유효 전도도로만 열을 가져간다.
//
// --- 장비 안 공기 경로 ---
// 장비 하나 안에서 공기는 부품 배열 순서대로 지나간다 (AirStage가 같은 연속 부품은 병렬). 뒤쪽 부품은 앞 단이 데운
// 공기를 받는다. 모든 부품이 AirInletTempK를 보면 냉각 성능을 과대평가한다.
// 출구 공기 온도는 나눗셈 없이 effectiveness로 직접 잇는다:
//   eps = 1 - exp(-UA / (MassFlowKgs * Cp)),  T_out = T_in + eps * (T_comp - T_in)
//   Heat = MassFlowKgs * Cp * eps * (T_comp - T_in) = CapacityRate * (T_out - T_in)
// 두 식이 같은 eps를 쓰므로 구간별 열량의 합과 전체 공기 엔탈피 상승이 정확히 같다.
//
// --- 슬롯 공기 유량 ---
// 슬롯마다 자기 장비 팬의 운전점(팬 곡선과 장비 임피던스의 교점)이다. 팬 없는 장비와 뽑힌 슬롯은 0이다.
// 랙 앞뒤 정압은 같다고 본다 (통로 압력차에 따른 빈 슬롯 재순환은 모델에 없다).
//
// D2 준수: 어디에서도 HeatW/(MassFlowKgs*Cp)를 계산하지 않는다. 유량이 분모에 오는
// 곳은 NTU뿐이고 유량 0에서는 분기로 처리한다. 유량이 0이면 CapacityRate가 0이라
// 실어 나가는 열도 0이 되고, 열이 축적되어 온도가 올라간다.
//
// 규약 6 준수: 열유속은 스텝당 한 번만 계산해서 주는 쪽과 받는 쪽에 부호만 바꿔
// 같은 값으로 적용한다. 각 노드가 같은 열유속을 따로 다시 계산하지 않는다.
//
// 규약 7 준수: 부품 상태는 평탄 배열 + 선형 인덱스다. 포인터 그래프를 만들지 않는다.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않으므로 워커 스레드에서 돌릴 수 있다.
class FRackThermalModel
{
public:
	// InitialState.ComponentTempK 길이가 맞지 않으면 CoolantTempK로 채워 맞춘다.
	void Initialize(const FRackThermalParams& InParams, const FRackThermalState& InitialState);

	// 고정 스텝 한 번을 명시적 오일러로 적분한다.
	void Step(const FRackThermalInput& Input, double DeltaTimeS);

	FRackThermalSnapshot MakeSnapshot(double SimTimeS) const;

	const FRackThermalState& GetState() const { return State; }
	const FRackThermalParams& GetParams() const { return Params; }

	int32 GetNumSleds() const { return Params.NumSleds; }
	int32 GetNumNodes() const { return NodeParams.Num() > 0 ? NodeParams.Num() : CountNodes(Params); }

	// ---- Slot / node layout (heterogeneous racks) ----
	// Node n belongs to slot s when SlotNodeStart[s] <= n < SlotNodeStart[s+1]. Valid after Initialize.
	int32 GetSlotNodeStart(int32 Slot) const { return SlotNodeStart.IsValidIndex(Slot) ? SlotNodeStart[Slot] : INDEX_NONE; }
	int32 GetSlotNodeCount(int32 Slot) const
	{
		return SlotNodeStart.IsValidIndex(Slot + 1) ? SlotNodeStart[Slot + 1] - SlotNodeStart[Slot] : 0;
	}
	const FRackComponentParams& GetNodeParams(int32 Node) const { return NodeParams[Node]; }

	// Usable before Initialize (actors size their inputs from Params).
	static int32 NumSlotsOf(const FRackThermalParams& InParams);
	static int32 CountNodes(const FRackThermalParams& InParams);
	// Per-node parameters in node order and SlotNodeStart (NumSlots + 1 entries).
	static void ResolveLayout(const FRackThermalParams& InParams, TArray<FRackComponentParams>& OutNodeParams,
		TArray<int32>& OutSlotNodeStart);
	// ComponentHeatW filled from every component's DefaultHeatW.
	static TArray<double> MakeDefaultHeatW(const FRackThermalParams& InParams);
	// Debug heat knob (keys 1/2 CPU, 3/4 GPU): FirstNodeHeatW is the new heat of the first node of that kind; every node
	// of the kind gets its own DefaultHeatW times the same multiple (first node heat / its default). Writing the first
	// node's watts into every node of the kind (the pre-2026-10-13 behaviour, applied on every start) put a Grace CPU's
	// 280 W into a management switch's 25 W control CPU. Returns false (array untouched) when the sizes do not match or
	// the first default is 0.
	static bool ApplyKindHeatScale(const FRackThermalParams& InParams, TArray<double>& InOutHeatW, ERackPartKind Kind, double FirstNodeHeatW);
	// Repairs exactly that old trace in a saved heat array (layout files saved from such runs): a CPU/GPU node whose part
	// default differs from the first node
	// of its kind but whose heat equals the first node's heat exactly (and not its own default) is rescaled as
	// ApplyKindHeatScale would. Any other value is a deliberate setting and is kept. Returns the number of nodes repaired.
	static int32 RepairKindHeatSpread(const FRackThermalParams& InParams, TArray<double>& InOutHeatW);

	// Design-point heat that leaves through air [W]: docked components' heat times their air share
	// ToAirUAWK / (ToAirUAWK + ToCoolantUAWK). Used to size the room's air cooling (2-2), not by Step().
	static double DesignHeatToAirW(const FRackThermalParams& Params, const FRackThermalInput& Input);
	// The rest of the docked design heat: the coolant share of every part (sizes the CDU heat exchanger).
	static double DesignHeatToCoolantW(const FRackThermalParams& Params, const FRackThermalInput& Input);

	// ---- NVIDIA GB200 NVL72 preset (device- and chip-level, sources in the .cpp) ----
	// 48U: 18 compute trays (Grace x2, LPDDR5X x2, Blackwell x4, NICs, DPUs, drives, PDB, 8 fans each),
	// 9 NVLink switch trays (NVSwitch x2, fully liquid-cooled), 8 power shelves (6 PSUs each, heat =
	// conversion loss of the live DC load), 2 management switches. About 120 kW per rack.
	static constexpr int32 Gb200Nvl72ComputeTrays = 18;
	static constexpr int32 Gb200Nvl72SwitchTrays = 9;
	static constexpr int32 Gb200Nvl72PowerShelves = 8;
	static constexpr double Gb200UnitM = .04445;
	// U0 bottom (rack-local). Keeps the S5 compute-tray grid: U7 is the old first-node height 0.392 m.
	static constexpr double Gb200SlotZ0M = .392 - 7 * .04445;
	enum EGb200DeviceType : int32 { Gb200DevCompute = 0, Gb200DevNvSwitch = 1, Gb200DevPowerShelf = 2, Gb200DevManagement = 3 };

	static FRackDeviceType Gb200ComputeTray();
	static FRackDeviceType Gb200NvLinkSwitchTray();
	static FRackDeviceType Gb200PowerShelf(int32 NumShelves);
	static FRackDeviceType ManagementSwitch();
	static FRackDeviceType QuantumSpineSwitch();

	static FRackThermalParams MakeGb200Nvl72Params();
	static TArray<double> MakeGb200Nvl72HeatW();

	// Row-end spine cabinet: one air-cooled Quantum switch per given slot height.
	static FRackThermalParams MakeQuantumSpineParams(const TArray<double>& SlotBottomZM);

	// Slots whose device is on the rack coolant loop.
	// With Docked, only docked ones. Hydraulic branch resistance scales with this.
	static int32 CountLiquidSlots(const FRackThermalParams& InParams, const TArray<bool>* Docked = nullptr);

	// ---- Fans ----
	// True when any device type of the layout has fans (reported in the snapshot).
	static bool IsFanMode(const FRackThermalParams& InParams);

	// Operating point of NumFans parallel fans at speed ratio r against dP = R * mdot^2 [kg/s].
	// Intersection of R*rho^2*Q^2 with r^2*P0*(1 - (Q/(r*Qt))^2), Qt = NumFans * MaxFlowM3s:
	//   Q = r * sqrt(P0 / (R*rho^2 + P0/Qt^2))
	// Closed form, no iteration; r = 0 gives 0 (no division by the flow, D2).
	static double FanOperatingMassFlowKgs(const FRackFanSpec& Fans, double ResistancePaS2Kg2,
		double AirDensityKgM3, double SpeedRatio);

	// Controller target speed ratio for a control temperature [K].
	static double FanTargetSpeedRatio(const FRackFanSpec& Fans, double ControlTempK);

	// Air-path resistance of a slot when docked [Pa*s^2/kg^2] (device value; 0 for an invalid slot).
	static double DockedResistanceOf(const FRackThermalParams& InParams, int32 Slot);

	// Fans of the device in a slot, or nullptr (fanless device, invalid slot).
	static const FRackFanSpec* FansOf(const FRackThermalParams& InParams, int32 Slot);

	// Heat multiplier of a part at utilization u: f + (1 - f) * u, f = IdleHeatFraction.
	static double LoadFactor(const FRackComponentParams& Part, double Utilization);

	// Utilization of a slot from the input (per-slot array when its length matches, else the scalar), 0..1.
	static double SlotUtilizationOf(const FRackThermalInput& Input, int32 Slot, int32 NumSlots);

	// Racks saved in a level keep an old copy of the device definitions. Replace every device type whose Name
	// matches a code preset (GB200 NVL72 devices, management / spine switch) with the current code definition so
	// the code stays the single source of physical defaults. The slot layout is kept. Returns true if changed.
	static bool RefreshKnownDeviceTypes(FRackThermalParams& InOutParams);

	// Cheap accessors for the coupling (the snapshot copies every per-node array).
	const TArray<double>& GetLastSlotAirMassFlowKgs() const { return LastSlotAirMassFlowKgs; }
	const TArray<double>& GetLastSlotHeatToAirW() const { return LastSlotHeatToAirW; }
	double GetLastTotalHeatW() const { return LastTotalHeatW; }
	double GetLastHeatRemovedByCoolantW() const { return LastHeatRemovedByCoolantW; }
	double GetLastTotalHeatToRoomAirW() const { return LastTotalHeatToRoomAirW; }
	bool IsInFanMode() const { return bFanMode; }
	double GetLastTotalFanPowerW() const { double W = 0.0; for (const double P : LastSlotFanPowerW) W += P; return W; }
	// PSU conversion loss of the last step [W] (sum of LossFractionOfRackLoad x DC load, without the PSU fans' share).
	// Recorded where the step computes it; already part of GetLastTotalHeatW (dashboard power breakdown, 2026-10-14).
	double GetLastConversionLossW() const { return LastConversionLossW; }

	// 누적 에너지 [J]. 보존 검사용.
	double GetEnergyInJ() const { return EnergyInJ; }
	double GetEnergyOutJ() const { return EnergyOutJ; }

	// 초기 상태 대비 모든 노드에 쌓인 에너지 [J].
	double GetEnergyStoredJ() const;

	// 유입 - 유출 - 축적 [J]. 0에 가까워야 한다.
	double GetEnergyResidualJ() const;

	// 잔차를 총 유입 에너지로 나눈 무차원 값. 유입이 0이면 0을 돌려준다.
	double GetNormalizedEnergyResidual() const;

	// 명시적 오일러 단조 조건: dt * (노드 전도도 합 / 열용량) <= 1.
	// 위반하면 진동하거나 발산한다.
	bool IsStepStable(const FRackThermalInput& Input, double DeltaTimeS) const;

	// 공기 쪽 effectiveness. 유량 0에서 나눗셈을 하지 않는다.
	//   UA <= 0            -> 0   (공기와 결합이 없는 부품은 공기를 데우지 않는다)
	//   CapacityRate <= 0  -> 1   (정체 공기는 부품 온도까지 데워진다. CapacityRate가
	//                              0이므로 실어 나가는 열은 그래도 0이다)
	static double AirEffectiveness(double ToAirUAWK, double AirCapacityRateWK);

private:
	FRackThermalParams Params;
	FRackThermalState State;
	FRackThermalState InitialState;

	double EnergyInJ = 0.0;
	double EnergyOutJ = 0.0;

	// Flattened layout, resolved once at Initialize (rule 7: linear indices, no pointer graph).
	TArray<FRackComponentParams> NodeParams;
	TArray<int32> SlotNodeStart;
	TArray<double> LastSlotHeatToAirW;

	// 마지막 스텝의 열유속. 스냅샷 표시용이며 다시 계산하지 않는다.
	TArray<double> LastHeatToCoolantW;
	TArray<double> LastHeatToAirW;
	TArray<double> LastHeatToAmbientW;
	TArray<double> LastSlotAirMassFlowKgs;
	TArray<double> LastSledAirOutletTempK;
	double LastTotalHeatW = 0.0;
	double LastConversionLossW = 0.0;
	double LastHeatRemovedByCoolantW = 0.0;
	double LastTotalHeatToRoomAirW = 0.0;
	bool bLastStepStable = true;

	// Fan mode (resolved at Initialize) and its last-step values per slot.
	bool bFanMode = false;
	TArray<double> LastSlotPressureRisePa;
	TArray<double> LastSlotFanPowerW;

	// 스텝마다 쓰는 임시 버퍼. 매 프레임 재할당하지 않으려고 멤버로 들고 있다.
	mutable TArray<double> ScratchSlotFlowKgs;

	// Slot air mass flows for this step: each slot's fan operating point.
	void ComputeSlotAirFlows(const FRackThermalInput& Input, TArray<double>& OutSlotFlowKgs) const;

	// Fan electric input of a slot at its current speed [W] (0 without fans or undocked).
	double SlotFanPowerW(int32 Slot, bool bDocked, const FRackThermalInput& Input) const;

	// Per-node heat of the current step [W] and where a slot's fan power is deposited (resolved at Initialize):
	// the device's Fan parts, else its air-cooled parts, else all its parts.
	TArray<double> ScratchNodeHeatW;
	TArray<uint8> NodeGetsFanHeat;
	TArray<int32> SlotFanHeatNodes;

	// Hottest air-cooled (non-fan) part of a slot [K]: the fan controller's input.
	double SlotControlTempK(int32 Slot) const;

	static double HeatAt(const TArray<double>& HeatW, int32 Index);
	static double DesignHeatShareW(const FRackThermalParams& Params, const FRackThermalInput& Input, bool bAir);
	static bool IsDocked(const TArray<bool>& SledDocked, int32 SledIndex, int32 NumSleds);
};
