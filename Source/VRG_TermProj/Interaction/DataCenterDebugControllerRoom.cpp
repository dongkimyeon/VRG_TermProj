// Room handle geometry is view-side UE cm; only authored room settings cross into SI metres.
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Simulation/Core/SimUnits.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"

namespace
{
	// Corner bits: X max=1, Y max=2. The opposite corner remains fixed, including on rejection.
	FVector RoomCornerCm(const FRoomAirDomainSettings& S, int32 Corner)
	{
		return SimUnits::MetersToUnreal(FVector(Corner & 1 ? S.MaxXYM.X : S.MinXYM.X,
			Corner & 2 ? S.MaxXYM.Y : S.MinXYM.Y, 0));
	}
	FString RoomGridDescription(const FAirGridParams& Grid, const FBox& Bounds)
	{
		return FString::Printf(TEXT("실제 범위 %s ~ %s m | 셀 %dx%dx%d = %d개 (%.6g m)"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString(), Grid.NumCellsX, Grid.NumCellsY, Grid.NumCellsZ,
			Grid.NumCellsX * Grid.NumCellsY * Grid.NumCellsZ, Grid.CellSizeM);
	}
}

bool ADataCenterDebugController::SetRoomBoundsEditing(bool bEnable)
{
	if (!bEnable)
	{
		EndRoomCornerDrag(false); bRoomBoundsEditing = false; RoomBoundsMessage.Reset(); return true;
	}
	if (bRoomBoundsEditing) return true;
	auto* Sim = FindSimActor();
	if (!Sim || !IsLayoutMode() || !IsCameraInputAvailable() || bAddingRack || IsLayoutDragging()
		|| bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	if (!Sim->GetRoomAirDomain().bManual)
	{
		RoomBoundsMessage = TEXT("방 설정에서 수동 모드를 먼저 적용하세요."); return false;
	}
	bRoomBoundsEditing = true; RoomDragPreview = Sim->GetRoomAirDomain();
	ValidateRoomBoundsPreview(); return true;
}

bool ADataCenterDebugController::GetRoomBoundsPreview(FRoomAirDomainSettings& Out, bool& bValid) const
{
	const auto* Sim = FindSimActor();
	if (!bRoomBoundsEditing || !IsLayoutMode() || !Sim || !Sim->GetRoomAirDomain().bManual) return false;
	Out = RoomDragCorner == INDEX_NONE ? Sim->GetRoomAirDomain() : RoomDragPreview;
	bValid = bRoomPreviewValid; return true;
}

int32 ADataCenterDebugController::PickRoomCorner(const FVector2D& Screen) const
{
	FRoomAirDomainSettings S; bool bValid;
	if (!GetRoomBoundsPreview(S, bValid) || !IsCameraInputAvailable() || Screen.ContainsNaN()) return INDEX_NONE;
	int32 Best = INDEX_NONE; double RadiusPx = 10.0; // Same UI pick radius as barrier gizmos, not physical tolerance.
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		FVector2D Projected;
		if (ProjectWorldLocationToScreen(RoomCornerCm(S, Corner), Projected))
		{
			const double Distance = FVector2D::Distance(Screen, Projected);
			if (Distance <= RadiusPx) { RadiusPx = Distance; Best = Corner; }
		}
	}
	return Best;
}

bool ADataCenterDebugController::BeginRoomCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction)
{
	auto* Sim = FindSimActor();
	FVector FloorCm;
	if (!bRoomBoundsEditing || !Sim || !Sim->GetRoomAirDomain().bManual || Corner < 0 || Corner > 3
		|| !IsLayoutMode() || !IsCameraInputAvailable() || IsLayoutDragging() || bAddingRack || bMarqueeActive
		|| Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !FloorPointSnapped(EyeCm, Direction.GetSafeNormal(), 0, 0, FloorCm)) return false;
	RoomDragBefore = RoomDragPreview = Sim->GetRoomAirDomain(); RoomDragTarget = Sim;
	RoomDragStartFloorCm = FloorCm; RoomDragCorner = Corner;
	bRoomMouseThresholdPassed = false; bRoomFloorValid = true;
	float X = 0, Y = 0; GetMousePosition(X, Y); RoomDragStartMouse = FVector2D(X, Y);
	ValidateRoomBoundsPreview(); return true;
}

void ADataCenterDebugController::ValidateRoomBoundsPreview()
{
	auto* Sim = FindSimActor(); FAirGridParams Grid; FBox Bounds(ForceInit); FString Reason;
	bRoomPreviewValid = Sim && Sim->ValidateRoomAirDomain(RoomDragPreview, Grid, Bounds, Reason);
	RoomBoundsMessage = bRoomPreviewValid ? RoomGridDescription(Grid, Bounds) : Reason;
	// Inclusion errors can still show the allocation plan. Resource errors must not report truncated cells.
	if (!bRoomPreviewValid)
		for (TActorIterator<AAirFieldDebugActor> It(GetWorld()); It; ++It)
		{
			FString GridReason;
			if (VrgRoomAirDomain::BuildGrid(RoomDragPreview, It->GridParams, Grid, Bounds, GridReason))
				RoomBoundsMessage += TEXT("\n") + RoomGridDescription(Grid, Bounds);
			break;
		}
}

void ADataCenterDebugController::UpdateRoomCornerDrag(const FVector& EyeCm, const FVector& Direction)
{
	if (RoomDragCorner == INDEX_NONE) return;
	if (!IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| RoomDragTarget.Get() != FindSimActor()) { EndRoomCornerDrag(false); return; }
	FVector FloorCm;
	if (!FloorPointSnapped(EyeCm, Direction.GetSafeNormal(), 0, 0, FloorCm))
	{ bRoomFloorValid = bRoomPreviewValid = false; RoomBoundsMessage = TEXT("커서가 바닥과 만나지 않아 확정할 수 없습니다."); return; }
	const bool bRecoveredFloor = !bRoomFloorValid; bRoomFloorValid = true;
	FVector Target = RoomCornerCm(RoomDragBefore, RoomDragCorner) + FloorCm - RoomDragStartFloorCm;
	if (PlaceSnapM > 0)
	{
		const double SnapCm = SimUnits::MetersToCentimeters(PlaceSnapM);
		Target.X = FMath::RoundToDouble(Target.X / SnapCm) * SnapCm;
		Target.Y = FMath::RoundToDouble(Target.Y / SnapCm) * SnapCm;
	}
	const FVector TargetM = SimUnits::UnrealToMeters(Target);
	auto Candidate = RoomDragBefore;
	(RoomDragCorner & 1 ? Candidate.MaxXYM.X : Candidate.MinXYM.X) = TargetM.X;
	(RoomDragCorner & 2 ? Candidate.MaxXYM.Y : Candidate.MinXYM.Y) = TargetM.Y;
	// Reuse the plan while stationary; a recovered ray still needs to clear its invalid-ray message.
	if (!(Candidate == RoomDragPreview) || bRecoveredFloor)
	{ RoomDragPreview = Candidate; ValidateRoomBoundsPreview(); } // No TrySet/Rebuild during preview.
}

void ADataCenterDebugController::UpdateRoomCornerFromCursor()
{
	if (RoomDragCorner == INDEX_NONE) return;
	FVector Eye, Dir; FVector2D Screen;
	if (!MouseRay(Eye, Dir, Screen) || !IsCameraInputAvailable()) { EndRoomCornerDrag(false); return; }
	bRoomMouseThresholdPassed |= FVector2D::Distance(RoomDragStartMouse, Screen) >= 4.0; // Existing UI drag threshold.
	if (bRoomMouseThresholdPassed) UpdateRoomCornerDrag(Eye, Dir);
}

bool ADataCenterDebugController::EndRoomCornerDrag(bool bCommit)
{
	if (RoomDragCorner == INDEX_NONE) return false;
	auto* Sim = RoomDragTarget.Get();
	const bool bAllowed = bCommit && bRoomPreviewValid && bRoomBoundsEditing && IsLayoutMode()
		&& IsCameraInputAvailable() && !IsInputKeyDown(EKeys::RightMouseButton) && Sim && Sim == FindSimActor();
	bool bOk = false;
	if (bAllowed)
	{
		if (!(Sim->GetRoomAirDomain() == RoomDragBefore)) RoomBoundsMessage = TEXT("드래그 중 방 설정이 바뀌어 확정을 거절했습니다.");
		else bOk = CommitRoomSettings(RoomDragPreview, RoomBoundsMessage);
	}
	RoomDragCorner = INDEX_NONE; RoomDragTarget.Reset();
	if (!bCommit) { RoomDragPreview = Sim ? Sim->GetRoomAirDomain() : RoomDragBefore; ValidateRoomBoundsPreview(); }
	return bOk;
}

bool ADataCenterDebugController::CommitRoomSettings(const FRoomAirDomainSettings& Settings, FString& Message)
{
	auto* Sim = FindSimActor(); if (!Sim || !IsLayoutMode()) return false;
	const auto Before = Sim->GetRoomAirDomain();
	if (!Sim->TrySetRoomAirDomain(Settings, Message)) return false;
	if (!(Before == Settings))
	{
		FLayoutTransformCommand Command; Command.bRoomSettings = true;
		Command.RoomSettingsTarget = Sim; Command.RoomBefore = Before; Command.RoomAfter = Settings;
		PushLayoutCommand(MoveTemp(Command));
	}
	Message = DescribeRoomSettingsPlan(Settings); return true;
}

void ADataCenterDebugController::DrawRoomBounds() const
{
	FRoomAirDomainSettings S; bool bValid;
	if (!GetRoomBoundsPreview(S, bValid) || !GetWorld() || !IsCameraInputAvailable()) return;
	const FColor Color = bValid ? FColor(255, 165, 0) : FColor::Red;
	// UI overlay; front depth avoids floor/equipment hiding handles. Physical bounds remain at Z=0.
	const int32 Order[] = {0, 1, 3, 2, 0};
	for (int32 I = 0; I < 4; ++I)
	{
		DrawDebugLine(GetWorld(), RoomCornerCm(S, Order[I]), RoomCornerCm(S, Order[I+1]), Color, false, 0, 1, 3);
		DrawDebugPoint(GetWorld(), RoomCornerCm(S, I), 16.f, Color, false, 0, 1); // UI pixels.
	}
}
