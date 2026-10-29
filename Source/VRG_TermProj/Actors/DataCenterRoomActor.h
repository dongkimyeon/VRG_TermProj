// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/InRackNetwork.h"
#include "Simulation/Hydraulics/CoolantInventory.h"
#include "Visualization/GjConnectionMesh.h"
#include "Visualization/PatchManager.h"
#include "DataCenterRoomActor.generated.h"

class ARackThermalActor;

// Fresh game-thread placement query; never rebuilds meshes or the hydraulic graph.
struct FRackPlacementRow
{
	FVector RearDir, AlongDir;
	TArray<ARackThermalActor*> Racks;
};
// The polyline each drawn pipe follows, world metres (rule 1: converted from the mesh path's cm here).
// Same points the meshes are built from, so the web 3D view draws the pipes the solver solves (2026-10-14).
struct FRoomPipeRoute
{
	int32 Edge = INDEX_NONE;   // network pipe index, or -1 - pump index for the CDU supply -> pump link (no pipe)
	bool bSupply = false;
	bool bBranch = false;      // rack branch (riser) vs header
	TArray<FVector> PointsM;
};
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

	// 2026-10-22: the layout controller holds the settle rebuild while an equipment drag / grab is live, and the room
	// rebuilds once right after the release. Before, the 0.4 s settle rebuild (~50 ms of cable and pipe meshes) fired
	// in the middle of the next drag when equipment was moved in quick succession, which read as drag lag.
	void SetLayoutRebuildHeld(bool bHeld) { bLayoutRebuildHeld = bHeld; }

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
	TArray<FRackPlacementRow> GetRackRowsForPlacement(const ARackThermalActor* Excluded,
		const TArray<ARackThermalActor*>* ExcludedGroup = nullptr) const;
	bool AreTransformsInSameRackRow(const FTransform& A, const FTransform& B) const;

	// 통합 액터가 빌려 가는 수력망. 수명은 이 액터가 쥔다.
	FHydraulicNetwork& GetMutableNetwork() { return Network; }
	// Native comparison switch; radiator/virtual rows retain the legacy network regardless.
	bool bUseInRackNetworks = true;
	const FInRackHandles* GetInRackHandles(const ARackThermalActor* Rack) const;
	bool SyncInRackDocking();
	bool IsInternalLeak(int32 Leak) const { return InternalLeakIndices.Contains(Leak); }
	const FHydraulicNetwork& GetNetwork() const { return Network; }
	const FHydraulicSolveResult& GetLastSolveResult() const { return LastResult; }

	// GetRoutedRacks()와 같은 순서로 늘어놓은 랙별 공급 분기 파이프 인덱스.
	// 수력이 랙에 유량을 넘길 때 이 인덱스로 찾는다.
	const TArray<int32>& GetRackBranchPipeIndices() const { return RackBranchPipeIndices; }
	// Game-thread diagnostic: only rack branches have a thermal sample. Headers/pumps are unknown.
	bool TryGetPipeTemperatureK(int32 PipeIndex, double& OutTempK) const;

	// Row index of every rack, in GetRoutedRacks() order, and the CDU serving a row (null if none).
	TArray<int32> GetRoutedRackRows() const;
	ACoolingDistributionUnitActor* GetRowCdu(int32 RowIndex) const { return Rows.IsValidIndex(RowIndex) ? Rows[RowIndex].Cdu.Get() : nullptr; }
	// Web dashboard cooling tab: rows and each row's pump in the hydraulic network (INDEX_NONE if none).
	int32 GetNumRows() const { return Rows.Num(); }
	int32 GetRowPumpIndex(int32 RowIndex) const { return Rows.IsValidIndex(RowIndex) ? Rows[RowIndex].PumpIndex : INDEX_NONE; }

	// 배관을 다시 이었는지. 통합 액터가 이번 스텝에 유량이 바뀌었는지 알아야 한다.
	bool ConsumeRebuiltFlag();

	// S2b height term: fluid density of the hydraulic network [kg/m^3]. The integrated actor passes its
	// CoolantDensityKgM3 (single source); a changed value re-solves the network now.
	void SetCoolantDensityKgM3(double InDensityKgM3);

	// ---- S2b step 2: leaks and coolant inventory ----
	// A leak on a rack's supply or return branch (the pipe is split at mid length, the orifice sits there).
	// Leaks are part of the network topology: adding / clearing one rebuilds the room on its next tick (enter the
	// simulation after it). A leak's hole size can change at any time without a rebuild (SetLeakHoleDiameterM).
	int32 AddLeak(ARackThermalActor* Rack, bool bReturnBranch, double HoleDiameterM, double DischargeCoefficient = 0.61);
	void ClearLeaks();
	int32 GetNumLeakSpecs() const { return LeakSpecs.Num(); }
	// While a simulation run holds the coupling's branch pipe indices, leak specs cannot be added or cleared: the
	// rebuild would renumber the pipes of later rows (the coupling would read header flows) and refill the tanks
	// mid-run. The integrated actor locks at simulation entry and unlocks in layout mode (2026-10-20, found through
	// the web leak layer). Hole sizes still change freely (SetLeakHoleDiameterM).
	void SetLeakTopologyLocked(bool bLocked) { bLeakTopologyLocked = bLocked; }
	bool IsLeakTopologyLocked() const { return bLeakTopologyLocked; }

	// ---- S2b step 4: isolation valves (Tools/S2b_누수_계획.md) ----
	// Every rack branch has a supply valve at the supply header and a return valve at the return header. Closing a
	// rack's valves takes both pipes out of the network and re-solves (not a large resistance). The result keeps two
	// different scopes apart (D4): racks cut off from the CDU by the closed valves (isolation, graph topology) and
	// racks whose branch flow changed by more than ValveImpactThreshold (hydraulic impact, from the re-solve).
	struct FValveReport
	{
		int32 RackIndex = INDEX_NONE;         // routed order
		bool bClosed = false;
		TArray<int32> IsolatedRacks;          // routed indices whose rack node is cut off
		TArray<int32> ImpactedRacks;          // routed indices with |dFlow|/Flow_before > threshold (not isolated)
		TArray<double> FlowBeforeM3s, FlowAfterM3s;   // every routed rack's branch flow, before / after
		bool bConverged = false;
	};
	// Returns false if the rack has no branch (no units, not routed) or nothing changed.
	bool SetRackValveClosed(const ARackThermalActor* Rack, bool bClosed);
	bool IsRackValveClosed(const ARackThermalActor* Rack) const;
	const FValveReport& GetLastValveReport() const { return LastValveReport; }
	int32 GetRackSupplyValvePipe(int32 RoutedIndex) const { return RackSupplyValvePipe.IsValidIndex(RoutedIndex) ? RackSupplyValvePipe[RoutedIndex] : INDEX_NONE; }
	int32 GetRackReturnValvePipe(int32 RoutedIndex) const { return RackReturnValvePipe.IsValidIndex(RoutedIndex) ? RackReturnValvePipe[RoutedIndex] : INDEX_NONE; }
	// Opens every valve (a new simulation run starts with all valves open; valve events re-apply them).
	void OpenAllValves();

	// S2b step 5: design branch flow per routed rack [m^3/s] = the solution with every leak closed and every valve
	// open (computed at each rebuild). Fault judgement compares the live flow against it.
	const TArray<double>& GetRackDesignFlowM3s() const { return RackDesignFlowM3s; }
	// Leak spec for a rack branch, added with a closed hole (0 m) if it does not exist yet (needs a rebuild).
	int32 EnsureLeakSpec(ARackThermalActor* Rack, bool bReturnBranch);
	// True when the coolant inventory of the rack's row is depleted.
	bool IsRackLoopDepleted(int32 RoutedIndex) const;

	// Relative branch flow change above which a rack is listed in the hydraulic impact scope. Display threshold
	// only: the report keeps every rack's before / after flow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Leak", meta = (ClampMin = "0.0"))
	double ValveImpactThreshold = 1.0e-3;
	// S2b step 3: network leak indices on a rack's supply / return branch (for the coupling's leak heat).
	void GetRackLeakIndices(const ARackThermalActor* Rack, TArray<int32>& OutSupply, TArray<int32>& OutReturn) const;
	bool SetLeakHoleDiameterM(int32 LeakSpec, double HoleDiameterM);
	// Network leak index of a leak spec (INDEX_NONE until the room is rebuilt with it).
	int32 GetLeakNetworkIndex(int32 LeakSpec) const { return LeakSpecs.IsValidIndex(LeakSpec) ? LeakSpecs[LeakSpec].NetworkLeak : INDEX_NONE; }
	// S2b step 6 (web leak layer, read only): the spec's rack and branch, and a network node's world position [m]
	// (the leak orifice sits on a node at mid length of its split branch; drawn pipe routes end at the same points).
	const ARackThermalActor* GetLeakSpecRack(int32 LeakSpec) const { return LeakSpecs.IsValidIndex(LeakSpec) ? LeakSpecs[LeakSpec].Rack.Get() : nullptr; }
	bool IsLeakSpecReturnBranch(int32 LeakSpec) const { return LeakSpecs.IsValidIndex(LeakSpec) && LeakSpecs[LeakSpec].bReturnBranch; }
	FVector GetNodePositionM(int32 NodeIndex) const { return NodePositionM.IsValidIndex(NodeIndex) ? NodePositionM[NodeIndex] : FVector::ZeroVector; }

	// ---- W1 live leak sites (Tools/누수_실시간_계획.md L1, 2026-10-23) ----
	// Every rebuild gives every routed rack three closed leak sites: mid supply branch, mid return branch (split like a
	// hand-placed leak) and its supply header tee (an existing node). A closed hole adds exactly nothing to the solve
	// and keeps the coolant inventory idle, so normal runs are unchanged; during a run a leak is a hole size change on
	// one of them (SetLeakHoleDiameterM), never a topology change (the coupling holds the pipe numbers).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room|Leak")
	bool bLiveLeakSites = true;
	bool IsLeakSpecHeader(int32 LeakSpec) const { return LeakSpecs.IsValidIndex(LeakSpec) && LeakSpecs[LeakSpec].bHeader; }
	bool IsLeakSpecLatent(int32 LeakSpec) const { return LeakSpecs.IsValidIndex(LeakSpec) && LeakSpecs[LeakSpec].bLatent; }
	// Shown (web layer, tank leak counts, inventory log): a hand-placed / event leak, or a live site opened in this run.
	bool IsLeakSpecShown(int32 LeakSpec) const;
	// The orifice node's position [m] and current hole [m] (INDEX_NONE / 0 before the spec is built).
	FVector GetLeakSpecPositionM(int32 LeakSpec) const;
	double GetLeakSpecHoleDiameterM(int32 LeakSpec) const;
	// Inner diameter of the pipe the site is on [m]: a full rupture opens this hole.
	double GetLeakSpecRuptureDiameterM(int32 LeakSpec) const { return IsLeakSpecHeader(LeakSpec) ? HeaderDiameterM : BranchDiameterM; }
	// Network leak indices counted for a tank: built orifices of shown specs only.
	int32 CountShownTankLeaks(const TArray<int32>& TankLeakIndices) const;
	const FCoolantInventory& GetCoolantInventory() const { return Inventory; }
	// One integrated step [s]: re-solves the network only while a leak is open / make-up is filling / a loop just
	// depleted (otherwise nothing changes and the static solution stays). Called by the integrated actor before the
	// coupling reads the flows of this step (rule 4).
	void StepCoolantInventory(double DtS, double NowS);
	// Tanks back to their fill state, pumps and leak sizes as built (entering the simulation starts a new run).
	void RestartCoolantInventory();
	// Fallback tank of rows without a CDU actor.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	FExpansionTankParams FallbackExpansionTank;

	// Drawn pipe paths (see FRoomPipeRoute), rebuilt with the pipe meshes.
	const TArray<FRoomPipeRoute>& GetPipeRoutesM() const { return PipeRoutesM; }

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
	TMap<TWeakObjectPtr<const ARackThermalActor>, FInRackHandles> InRackHandles;
	TSet<int32> InternalLeakIndices;
	FHydraulicSolveResult LastResult;

	// 솔버가 모르는 기하. 노드 인덱스와 1:1로 대응한다.
	TArray<FVector> NodePositionM;

	TArray<FRackRow> Rows;

	// 랙 자리별 공급 분기 파이프 인덱스. 줄 순서대로 펼친 평탄 배열.
	TArray<int32> RackBranchPipeIndices;
	TArray<int32> RackReturnPipeIndices;
	TArray<int32> RackSupplyValvePipe, RackReturnValvePipe;   // S2b step 4, routed order
	TArray<double> RackDesignFlowM3s;                          // S2b step 5, routed order
	TArray<TWeakObjectPtr<const ARackThermalActor>> ClosedValveRacks;
	FValveReport LastValveReport;

	// 파이프마다 공급 쪽인지. 표시용이며 계산에 쓰이지 않는다.
	TArray<bool> bPipeIsSupplySide;

	struct FRoomLeakSpec
	{
		TWeakObjectPtr<ARackThermalActor> Rack;
		bool bReturnBranch = false;
		double HoleDiameterM = 0.0;
		double DischargeCoefficient = 0.61;
		int32 NetworkLeak = INDEX_NONE;
		bool bHeader = false;       // W1: the orifice sits on the rack's supply header tee (no split), not mid branch
		bool bLatent = false;       // W1: a live leak site made by the room (closed until opened during a run)
		bool bEverOpened = false;   // W1: opened at least once in the current run (shown from then on)
	};
	TArray<FRoomLeakSpec> LeakSpecs;
	bool bLeakTopologyLocked = false;
	// Branch pipes created or renumbered by leak splits (drawn as branches).
	TSet<int32> LeakBranchPipes;
	FCoolantInventory Inventory;
	int32 LastInventoryLogSecond = -1;
	// D10: wall time of the inventory re-solves since the last [Leak] line (game thread only).
	double InventorySolveMsSum = 0.0;
	int32 InventorySolveCount = 0;

	TArray<FRoomPipeRoute> PipeRoutesM;

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
	bool bLayoutRebuildHeld = false, bLayoutRebuildHeldLastTick = false;
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
	// S2b height (2026-10-20): every node's elevation is the mean height of the drawn pipe ends that meet there
	// (GJ ports, CDU ports and the pump supply route included), so the solver's z is the mesh's z.
	// A node no drawn end touches keeps its anchor height. Called before the solve.
	void AssignNodeElevationsFromRoutes();
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
