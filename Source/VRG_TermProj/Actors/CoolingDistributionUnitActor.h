// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Hydraulics/CoolantInventory.h"
#include "CoolingDistributionUnitActor.generated.h"

// Authored layout values only. Defaults are captured from the actor, never duplicated here.
USTRUCT()
struct FCduLayoutSettings
{
	GENERATED_BODY()
	UPROPERTY() FVector SizeM = FVector::ZeroVector;
	UPROPERTY() double ConnectionHeightM = 0;
	UPROPERTY() double ConnectionSpacingM = 0;
	UPROPERTY() bool bUseReferenceModel = false;
	UPROPERTY() double ReferencePressurePa = 0;
	UPROPERTY() double PumpShutoffHeadPa = 0;
	UPROPERTY() double PumpCurveCoeffPaS2M6 = 0;
	UPROPERTY() double PumpEfficiency = 0;
	UPROPERTY() double PumpSpeedRatio = 0;
	UPROPERTY() double FacilitySupplyTempK = 0;
	UPROPERTY() double FacilityMassFlowKgs = 0;
	UPROPERTY() double HxUAWK = 0;
	UPROPERTY() double FacilityDesignRiseK = 0;
	UPROPERTY() double HxDesignApproachK = 0;
	bool IsValid() const;
	bool operator==(const FCduLayoutSettings& Other) const;
};

class UStaticMeshComponent;

// CDU(냉각수 분배 장치). 레벨에 직접 놓는다.
//
// 수력 네트워크에서 이 액터는 두 가지다:
//   - 유일한 고정 압력 노드 (여기가 없으면 압력이 결정되지 않는다)
//   - 펌프 (곡선과 효율을 들고 있다)
//
// 배관은 이 액터를 찾아서 잇는다. 액터를 옮기면 배관도 따라온다.
// 라디에이터(열 쪽)는 아직 랙 액터가 각자 들고 있다. 냉각수 루프를 열 도메인까지
// 하나로 묶는 것은 S4의 일이다.
UCLASS()
class VRG_TERMPROJ_API ACoolingDistributionUnitActor : public AActor
{
	GENERATED_BODY()

public:
	ACoolingDistributionUnitActor();
	UPROPERTY(VisibleInstanceOnly, Category = "Layout") FString LayoutId;
	UPROPERTY(Transient) bool bRuntimeLayoutCdu = false;
	FCduLayoutSettings CaptureLayoutSettings() const;
	void ApplyLayoutSettings(const FCduLayoutSettings& Settings);

	virtual void Tick(float DeltaTime) override;

	// 레벨 뷰포트에서도 틱이 돌게 해서 Play 없이 배치를 확인할 수 있게 한다.
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	// ---- 외형 (m) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CDU")
	FVector SizeM = FVector(0.9, 0.8, 1.9);

	// 배관이 붙는 높이 [m]. 바닥 배관 높이에 맞춘다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CDU")
	double ConnectionHeightM = 0.25;

	// 공급 토출구와 회수 흡입구 사이 거리 [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CDU")
	double ConnectionSpacingM = 0.30;

	// Reference-inspired cabinet; use the original primitive body if disabled or unavailable.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bUseReferenceModel = true;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Visualization")
	void RefreshVisuals() { RebuildMeshes(); bMeshesDirty = false; }

	// ---- 수력 ---- (pressures are gauge, Pa(g): ReferencePressurePa is above room air)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReferencePressurePa = 2.0e5;

	// S2b step 2: expansion tank / make-up water of this CDU's loop (FExpansionTankParams notes the sources and the
	// estimates). ReferencePressurePa is its fill pressure. Not part of the saved layout settings yet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	FExpansionTankParams ExpansionTank;

	// 한 줄(랙 8대, 18.4 L/s)을 회전수비 1에서 여유 없이 미는 운전점에서 역산한 곡선이다(체절 - 계수 Q²).
	// 2026-11-12: 랙 저항 정정(ADataCenterRoomActor::RackBranchResistancePaS2M6, 랙 ΔP 179.9 -> 79.4 kPa)으로 설계 루프
	// 강하가 약 195 -> 94.5 kPa(헤더 약 15 kPa는 그대로)라, 같은 곡선 모양을 0.485배로 줄였다: 4e5 -> 1.94e5, 6e8 -> 2.91e8.
	// 설계점 양정 = 1.94e5 - 2.91e8 x 0.0184² = 95.5 kPa. 다른 랙을 쓰면 여기부터 다시 역산해야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpShutoffHeadPa = 1.94e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpCurveCoeffPaS2M6 = 2.91e8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	double PumpEfficiency = 0.7;

	// 회전수비. 상사식으로 체절양정을 r^2으로 스케일한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	double PumpSpeedRatio = 1.0;

	// ---- Heat exchanger to facility water (2026-09-27, used when the integrated actor runs CDU mode) ----
	// Facility water (FWS) supply temperature [K]. 22 C keeps the per-rack radiators' ambient for comparison;
	// warm-water liquid cooling plants often run higher. A site value, not a product rating.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Exchanger")
	double FacilitySupplyTempK = 295.15;

	// Facility water flow [kg/s] and exchanger UA [W/K]. 0 = size from the row's design coolant load:
	// flow carries it at FacilityDesignRiseK; UA reaches the supply HxDesignApproachK above the facility water.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Exchanger", meta = (ClampMin = "0.0"))
	double FacilityMassFlowKgs = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Exchanger", meta = (ClampMin = "0.0"))
	double HxUAWK = 0.0;

	// Design assumptions for the sizing above (not measured): facility water rise [K], supply approach [K].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Exchanger", meta = (ClampMin = "1.0"))
	double FacilityDesignRiseK = 10.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Exchanger", meta = (ClampMin = "0.5"))
	double HxDesignApproachK = 4.0;

	// 공급 토출구 / 회수 흡입구의 월드 좌표 [m].
	// 배관을 잇는 쪽이 이 두 점을 쓴다. 단위는 m이므로 그리는 쪽이 cm로 바꾼다 (규약 1).
	UFUNCTION(BlueprintCallable, Category = "CDU")
	FVector GetSupplyPortWorldM() const;

	UFUNCTION(BlueprintCallable, Category = "CDU")
	FVector GetReturnPortWorldM() const;

	// ---- CDU control and redundancy (2026-11-03, Tools/실사양정합_계획.md P3) ----
	// Real CDUs run two independent loops (DMTF Redfish CDU controls draft; vendor CDUs): the pumps' VFD holds the
	// secondary differential pressure (or flow), and the primary valve holds the secondary supply temperature.
	// Secondary pump speed control: the VFD holds the differential pressure at the hydraulically remote (last) rack
	// at the value that rack needs for its design flow, 2 R_branch Q_design^2 (remote differential-pressure sensing,
	// variable-flow pumping practice). Debug keys 5/6 scale this setpoint by r^2 (equivalent to a speed ratio r at
	// design). Off = the old fixed-speed pump.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bPumpDpControl = true;

	// Rack branch design flow [m^3/s]: GB200 NVL72 2.3 L/s = 138 L/min. Per kW: 138 / 104 kW liquid-cooled heat =
	// 1.33 L/min/kW (OCP 1.25-2.0 L/min/kW range); per total rack 119 kW it is 1.16. Above Lenovo's GB300 NVL72 minimum
	// of 89 L/min at a 35 C supply (2026-11-12 check).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.0"))
	double RackDesignFlowM3s = 2.3e-3;

	// VFD limits and ramp. ESTIMATES (typical VFD minimum speed 20-30 %; no public CDU ramp rate found). The pump
	// curve was sized for the design row at speed 1 (no margin), so the maximum is 1.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double PumpMinSpeedRatio = 0.3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	double PumpMaxSpeedRatio = 1.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.001"))
	double PumpRampPerS = 0.1;

	// Secondary supply temperature control through the primary (facility) valve. Setpoint = design facility supply +
	// HxDesignApproachK (the design operating point; no extra margin). Off = the old fixed facility flow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control")
	bool bSupplyTempControl = true;
	// Valve full stroke time [s] and equal-percentage rangeability, used without the plant model (with it, the FWS
	// loop's valves are used). ESTIMATES, the same values as the plant's valves (ValveStrokeS, ValveRangeability).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.1"))
	double PrimaryValveStrokeS = 60.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "1.01"))
	double PrimaryValveRangeability = 50.0;

	// N+1 pumps (vendor CDUs: duty + standby, e.g. "dual hot-swap pumps"). Identical pumps in parallel behind check
	// valves with one running are hydraulically the same as that one pump, so the network keeps one pump element and
	// this CDU keeps the pump units' state. Two pumps running together (lead-lag) is not modelled.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0"))
	int32 StandbyPumps = 1;
	// Automatic changeover to a standby pump after a pump fault [s]: the loop has no active pump meanwhile. ESTIMATE.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.0"))
	double PumpSwitchoverS = 2.0;

	// 디버그 키용.
	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	void SetPumpSpeedRatio(double InSpeedRatio);

	// Runtime command only; not layout/save settings. A new simulation resets it.
	bool IsPumpPowered() const { return bPumpPowered; }
	void SetPumpPowered(bool bPowered) { bPumpPowered=bPowered; }

	// Runtime pump unit faults (N+1). A fault of the running pump starts the changeover; with no healthy pump left the
	// loop has no active pump. Not saved; a new simulation repairs every pump.
	int32 GetFailedPumps() const { return FailedPumps; }
	int32 GetNumPumps() const { return 1 + FMath::Max(StandbyPumps, 0); }
	bool HasHealthyPump() const { return FailedPumps < GetNumPumps(); }
	double GetSwitchoverRemainingS() const { return SwitchoverRemainingS; }
	// Fails the running pump. Returns false when no healthy pump is left to fail.
	bool FailRunningPump() { if (!HasHealthyPump()) return false; ++FailedPumps; SwitchoverRemainingS = HasHealthyPump() ? PumpSwitchoverS : 0.0; return true; }
	void RepairPumps() { FailedPumps = 0; SwitchoverRemainingS = 0.0; }
	void AdvancePumpSwitchover(double DtS) { SwitchoverRemainingS = FMath::Max(0.0, SwitchoverRemainingS - DtS); }
	// The network pump runs: CDU powered (W4), a healthy pump, and no changeover in progress.
	bool IsPumpRunning() const { return bPumpPowered && HasHealthyPump() && SwitchoverRemainingS <= 0.0; }
private:
	bool bPumpPowered=true;
	int32 FailedPumps = 0;
	double SwitchoverRemainingS = 0.0;
public:

protected:
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> BodyMeshes;

	bool bMeshesDirty = true;

	void RebuildMeshes();
};
