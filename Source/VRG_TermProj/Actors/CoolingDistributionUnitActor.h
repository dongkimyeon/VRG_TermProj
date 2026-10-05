// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
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

	// ---- 수력 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReferencePressurePa = 2.0e5;

	// GB200 NVL72 한 줄(랙 8대, 18.4 L/s)을 약 195 kPa로 미는 운전점에서 역산한 값이다.
	// 다른 랙을 쓰면 여기부터 다시 역산해야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpShutoffHeadPa = 4.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpCurveCoeffPaS2M6 = 6.0e8;

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

	// 디버그 키용.
	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	void SetPumpSpeedRatio(double InSpeedRatio);

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
