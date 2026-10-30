// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DebugKnob.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Simulation/Air/RoomAirDomain.h"
#include "DataCenterDebugController.generated.h"

class ARackThermalActor;
class ADataCenterRoomActor;
class UInputAction;
class UInputMappingContext;
class ULayoutRackPalette;
class URoomSettingsWidget;
class ADataCenterSimulationActor;

// Gizmo for a single selected wall / curtain (2026-10-06), Unreal-style: W move, E rotate, R scale, Space cycles.
// Move and rotate act along / about the world axes, scale along the barrier's own axes (always local, as in Unreal).
UENUM()
enum class ELayoutGizmoMode : uint8 { Translate, Rotate, Scale };

// 런타임에 시뮬레이션 파라미터를 돌려 보기 위한 디버그 입력.
//
// EnhancedInput을 쓰지만 InputAction/MappingContext 에셋을 만들지 않고 코드에서
// 만든다. 에셋을 만들면 물리 파라미터의 기본값이 코드와 에셋 두 곳에 흩어진다.
//
// 이 컨트롤러는 값을 정하지 않는다. 손잡이(FDebugKnob)가 범위와 증분을 들고 있고,
// 여기서는 키를 손잡이에 잇고 결과를 액터에 밀어 넣기만 한다.
//
// 손잡이의 시작값과 범위는 레벨에서 찾은 첫 랙의 현재 값에서 잡는다.
// 코드에 숫자를 박아 두면 프리셋 값이 바뀌는 순간 범위가 어긋난다.
//
// 대상은 한 번만 찾지 않는다. ADataCenterRoomActor는 첫 틱에 랙을 띄우므로
// BeginPlay 시점에는 아직 없다. 주기적으로 다시 훑는다.
//
// 키 배치:
//   1 / 2        CPU 발열 내리기 / 올리기 (칩당, 모든 Grace)
//   3 / 4        GPU 발열 내리기 / 올리기 (칩당, 모든 Blackwell)
//   5 / 6        펌프 회전수비 내리기 / 올리기
//   7 / 8        라디에이터 UA 내리기 / 올리기
//   E            바라보는 유닛 하나 뽑기 / 꽂기 (다른 유닛·다른 랙은 건드리지 않는다)
//   R            시뮬레이션 리셋
//   H            도움말 표시 토글
//
// "바라보는 유닛"은 카메라 시선이 처음 닿는 랙 상자(RackSizeM, 공기 격자에서 고체로 막는 것과 같은 상자)에서
// 시선이 들어간 높이의 슬롯이다. 메시는 충돌이 꺼져 있어 라인 트레이스 대신 이 상자와 직접 교차시킨다.
// 뽑힌 유닛은 앞으로 나와 있지만 같은 높이에서 랙 앞면을 보면 다시 잡힌다. 랙은 초록, 유닛은 노란 상자로 그린다.
UCLASS()
class VRG_TERMPROJ_API ADataCenterDebugController : public APlayerController
{
	GENERATED_BODY()

public:
	int32 GetAimedInternalLeakSite() const { return LeakToolMode != 0 && IsCameraInputAvailable() && !IsInputKeyDown(EKeys::RightMouseButton) ? LeakToolAimed : INDEX_NONE; }
	ADataCenterDebugController();

	virtual void Tick(float DeltaTime) override;
	virtual void UpdateRotation(float DeltaTime) override;
	virtual void PlayerTick(float DeltaTime) override;
	virtual bool IsMoveInputIgnored() const override;
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

	// View navigation only: axes = forward/right/world-up, wheel = notches. SI speed converted at the pawn boundary.
	void ApplyLayoutCameraInput(float DeltaTime, const FVector& Axes, float Wheel, bool bFly);
	double GetLayoutCameraSpeedMs() const { return LayoutCameraSpeedMs; }

	// The patch window temporarily owns mouse focus; restore the current mode when it closes.
	void SetPatchMapOpen(bool bOpen);
	void RefreshInteractionInputMode();
	bool OpenRoomSettings(bool bCeiling = false, bool bHole = false);
	bool OpenRoomCeilingSettings() { return OpenRoomSettings(true); }
	bool OpenRoomCeilingHoleSettings() { return OpenRoomSettings(false,true); }
	bool AddRoomCeilingHole(const FRoomCeilingHole& Hole, FString& Message);
	bool AddRoomCeiling(const FRoomCeilingPlate& Plate, FString& Message);
	FString DescribeRoomCeilingPlan(const FRoomCeilingPlate& Plate);
	bool SetRoomCeilingEditing(bool bEnable);
	void ToggleRoomCeilingEditing() { SetRoomCeilingEditing(!bRoomCeilingEditing); }
	bool IsRoomCeilingEditing() const { return bRoomCeilingEditing; }
	bool SelectRoomCeilingAlongRay(const FVector& EyeCm, const FVector& Direction, bool bCycle = false, bool bKeepSelected = false, bool bToggle = false);
	int32 GetSelectedRoomCeilingIndex() const;
	TArray<int32> GetSelectedRoomCeilingIndices() const;
	bool TryDeleteRoomCeiling();
	bool SetRoomCeilingHoleEditing(bool bEnable);
	void ToggleRoomCeilingHoleEditing() { SetRoomCeilingHoleEditing(!bRoomCeilingHoleEditing); }
	bool IsRoomCeilingHoleEditing() const { return bRoomCeilingHoleEditing; }
	bool SelectRoomCeilingHoleAlongRay(const FVector& EyeCm, const FVector& Direction, bool bCycle = false, bool bKeepSelected = false, bool bToggle = false);
	int32 GetSelectedRoomCeilingHoleIndex() const;
	TArray<int32> GetSelectedRoomCeilingHoleIndices() const;
	bool TryDeleteRoomCeilingHole();
	int32 PickRoomCeilingHoleCorner(const FVector2D& Screen) const;
	bool BeginRoomCeilingHoleCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction);
	bool BeginRoomCeilingHoleMoveDrag(const FVector& EyeCm, const FVector& Direction);
	bool BeginRoomCeilingHolePointerDrag(int32 PickedHandle, const FVector& EyeCm, const FVector& Direction);
	int32 PickRoomCeilingCorner(const FVector2D& Screen) const;
	bool BeginRoomCeilingCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction);
	bool BeginRoomCeilingHeightDrag(const FVector& EyeCm, const FVector& Direction) { return BeginRoomCeilingCornerDrag(RoomHeightHandle,EyeCm,Direction); }
	static constexpr int32 CeilingMoveHandle = 5; // Whole-plate XY translation; shares the handle gesture lifecycle.
	bool BeginRoomCeilingMoveDrag(const FVector& EyeCm, const FVector& Direction) { return BeginRoomCeilingCornerDrag(CeilingMoveHandle,EyeCm,Direction); }
	bool BeginRoomCeilingPointerDrag(int32 PickedHandle, const FVector& EyeCm, const FVector& Direction);
	void UpdateRoomCeilingCornerDrag(const FVector& EyeCm, const FVector& Direction);
	bool EndRoomCeilingCornerDrag(bool bCommit);
	bool GetRoomCeilingPreview(FRoomCeilingPlate& Out, bool& bValid) const;
	bool GetRoomCeilingListPreview(TArray<FRoomCeilingPlate>& Out, bool& bValid) const;
	FString GetRoomCeilingEditMessage() const { return CeilingEditMessage; }
	void CloseRoomSettings(URoomSettingsWidget* ExpectedWidget = nullptr);
	bool IsRoomSettingsOpen() const { return bRoomSettingsOpen; }
	// Key hint lines for the HUD: one short line, or the grouped list after H.
	void GetHelpLines(TArray<FString>& Out) const;
	FRoomAirDomainSettings GetRoomSettingsDraft() const;
	FString DescribeRoomSettingsPlan(const FRoomAirDomainSettings& Settings);
	bool ApplyRoomSettings(const FRoomAirDomainSettings& Settings, FString& Message);
	// Exclusive room handles; view rays are UE cm, authored preview/settings remain metres.
	bool SetRoomBoundsEditing(bool bEnable);
	void ToggleRoomBoundsEditing() { SetRoomBoundsEditing(!bRoomBoundsEditing); }
	bool IsRoomBoundsEditing() const { return bRoomBoundsEditing; }
	static constexpr int32 RoomHeightHandle = 4; // Corners0..3, ceiling-centre handle4; shared gesture/command.
	bool BeginRoomCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction);
	bool BeginRoomHeightDrag(const FVector& EyeCm, const FVector& Direction) { return BeginRoomCornerDrag(RoomHeightHandle, EyeCm, Direction); }
	void UpdateRoomCornerDrag(const FVector& EyeCm, const FVector& Direction);
	bool EndRoomCornerDrag(bool bCommit);
	bool GetRoomBoundsPreview(FRoomAirDomainSettings& Out, bool& bValid) const;
	FString GetRoomBoundsMessage() const { return RoomBoundsMessage; }
	int32 PickRoomCorner(const FVector2D& Screen) const;

	// CPU 칩 하나의 전부하 발열 [W].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob CpuHeatKnob;

	// GPU 칩 하나의 전부하 발열 [W].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob GpuHeatKnob;

	// 펌프 회전수비 [무차원]. 상사식으로 체절양정이 r^2로 스케일된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob PumpSpeedKnob;

	// 라디에이터 크기 [W/K]. 자릿수를 훑어야 하므로 곱셈으로 움직인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob RadiatorUAKnob;

	// Workload utilization of every rack [dimensionless, 0..1]. Part heat follows its idle fraction.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	FDebugKnob UtilizationKnob;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowHelp = false;  // collapsed: one short line; H shows the full key list

	// How far the view ray looks for a rack to select [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.5"))
	double MaxSelectDistanceM = 15.0;

	// Nearest rack whose box (RackSizeM, base at the actor origin) the ray hits within MaxDistanceCm, or null.
	// Unreal units here (cm): this is view picking, not a solver.
	static ARackThermalActor* PickRackAlongRay(const TArray<TObjectPtr<ARackThermalActor>>& Racks,
		const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm, double* OutHitZM = nullptr);

	// Slot whose height range (rack-local, m) contains HeightM, or INDEX_NONE (blank panel, frame).
	static int32 SlotAtHeight(const ARackThermalActor& Rack, double HeightM);

	// ---- Layout mode placement (2026-09-27) ----
	// Racks, spines, CDUs and CRAH units: box of SizeM (depth, width, height) with the origin at the bottom centre.
	static bool PlaceableBoxM(const AActor* Actor, FVector& OutSizeM);
	// Nearest placeable whose box the ray enters within MaxDistanceCm, or null (OutDistanceCm: where it enters).
	static AActor* PickPlaceableAlongRay(UWorld* World, const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm, double* OutDistanceCm = nullptr);
	// Where the ray meets the horizontal plane at PlaneZCm, snapped in X and Y to SnapM; false if it never does.
	static bool FloorPointSnapped(const FVector& EyeCm, const FVector& Dir, double PlaneZCm, double SnapM, FVector& OutCm);
	// Projected equipment boxes, with 1 cm clearance on a separating axis (UI placement rule).
	// Barriers may overlap each other (wall corners) but not equipment footprints.
	static bool IsLayoutPlacementBlocked(const AActor* Actor, const FTransform& Proposed, const TArray<AActor*>* Ignored = nullptr);
	// True when a manual room is set and the box (with its air duct cells for cabinets / coolers) would not fit it: the same
	// test Load and simulation entry use (room equipment only, 2026-10-23).
	static bool IsOutsideManualRoom(UWorld* World, const FVector& SizeM, const FTransform& Proposed, bool bCabinet = false, bool bCrah = false, bool bTopIntake = false);
	static bool IsLayoutBoxBlocked(UWorld* World, const FVector& SizeM, const FTransform& Proposed, const TArray<AActor*>* Ignored = nullptr,
		bool bCandidateIsBarrier = false, double ClearanceCm = 1.0);
	// Addition is a value-only preview; no actors or pipes until confirmation.
	void BeginRackAddition();
	void BeginSpineAddition();
	void BeginCduAddition();
 void BeginInRowCrahAddition();
 void BeginPerimeterCrahAddition();
 void BeginCrahAddition() { BeginInRowCrahAddition(); }
 ACrahUnitActor* ConfirmCrahAddition();
	ACoolingDistributionUnitActor* ConfirmCduAddition();
	ASpineCabinetActor* ConfirmSpineAddition();
	bool BeginRackDuplication();
	bool BeginSpineDuplication();
	bool BeginCduDuplication(); // Entire native CDU selection; authored values only.
	bool BeginCrahDuplication(); // Entire native cooler selection; no automatic origin/bounds or live state.
	// Walls / curtains (ALayoutBarrierActor): same value preview, 5 cm snap, R/Esc as equipment addition.
	void BeginBarrierAddition(EBarrierKind Kind);
	void BeginWallAddition() { BeginBarrierAddition(EBarrierKind::Wall); }
	void BeginCurtainAddition() { BeginBarrierAddition(EBarrierKind::Curtain); }
	bool BeginBarrierDuplication(); // Entire barrier selection; kind and full transform (size) only.
	ALayoutBarrierActor* ConfirmBarrierAddition();
	bool TryDeleteLayoutBarrier(); // Entire barrier selection, one Undo command.
	// S8 F8-2 plant units (outside the room). The first addition of a kind turns that kind's automatic units (the last
	// simulation entry's sizing) into actors at their yard positions, in the same command as the new unit, so one Undo
	// returns to the automatic plant. Refused before any plant was built (the automatic count is not known yet).
	void BeginPlantAddition(EPlantUnitKind Kind);
	// S8 F8-3: entire plant unit selection (kinds may mix); kind, size and transform only, new identities. Nothing is
	// converted (the selected kinds already have units) and no plant needs to have been built.
	bool BeginPlantDuplication();
	APlantEquipmentActor* ConfirmPlantAddition();
	// vrg.Cam (2026-10-23): puts the view at an eye point [m] looking at pitch / yaw [deg] and stops the start framing,
	// so a scripted run can capture a chosen part of the scene (screenshots for checks; not a user camera mode).
	bool SetViewpointM(const FVector& EyeM, const FRotator& Look);
	// 2026-10-22: the yard's automatic units are drawn instances, not actors. A press on one (nearer than any placeable)
	// turns its kind into actors at the drawn positions, the same conversion as the first addition (one Undo step), and
	// returns the pressed unit's actor so it is selected and dragged like any placed unit. Null when nothing was hit.
	APlantEquipmentActor* ConvertAutomaticPlantAlongRay(const FVector& EyeCm, const FVector& Direction);
	bool TryDeleteLayoutPlant(); // Entire plant unit selection, one Undo command (no units of a kind = automatic again).
	bool IsAddingPlant() const { return bAddingPlant; }
	void CancelRackAddition();
	void UpdateRackAdditionAlongRay(const FVector& EyeCm, const FVector& Direction);
	bool GetRackAdditionPreview(FTransform& Out, FVector& SizeM, int32 MemberIndex = 0) const;
	int32 GetRackAdditionPreviewCount() const { return bAddingRack ? FMath::Max(1, FMath::Max(CrahDuplicationMembers.Num(), FMath::Max(CduDuplicationMembers.Num(), FMath::Max(RackDuplicationMembers.Num(), FMath::Max(SpineDuplicationMembers.Num(), FMath::Max(BarrierDuplicationMembers.Num(), PlantDuplicationMembers.Num())))))) : 0; }
	bool TryRotateRackAddition();
	bool IsRackAdditionBlocked() const;
	ARackThermalActor* ConfirmRackAddition();
	bool IsAddingRack() const { return bAddingRack; }
	// Shared by legacy G and cursor placement; layout-only, no solver changes.
	bool TryLayoutMove(AActor* Actor, const FVector& TargetCm);
	bool TryLayoutRotation(AActor* Actor, const FRotator& Rotation, bool bRecordHistory = false);
	bool TryRotateLayoutSelection();
	// Runtime layout only: all selected equipment must be racks, with no gesture in progress.
	bool TryDeleteLayoutRack();
	bool TryDeleteLayoutCdu();
 bool TryDeleteLayoutCrah(); // Entire native CRAH selection; other equipment remains separate.
	bool TryDeleteLayoutSpine(); // Entire native spine selection; rack deletion remains separate.
	// Game-thread layout transforms only (UE cm at this actor boundary), not simulation state.
	bool TryUndoLayout();
	bool TryRedoLayout();
	void ClearLayoutHistory();
	void ResetLayoutEditsForLoad();
	void RefreshLayoutEquipmentReferences(bool bKeepSelection = false);
	void NotifyLayoutBarrierRecreated(ALayoutBarrierActor* Barrier, ALayoutBarrierActor* Previous = nullptr);
	void NotifyLayoutPlantRecreated(APlantEquipmentActor* Unit, APlantEquipmentActor* Previous = nullptr);
	void NotifyLayoutRackRecreated(ARackThermalActor* Rack, ARackThermalActor* Previous = nullptr) { RebindLayoutRack(Rack, Previous); }
 void NotifyLayoutCrahRecreated(ACrahUnitActor* Crah, ACrahUnitActor* Previous = nullptr);
	void NotifyLayoutCduRecreated(ACoolingDistributionUnitActor* Cdu, ACoolingDistributionUnitActor* Previous = nullptr);
	void NotifyLayoutSpineRecreated(ASpineCabinetActor* Spine, ASpineCabinetActor* Previous = nullptr);
	int32 GetLayoutUndoCount() const { return LayoutUndo.Num(); }
	int32 GetLayoutRedoCount() const { return LayoutRedo.Num(); }

	// Grid step of layout moves [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0"))
	double PlaceSnapM = 0.05;

	// Start view (2026-09-27): the level's PlayerStart is inside the racks, so on the first frame with a pawn the view
	// is moved to see every rack / spine / CDU / CRAH box, from the same oblique direction as vrg.Air.FrameDomain.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bFrameEquipmentOnStart = true;

	// Eye point and look rotation that fit all placeable boxes in a view of the given full horizontal FOV and aspect
	// (width / height); false if the world has no equipment.
	static bool EquipmentViewpoint(UWorld* World, double HorizontalFovDeg, double Aspect, FVector& OutEyeCm, FRotator& OutLook,
		const AActor* OnlyActor = nullptr, const TArray<AActor*>* OnlyActors = nullptr);
	void OnFocusEquipment();
	// R dispatch: rotate the clicked selection in layout; retain simulation reset.
	void OnResetSim();
	// Layout selection only: never moves equipment or advances the solver. A miss clears the selection.
	void SelectLayoutAlongRay(const FVector& EyeCm, const FVector& Direction, bool bToggle = false, bool bKeepSelected = false);
	TArray<AActor*> GetSelectedLayoutActors() const;
	AActor* GetSelectedLayoutActor() const;
	// Transactional preview: only EndLayoutDrag(true) moves the actor (no pipe rebuild while dragging).
	void BeginLayoutDragAlongRay(const FVector& EyeCm, const FVector& Direction);
	void UpdateLayoutDragAlongRay(const FVector& EyeCm, const FVector& Direction, bool bDisableRowSnap = false);
	void EndLayoutDrag(bool bCommit);
	bool IsLayoutDragging() const { return !DragMembers.IsEmpty() || GizmoDrag.bActive || RoomDragCorner != INDEX_NONE || CeilingDragCorner != INDEX_NONE; }
	// ---- Barrier gizmo (layout mode, one selected barrier). UE cm at this view boundary. ----
	ALayoutBarrierActor* GetLayoutGizmoTarget() const;
	ELayoutGizmoMode GetLayoutGizmoMode() const { return GizmoMode; }
	bool SetLayoutGizmoMode(ELayoutGizmoMode Mode);
	// Gizmo centre (box centre) and handle length for the current view [cm].
	bool GetLayoutGizmoFrame(FVector& PivotCm, double& LengthCm) const;
	// World direction of handle Axis (0 X, 1 Y, 2 Z) in the current mode.
	FVector GetLayoutGizmoAxis(int32 Axis) const;
	// Handle under a screen point (pixels), INDEX_NONE if none within 10 px.
	int32 PickLayoutGizmoAxis(const FVector2D& Screen) const;
	// Live drag of one handle: the barrier follows; release commits (Undo) unless blocked, cancel restores.
	bool BeginLayoutGizmoDrag(int32 Axis, const FVector& EyeCm, const FVector& Direction, const FVector2D& Screen = FVector2D::ZeroVector);
	void UpdateLayoutGizmoDrag(const FVector& EyeCm, const FVector& Direction, const FVector2D& Screen = FVector2D::ZeroVector, bool bNoSnap = false);
	void EndLayoutGizmoDrag(bool bCommit);
	bool IsLayoutGizmoDragging() const { return GizmoDrag.bActive; }
	bool IsLayoutGizmoBlocked() const;
	// UI snaps (not physical): 15 degree rotation steps; moves and size changes use PlaceSnapM. Alt disables.
	double GizmoRotateSnapDeg = 15.0;
	FVector GetLayoutDragPreviewCm() const { return DragPreviewCm; }
	bool IsLayoutDragPreviewBlocked() const;
	bool GetLayoutDragPreviewTransform(const AActor* Actor, FTransform& Out) const;
	bool GetLayoutRowSnapGuide(FVector& StartCm, FVector& EndCm) const;
	// Viewport pixel rectangle, clipped at UE's reversed-Z near plane; no mesh/occlusion picking.
	static bool ProjectLayoutBoxToScreen(const AActor* Actor, const FMatrix& ViewProjection, const FIntRect& ViewRect, FBox2D& Out);
	bool SelectLayoutInScreenRect(const FBox2D& Rect, const FMatrix& ViewProjection, const FIntRect& ViewRect, bool bAdd);
	void BeginLayoutMarquee(const FVector2D& Start, bool bAdd);
	void UpdateLayoutMarquee(const FVector2D& Current);
	void EndLayoutMarquee(bool bCommit);
	bool IsLayoutMarqueeActive() const { return bMarqueeActive; }
	bool GetLayoutMarqueeRect(FBox2D& Out) const;

	// 레벨에서 대상을 다시 훑는 주기 [s]. 방 액터가 랙을 늦게 띄우기 때문에 필요하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	float TargetRefreshIntervalS = 0.5f;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

private:
 struct FLayoutCrahContact { FName Name; TWeakObjectPtr<UClass> Class; FTransform Transform; FVector SizeM; };
	struct FLayoutTransformMember
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform Before;
		FTransform After;
		FString RackId;
		FString SpineId;
		FString CduId;
		FString CrahId;
		FString BarrierId;
		FString PlantId;   // S8 F8 plant unit
		// Neighbours touching the actor at Before / After when recorded: Undo / Redo may touch them again (2026-10-23).
		TArray<FLayoutCrahContact> BeforeContacts, AfterContacts;
	};
	struct FLayoutRackMember
	{
		FString Id;
		FName Name;
		TWeakObjectPtr<UClass> Class;
		FTransform Transform;
		FRackLayoutSettings Settings;
		TArray<FLayoutCrahContact> Contacts;   // neighbours touching it when recorded (any class, despite the name)
	};
	struct FLayoutSpineMember
	{
		FString Id; FName Name; FTransform Transform; FSpineLayoutSettings Settings;
		bool bRuntime = true;
	};
	struct FLayoutCduMember
	{
		FString Id; FName Name; FTransform Transform; FCduLayoutSettings Settings;
		bool bRuntime = true;
	};
	// Existing automatic boxes may touch rack ends. Preserve only those unchanged contacts on conversion/Redo.
 struct FLayoutCrahMember { FCrahLayoutRecord Record; TArray<FLayoutCrahContact> ExistingContacts; };
 struct FLayoutTransformCommand
	{
		bool bRoomSettings = false;
		bool bRoomCeilings = false;
		TArray<FRoomCeilingPlate> CeilingsBefore, CeilingsAfter;
		TWeakObjectPtr<ADataCenterSimulationActor> RoomSettingsTarget;
		FRoomAirDomainSettings RoomBefore;
		FRoomAirDomainSettings RoomAfter;
		TArray<FLayoutTransformMember> Members;
		bool bRackPresence = false;
		bool bBeforePresent = false;
		TArray<FLayoutRackMember> Racks;
		TArray<FLayoutSpineMember> Spines;
		TArray<FLayoutCduMember> Cdus;
  TArray<FLayoutCrahMember> Crahs;
  TArray<FLayoutBarrierRecord> Barriers;
  TArray<FPlantEquipmentRecord> PlantUnits;   // S8 F8-2 presence (added / converted / deleted units)
  bool bCrahAutomaticBefore = false;
  uint8 CrahLayoutBefore = 0;
	};
	TArray<FLayoutTransformCommand> LayoutUndo;
	TArray<FLayoutTransformCommand> LayoutRedo;
	void RecordLayoutTransform(AActor* Actor, const FTransform& Before);
	void RecordLayoutTransforms(const TArray<AActor*>& Actors, const TArray<FTransform>& Before);
	bool RestoreLayoutTransform(bool bRedo);
	void PushLayoutCommand(FLayoutTransformCommand Command);
	FLayoutTransformCommand CaptureRackPresence(ARackThermalActor* Rack, bool bBeforePresent);
	bool RestoreRackPresence(const FLayoutTransformCommand& Command, bool bRedo);
	bool RestoreCrahPresence(const FLayoutTransformCommand& Command, bool bRedo);
 bool RestoreCduPresence(const FLayoutTransformCommand& Command, bool bRedo);
	bool RestoreSpinePresence(const FLayoutTransformCommand& Command, bool bRedo);
	void RebindLayoutRack(ARackThermalActor* Rack, ARackThermalActor* Previous = nullptr);
	UPROPERTY(Transient)
	TObjectPtr<ULayoutRackPalette> RackPalette;
	UPROPERTY(Transient) TObjectPtr<URoomSettingsWidget> RoomSettingsWidget;
	bool bRoomSettingsOpen = false;
	bool bRoomCeilingOpen = false;
	bool bRoomCeilingHoleOpen = false;
	bool bRoomCeilingEditing = false;
	bool bRoomCeilingHoleEditing = false;
	int32 SelectedRoomCeilingHole = INDEX_NONE;
	int32 SelectedRoomCeiling = INDEX_NONE;
	TArray<int32> SelectedRoomCeilings;
	TArray<int32> SelectedRoomCeilingHoles;
	TArray<FRoomCeilingPlate> SelectedCeilingList;
	TWeakObjectPtr<ADataCenterSimulationActor> SelectedCeilingTarget;
	void ClearRoomCeilingSelection();
	int32 CeilingDragCorner = INDEX_NONE;
	int32 CeilingDragHole = INDEX_NONE; // Selected hole in the captured parent list; same gesture/history as plate editing.
	bool BeginRoomCeilingDrag(int32 Corner, bool bHole, const FVector& EyeCm, const FVector& Direction);
	FRoomCeilingPlate CeilingDragPreview;
	TArray<FRoomCeilingPlate> CeilingDragPlatesPreview;
	TArray<int32> CeilingDragItems; // Captured plate or hole indices; one atomic whole-list command.
	FRoomAirDomainSettings CeilingDragRoom;
	FVector CeilingDragStartCm = FVector::ZeroVector;
	double CeilingDragStartHeightCm = 0;
	FVector2D CeilingDragStartMouse = FVector2D::ZeroVector;
	bool bCeilingMouseThresholdPassed = false;
	bool bCeilingPreviewValid = false;
	FString CeilingEditMessage;
	void ValidateRoomCeilingPreview();
	void UpdateRoomCeilingFromCursor();
	FString DescribeRoomCeilingCandidate(const FRoomCeilingPlate& P, const TArray<FRoomCeilingPlate>& Plates);
	bool bRoomBoundsEditing = false;
	int32 RoomDragCorner = INDEX_NONE;
	FRoomAirDomainSettings RoomDragBefore, RoomDragPreview;
	FVector RoomDragStartFloorCm = FVector::ZeroVector;
	double RoomDragStartHeightCm = 0;
	FVector2D RoomDragStartMouse = FVector2D::ZeroVector;
	bool bRoomMouseThresholdPassed = false;
	bool bRoomRayValid = true;
	TWeakObjectPtr<ADataCenterSimulationActor> RoomDragTarget;
	bool bRoomPreviewValid = false;
	FString RoomBoundsMessage;
	void ValidateRoomBoundsPreview();
	void UpdateRoomCornerFromCursor();
	void DrawRoomBounds() const;
	void DrawRoomCeilings() const;
	bool CommitRoomSettings(const FRoomAirDomainSettings& Settings, FString& Message);
	bool bAddingRack = false; // Shared equipment preview activity; existing input guards use this flag.
	bool bAddingSpine = false;
	bool bAddingCdu = false;
 bool bAddingCrah = false;
	bool bAddingBarrier = false;
	EBarrierKind BarrierAdditionKind = EBarrierKind::Wall;
	TArray<FLayoutBarrierRecord> BarrierDuplicationMembers; // Kind and full transform only.
	bool RestoreBarrierPresence(const FLayoutTransformCommand& Command, bool bRedo);
	bool bAddingPlant = false;
	EPlantUnitKind PlantAdditionKind = EPlantUnitKind::FwsDryCooler;
	TArray<FPlantEquipmentRecord> PlantDuplicationMembers; // Kind, size and transform only (no identity).
	// Duplication previews keep the members' height; a fresh addition goes to the floor plane.
	bool HasDuplicationMembers() const { return !RackDuplicationMembers.IsEmpty() || !SpineDuplicationMembers.IsEmpty() || !CduDuplicationMembers.IsEmpty()
		|| !CrahDuplicationMembers.IsEmpty() || !BarrierDuplicationMembers.IsEmpty() || !PlantDuplicationMembers.IsEmpty(); }
	bool RestorePlantPresence(const FLayoutTransformCommand& Command, bool bRedo);
	ELayoutGizmoMode GizmoMode = ELayoutGizmoMode::Translate;
	int32 GizmoHoverAxis = INDEX_NONE;
	struct FGizmoDrag
	{
		bool bActive = false;
		int32 Axis = INDEX_NONE;
		TWeakObjectPtr<ALayoutBarrierActor> Target;
		FTransform Start;
		FVector PivotCm = FVector::ZeroVector, AxisDir = FVector::ZeroVector, StartVector = FVector::ZeroVector;
		double StartParamCm = 0.0;
		bool bScreenRotate = false;  // ray almost in the ring plane: use horizontal mouse motion instead
		FVector2D StartScreen = FVector2D::ZeroVector;
	} GizmoDrag;
	void OnGizmoTranslateKey();
	void OnGizmoCycleKey();
	void DrawLayoutGizmo() const;
	bool MouseRay(FVector& EyeCm, FVector& Direction, FVector2D& Screen) const;
 ECrahUnitKind CrahAdditionKind = ECrahUnitKind::InRow;
 void BeginCrahAdditionOfKind(ECrahUnitKind Kind);
	bool bRackAdditionHasFloor = false;
	FTransform RackAdditionTransform = FTransform::Identity;
	TArray<FLayoutRackMember> RackDuplicationMembers;
	TArray<FLayoutSpineMember> SpineDuplicationMembers; // Authored values only, no source actor/model references.
	TArray<FLayoutCduMember> CduDuplicationMembers; // Authored values, no actor/model references.
	TArray<FCrahLayoutRecord> CrahDuplicationMembers; // Authored settings/transforms only; fresh runtime provenance.
	// Capture Ctrl at the D press event, before a short chord's release can reach frame-based actions.
	bool bLayoutLeftControl = false;
	bool bLayoutRightControl = false;
	void UpdateRackAdditionFromCursor();
	void LogLayoutInputState(const TCHAR* Context) const;
	int32 OpenPatchMaps = 0;
	// W1 live leak tool (simulation mode, L): 0 off, 1 small hole, 2 30 mm, 3 rupture, 4 repair. The aimed site is the one
	// nearest to the cursor ray (drawn yellow); a click applies the mode to it (ADataCenterSimulationActor::SetLiveLeak).
	int32 LeakToolMode = 0;
	int32 LeakToolAimed = INDEX_NONE;
	int32 LeakToolKindCycle = 0;
	bool bLastCursorInputMode = false;
	void OnCycleLeakTool();
	void OnCycleLeakSiteKind();
	// Air display modes (V). Plan view moves the camera above the room and the next mode puts it back.
	void OnCycleAirView();
	FVector PlanSavedEyeCm = FVector::ZeroVector;
	FRotator PlanSavedLook = FRotator::ZeroRotator;
	bool bPlanViewSaved = false;
	void UpdateLeakTool();
	bool ApplyLeakToolClick();
	bool bInputModeInitialized = false;
	bool bLastLayoutInputMode = false;
	// UI tuning, not equipment/physical parameters: 3 m/s, wheel x1.25, limits 0.25..24 m/s.
	double LayoutCameraSpeedMs = 3.0;
	bool IsCameraInputAvailable() const;
	bool IsEquipmentInputAvailable() const { return !bRoomBoundsEditing && !bRoomCeilingEditing && IsCameraInputAvailable(); }
	void OnCameraWheelUp();
	void OnCameraWheelDown();
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> Actions;

	// 레벨에서 찾은 대상. 랙은 여러 대일 수 있고 손잡이는 전부에 같이 적용한다.
	UPROPERTY(Transient)
	TArray<TObjectPtr<ARackThermalActor>> Racks;

	UPROPERTY(Transient)
	TObjectPtr<ADataCenterRoomActor> Room;

	int32 SelectedSledIndex = INDEX_NONE;  // the unit under the crosshair
	float TimeSinceRefreshS = 0.0f;

	// The rack the camera looks at (updated every tick). Unit selection and E act on it only.
	TWeakObjectPtr<ARackThermalActor> TargetRack;
	ARackThermalActor* FindLookedAtRack(double& OutHitZM) const;
	void DrawTargetHighlight() const;
	bool bKnobsInitialized = false;

	// 키 하나를 InputAction으로 만들고 매핑에 넣는다.
	UInputAction* AddKeyAction(FName ActionName, const FKey& Key);

	void FindTargetActors();

	// 랙 수가 달라졌으면 대상을 다시 훑고 손잡이 범위를 다시 잡는다.
	void RefreshTargetsIfStale(float DeltaTime);

	// 첫 랙의 현재 값에서 손잡이의 시작값과 범위를 잡는다.
	void InitializeKnobsFromTargets();

	// 손잡이 값을 대상 액터에 반영한다. 손잡이를 돌린 직후에만 부른다.
	void ApplyHeatKnobs();
	void ApplyPumpSpeedKnob();
	void ApplyRadiatorKnob();
	void ApplyUtilizationKnob();

	void OnCpuHeatDown();
	void OnCpuHeatUp();
	void OnGpuHeatDown();
	void OnGpuHeatUp();
	void OnPumpSpeedDown();
	void OnPumpSpeedUp();
	void OnRadiatorDown();
	void OnRadiatorUp();
	void OnUtilizationDown();
	void OnUtilizationUp();
	void OnToggleSelectedSled();
	void OnToggleSimMode();
	void OnGrabOrDrop();
	void OnRotatePlaceable();
	void OnDeleteLayoutEquipment();
	void OnDuplicateLayoutRack();
	void OnCycleContainment();
	class ADataCenterSimulationActor* FindSimActor() const;
	bool IsLayoutMode() const;
	TWeakObjectPtr<AActor> Grabbed;
	FVector GrabOffsetCm = FVector::ZeroVector;  // actor origin minus the floor point it was grabbed at
	TWeakObjectPtr<AActor> HoveredPlaceable;
	TArray<TWeakObjectPtr<AActor>> LayoutSelection;
	void OnSelectLayoutEquipment();
	void OnReleaseLayoutEquipment();
	void OnCancelLayoutDrag();
	void UpdateLayoutDragFromCursor();
	void RefreshLayoutRowSnap();
	struct FLayoutDragMember
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform Start;
	};
	TArray<FLayoutDragMember> DragMembers;
	TWeakObjectPtr<AActor> DragAnchor;
	FTransform DragStartTransform;
	FVector DragStartFloorCm = FVector::ZeroVector;
	FVector DragPreviewCm = FVector::ZeroVector;
	FVector DragRawPreviewCm = FVector::ZeroVector;
	FQuat DragPreviewRotation = FQuat::Identity;
	bool bDragRowSnapped = false;
	bool bDragDisableRowSnap = false;
	FVector RowGuideStartCm, RowGuideEndCm;
	FVector2D DragStartMouse = FVector2D::ZeroVector;
	bool bDragMoved = false;
	bool bDragMouseThresholdPassed = false;
	bool bMarqueeActive = false;
	bool bMarqueeMoved = false;
	bool bMarqueeAdd = false;
	FVector2D MarqueeStart = FVector2D::ZeroVector;
	FVector2D MarqueeEnd = FVector2D::ZeroVector;
	void UpdateLayoutMarqueeFromCursor();
	int32 StartFrameAttempts = 0;
	FVector FramedEyeCm = FVector::ZeroVector;
	bool bFramedMoveLogged = false;
	bool TryFrameEquipment(const AActor* OnlyActor = nullptr, const TArray<AActor*>* OnlyActors = nullptr);
	void OnToggleHelp();

	void DrawHelp() const;
};
