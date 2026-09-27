// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Visualization/GjConnectionMesh.h"
#include "Visualization/PatchManager.h"
#include "DataCenterRoomActor.generated.h"

class ARackThermalActor;
class ACoolingDistributionUnitActor;
class UStaticMeshComponent;

// 레벨에 놓인 랙과 CDU를 찾아 배관을 자동으로 잇는다.
//
// --- 자리를 만들지 않고 읽는다 ---
// 전에는 이 액터가 행·열 수를 받아 랙 자리를 만들어 냈다. 그러면 랙을 원하는 데
// 옮길 수 없고, 에디터에서는 진짜 랙 대신 상자만 보였다.
// 지금은 반대다. `ARackThermalActor`와 `ACoolingDistributionUnitActor`를 직접 놓으면
// 이 액터가 그 위치를 읽어 배관을 잇는다. 랙을 옮기면 배관이 따라온다.
// 처음 깔 때가 번거로우면 SpawnRackGrid() 버튼으로 찍어 낸 뒤 옮기면 된다.
//
// --- 배관은 장식이 아니다 ---
// 눈에 보이는 관은 수력 솔버가 실제로 푸는 파이프를 그대로 그린 것이다.
// 위상은 InRowLoopBuilder가 만들고 이 액터는 좌표만 붙인다 (솔버는 위상만 안다).
// 관 색상은 공급(파랑)/회수(빨강) 역할이고 밝기가 계산된 유량 크기다.
//
// --- 줄 묶기 ---
// 같은 방향을 보면서 뒷면이 한 줄로 늘어선 랙들을 한 줄로 묶는다.
// 줄마다 가장 가까운 CDU를 붙이고 그 줄의 수력 루프를 따로 푼다.
//   CDU -> 펌프 -> 공급 헤더(줄을 따라) -> 랙 분기(뒤로 올라옴) -> 회수 헤더 -> CDU
UCLASS()
class VRG_TERMPROJ_API ADataCenterRoomActor : public AActor
{
	GENERATED_BODY()

public:
	ADataCenterRoomActor();

	virtual void Tick(float DeltaTime) override;

	// 레벨 뷰포트에서도 틱이 돌게 해서 Play 없이 배치를 확인할 수 있게 한다.
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	// 랙이나 CDU를 옮긴 뒤 누른다. 배관을 다시 잇고 다시 푼다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Room")
	void RebuildRoom();

	// Rows and the hydraulic network now (the meshes follow over frames). Entering simulation mode needs the rows
	// before the coupling is built; RebuildRoom() only flags it for the next tick.
	// Only when something changed since the last rebuild, so entering simulation does not re-create every pipe mesh.
	// S6: electrical input of every row pump at the last hydraulic solution [W] (operating point, D8).
	double GetTotalPumpElectricalW() const;

	void RebuildRoomNow() { if (bNeedsRebuild || Rows.Num() == 0 || GjLayoutHash() != LastGjLayoutHash) RebuildTimed(); }

	// Immediate editor/script refresh; the numerical graph still comes from InRowLoopBuilder.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Visualization|GJ")
	void RefreshGjConnections();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|GJ")
	bool bUseGjConnections = false;

	// ---- Patch manager (GJ only; display plan, not a solver input) ----
	// Puts one ASpineCabinetActor at the end of every row that has none. Rows share the end with the
	// larger world coordinate so the overhead trays close into one loop.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Visualization|GJ")
	void SpawnSpineCabinets();

	// Rear face of the racks to the tray centreline [m]. The tray hangs over the hot aisle at CableTrayHeightM.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|GJ", meta = (ClampMin = "0.1"))
	double TrayStandoffM = 0.27;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|GJ", meta = (ClampMin = "0", ClampMax = "16"))
	int32 SpineLinksPerRowPair = 4;

	const Patch::FPlan& GetPatchPlan() const { return PatchPlan; }
	const TArray<Patch::FDevice>& GetPatchDevices() const { return PatchDevices; }

	// For scripts: the one-line patch status and whether anything is unpatched / double-booked.
	UFUNCTION(BlueprintCallable, Category = "Visualization|GJ")
	FString GetPatchSummary() const { return PatchPlan.Summary(); }

	UFUNCTION(BlueprintCallable, Category = "Visualization|GJ")
	bool IsPatchComplete() const { return PatchPlan.Unconnected.IsEmpty() && PatchPlan.RejectedClaims == 0 && PatchPlan.Cables.Num() > 0; }

	// 랙을 격자로 찍어 낸다. 찍은 뒤에는 개별 액터이므로 자유롭게 옮길 수 있다.
	// 처음 깔 때만 쓰는 보조 기능이다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Room|Setup")
	void SpawnRackGrid();

	// SpawnRackGrid가 찍어 낸 랙을 전부 지운다. 손으로 놓은 랙은 건드리지 않는다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Room|Setup")
	void DestroySpawnedRackGrid();

	// ---- SpawnRackGrid 설정 (m) ----
	// 배관을 잇는 데는 쓰이지 않는다. 찍어 낼 때만 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup", meta = (ClampMin = "1", ClampMax = "16"))
	int32 NumRows = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup", meta = (ClampMin = "1", ClampMax = "32"))
	int32 RacksPerRow = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup")
	FVector RackSizeM = FVector(1.2, 0.6, 2.0);

	// 랙 중심 사이 거리 (줄 방향). 랙 폭보다 커야 틈이 생긴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup")
	double RackPitchM = 0.66;

	// 마주 보는 두 줄의 앞면 사이 거리 (콜드 통로).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup")
	double AisleWidthM = 2.4;

	// 쌍과 쌍 사이에 두는 뒷면 서비스 통로.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup")
	double ServiceAisleWidthM = 1.6;

	// 랙 한 대의 슬롯 수. GB200 NVL72는 컴퓨트 트레이 18장이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Setup", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SledsPerRack = 18;

	// ---- 배관 배치 (m) ----
	// 랙 뒷면에서 배관 중심까지의 거리.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing")
	double PipeStandoffM = 0.18;

	// 공급관과 회수관 사이 거리 (뒤로 더 나간 쪽이 회수관).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing")
	double SupplyReturnSpacingM = 0.16;

	// 냉각수 헤더 높이. GB200 NVL72 참고 배치는 헤더가 바닥 레벨을 따라 돌고
	// 분기관이 랙 밑에서 위로 올라간다 (천장 배관이 아니다).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing")
	double HeaderHeightM = 0.25;

	// 랙 뒷면 매니폴드 높이. 분기관이 바닥 헤더에서 여기까지 올라왔다가
	// 다시 회수 헤더로 내려간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing")
	double RackManifoldZM = 1.1;

	// 같은 줄로 묶는 허용 오차 [m]. 뒷면이 이만큼 안에서 일치하면 한 줄로 본다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing", meta = (ClampMin = "0.05"))
	double RowGroupToleranceM = 0.5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing")
	double CableTrayHeightM = 2.85;

	// After a rack or CDU moves, wait until nothing has moved for this long [s, real time] before rebuilding the
	// pipes and cables (a rebuild takes seconds; doing it every drag frame froze the editor).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routing", meta = (ClampMin = "0.0"))
	double LayoutSettleS = 0.4;

	// Per-frame time for creating pipe / cable meshes after a rebuild [ms, real time].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|GJ", meta = (ClampMin = "1.0"))
	double MeshBuildBudgetMs = 8.0;

	// ---- 굵기 (m) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double HeaderDiameterM = 0.11;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double BranchDiameterM = 0.05;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double CableDiameterM = 0.022;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0", ClampMax = "12"))
	int32 CablesPerRack = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowCableTray = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

	// ---- 수력 파라미터 ----
	// GB200 NVL72 운전점에서 역산했다.
	//   랙 1대 105 kW 중 액랭 97 kW를 냉각수 ΔT 약 10 K로 빼려면 랙당 2.3 L/s가 필요하다.
	//   줄에 8대면 18.4 L/s이고, 그 유량에서 루프 전체 압력 강하가 약 195 kPa이 되도록 잡았다.
	// 감으로 넣은 값이 아니다. 다른 랙을 쓰면 여기부터 다시 역산해야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double HeaderSegmentResistancePaS2M6 = 6.0e6;

	// 슬롯이 전부 찼을 때의 랙 분기 저항. 유닛을 뽑으면 R*(N/n)^2로 올라간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double RackBranchResistancePaS2M6 = 1.7e10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReturnToCduResistancePaS2M6 = 1.5e7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	FHydraulicSolveSettings SolveSettings;

	// 레벨에 CDU가 하나도 없을 때 쓰는 값. CDU를 놓으면 그쪽 값이 이긴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double FallbackCduReferencePressurePa = 2.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double FallbackPumpShutoffHeadPa = 4.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double FallbackPumpCurveCoeffPaS2M6 = 6.0e8;

	// 디버그 키용. 레벨의 모든 CDU에 같은 회전수비를 준다.
	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	void SetPumpSpeedRatio(double InSpeedRatio);

	UFUNCTION(BlueprintCallable, Category = "Hydraulics")
	double GetPumpSpeedRatio() const { return LastPumpSpeedRatio; }

	UFUNCTION(BlueprintCallable, Category = "Room")
	int32 GetNumRoutedRacks() const;

	// 랙이 차지하는 전체 범위의 월드 좌표 [m]. 공기 격자를 맞출 때 쓴다.
	UFUNCTION(BlueprintCallable, Category = "Room")
	FBox GetRoomBoundsM() const;

	// 줄 묶기에 쓰인 랙 목록. 공기 열원을 랙마다 놓을 때 쓴다.
	UFUNCTION(BlueprintCallable, Category = "Room")
	TArray<ARackThermalActor*> GetRoutedRacks() const;

	// 통합 액터가 빌려 가는 수력망. 수명은 이 액터가 쥔다.
	FHydraulicNetwork& GetMutableNetwork() { return Network; }
	const FHydraulicSolveResult& GetLastSolveResult() const { return LastResult; }

	// GetRoutedRacks()와 같은 순서로 늘어놓은 랙별 공급 분기 파이프 인덱스.
	// 수력이 랙에 유량을 넘길 때 이 인덱스로 찾는다.
	const TArray<int32>& GetRackBranchPipeIndices() const { return RackBranchPipeIndices; }
	// Game-thread diagnostic: only rack branches have a thermal sample. Headers/pumps are unknown.
	bool TryGetPipeTemperatureK(int32 PipeIndex, double& OutTempK) const;

	// Row index of every rack, in GetRoutedRacks() order, and the CDU serving a row (null if none).
	TArray<int32> GetRoutedRackRows() const;
	ACoolingDistributionUnitActor* GetRowCdu(int32 RowIndex) const { return Rows.IsValidIndex(RowIndex) ? Rows[RowIndex].Cdu.Get() : nullptr; }

	// 배관을 다시 이었는지. 통합 액터가 이번 스텝에 유량이 바뀌었는지 알아야 한다.
	bool ConsumeRebuiltFlag();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	// 한 줄. 같은 방향을 보면서 뒷면이 한 줄로 늘어선 랙들이다.
	struct FRackRow
	{
		// 줄 방향으로 정렬돼 있다.
		TArray<TObjectPtr<ARackThermalActor>> Racks;

		// 앞면에서 뒷면으로 향하는 단위 벡터 (월드).
		FVector RearDir = FVector::ForwardVector;

		// 줄을 따라가는 단위 벡터 (월드).
		FVector AlongDir = FVector::RightVector;

		// 이 줄을 맡는 CDU. 없으면 nullptr.
		TObjectPtr<ACoolingDistributionUnitActor> Cdu;

		int32 PumpIndex = INDEX_NONE;
	};

	FHydraulicNetwork Network;
	FHydraulicSolveResult LastResult;

	// 솔버가 모르는 기하. 노드 인덱스와 1:1로 대응한다.
	TArray<FVector> NodePositionM;

	TArray<FRackRow> Rows;

	// 랙 자리별 공급 분기 파이프 인덱스. 줄 순서대로 펼친 평탄 배열.
	TArray<int32> RackBranchPipeIndices;
	TArray<int32> RackReturnPipeIndices;

	// 파이프마다 공급 쪽인지. 표시용이며 계산에 쓰이지 않는다.
	TArray<bool> bPipeIsSupplySide;

	// SpawnRackGrid가 찍어 낸 랙. 지울 때 손으로 놓은 랙과 구분한다.
	UPROPERTY()
	TArray<TObjectPtr<ARackThermalActor>> GridSpawnedRacks;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PipeMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> DecorMeshes;

	// One visual curve may have several mesh spans. Every hydraulic span retains its edge ID.
	TArray<int32> PipeMeshFlowIndices; // >=0: pipe; negative: -1-pump index
	TArray<bool> PipeMeshSupply;
	TMap<int32, FVector> PipeStartOverrideCm;
	TMap<int32, FVector> PipeEndOverrideCm;
	struct FGjPumpRoute { int32 Pump; FVector StartCm, EndCm, Outward; };
	TArray<FGjPumpRoute> GjPumpRoutes;
	uint32 LastGjLayoutHash = 0;
	// Debounce of layout changes (see Tick): the hash seen last frame and when it last changed [s, real time].
	uint32 PendingLayoutHash = 0;
	double LayoutChangedAtS = 0.0;
	void RebuildTimed();

	// Pipe routing around cabinets (display only; the solver's topology does not change). Boxes of every rack,
	// spine and placed CRAH unit, in their own frames (cm, origin at the bottom centre).
	struct FCabinetBox { FTransform ToWorld; FVector HalfCm; double HeightCm; };
	TArray<FCabinetBox> CabinetBoxes;
	void GatherCabinetBoxes();
	bool SegmentHitsCabinet(const FVector& ACm, const FVector& BCm) const;
	int32 CountCabinetHits(const TArray<FVector>& PointsCm) const;
	// From / To in world cm; a non-zero lead direction adds a 30 cm straight lead at that end. Returns the preferred
	// path if it is clear, else a floor-level L around the cabinets, else an overhead route above them.
	TArray<FVector> RouteAroundCabinets(const TArray<FVector>& Preferred, const FVector& From, const FVector& FromLead,
		const FVector& To, const FVector& ToLead) const;
	int32 LastPipeCabinetHits = 0, LastRoutedAround = 0, LastPreferredHits = 0;

	// GJ pipe and cable spans waiting to become components. Creating ~1300 spline meshes takes seconds, so Tick
	// creates them MeshBuildBudgetMs at a time (the editor keeps responding and the curves fill in); a synchronous
	// RefreshGjConnections() still finishes them at once.
	struct FPendingSegment
	{
		GjConnectionMesh::FSplineSegment Segment;
		int32 Asset = 0;           // index into PendingAssets (mesh, material)
		double Scale = 1.0;
		bool bPipe = false;
		int32 Edge = INDEX_NONE;   // pipe only: hydraulic edge (>= 0) or -1 - pump
		bool bSupply = false;
	};
	TArray<FPendingSegment> PendingSegments;
	int32 PendingCursor = 0;
	double PendingStartS = 0.0;
	int32 PendingFrames = 0;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> PendingAssets;  // body mesh, surface material pairs; kept alive while pending
	int32 AddPendingAsset(UStaticMesh* Body, UMaterialInterface* Surface);
	void QueuePath(const TArray<FVector>& LocalPointsCm, UStaticMesh* Body, UMaterialInterface* Surface, double BendCm,
		double Scale, bool bPipe, int32 Edge, bool bSupply);
	// BudgetMs <= 0: everything now.
	void DrainPendingSegments(double BudgetMs);
	double DecorBuildTimesMs[4] = {0, 0, 0, 0};  // destroy, patch plan, interference check, cable meshes
	uint32 GjLayoutHash() const;
	void BuildGjPipeMeshes();
	void BuildGjDecorMeshes();
	Patch::FPlan PatchPlan;
	TArray<Patch::FDevice> PatchDevices;

	bool bNeedsRebuild = true;

	// 마지막으로 물어본 뒤에 배관을 다시 이었는지. 통합 액터가 읽고 지운다.
	bool bRebuiltSinceLastQuery = false;

	double LastPumpSpeedRatio = 1.0;

	// 레벨에서 랙과 CDU를 찾아 줄로 묶는다.
	void DiscoverRows();

	void BuildNetwork();
	void BuildPipeMeshes();
	void BuildDecorMeshes();
	void DestroyMeshes(TArray<TObjectPtr<UStaticMeshComponent>>& Meshes);

	void UpdatePipeColors() const;
	mutable bool bPipeColorsDirty = true;
	mutable bool bLastPipeTemperatureMode = false;
	mutable double LastPipeTemperatureDisplayS = -1.0;
	void DrawSummary() const;

	// 랙 뒷면 바깥의 배관 기준점 [m].
	FVector RackRearAnchorM(const ARackThermalActor& Rack, const FVector& RearDir, double ExtraOffsetM) const;

	FVector NodeWorldOffsetCm(int32 NodeIndex) const;
};
