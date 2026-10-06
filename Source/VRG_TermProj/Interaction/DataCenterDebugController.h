// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/DebugKnob.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
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
	bool OpenRoomSettings();
	void CloseRoomSettings(URoomSettingsWidget* ExpectedWidget = nullptr);
	bool IsRoomSettingsOpen() const { return bRoomSettingsOpen; }
	FRoomAirDomainSettings GetRoomSettingsDraft() const;
	FString DescribeRoomSettingsPlan(const FRoomAirDomainSettings& Settings);
	bool ApplyRoomSettings(const FRoomAirDomainSettings& Settings, FString& Message);

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
	bool bShowHelp = true;

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
	// Nearest placeable whose box the ray enters within MaxDistanceCm, or null.
	static AActor* PickPlaceableAlongRay(UWorld* World, const FVector& EyeCm, const FVector& Dir, double MaxDistanceCm);
	// Where the ray meets the horizontal plane at PlaneZCm, snapped in X and Y to SnapM; false if it never does.
	static bool FloorPointSnapped(const FVector& EyeCm, const FVector& Dir, double PlaneZCm, double SnapM, FVector& OutCm);
	// Projected equipment boxes, with 1 cm clearance on a separating axis (UI placement rule).
	static bool IsLayoutPlacementBlocked(const AActor* Actor, const FTransform& Proposed, const TArray<AActor*>* Ignored = nullptr);
	static bool IsLayoutBoxBlocked(UWorld* World, const FVector& SizeM, const FTransform& Proposed, const TArray<AActor*>* Ignored = nullptr);
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
	void CancelRackAddition();
	void UpdateRackAdditionAlongRay(const FVector& EyeCm, const FVector& Direction);
	bool GetRackAdditionPreview(FTransform& Out, FVector& SizeM, int32 MemberIndex = 0) const;
	int32 GetRackAdditionPreviewCount() const { return bAddingRack ? FMath::Max(1, FMath::Max(CrahDuplicationMembers.Num(), FMath::Max(CduDuplicationMembers.Num(), FMath::Max(RackDuplicationMembers.Num(), SpineDuplicationMembers.Num())))) : 0; }
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
	bool IsLayoutDragging() const { return !DragMembers.IsEmpty(); }
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
	struct FLayoutTransformMember
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform Before;
		FTransform After;
		FString RackId;
		FString SpineId;
		FString CduId;
		FString CrahId;
	};
	struct FLayoutRackMember
	{
		FString Id;
		FName Name;
		TWeakObjectPtr<UClass> Class;
		FTransform Transform;
		FRackLayoutSettings Settings;
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
 struct FLayoutCrahContact { FName Name; TWeakObjectPtr<UClass> Class; FTransform Transform; FVector SizeM; };
 struct FLayoutCrahMember { FCrahLayoutRecord Record; TArray<FLayoutCrahContact> ExistingContacts; };
 struct FLayoutTransformCommand
	{
		bool bRoomSettings = false;
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
	bool bAddingRack = false; // Shared equipment preview activity; existing input guards use this flag.
	bool bAddingSpine = false;
	bool bAddingCdu = false;
 bool bAddingCrah = false;
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
	bool bInputModeInitialized = false;
	bool bLastLayoutInputMode = false;
	// UI tuning, not equipment/physical parameters: 3 m/s, wheel x1.25, limits 0.25..24 m/s.
	double LayoutCameraSpeedMs = 3.0;
	bool IsCameraInputAvailable() const;
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
