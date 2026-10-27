// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Evaluation/VrgEvaluation.h"
#include "Simulation/Sensors/InstrumentBank.h"
#include "Simulation/Evaluation/VrgFaultMonitor.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "Simulation/Water/LeakSpray.h"
#include "Simulation/Water/FloorWater.h"
#include "Simulation/Air/AirStreamlines.h"
#include "Simulation/Air/AirFlowAverager.h"
#include "Visualization/AirFlowView.h"
#include "Actors/PlantEquipmentActor.h"
#include "Simulation/Evaluation/VrgDashboardSummary.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Simulation/Air/RoomAirDomain.h"
#include "Interaction/VrgLayoutFile.h"
#include "DataCenterSimulationActor.generated.h"

class AAirFieldDebugActor;
class ADataCenterRoomActor;
class ARackThermalActor;
class ASpineCabinetActor;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;

// Layout vs simulation (2026-09-27). Layout: nothing is integrated, cabinets can be moved freely. Simulation: the
// coupling is built from the current layout in one go and the clock starts at 0.
UENUM(BlueprintType)
enum class ESimMode : uint8
{
	Layout,
	Simulation,
};

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

	// ---- Layout / simulation mode ----
	// Play starts in layout mode unless this is off (then it starts simulating at once, the pre-2026-09-27 way).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Simulation")
	bool bStartInLayoutMode = true;

	// Builds everything from the current layout (rows and hydraulics, air grid fitted and reset, racks reset,
	// coupling) and starts the clock at 0.
	UFUNCTION(BlueprintCallable, Category = "Simulation")
	void EnterSimulationMode();
	// Layout-only, validate before committing. Existing worlds default to automatic fit.
	bool TrySetRoomAirDomain(const FRoomAirDomainSettings& Settings, FString& Reason);
	bool ValidateRoomAirDomain(const FRoomAirDomainSettings& Settings, FAirGridParams& OutGrid,
		FBox& OutActualBoundsM, FString& Reason, const TArray<FRoomCeilingPlate>* CeilingOverride = nullptr);
	// Value-only layout API; no actor spawn, solid mutation, history or persistence until later work.
	bool TrySetRoomCeilingPlates(const TArray<FRoomCeilingPlate>& Plates, FString& Reason);
	const TArray<FRoomCeilingPlate>& GetRoomCeilingPlates() const { return RoomCeilingPlates; }
	int32 GetNumCeilingCells() const { return CeilingSolidCells; } // newly owned cells, overlaps counted once
	const FRoomAirDomainSettings& GetRoomAirDomain() const { return RoomAirDomain; }
	// Layout check of one room equipment box against the manual room: the same outer box, air duct cells and ceiling plate
	// tests as ValidateRoomAirDomain. True without a manual room (2026-10-23).
	bool CheckLayoutBoxInManualRoom(const FVector& SizeM, const FTransform& Transform, bool bCabinet, bool bCrah, bool bTopIntake, FString& Reason) const;
	const FString& GetRoomAirDomainMessage() const { return RoomAirDomainMessage; }

	// Stops integration and drops the simulated state (the next EnterSimulationMode starts fresh).
	UFUNCTION(BlueprintCallable, Category = "Simulation")
	void EnterLayoutMode();

	UFUNCTION(BlueprintCallable, Category = "Simulation")
	ESimMode GetSimMode() const { return Mode; }

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

	// Display only. Automatic units share the authored models used by placed CRAH actors.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bUseCrahReferenceModels = true;

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
 // Read current automatic geometry without touching solver, mesh, clock or active planning caches.
 bool CaptureAutomaticCrahLayout(TArray<FCrahLayoutRecord>& Out);
 void RefreshCrahLayoutDisplay();
 bool GetAutomaticCrahLayout(TArray<FCrahLayoutRecord>& Out) { return CaptureAutomaticCrahLayout(Out); }
 void RefreshCrahLayoutVisuals() { RefreshCrahLayoutDisplay(); }
 int32 GetNumActiveCrahUnits() const { return GetActiveCrahUnitCount(); }
 int32 GetActiveCrahUnitCount() const { return CrahUnits.Num(); }
 double GetCrahTotalFlowM3s() const { return CrahUnitFlowM3s*CrahUnits.Num(); }
 double GetCrahTotalUAWK() const { return CrahUnitUAWK*CrahUnits.Num(); }
 double GetCrahTotalFanW() const { return CrahUnitFanW*CrahUnits.Num(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Coolant")
	ECoolantRejection CoolantRejection = ECoolantRejection::CduHeatExchanger;

	// S8 F1/F2 (2026-10-20): facility water (FWS) plant for the CDU primaries. Off (default) = the fixed design
	// facility flow and supply temperature of every CDU as before. On = the CDU groups become branches of an FWS
	// network (pumps N+1, differential-pressure control, valves, dry cooler coils) sized at those same design flows;
	// each exchanger takes its primary flow from the network solution and (F2) its supply temperature from the plant's
	// supply volume, cooled by the dry coolers against the outdoor air (PlantThermalSpec; bFixedSupplyTemp keeps the
	// old fixed temperature as a comparison). Applied when the simulation is entered (vrg.Plant.Enable).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Plant")
	bool bFacilityPlant = false;

	// S8 F7: draw the plant yard (equipment and the plant networks' pipes) whenever a plant is built, in layout and
	// simulation mode, with or without vrg.Plant.Enable (vrg.Plant.Show).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Plant")
	bool bShowPlant = true;
	class AFacilityPlantDisplayActor* GetPlantDisplay() const { return PlantDisplay.Get(); }
	// Spawns / refreshes / removes the plant display (4 Hz wall time unless forced; never touches the clock).
	void UpdatePlantDisplay(bool bForce);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Plant")
	FFacilityLoopSpec PlantSpec;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Plant")
	FFacilityThermalSpec PlantThermalSpec;

	const FFacilityPlant& GetFacilityPlant() const { return Plant; }
	FFacilityPlant& GetMutableFacilityPlant() { return Plant; }
	// S8 F6b: one facility fault (console vrg.Plant.Fault, event "plant"), applied now at the current SimTime:
	//   leak <fws|chw|cw> <supply|return|branchsupply|branchreturn> <branch> <mm> | leakclear | pump <loop> <n> |
	//   chiller <count> | trim <count> | drycooler <fws|cw> <count>
	// The reach is the re-solved network's (D4); the room's fault judgement then follows the temperatures as before.
	// False (with a warning) when the plant thermal model is not running or the arguments do not apply.
	bool ApplyPlantFault(const TArray<FString>& Args);
	bool IsFacilityPlantActive() const { return bPlantActive; }

	// ---- W1 live leaks (Tools/누수_실시간_계획.md L1, 2026-10-23) ----
	// Every place a leak can start during a run: the room's closed sites (three per rack, ADataCenterRoomActor) and,
	// while the plant runs, the facility loops' header and branch nodes. A leak is a hole size change at a site; the
	// network re-solves, nothing is rebuilt (the coupling keeps its pipe numbers). Positions [m]: the room's node
	// positions, the plant yard's drawn node positions (schematic, F7).
	struct FLiveLeakSite
	{
		bool bPlant = false;
		int32 RoomSpec = INDEX_NONE;
		EPlantLoopId Loop = EPlantLoopId::Fws;
		EPlantLeakSite PlantSite = EPlantLeakSite::SupplyHeader;
		int32 Branch = INDEX_NONE;
		FVector PositionM = FVector::ZeroVector;
		FString Label;
		// W2: where the jet points (scenario input; default = fixed-seed random in the downward cone, ESTIMATE).
		FVector SprayDirectionM = FVector(0, 0, -1);
	};
	// ---- W2 spray (Tools/누수_실시간_계획.md L2, 2026-10-26) ----
	// The water that left the holes: jet, drops, impacts, films, floor (FLeakSpray). Fed each step with the leaked volume
	// the hydraulics booked (tank ledger / plant loop flow), so it never computes its own leak (rule 6).
	UPROPERTY(EditAnywhere, Category = "Leak")
	bool bLeakSpray = true;
	// Debug drawing of the computed parcels (vrg.Water.Show). Display only.
	UPROPERTY(EditAnywhere, Category = "Leak")
	bool bShowSpray = true;
	// ---- Air view (2026-10-27, the professor's streamtrace figure; Visualization/AirFlowView.h) ----
	// Display only: the solver is not touched. Streamlines follow the time-averaged flow (last 30 s, FAirFlowAverager),
	// rebuilt once a second while a stream / plan / arrow mode is on. V cycles the modes (ADataCenterDebugController).
	void SetAirView(EAirView View);
	EAirView CycleAirView();
	EAirView GetAirView() const { return AirView; }
	// Colour bars for the HUD and one note line (average window or "instant").
	void GetAirFlowLegends(TArray<FAirFlowLegend>& Out, FString& OutNote) const;
	// Traces and uploads the view now (normally once a second). Tests call it.
	void RebuildAirFlowView();
	const TArray<FAirStreamline>& GetStreamlines() const { return Streamlines; }
	const TArray<FAirStreamline>& GetPlanStreamlines() const { return PlanStreamlines; }
	FVector GetStreamlineOriginM() const { return StreamlineOriginM; }
	double GetAirFlowAverageS() const { return bFlowGridValid ? FlowAverager.GetCoveredS() : 0.0; }
	int32 GetAirFlowInstanceCount() const;
	FBox GetAirDomainBoundsM() const;
	// Height of the plan slice [m] above the domain floor, and how far above the domain top the plan is drawn (a map
	// over the room, so roofs and racks do not hide it from above).
	double PlanHeightM = 1.1;
	double PlanDrawOffsetM = 2.0;
	double GetPlanDrawZM() const;
	const FLeakSpray& GetLeakSpray() const { return Spray; }
	// W3: the water on the room floor (shallow water), fed with exactly what the spray delivered to the floor each step.
	const FFloorWater& GetFloorWater() const { return FloorWater; }
	// Floor water settings (cell size, Manning n, contact angle; the next simulation entry uses them).
	FFloorWaterSettings FloorWaterSettings;
	double GetFloorOffGridM3() const { return FloorOffGridM3; }
	// Sets the jet direction of a site (normalised; a zero vector restores the default cone direction).
	bool SetLiveLeakDirection(int32 Site, const FVector& DirectionM);
	// The default jet direction of a site: in the downward cone, seeded by its position (same in every run).
	static FVector DefaultSprayDirection(const FVector& PositionM, double HalfAngleRad);
	void LogSprayStatus() const;
	// Screen check of the spray (vrg.Water.Demo): enters the simulation if needed, then at t >= 1 s opens the supply branch
	// site nearest the room centre with its jet aimed across the aisle, and puts the view beside it.
	void RequestWaterDemo(double HoleDiameterM) { WaterDemoHoleM = HoleDiameterM; }
	// Rebuilt at simulation entry and by vrg.Leak.Sites (plant sites only while the plant is built).
	void RebuildLiveLeakSites();
	const TArray<FLiveLeakSite>& GetLiveLeakSites() const { return LiveLeakSites; }
	// Nearest site to a point within MaxDistanceM, INDEX_NONE if none.
	int32 FindLiveLeakSite(const FVector& PointM, double MaxDistanceM = 2.0) const;
	// Current hole [m] of a site, and the pipe inner diameter a full rupture opens.
	double GetLiveLeakHoleDiameterM(int32 Site) const;
	double GetLiveLeakRuptureDiameterM(int32 Site) const;
	// Opens / resizes / repairs (0) a site now, in simulation mode only: logs "[Leak] ... 실시간 누수(Source)" and records
	// it in the fault monitor (report s2b.leaks). False outside a run or for a bad site.
	bool SetLiveLeak(int32 Site, double HoleDiameterM, const TCHAR* Source);
	const FFacilityWaterLoop& GetFacilityLoop() const { return Plant.GetLoop(); }
	const FCouplingBridge& GetCouplingBridge() const { return Bridge; }
	const FFacilityLoopResult& GetFacilityLoopResult() const { return Plant.GetLoopResult(); }
	// S8 F5: the room ledger and the plant ledger together, out to the outdoor air. The CDU duties and the CRAH coil heat
	// are the room's outflow and the plant's inflow (the same values, rule 6), so they cancel; what is left: inputs =
	// IT + CRAH fans + CDU pumps + compressors + plant pumps, outputs = dry coolers + plant pump losses, storage = racks,
	// air, coolant and plant water. (residual J) / (all inputs J); 0 without the plant model.
	double GetSiteEnergyResidual() const;
	double GetSiteEnergyInJ() const;
	// S8 F8-2: the units of one kind the automatic sizing rule gave at the last simulation entry with the plant on, as
	// layout records at their schematic yard positions (beside the current room). Empty with true when units of that kind
	// are already placed (nothing to convert) or that plant had none; false (Reason) when no plant was built yet.
	bool CaptureAutomaticPlantUnits(EPlantUnitKind Kind, TArray<FPlantEquipmentRecord>& Out, FString& Reason) const;

	// Blanking / cold-aisle roof. Default off: the level's own layout (gaps and all) is the reference.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooling|Containment")
	EAisleContainment Containment = EAisleContainment::None;

	// Air-grid cells turned solid by Containment at the last rebuild.
	UFUNCTION(BlueprintCallable, Category = "Cooling|Containment")
	int32 GetNumContainmentCells() const { return PassiveSolidCells; }

	// ---- Layout walls / curtains (ALayoutBarrierActor), 2026-10-06 ----
	// Air cells turned solid by barriers, barrier cells left open next to equipment (duct faces / intake layers),
	// and fan ducts whose intake and exhaust ended up in separate air regions at the last rebuild.
	int32 GetNumBarrierCells() const { return BarrierSolidCells; }
	int32 GetNumBarrierCellsKeptOpen() const { return BarrierCellsKeptOpen; }
	int32 GetNumSplitDucts() const { return SplitDucts; }

	// ---- Automatic room shell (2026-10-07) ----
	// Four walls and a ceiling drawn on the faces of the air domain, every frame in both modes: the planned
	// auto-fit (layout, auto room), the manual room's actual grid bounds (layout, manual room) or the live grid
	// (simulation). Display only. The air grid boundary already is an impermeable, adiabatic wall and ceiling,
	// so the shell adds no solid cells, heat capacity or leakage (what is shown is what is computed).
	// Ceiling holes / exhaust through the ceiling are not modelled.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Room")
	bool bAutoRoomShell = true;

	// Shell thickness [m], laid outside the domain so it never covers an air cell. Display value.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Room", meta = (ClampMin = "0.01"))
	double RoomShellThicknessM = 0.2;

	// Hide the walls the camera is outside of and the ceiling when the camera is above it, so the room can be
	// looked into from outside.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Room")
	bool bRoomShellCutaway = true;

	// Box [m] the shell currently encloses; false when there is nothing to enclose (no air actor / no racks).
	bool GetRoomShellBoundsM(FBox& OutBoundsM) const;
	// Shell piece order: -X, +X, -Y, +Y walls, ceiling.
	static constexpr int32 NumRoomShellPieces = 5;
	// Which pieces stay visible for a camera at CameraM [m] (cutaway rule), in the order above.
	static void ComputeRoomShellVisibility(const FBox& BoundsM, const FVector& CameraM, bool bCutaway, bool OutVisible[NumRoomShellPieces]);
	// Visible shell pieces after the last update (0 when off).
	int32 GetNumVisibleRoomShellPieces() const;
	// Automatic (actor-less) CRAH display meshes only; room shell and containment meshes are separate components.
	int32 GetNumAutomaticCrahMeshes() const { return CrahMeshes.Num(); }
	const FBox& GetDisplayedRoomShellBoundsM() const { return RoomShellBoundsM; }
	void UpdateRoomShell();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	// ---- S6 evaluation (2026-09-27) ----
	FVrgEvaluationSettings EvalSettings;
	// bFlag: leak event on the return branch (S2b step 5).
	struct FEvalEvent { double TimeS = 0.0; FString Kind; int32 Index = INDEX_NONE; double Value = 0.0; bool bApplied = false; bool bFlag = false; TArray<FString> Args; };
	// S2b step 5: fault judgement, leak / valve record and tank history of the current run (reset at entry).
	FVrgFaultSettings FaultSettings;
	const FVrgFaultMonitor& GetFaultMonitor() const { return FaultMonitor; }
	TArray<FEvalEvent> EvalEvents;
	// Written when the queued warm-up steps finish (vrg.Eval.ReportWhenDone), then cleared.
	FString PendingReportName;
	// Writes Saved/Reports/<Name>.json and .md from the running evaluation; false before any simulation step.
	bool WriteEvaluationReport(const FString& Name, FString& OutSummary) const;
	const FVrgEvaluation& GetEvaluation() const { return Eval; }
	// 2026-10-22 instruments on the pipes (flow, water temperature, leak-detection cable every vrg.Sensors.PipeIntervalM)
	// and on the plant units / CDUs (DataCenterSimulationInstruments.cpp). Simulated measurements (D9).
	const FInstrumentBank& GetInstruments() const { return Instruments; }
	// One line of an instrument point's latest readings for the viewport (empty before its first reading).
	FString DescribeInstrument(int32 Point) const;
	// Web dashboard stage 1: summary card values at the current SimTime (window values = the report's).
	FVrgDashboardSummary BuildDashboardSummary() const;
	// Web dashboard stage 3: plan view of the equipment boxes in the world now, plus the inlet window means.
	// Automatic CRAH units (no actor) are listed only in simulation mode, where their placement is current.
	FVrgDashboardLayout BuildDashboardLayout() const;
	// Diagnostic (vrg.Web.ProfileLayout): average ms of the full actor walk (used) vs a class-wise walk (comparison),
	// the whole layout build and its JSON, over Repeats runs on the game thread. Returns one log line.
	FString ProfileDashboardLayout(int32 Repeats) const;
	// Web dashboard stage 4: one row per evaluation inlet (racks, then spines) in simulation mode, plus the slots and
	// parts of DetailInlet (one MakeSnapshot) when it is a valid inlet.
	FVrgDashboardRacks BuildDashboardRacks(int32 DetailInlet) const;
	// Web dashboard tab 4 (2026-10-14): CRAH units, CDUs, pumps and the power breakdown of the last step, copied from
	// values the coupled step already computed (empty lists outside simulation mode).
	FVrgDashboardCooling BuildDashboardCooling() const;
	// S2b step 6 (2026-10-20): leak layer. Leaks, tanks, per-rack valve / isolation / hydraulic impact and the fault
	// monitor record, copied from the room's hydraulic solution and inventory, the coupling and the monitor (no new model).
	FVrgDashboardLeaks BuildDashboardLeaks() const;
	// The fault judgement's input of this step (the dashboard shows the same values the judgement used).
	FVrgFaultInput MakeFaultInput() const;
	// Stage 5: handles a report save the page queued (same writer as vrg.Eval.Report) and records the result for the
	// report list. Called from each dashboard publish on the game thread; returns true when it handled one.
	bool ProcessDashboardRequests();
	// vrg.Web.ProfileLayout <N> wait: run the profile once the queued warm-up steps finish (0 = none).
	int32 PendingProfileRepeats = 0;

private:
	friend bool VrgLayoutFile::Load(UWorld* World, const FString& Name, FString& OutSummary);
	UPROPERTY() FRoomAirDomainSettings RoomAirDomain;
	UPROPERTY() TArray<FRoomCeilingPlate> RoomCeilingPlates;
	FString RoomAirDomainMessage;
	FVrgEvaluation Eval;
	FInstrumentBank Instruments;
	TArray<FVector> InstrumentPlantNodesM[3];   // plant network node positions at the build (where plant leaks sit)
	void RebuildInstruments();
	void InstrumentsAfterStep();
	FVrgFaultMonitor FaultMonitor;
	int32 LastTankSampleSecond = -1;
	void FaultAfterStep();
	bool bEvalReady = false;
	void EvalBeforeStep();
	void EvalAfterStep();
	// HUD only: the coupling snapshot walks the whole air grid for its ledger, so it is rebuilt once per sim time.
	mutable FCouplingSnapshot DisplaySnapshot;
	mutable double DisplaySnapshotSimTimeS = -1.0;
	mutable bool bDisplaySnapshotValid = false;
	// Snapshot rebuilt at most once per SimTime for display (HUD summary, web dashboard).
	const FCouplingSnapshot& GetCachedDisplaySnapshot() const;
	// Game-thread copy for the web dashboard (rule 5), every DashboardPublishPeriodS of real time while it runs.
	void PublishDashboard();
	double LastDashboardPublishS = -1.0e9;
	static constexpr double DashboardPublishPeriodS = 0.5;
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
	ESimMode Mode = ESimMode::Simulation;
	// Layout mode: keep every cabinet model and the air solver from integrating on their own.
	void HoldModelsForLayout();
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
	bool HasUnroutedRacks() const;

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
	FFacilityPlant Plant;
	TArray<FLiveLeakSite> LiveLeakSites;
	FLeakSpray Spray;
	FFloorWater FloorWater;
	// Air view (2026-10-27): streamtubes / plan slice / arrows of the 30 s average flow (AirFlowView.h).
	EAirView AirView = EAirView::Volume;
	FAirFlowAverager FlowAverager;
	FUniformAirGrid FlowGrid;          // the averaged field the view draws (valid once a block closed)
	bool bFlowGridValid = false;
	TArray<FAirStreamline> Streamlines;
	TArray<FAirStreamline> PlanStreamlines;
	FVector StreamlineOriginM = FVector::ZeroVector;
	double LastFlowViewWallS = -1.0;
	UPROPERTY(Transient) TObjectPtr<class AAirFlowDisplayActor> FlowDisplay;
	void AirFlowAfterStep(double DtS);
	void UpdateAirFlowView();
	double FloorOffGridM3 = 0.0;   // floor deposits that missed the floor grid (kept here, reported)
	TArray<double> SprayTankLeakedM3;   // each tank's cumulative leaked volume at the last spray step
	int32 LastSprayLogSecond = -1;
	FSprayGeometry BuildSprayGeometry() const;
	void SprayAfterStep(double DtS);
	void DrawSpray() const;
	double WaterDemoHoleM = -1.0;   // pending vrg.Water.Demo hole [m]; < 0 = none
	void TickWaterDemo();
	// S8 F8-2: the same plant with the automatic counts (no placed units), built at the same entry; layout conversion only.
	TUniquePtr<FFacilityPlant> AutomaticPlant;
	bool bPlantActive = false;
	bool bPlantBuilt = false;         // Plant built at the last coupling build (drawn even when not coupled)
	bool bPlantPreviewTried = false;  // layout-mode start: one build so the yard shows before the first run
	int32 LastPlantDisplayPipes = -1;
	UPROPERTY(Transient)
	TObjectPtr<class AFacilityPlantDisplayActor> PlantDisplay;
	double LastPlantDisplayWallS = -1.0;
	struct FPlantDisplayInput MakePlantDisplayInput() const;
	TArray<int32> CduGroupRows;   // S8 F5: room row of every CDU group (its pump feeds the group's coolant)
	int32 LastPlantLogSecond = -1;
	// S8 F1: builds and solves the FWS network from the sized CDU groups and hands each exchanger its branch flow.
	void BuildFacilityLoop(TArray<FCduCouplingGroup>& Groups);
	// 2026-10-22: the inputs of the last plant build, so layout mode can rebuild the drawn plant when placed units are
	// added or deleted (their pipes show at once instead of at the next simulation entry). Display only: layout mode
	// does not step the plant, and the next entry builds everything again.
	struct FPlantBuildInputs
	{
		bool bValid = false;
		FFacilityLoopSpec Spec;        // before the placed counts
		double DesignHeatW = 0.0, WaterCp = 0.0;
		TArray<FPlantCrahCoil> Coils;
		FPlantEquipmentCounts Placed;  // the counts the current Plant was built with
	} PlantBuild;
	bool BuildPlantWithCounts(const FPlantEquipmentCounts& Placed);
	void RebuildPlantIfCountsChanged();
	// S8 F2: CDU inlet = plant supply before the coupling step; the plant advances with that step's duties after it.
	void PlantBeforeStep();
	void PlantAfterStep(double DtS);
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
	static constexpr int32 BarrierSolidOwner = -3;
	int32 BarrierSolidCells = 0;
	int32 BarrierCellsKeptOpen = 0;
	static constexpr int32 CeilingSolidOwner = -4;
	int32 CeilingSolidCells = 0;
	void MarkCeilingSolidCells(FUniformAirGrid& Grid, const FVector& GridOriginM);
	int32 SplitDucts = 0;
	// Walls / curtains as solids. Runs after the cabinets, CRAH units and containment have their cells.
	void MarkBarrierSolidCells(FUniformAirGrid& Grid, const FVector& GridOriginM);

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
	// Per CrahUnits entry: where it stands and what it is, for the dashboard (placements can be re-planned later).
	struct FCrahUnitLabel { FVector2D XYM; bool bTopIntake = false; bool bPlaced = false; };
	TArray<FCrahUnitLabel> CrahUnitLabels;
	double CrahUnitFlowM3s = 0.0, CrahUnitUAWK = 0.0, CrahUnitFanW = 0.0;
	int32 CrahUnitsDropped = 0;
	bool bCrahFellBackToCoil = false;
	// Places units from the rack rows (CrahLayout). Units that leave the air grid are dropped and counted.
	void PlanCrahUnits();
 void FilterCrahPlacementsToGrid(AAirFieldDebugActor* Field);
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

	UPROPERTY()
	TObjectPtr<UMaterialInterface> RoomShellMaterial;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> RoomShellMeshes;
	FBox RoomShellBoundsM = FBox(ForceInit);
	double RoomShellBuiltThicknessM = -1.0;
};
