#pragma once
#include "CoreMinimal.h"

// Exact linear enthalpy transport at a solved hydraulic operating point. Columns:
// tray temperatures, return storage temperature, supply BC, return BC. W/K.
// Zero-capacity junctions are eliminated by their directed mixing equations, not
// by replacing the hydraulic solve or prescribing tray flow fractions.
struct FTrayNetworkTransport
{
	int32 Reservoirs = 0;
	TArray<double> NetWK, BoundaryWK, LeakWK;
	TArray<double> ClassLeakWK; // supply (tee/connector), tray, return (connector/tee)
	TArray<double> ClassLeakKgs;
	double LeakKgs = 0.0, MassResidualKgs = 0.0;
	bool IsValid(int32 Trays) const;
	double Dot(const TArray<double>& Coefficients, int32 Row, const TArray<double>& TrayK,
		double ReturnK, double SupplyK, double ReturnBoundaryK, double ReferenceK) const;
};

// W3.5: 평탄 트레이 냉각수 노드 + 회수 혼합 노드 하나. 공급 체적 지연은 두지 않는다.
// 노드 간 이류는 한 번 계산해 양쪽에 적용한다. 양의 유량은 공급→트레이→회수 방향.
struct FTrayCoolantInput
{
	FTrayNetworkTransport Network; // empty: legacy independent tray/return model
	TArray<double> InletMassFlowKgs, OutletMassFlowKgs, LeakMassFlowKgs;
	TArray<double> HeatFromPartsW, PartsConductanceWK;
	double SupplyTempK = 293.15, ReturnBoundaryTempK = 293.15;
	double ReturnMassFlowKgs = 0.0, ReturnLeakMassFlowKgs = 0.0;
};

class FTrayCoolantModel
{
public:
	// 추정: 기존 총 열용량의 60%를 설계 유량 비례로 트레이에, 40%를 회수 혼합 체적에 둔다.
	// 체적 분배의 실측 자료 없음. 근거/한계: Tools/W3.5_근거와측정.md.
	bool Initialize(const TArray<double>& DesignFractions, double TotalCapacityJK, double InitialTempK, double CpJKgK = 4182.0);
	bool Step(const FTrayCoolantInput& Input, double DtS);
	// Positive explicit-Euler step limit; zero means invalid input. No state/ledger mutation.
	double GetStableStepLimitS(const FTrayCoolantInput& Input) const;
	const TArray<double>& GetTrayTempK() const { return TrayTempK; }
	const TArray<double>& GetTrayCapacityJK() const { return TrayCapacityJK; }
	double GetReturnTempK() const { return ReturnTempK; }
	double GetReturnCapacityJK() const { return ReturnCapacityJK; }
	double GetReferenceTempK() const { return ReferenceTempK; }
	double GetStoredJ() const;
	double GetResidualJ() const { return HeatInJ - HeatOutJ - GetStoredJ(); }
	double GetHeatInJ() const { return HeatInJ; }
	double GetHeatOutJ() const { return HeatOutJ; }
	// 이미 계산한 누수 엔탈피 [W], 기준 InitialTempK. 결합은 이 값을 장부에 전달하고 다시 계산하지 않는다.
	double GetLastLeakHeatW() const { return LastLeakHeatW; }
	double GetLastBoundaryHeatOutW() const { return LastBoundaryHeatOutW; }
	double GetLastMassResidualKgs() const { return LastMassResidualKgs; }
	bool WasStepStable() const { return bStepStable; }
private:
	TArray<double> TrayTempK, TrayCapacityJK, NextTempK, NetHeatW;
	double ReturnTempK = 293.15, ReturnCapacityJK = 0.0, ReferenceTempK = 293.15, CpJKgK = 4182.0;
	double HeatInJ = 0.0, HeatOutJ = 0.0, LastLeakHeatW = 0.0, LastBoundaryHeatOutW = 0.0;
	double LastMassResidualKgs = 0.0;
	bool bStepStable = false;
};
