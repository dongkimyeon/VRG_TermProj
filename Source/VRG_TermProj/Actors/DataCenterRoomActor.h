// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "DataCenterRoomActor.generated.h"

class ARackThermalActor;
class UStaticMeshComponent;

// 데이터센터 한 방. 랙을 배치하면 배관과 배선이 자동으로 랙 뒤에 붙는다.
//
// --- 배관은 장식이 아니다 ---
// 눈에 보이는 관은 수력 솔버가 실제로 푸는 파이프를 그대로 그린 것이다.
// 노드 위치를 이 액터가 m 단위로 들고 있고, 솔버는 위상만 안다 (규약 1).
// 관 색은 계산된 유량 크기다. 온도가 아니다.
//
// --- 배치 ---
// 랙은 두 줄이 통로를 사이에 두고 마주 보는 쌍으로 놓인다 (핫/콜드 통로).
// 앞면이 통로를 향하고, 배관과 배선은 전부 뒷면에 붙는다.
//
//   행 0 (yaw 0, 앞면 +X)        통로        행 1 (yaw 180, 앞면 -X)
//   [랙][랙][랙][랙]         <--------->      [랙][랙][랙][랙]
//    |배관 뒤                                   배관 뒤|
//
// --- 수력 위상 (행마다 하나씩) ---
//   CDU -> 펌프 -> 공급 헤더(행을 따라) -> 랙 분기(뒤로 내려옴) -> 회수 헤더 -> CDU
// AHydraulicNetworkDebugActor가 검증한 인로우 루프와 같은 위상이다.
UCLASS()
class VRG_TERMPROJ_API ADataCenterRoomActor : public AActor
{
	GENERATED_BODY()

public:
	ADataCenterRoomActor();

	virtual void Tick(float DeltaTime) override;

	// 레벨 뷰포트에서도 틱이 돌게 해서 Play 없이 배치를 확인할 수 있게 한다.
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Room")
	void RebuildRoom();

	// ---- 배치 (m) ----
	// 통로 하나를 사이에 둔 두 줄이 1쌍이다. 홀수면 마지막 쌍은 한 줄만 선다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (ClampMin = "1", ClampMax = "16"))
	int32 NumRows = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (ClampMin = "1", ClampMax = "32"))
	int32 RacksPerRow = 8;

	// 랙 외형. ARackThermalActor의 RackSizeM과 같아야 배관이 뒷면에 맞는다.
	// X = 깊이, Y = 폭, Z = 높이.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector RackSizeM = FVector(1.2, 0.6, 2.0);

	// 랙 중심 사이 거리 (행 방향). 랙 폭보다 커야 틈이 생긴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double RackPitchM = 0.66;

	// 마주 보는 두 줄의 앞면 사이 거리 (콜드 통로).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double AisleWidthM = 2.4;

	// 쌍과 쌍 사이에 두는 뒷면 서비스 통로.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double ServiceAisleWidthM = 1.6;

	// 랙 뒷면에서 배관 중심까지의 거리.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double PipeStandoffM = 0.18;

	// 공급관과 회수관 사이 거리 (뒤로 더 나간 쪽이 회수관).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double SupplyReturnSpacingM = 0.16;

	// 냉각수 헤더 높이. GB200 NVL72 참고 배치는 헤더가 바닥 레벨을 따라 돌고
	// 분기관이 랙 밑에서 위로 올라간다 (천장 배관이 아니다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double HeaderHeightM = 0.25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double CableTrayHeightM = 2.85;

	// 랙 뒷면 매니폴드 높이. 분기관이 바닥 헤더에서 여기까지 올라왔다가
	// 다시 회수 헤더로 내려간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double RackManifoldZM = 1.1;

	// 행의 첫 랙에서 CDU까지의 거리.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	double CduOffsetM = 1.4;

	// ---- 굵기 (m) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double HeaderDiameterM = 0.11;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double BranchDiameterM = 0.05;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double CableDiameterM = 0.022;

	// 랙 1대에 내려오는 케이블 가닥 수.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0", ClampMax = "12"))
	int32 CablesPerRack = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowCableTray = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

	// ---- 수력 파라미터 ----
	// GB200 NVL72 운전점에서 역산했다. AHydraulicNetworkDebugActor(15 kW 하이브리드 랙)와는
	// 다른 장비이므로 값이 다르다.
	//   랙 1대 105 kW 중 액랭 97 kW를 냉각수 ΔT 약 10 K로 빼려면 랙당 2.3 L/s가 필요하다.
	//   행 8대면 18.4 L/s이고, 그 유량에서 루프 전체 압력 강하가 약 195 kPa이 되도록 잡았다.
	// 감으로 넣은 값이 아니다. 바꾸면 타당성을 다시 봐야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double HeaderSegmentResistancePaS2M6 = 6.0e6;

	// 슬롯이 전부 찼을 때의 랙 분기 저항. 유닛을 뽑으면 R*(N/n)^2로 올라간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double RackBranchResistancePaS2M6 = 1.7e10;

	// 랙 한 대의 슬롯 수. GB200 NVL72는 컴퓨트 트레이 18장이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SledsPerRack = 18;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReturnToCduResistancePaS2M6 = 1.5e7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double CduReferencePressurePa = 2.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpShutoffHeadPa = 4.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpCurveCoeffPaS2M6 = 6.0e8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	double PumpEfficiency = 0.7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	double PumpSpeedRatio = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	FHydraulicSolveSettings SolveSettings;

	// ---- 랙 액터 ----
	// 켜면 Play할 때 자리마다 진짜 ARackThermalActor를 띄운다 (유닛을 뽑을 수 있다).
	// 에디터에서는 자리만 상자로 보여준다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	bool bSpawnRackActors = true;

	// 디버그 키용. 방 전체 펌프에 같은 회전수비를 준다.
	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	void SetPumpSpeedRatio(double InSpeedRatio);

	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	double GetPumpSpeedRatio() const { return PumpSpeedRatio; }

	// 자리(행, 열)에 놓인 랙 액터. 없으면 nullptr.
	UFUNCTION(BlueprintCallable, Category = "Room")
	ARackThermalActor* GetRackAt(int32 RowIndex, int32 ColumnIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Room")
	int32 GetNumRackSlots() const { return FMath::Max(1, NumRows) * FMath::Max(1, RacksPerRow); }

	// 랙 자리의 월드 좌표 [m]. 바닥 중심이다.
	// 다른 도메인(공기 격자)이 랙 위치를 알아야 할 때 쓴다. 단위는 m이므로
	// 부르는 쪽이 필요할 때만 cm로 바꾼다 (규약 1).
	UFUNCTION(BlueprintCallable, Category = "Room")
	FVector GetRackWorldLocationM(int32 RowIndex, int32 ColumnIndex) const;

	// 랙이 차지하는 전체 범위의 월드 좌표 [m]. 배관과 트레이까지 포함한다.
	UFUNCTION(BlueprintCallable, Category = "Room")
	FBox GetRoomBoundsM() const;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	// 행 하나의 기하. 한 번 계산해 배관·배선·랙 배치가 전부 같은 값을 쓰게 한다.
	struct FRowGeometry
	{
		// 랙 중심이 놓이는 X [m].
		double RackCenterXM = 0.0;

		// 랙이 바라보는 방향. +1이면 앞면이 +X, -1이면 -X.
		double FacingSign = 1.0;

		// 공급관 / 회수관 중심 X [m]. 둘 다 랙 뒤쪽이다.
		double SupplyXM = 0.0;
		double ReturnXM = 0.0;

		// 케이블 트레이 중심 X [m].
		double TrayXM = 0.0;
	};

	FHydraulicNetwork Network;
	FHydraulicSolveResult LastResult;

	// 솔버가 모르는 기하. 노드 인덱스와 1:1로 대응한다.
	TArray<FVector> NodePositionM;

	// 행마다의 기하. 길이 NumRows.
	TArray<FRowGeometry> RowGeometries;

	// 행별 펌프 인덱스. 길이 NumRows.
	TArray<int32> RowPumpIndices;

	// 랙 자리별 공급 분기 파이프 인덱스. 평탄 배열 [Row * RacksPerRow + Column].
	TArray<int32> RackBranchPipeIndices;

	// 파이프마다 공급 쪽인지. 길이는 Network.GetNumPipes().
	// 표시용이며 계산에 쓰이지 않는다 - 솔버는 어느 쪽이 공급인지 모른다.
	TArray<bool> bPipeIsSupplySide;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PipeMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> DecorMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ARackThermalActor>> SpawnedRacks;

	bool bNeedsRebuild = true;

	FRowGeometry ComputeRowGeometry(int32 RowIndex) const;

	// 그 자리의 랙에 지금 꽂혀 있는 유닛 수. Play 중이면 랙 액터에서 읽는다.
	int32 GetDockedSledsAt(int32 RowIndex, int32 ColumnIndex) const;

	// 행 방향 위치 [m]. 열 0이 원점 쪽이다.
	double ColumnYM(int32 ColumnIndex) const;

	void BuildNetwork();
	void BuildPipeMeshes();
	void BuildDecorMeshes();
	void SpawnRacks();
	void DestroySpawnedRacks();
	void DestroyMeshes(TArray<TObjectPtr<UStaticMeshComponent>>& Meshes);

	void UpdatePipeColors() const;
	void DrawSummary() const;

	FVector NodeWorldOffsetCm(int32 NodeIndex) const;
};
