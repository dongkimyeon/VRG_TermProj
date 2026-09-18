// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "HydraulicNetworkDebugActor.generated.h"

// S2 배관망을 레벨에서 눈으로 보기 위한 액터.
//
// 실제 데이터센터 인로우 냉각 루프 배치를 따른다:
//   CDU -> 공급 헤더(행을 따라 진행) -> 랙별 분기 -> 회수 헤더 -> CDU
//
// 솔버는 좌표를 모른다 (위상만 안다). 노드 위치는 이 액터가 m 단위로 들고 있다가
// 그리는 순간에만 cm로 바꾼다 (규약 1).
//
// 색은 계산된 유량 크기를 나타낸다. 온도가 아니다 — S2에는 온도가 없다.
// 공급/회수 구분은 색이 아니라 배치(헤더 위치)로 표현한다.
UCLASS()
class VRG_TERMPROJ_API AHydraulicNetworkDebugActor : public AActor
{
	GENERATED_BODY()

public:
	AHydraulicNetworkDebugActor();

	virtual void Tick(float DeltaTime) override;

	// 레벨 뷰포트에서도 틱이 돌게 해서 Play 없이 배치를 확인할 수 있게 한다.
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Hydraulics")
	void Resolve();

	// ---- 배치 (m) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (ClampMin = "1", ClampMax = "64"))
	int32 NumRacks = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double RackSpacingM = 0.8;

	// CDU에서 첫 랙까지의 거리.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double CduOffsetM = 1.5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double SupplyHeaderYM = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double ReturnHeaderYM = 1.6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double HeaderHeightM = 0.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double RackNodeHeightM = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector RackSizeM = FVector(1.2, 0.6, 2.0);

	// ---- 수력 파라미터 ----
	// 기본값은 15 kW 랙을 냉각수 ΔT 약 4~5 K로 식히는 운전점(랙당 0.7~0.9 L/s)에서
	// 역산했다. 유량이 작으므로 저항 계수(Pa*s^2/m^6)는 큰 값이 된다.
	// 저항은 Δp = R * Flow^2 이므로, L/s 단위로 생각하면 감이 맞지 않는 게 정상이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double HeaderSegmentResistancePaS2M6 = 2.7e8;

	// 랙 콜드플레이트 분기. 공급 쪽과 회수 쪽에 각각 적용된다.
	// 실제 시스템에서도 콜드플레이트가 루프 저항의 대부분을 차지한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double RackBranchResistancePaS2M6 = 1.4e11;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReturnToCduResistancePaS2M6 = 5.4e8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double CduReferencePressurePa = 2.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpShutoffHeadPa = 4.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpCurveCoeffPaS2M6 = 8.0e9;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	double PumpEfficiency = 0.7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	FHydraulicSolveSettings SolveSettings;

	// ---- 표시 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowRackBoxes = true;

	// 끄면 랙 분기 유량만 표시한다. 켜면 모든 배관에 유량을 적는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowAllFlowLabels = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowNodePressures = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FHydraulicNetwork Network;
	FHydraulicSolveResult LastResult;

	// 솔버가 모르는 기하 정보. 노드 인덱스와 1:1로 대응한다.
	TArray<FVector> NodePositionM;

	// 랙별 공급 분기 파이프 인덱스. 유량 라벨과 편차 계산에 쓴다.
	TArray<int32> RackBranchPipeIndices;

	bool bNeedsRebuild = true;

	void RebuildAndSolve();
	void DrawNetwork() const;
	void DrawSummary() const;

	FVector NodeWorldLocation(int32 NodeIndex) const;
	static FLinearColor FlowMagnitudeToColor(double NormalizedMagnitude);
};
