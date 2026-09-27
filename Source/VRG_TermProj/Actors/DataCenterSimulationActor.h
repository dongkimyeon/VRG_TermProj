// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "DataCenterSimulationActor.generated.h"

class AAirFieldDebugActor;
class ADataCenterRoomActor;
class ARackThermalActor;
class ASpineCabinetActor;
class UStaticMesh;
class UStaticMeshComponent;

// Where the rack coolant loops reject their heat (2026-09-27).
UENUM(BlueprintType)
enum class ECoolantRejection : uint8
{
	// Legacy: one radiator per rack straight to ambient (FRadiatorParams on the rack). No shared capacity.
	PerRackRadiator,
	// Each row's CDU: returns mix, one liquid-to-liquid exchanger against facility water, one supply temperature.
	// Rows without a CDU keep their racks' radiators.
	CduHeatExchanger,
};

// Passive air barriers (2026-09-27). Only the air grid sees them (solid cells).
UENUM(BlueprintType)
enum class EAisleContainment : uint8
{
	None,
	// Blanking: the gaps between cabinets and the slivers between neighbours in every row are closed.
	Blanking,
	// Blanking plus a roof over every cold aisle at rack-top height. Aisle ends stay open (partial containment).
	BlankingAndColdAisleRoof,
};

// How the room air is cooled (2026-09-27).
UENUM(BlueprintType)
enum class ECrahLayout : uint8
{
	// Legacy lumped coil: every fluid cell above CoilBottomZM loses UA * (T - supply). No airflow, no capacity limit.
	CeilingCoil,
	// In-row coolers at both ends of every rack row: hot-aisle air in at the rear, cold air out at the front.
	InRow,
	// Perimeter CRAH at both ends of every cold aisle: return air in at the top, supply out of the lower front.
	// There is no raised floor in the model, so the supply goes straight into the aisle instead of a plenum.
	Perimeter,
};

// S4: 세 도메인을 하나의 시계로 묶는 통합 액터 (MVP).
//
// 레벨에서 찾는 것:
//   ADataCenterRoomActor   -> 수력망 (랙 분기 유량)
//   AAirFieldDebugActor    -> 공기 격자 (랙 흡기 온도 / 배기 열원)
//   ARackThermalActor 여럿 -> 랙 열모델
//
// --- 규약 4: 시계는 하나다 ---
// 이 액터가 SimClock을 쥐고, 랙과 공기 액터는 bDrivenExternally로 자기 적분을 멈춘다.
// 액터마다 따로 돌리면 같은 시간 구간을 두 번 적분하거나 서로 어긋난 시각을 본다.
// 수력은 유량이 바뀔 때만 다시 풀지만, 그 사이에도 열과 공기는 같은 dt로 돈다.
//
// --- 규약 6: 주고받는 값은 한 번만 계산한다 ---
// 실제 교환은 FCouplingBridge가 한다. 이 액터는 기하(어느 셀이 랙 흡기인지,
// 어느 파이프가 이 랙의 분기인지)를 풀어서 다리에 넘기는 일만 한다.
//
// --- 랙은 공기 격자에서 고체다 ---
// 랙이 차지하는 셀을 고체로 표시해 기류가 랙을 통과하지 못하게 한다.
// 앞뒤는 뚫려 있어야 하므로 랙 몸통 안쪽만 막고 앞뒤 한 겹은 남긴다.
UCLASS()
class VRG_TERMPROJ_API ADataCenterSimulationActor : public AActor
{
	GENERATED_BODY()

public:
	ADataCenterSimulationActor();

	virtual void Tick(float DeltaTime) override;

	// 배치가 바뀐 뒤 누른다. 연결을 다시 풀고 시계를 0으로 되돌린다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void RebuildCoupling();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Simulation")
	void PauseSim();

	UFUNCTION(BlueprintCallable, Category = "Simulation")
	FCouplingSnapshot GetSnapshot() const;

	// 화면 확인용으로 N스텝을 앞당긴다. 공기 액터의 같은 기능은 이 액터가 구동할 때
	// 스스로 취소하므로(규약 4: 같은 구간을 두 번 적분하면 안 된다), 통합 구성에서는
	// 이쪽을 써야 한다. 한 프레임에 다 돌리지 않고 시간 예산만큼만 소화한다.
	void QueueWarmupSteps(int32 NumSteps);

	// 고정 시뮬레이션 스텝 [s]. 모든 도메인이 이 값을 쓴다 (규약 4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	double FixedStepS = 0.02;

	// 한 프레임에 돌릴 스텝 수 상한. 공기 압력 풀이가 무거워서 필요하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxStepsPerFrame = 4;

	// 물의 밀도 [kg/m^3]. 수력의 체적유량을 질량유량으로 바꿀 때만 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	double CoolantDensityKgM3 = 997.0;

	// ---- 공기 결합 (m) ----
	// 랙 앞면에서 흡기 셀을 잡을 거리. 이 안쪽 셀들의 평균 온도가 랙 흡기 온도가 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	double InletSampleDepthM = 0.4;

	// 랙 뒷면에서 배기 셀을 잡을 거리. 랙이 공기로 내보낸 열이 여기 들어간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	double ExhaustDepthM = 0.5;

	// 랙을 공기 격자에서 고체로 막을지. 끄면 기류가 랙을 통과한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	bool bMarkRacksAsSolid = true;

	// Rack fans drive air through the rack body in the air grid (intake faces in front, exhaust faces behind,
	// velocity from each slot's fan operating point). Needs bMarkRacksAsSolid. Off = racks only exchange heat
	// with the room air and the room air moves by buoyancy alone (the pre-2026-09-25 behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Coupling")
	bool bFanThroughFlow = true;

	// ---- CRAH 냉각 코일 ----
	// 코일을 둘 높이 범위 [m]. 천장 쪽 한 층을 코일로 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilBottomZM = 2.6;

	// 2-2: coil UA sized from the design point instead of a fixed number.
	//   UA = air-side IT load / CoilDesignApproachK
	// The air-side load is each rack's component heat that leaves through air (air-cooled parts, and the
	// air share of any part cooled both ways). A fixed 3000 W/K removed 28 kW against a ~133 kW air load
	// in L_Test, so the room heated without bound.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	bool bAutoSizeCoil = true;

	// Design difference between the air at the coil and the coil supply temperature [K].
	// 10 K with an 18 C supply means ceiling air settles near 28 C at design load.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling", meta = (ClampMin = "1.0"))
	double CoilDesignApproachK = 10.0;

	// Coil conductance [W/K] when bAutoSizeCoil is off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilTotalUAWK = 3000.0;

	// UA actually used after the last rebuild (auto-sized or CoilTotalUAWK), and the air load it was sized for.
	UFUNCTION(BlueprintCallable, Category = "Cooling")
	double GetEffectiveCoilUAWK() const { return EffectiveCoilUAWK; }
	UFUNCTION(BlueprintCallable, Category = "Cooling")
	double GetDesignAirLoadW() const { return DesignAirLoadW; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling")
	double CoilSupplyTempK = 291.15;

	// ---- CRAH units (fan-driven ducts with a coil, see FCrahCouplingUnit) ----
	// Units are placed from the rack rows at every rebuild; nothing is saved in the level. If none fits inside
	// the air grid the lumped ceiling coil is used instead and the summary says so.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH")
	ECrahLayout CrahLayout = ECrahLayout::InRow;

	// Design air temperature rise through the racks = drop through the CRAH coils [K]. Sets the total CRAH
	// airflow: design air load / (rho * cp * this). A design assumption, not a measured value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH", meta = (ClampMin = "2.0"))
	double CrahDesignAirDeltaTK = 12.0;

	// Design supply air minus chilled water temperature [K]. Sets the coil UA through epsilon-NTU at design flow:
	// eps = dT / (dT + approach), UA = -ln(1 - eps) * mdot * cp. A design assumption, not a measured value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH", meta = (ClampMin = "0.5"))
	double CrahDesignApproachK = 5.0;

	// Unit box [m]: X depth (along its airflow), Y width, Z height.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH")
	FVector CrahInRowSizeM = FVector(1.068, 0.6, 2.236);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH")
	FVector CrahPerimeterSizeM = FVector(0.9, 2.0, 2.0);

	// CRAH fan electric input at the operating point (D8): P = FanPressurePa * flow / FanEfficiency.
	// 250 Pa (coil + filter + casing) and 0.6 (EC fan and motor, wire to air) are ESTIMATES for a chilled-water
	// cooler, not a product rating. The power becomes heat in the supply air and a ledger input (not IT heat).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH", meta = (ClampMin = "0.0"))
	double CrahFanPressurePa = 250.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	double CrahFanEfficiency = 0.6;

	// Gap between a perimeter unit and the ends of the rows it serves [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|CRAH")
	double CrahPerimeterClearanceM = 0.4;

	UFUNCTION(BlueprintCallable, Category = "Cooling|CRAH")
	int32 GetNumCrahUnits() const { return CrahPlacements.Num(); }

	// Hand placement: ACrahUnitActor units in the level are used instead of the automatic layout (any layout but
	// CeilingCoil). This button spawns such actors where the automatic layout (CrahLayout) would put them, as a
	// starting point to drag around. Run it in the editor, then save the level.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Cooling|CRAH")
	void SpawnCrahActorsFromLayout();

	// True when the last rebuild used hand-placed ACrahUnitActor units.
	bool IsUsingPlacedCrahs() const { return bUsingPlacedCrahs; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Coolant")
	ECoolantRejection CoolantRejection = ECoolantRejection::CduHeatExchanger;

	// Blanking / cold-aisle roof. Default off: the level's own layout (gaps and all) is the reference.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Containment")
	EAisleContainment Containment = EAisleContainment::None;

	// Air-grid cells turned solid by Containment at the last rebuild.
	UFUNCTION(BlueprintCallable, Category = "Cooling|Containment")
	int32 GetNumContainmentCells() const { return PassiveSolidCells; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FSimClock Clock;
	FCouplingBridge Bridge;

	UPROPERTY(Transient)
	TObjectPtr<ADataCenterRoomActor> Room;

	UPROPERTY(Transient)
	TObjectPtr<AAirFieldDebugActor> AirField;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ARackThermalActor>> Racks;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ASpineCabinetActor>> Spines;

	bool bSimRunning = false;
	double EffectiveCoilUAWK = 0.0;
	double DesignAirLoadW = 0.0;
	bool bNeedsRebuild = true;

	// 남은 예열 스텝. 프레임마다 예산만큼만 소화한다.
	int32 PendingWarmupSteps = 0;
	int32 WarmupStepsReported = 0;
	// Time-averaged cabinet inlet temperature over the last quarter of the warmup [K sum, samples]. The room flow
	// keeps fluctuating (plumes, fan jets), so one instant is not a fair basis for comparing layouts.
	TArray<double> WarmupInletSumK, WarmupInletMaxK;
	int32 WarmupInletSamples = 0;
	double WarmupCouplingMs = 0.0, WarmupRackMs = 0.0;  // wall time over the warmup (measurement)
	void DrainWarmupSteps();

	// 연결이 성립했는지. 안 되면 화면에 이유를 띄운다.
	FString LastSetupMessage;

	void FindTargetActors();

	// 랙 기하를 공기 격자 인덱스로 풀어 다리에 넘길 연결 정보를 만든다.
	void BuildCoupling();

	// 랙이 차지하는 셀을 고체로 표시한다.
	void MarkRackSolidCells();

	// Which cabinet (racks first, then spines) owns each solid cell; INDEX_NONE for fluid.
	TArray<int32> SolidOwner;

	// Intake / exhaust faces of one cabinet and the layer -> slot weights (see FRackCouplingLink).
	void BuildFanFaces(int32 CabinetIndex, const AActor& Cabinet, const FUniformAirGrid& Grid, const FVector& GridOriginM,
		const ARackThermalActor* Rack, const ASpineCabinetActor* Spine, int32 NumSlots, FRackCouplingLink& Link) const;

	// 천장 쪽 한 층을 CRAH 코일로 잡는다.
	void BuildCoolingCoil();

	// ---- CDU heat exchangers ----
	struct FCduSummary
	{
		FString Name;
		int32 NumRacks = 0;
		double DesignDutyW = 0.0, UAWK = 0.0, FacilityMassFlowKgs = 0.0, FacilitySupplyTempK = 0.0;
	};
	TArray<FCduSummary> CduSummaries;
	// Groups the racks of every row that has a CDU and sizes the exchanger (after Bridge.Initialize).
	void BuildCduGroups();

	// ---- Rows and cold aisles (from the cabinets, every rebuild) ----
	struct FCabinetRow
	{
		FVector Forward, Along;
		double Across = 0.0, Depth = 0.0, Height = 0.0, BaseZ = 0.0;
		double MinAlong = TNumericLimits<double>::Max(), MaxAlong = TNumericLimits<double>::Lowest();
		TArray<FVector2D> Spans;  // (start, end) along the row of every cabinet, sorted after grouping
	};
	struct FColdAisle
	{
		int32 RowA = 0, RowB = 0;    // RowA's forward points into the aisle
		FVector Forward, Along, Centre;
		double MinAlong = 0.0, MaxAlong = 0.0, BaseZ = 0.0, TopZ = 0.0;
	};
	TArray<FCabinetRow> CabinetRows;
	TArray<FColdAisle> ColdAisles;
	int32 NumUnpairedRows = 0;
	void GroupCabinetRows();
	static constexpr int32 PassiveSolidOwner = -2;
	int32 PassiveSolidCells = 0;

	// ---- CRAH units ----
	struct FCrahPlacement
	{
		FVector BaseM;      // bottom centre, world [m]
		FVector ForwardM;   // supply direction (grid axis)
		FVector SizeM;      // depth, width, height
		bool bTopIntake;    // perimeter: intake through the top face; in-row: through the rear face
		double FlowM3s = 0.0;  // set airflow (placed actor); 0 = share of the design airflow
		bool bPlaced = false;  // from an ACrahUnitActor (it draws itself)
	};
	bool bUsingPlacedCrahs = false;
	// The automatic layout only (ignores placed actors).
	void PlanAutoCrahUnits();
	TArray<FCrahPlacement> CrahPlacements;
	TArray<FCrahCouplingUnit> CrahUnits;
	double CrahUnitFlowM3s = 0.0, CrahUnitUAWK = 0.0, CrahUnitFanW = 0.0;
	int32 CrahUnitsDropped = 0;
	bool bCrahFellBackToCoil = false;
	// Places units from the rack rows (CrahLayout). Units that leave the air grid are dropped and counted.
	void PlanCrahUnits();
	// Faces, airflow and coil of every placed unit (after the solid cells are marked and the air load known).
	void BuildCrahUnits();

	UPROPERTY()
	TObjectPtr<UStaticMesh> CrahMesh;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> CrahMeshes;
	void UpdateCrahMeshes();
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> ContainmentMeshes;
	// Blanking panels in row gaps wider than 5 cm and the cold-aisle roofs (a thin plate; the grid blocks one cell).
	void UpdateContainmentMeshes();

	void DrawSummary() const;
};
