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
		if (Corner == ADataCenterDebugController::RoomHeightHandle)
			return SimUnits::MetersToUnreal(FVector((S.MinXYM.X+S.MaxXYM.X)*.5, (S.MinXYM.Y+S.MaxXYM.Y)*.5, S.CeilingHeightM));
		return SimUnits::MetersToUnreal(FVector(Corner & 1 ? S.MaxXYM.X : S.MinXYM.X,
			Corner & 2 ? S.MaxXYM.Y : S.MinXYM.Y, 0));
	}
	// Closest point on the ceiling-centre vertical axis to a forward view ray, in UE cm.
	// Same conditioning threshold1e-4 as the existing barrier gizmo's ClosestLineParam; UI geometry, not physics.
	bool RoomHeightAxisParam(const FRoomAirDomainSettings& S, const FVector& Eye, const FVector& Direction, double& HeightCm)
	{
		if (Eye.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
		const FVector D=Direction.GetSafeNormal(); FVector Pivot=RoomCornerCm(S,ADataCenterDebugController::RoomHeightHandle); Pivot.Z=0;
		const FVector W=Pivot-Eye; const double B=D.Z, Denom=1-B*B;
		if (Denom<1e-4) return false;
		const double E=FVector::DotProduct(D,W), T=(E-B*W.Z)/Denom;
		HeightCm=(B*E-W.Z)/Denom;
		return FMath::IsFinite(HeightCm) && FMath::IsFinite(T) && T>=0;
	}
	FString RoomGridDescription(const FAirGridParams& Grid, const FBox& Bounds)
	{
		return FString::Printf(TEXT("실제 범위 %s ~ %s m | 셀 %dx%dx%d = %d개 (%.6g m)"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString(), Grid.NumCellsX, Grid.NumCellsY, Grid.NumCellsZ,
			Grid.NumCellsX * Grid.NumCellsY * Grid.NumCellsZ, Grid.CellSizeM);
	}
	FRoomAirDomainSettings CeilingHandleSettings(const FRoomCeilingPlate& P)
	{
		FRoomAirDomainSettings S; S.MinXYM=P.MinXYM; S.MaxXYM=P.MaxXYM; S.CeilingHeightM=P.HeightM; return S;
	}
	FVector CeilingCornerCm(const FRoomCeilingPlate& P, int32 Corner)
	{
		const auto S=CeilingHandleSettings(P);
		auto Point=RoomCornerCm(S,Corner); Point.Z=SimUnits::MetersToCentimeters(P.HeightM); return Point;
	}
	double HeightTargetM(double BeforeM, double HeightCm, double StartHeightCm, double SnapM)
	{
		double TargetCm=SimUnits::MetersToCentimeters(BeforeM)+HeightCm-StartHeightCm;
		if(SnapM>0) { const double SnapCm=SimUnits::MetersToCentimeters(SnapM); TargetCm=FMath::RoundToDouble(TargetCm/SnapCm)*SnapCm; }
		return SimUnits::CentimetersToMeters(TargetCm);
	}
	// Shared room/plate corner delta and absolute XY snap; fixed opposite corner is never sorted/swapped.
	FVector CornerTargetM(const FVector& CornerCm, const FVector& PointCm, const FVector& StartCm, double SnapM)
	{
		FVector Target=CornerCm+PointCm-StartCm;
		if(SnapM>0)
		{
			const double SnapCm=SimUnits::MetersToCentimeters(SnapM);
			Target.X=FMath::RoundToDouble(Target.X/SnapCm)*SnapCm;
			Target.Y=FMath::RoundToDouble(Target.Y/SnapCm)*SnapCm;
		}
		return SimUnits::UnrealToMeters(Target);
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
	if (!Sim || bRoomCeilingEditing || !IsLayoutMode() || !IsCameraInputAvailable() || bAddingRack || IsLayoutDragging()
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
	for (int32 Corner = 0; Corner <= RoomHeightHandle; ++Corner)
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
	if (!bRoomBoundsEditing || !Sim || !Sim->GetRoomAirDomain().bManual || Corner < 0 || Corner > RoomHeightHandle
		|| !IsLayoutMode() || !IsCameraInputAvailable() || IsLayoutDragging() || bAddingRack || bMarqueeActive
		|| Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	if (Corner == RoomHeightHandle)
	{
		if (!RoomHeightAxisParam(Sim->GetRoomAirDomain(),EyeCm,Direction,RoomDragStartHeightCm))
		{ RoomBoundsMessage=TEXT("높이 축과 시선이 거의 평행하거나 교점이 시선 뒤에 있어 높이를 정할 수 없습니다. 카메라 각도를 바꾸세요."); return false; }
	}
	else if (!FloorPointSnapped(EyeCm,Direction.GetSafeNormal(),0,0,FloorCm)) return false;
	RoomDragBefore = RoomDragPreview = Sim->GetRoomAirDomain(); RoomDragTarget = Sim;
	if (Corner != RoomHeightHandle) RoomDragStartFloorCm = FloorCm;
	RoomDragCorner = Corner;
	bRoomMouseThresholdPassed = false; bRoomRayValid = true;
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
	if (RoomDragCorner == RoomHeightHandle)
	{
		double HeightCm;
		if (!RoomHeightAxisParam(RoomDragBefore,EyeCm,Direction,HeightCm))
		{ bRoomRayValid=bRoomPreviewValid=false; RoomBoundsMessage=TEXT("시선으로 높이를 정할 수 없어 확정할 수 없습니다. 카메라 각도를 바꾸세요."); return; }
		const bool bRecoveredRay=!bRoomRayValid; bRoomRayValid=true;
		auto Candidate=RoomDragBefore; Candidate.CeilingHeightM=HeightTargetM(RoomDragBefore.CeilingHeightM,HeightCm,RoomDragStartHeightCm,PlaceSnapM);
		if (!(Candidate==RoomDragPreview) || bRecoveredRay) { RoomDragPreview=Candidate; ValidateRoomBoundsPreview(); }
		return; // XY unchanged; same validation, commit/history and cursor cancellation as corner drags.
	}
	FVector FloorCm;
	if (!FloorPointSnapped(EyeCm, Direction.GetSafeNormal(), 0, 0, FloorCm))
	{ bRoomRayValid = bRoomPreviewValid = false; RoomBoundsMessage = TEXT("커서가 바닥과 만나지 않아 확정할 수 없습니다."); return; }
	const bool bRecoveredFloor = !bRoomRayValid; bRoomRayValid = true;
	const FVector TargetM = CornerTargetM(RoomCornerCm(RoomDragBefore,RoomDragCorner),FloorCm,RoomDragStartFloorCm,PlaceSnapM);
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

FString ADataCenterDebugController::DescribeRoomCeilingPlan(const FRoomCeilingPlate& P)
{
	auto* Sim=FindSimActor(); TArray<FRoomCeilingPlate> Plates;
	if(Sim) Plates=Sim->GetRoomCeilingPlates(); Plates.Add(P);
	return DescribeRoomCeilingCandidate(P,Plates);
}

FString ADataCenterDebugController::DescribeRoomCeilingCandidate(const FRoomCeilingPlate& P, const TArray<FRoomCeilingPlate>& Plates)
{
	auto* Sim=FindSimActor();
	FString Message=FString::Printf(TEXT("요청 XY %s ~ %s m · 높이 %.6g m\n"),*P.MinXYM.ToString(),*P.MaxXYM.ToString(),P.HeightM);
	if (!Sim) return Message+TEXT("통합 시뮬레이션 액터가 없습니다.");
	FAirGridParams GP; FBox Bounds(ForceInit); FString Reason; TArray<FRoomCeilingPlan> Plans;
	AAirFieldDebugActor* Air=nullptr;
	for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Air=*It; break; }
	if (!Air || !VrgRoomAirDomain::BuildGrid(Sim->GetRoomAirDomain(),Air->GridParams,GP,Bounds,Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),{P},GP,Bounds,Plans,Reason)) return Message+Reason;
	const auto& Plan=Plans[0]; const auto N=Plan.MaxCellExclusive-Plan.MinCell;
	Message+=FString::Printf(TEXT("계산 최소 %s m\n계산 최대 %s m\n두께 %.6g m · 셀 %dx%dx%d = %d개\n"),
		*Plan.ActualBoundsM.Min.ToString(),*Plan.ActualBoundsM.Max.ToString(),GP.CellSizeM,N.X,N.Y,N.Z,N.X*N.Y*N.Z);
	if (!Sim->ValidateRoomAirDomain(Sim->GetRoomAirDomain(),GP,Bounds,Reason,&Plates)) Message+=TEXT("거절: ")+Reason;
	return Message+TEXT("고체는 다음 시뮬레이션 진입에 적용 · 판 목록은 배치 저장에 포함.");
}

bool ADataCenterDebugController::AddRoomCeiling(const FRoomCeilingPlate& P, FString& Message)
{
	auto* Sim=FindSimActor();
	if (!bRoomSettingsOpen || !bRoomCeilingOpen || OpenPatchMaps>0 || !IsLayoutMode() || !Sim)
	{ Message=TEXT("배치 모드의 부분 천장 창에서 추가하세요."); return false; }
	Message=DescribeRoomCeilingPlan(P);
	const auto Before=Sim->GetRoomCeilingPlates(); auto Plates=Before; Plates.Add(P); FString Reason;
	if (!Sim->TrySetRoomCeilingPlates(Plates,Reason)) { Message+=TEXT("\n")+Reason; return false; }
	FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
	Command.CeilingsBefore=Before; Command.CeilingsAfter=Plates; PushLayoutCommand(MoveTemp(Command));
	ClearRoomCeilingSelection();
	return true;
}

void ADataCenterDebugController::ClearRoomCeilingSelection()
{
	EndRoomCeilingCornerDrag(false);
	SelectedRoomCeiling=INDEX_NONE; SelectedCeilingList.Reset(); SelectedCeilingTarget.Reset();
	CeilingEditMessage.Reset();
}

bool ADataCenterDebugController::SetRoomCeilingEditing(bool bEnable)
{
	if (!bEnable) { bRoomCeilingEditing=false; ClearRoomCeilingSelection(); return true; }
	auto* Sim=FindSimActor();
	if (!Sim || !GetWorld()->IsGameWorld() || !Sim->GetRoomAirDomain().bManual || !IsLayoutMode() || !IsCameraInputAvailable()
		|| bRoomBoundsEditing || bAddingRack || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	if (!bRoomCeilingEditing) { ClearRoomCeilingSelection(); LayoutSelection.Reset(); }
	bRoomCeilingEditing=true; return true;
}

int32 ADataCenterDebugController::GetSelectedRoomCeilingIndex() const
{
	auto* Sim=FindSimActor();
	return bRoomCeilingEditing && IsLayoutMode() && Sim && Sim==SelectedCeilingTarget.Get()
		&& Sim->GetRoomCeilingPlates()==SelectedCeilingList && SelectedCeilingList.IsValidIndex(SelectedRoomCeiling)
		? SelectedRoomCeiling : INDEX_NONE;
}

bool ADataCenterDebugController::SelectRoomCeilingAlongRay(const FVector& EyeCm, const FVector& Direction)
{
	if (!bRoomCeilingEditing || !IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| bAddingRack || bRoomBoundsEditing || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid()
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
	auto* Sim=FindSimActor(); AAirFieldDebugActor* Air=nullptr;
	for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Air=*It; break; }
	FAirGridParams GP; FBox Bounds(ForceInit); FString Reason; TArray<FRoomCeilingPlan> Plans;
	if (!Sim || !Air || !VrgRoomAirDomain::BuildGrid(Sim->GetRoomAirDomain(),Air->GridParams,GP,Bounds,Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),Sim->GetRoomCeilingPlates(),GP,Bounds,Plans,Reason)) return false;
	const FVector Eye=SimUnits::UnrealToMeters(EyeCm), Dir=Direction.GetSafeNormal();
	int32 Best=INDEX_NONE; double BestT=TNumericLimits<double>::Max();
	for(int32 I=0;I<Plans.Num();++I)
	{
		// Same forward slab test/parallel tolerance as equipment picking, here against computed metre bounds.
		const auto& B=Plans[I].ActualBoundsM; double Lo=0,Hi=BestT; bool bHit=true;
		for(int32 Axis=0;Axis<3 && bHit;++Axis)
		{
			if(FMath::Abs(Dir[Axis])<1e-9) { bHit=Eye[Axis]>=B.Min[Axis] && Eye[Axis]<=B.Max[Axis]; continue; }
			double A=(B.Min[Axis]-Eye[Axis])/Dir[Axis], Z=(B.Max[Axis]-Eye[Axis])/Dir[Axis]; if(A>Z) Swap(A,Z);
			Lo=FMath::Max(Lo,A); Hi=FMath::Min(Hi,Z); bHit=Lo<=Hi;
		}
		if(bHit && (Best==INDEX_NONE || Lo<BestT)) { Best=I; BestT=Lo; } // Exact ties keep first list entry.
	}
	ClearRoomCeilingSelection();
	if(Best==INDEX_NONE) return false; // Valid empty click clears; malformed/deprojection failure does not.
	SelectedRoomCeiling=Best; SelectedCeilingTarget=Sim; SelectedCeilingList=Sim->GetRoomCeilingPlates(); bCeilingPreviewValid=true; return true;
}

bool ADataCenterDebugController::TryDeleteRoomCeiling()
{
	if (!bRoomCeilingEditing || !IsLayoutMode() || !IsCameraInputAvailable() || bRoomBoundsEditing || bAddingRack
		|| IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const int32 Index=GetSelectedRoomCeilingIndex(); if(Index==INDEX_NONE) { ClearRoomCeilingSelection(); return false; }
	auto* Sim=FindSimActor(); const auto Before=Sim->GetRoomCeilingPlates(); auto After=Before; After.RemoveAt(Index);
	FString Reason;
	if(!Sim->TrySetRoomCeilingPlates(After,Reason))
	{
		UE_LOG(LogTemp,Display,TEXT("[VrgLayout] 천장 삭제 거절: %s"),*Reason);
		if(GEngine) GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID())+23,6,FColor::Red,Reason);
		return false;
	}
	FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
	Command.CeilingsBefore=Before; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
	ClearRoomCeilingSelection(); return true;
}

bool ADataCenterDebugController::GetRoomCeilingPreview(FRoomCeilingPlate& Out, bool& bValid) const
{
	const int32 Index=GetSelectedRoomCeilingIndex();
	if(Index==INDEX_NONE) return false;
	Out=CeilingDragCorner==INDEX_NONE ? SelectedCeilingList[Index] : CeilingDragPreview;
	bValid=bCeilingPreviewValid; return true;
}

int32 ADataCenterDebugController::PickRoomCeilingCorner(const FVector2D& Screen) const
{
	FRoomCeilingPlate P; bool bValid;
	if(!GetRoomCeilingPreview(P,bValid) || !IsCameraInputAvailable() || Screen.ContainsNaN()) return INDEX_NONE;
	int32 Best=INDEX_NONE; double RadiusPx=10; // Existing room/barrier UI pick radius, not physical tolerance.
	for(int32 I=0;I<=RoomHeightHandle;++I)
	{
		FVector2D XY; if(!ProjectWorldLocationToScreen(CeilingCornerCm(P,I),XY)) continue;
		const double Distance=FVector2D::Distance(Screen,XY);
		if(Distance<=RadiusPx) { RadiusPx=Distance; Best=I; }
	}
	return Best;
}

bool ADataCenterDebugController::BeginRoomCeilingCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction)
{
	const int32 Index=GetSelectedRoomCeilingIndex(); FVector Point=FVector::ZeroVector;
	if(Index==INDEX_NONE || Corner<0 || Corner>RoomHeightHandle || !IsCameraInputAvailable() || IsLayoutDragging()
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()
		|| bAddingRack || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	if(Corner==RoomHeightHandle)
	{
		if(!RoomHeightAxisParam(CeilingHandleSettings(SelectedCeilingList[Index]),EyeCm,Direction,CeilingDragStartHeightCm))
		{ CeilingEditMessage=TEXT("높이 축과 시선이 거의 평행하거나 교점이 시선 뒤에 있어 높이를 정할 수 없습니다. 카메라 각도를 바꾸세요."); return false; }
	}
	else if(!FloorPointSnapped(EyeCm,Direction.GetSafeNormal(),SimUnits::MetersToCentimeters(SelectedCeilingList[Index].HeightM),0,Point)) return false;
	CeilingDragPreview=SelectedCeilingList[Index]; CeilingDragRoom=FindSimActor()->GetRoomAirDomain();
	CeilingDragCorner=Corner; if(Corner!=RoomHeightHandle) CeilingDragStartCm=Point; bCeilingMouseThresholdPassed=false;
	float X=0,Y=0; GetMousePosition(X,Y); CeilingDragStartMouse=FVector2D(X,Y);
	ValidateRoomCeilingPreview(); return true;
}

void ADataCenterDebugController::ValidateRoomCeilingPreview()
{
	auto* Sim=FindSimActor(); const int32 Index=GetSelectedRoomCeilingIndex();
	if(!Sim || Index==INDEX_NONE) { bCeilingPreviewValid=false; return; }
	auto Plates=SelectedCeilingList; Plates[Index]=CeilingDragPreview;
	FAirGridParams Grid; FBox Bounds(ForceInit); FString Reason;
	bCeilingPreviewValid=Sim->ValidateRoomAirDomain(Sim->GetRoomAirDomain(),Grid,Bounds,Reason,&Plates);
	CeilingEditMessage=DescribeRoomCeilingCandidate(CeilingDragPreview,Plates);
	if(!bCeilingPreviewValid && !CeilingEditMessage.Contains(Reason)) CeilingEditMessage+=TEXT("\n거절: ")+Reason;
}

void ADataCenterDebugController::UpdateRoomCeilingCornerDrag(const FVector& EyeCm, const FVector& Direction)
{
	if(CeilingDragCorner==INDEX_NONE) return;
	auto* Sim=FindSimActor(); const int32 Index=GetSelectedRoomCeilingIndex();
	if(Index==INDEX_NONE || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !Sim || !(Sim->GetRoomAirDomain()==CeilingDragRoom)) { EndRoomCeilingCornerDrag(false); return; }
	if(CeilingDragCorner==RoomHeightHandle)
	{
		double HeightCm;
		if(!RoomHeightAxisParam(CeilingHandleSettings(SelectedCeilingList[Index]),EyeCm,Direction,HeightCm))
		{ bCeilingPreviewValid=false; CeilingEditMessage=TEXT("시선으로 높이를 정할 수 없어 확정할 수 없습니다. 카메라 각도를 바꾸세요."); return; }
		auto Candidate=SelectedCeilingList[Index];
		Candidate.HeightM=HeightTargetM(Candidate.HeightM,HeightCm,CeilingDragStartHeightCm,PlaceSnapM);
		CeilingDragPreview=Candidate; ValidateRoomCeilingPreview(); return; // XY exact; reuse all list/commit/cancel gates.
	}
	FVector Point;
	if(EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()
		|| !FloorPointSnapped(EyeCm,Direction.GetSafeNormal(),SimUnits::MetersToCentimeters(CeilingDragPreview.HeightM),0,Point))
	{ bCeilingPreviewValid=false; CeilingEditMessage=TEXT("커서가 판 높이 평면과 만나지 않아 확정할 수 없습니다."); return; }
	const auto Target=CornerTargetM(CeilingCornerCm(SelectedCeilingList[Index],CeilingDragCorner),Point,CeilingDragStartCm,PlaceSnapM);
	auto Candidate=SelectedCeilingList[Index];
	(CeilingDragCorner&1 ? Candidate.MaxXYM.X : Candidate.MinXYM.X)=Target.X;
	(CeilingDragCorner&2 ? Candidate.MaxXYM.Y : Candidate.MinXYM.Y)=Target.Y;
	// Validate again while stationary: equipment may have moved externally without changing the list.
	CeilingDragPreview=Candidate; ValidateRoomCeilingPreview();
}

void ADataCenterDebugController::UpdateRoomCeilingFromCursor()
{
	if(CeilingDragCorner==INDEX_NONE) return;
	FVector Eye,Dir; FVector2D Screen;
	if(!MouseRay(Eye,Dir,Screen) || !IsCameraInputAvailable()) { EndRoomCeilingCornerDrag(false); return; }
	bCeilingMouseThresholdPassed |= FVector2D::Distance(CeilingDragStartMouse,Screen)>=4; // Existing UI drag threshold.
	if(bCeilingMouseThresholdPassed) UpdateRoomCeilingCornerDrag(Eye,Dir);
}

bool ADataCenterDebugController::EndRoomCeilingCornerDrag(bool bCommit)
{
	if(CeilingDragCorner==INDEX_NONE) return false;
	const int32 Index=GetSelectedRoomCeilingIndex(); auto* Sim=FindSimActor(); bool bOk=false;
	if(bCommit && bCeilingPreviewValid && Index!=INDEX_NONE && Sim && Sim->GetRoomAirDomain()==CeilingDragRoom
		&& IsCameraInputAvailable() && !IsInputKeyDown(EKeys::RightMouseButton) && !(CeilingDragPreview==SelectedCeilingList[Index]))
	{
		auto After=SelectedCeilingList; After[Index]=CeilingDragPreview; FString Reason;
		if(Sim->TrySetRoomCeilingPlates(After,Reason))
		{
			FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
			Command.CeilingsBefore=SelectedCeilingList; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
			SelectedCeilingList=After; bOk=true; // Same index stays selected in our own successful replacement.
		}
		else { bCeilingPreviewValid=false; CeilingEditMessage=TEXT("거절: ")+Reason; }
	}
	CeilingDragCorner=INDEX_NONE;
	if(!bCommit) { CeilingEditMessage.Reset(); bCeilingPreviewValid=true; }
	return bOk;
}

void ADataCenterDebugController::DrawRoomCeilings() const
{
	auto* Sim=FindSimActor(); if (!Sim || !IsLayoutMode() || !IsCameraInputAvailable()) return;
	AAirFieldDebugActor* Air=nullptr; for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Air=*It; break; }
	if (!Air || Sim->GetRoomCeilingPlates().IsEmpty()) return;
	FAirGridParams GP; FBox Bounds(ForceInit); FString Reason; TArray<FRoomCeilingPlan> Plans;
	if (!VrgRoomAirDomain::BuildGrid(Sim->GetRoomAirDomain(),Air->GridParams,GP,Bounds,Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),Sim->GetRoomCeilingPlates(),GP,Bounds,Plans,Reason)) return;
	for(int32 I=0;I<Plans.Num();++I)
	{
		const auto& P=Sim->GetRoomCeilingPlates()[I]; const auto& B=Plans[I].ActualBoundsM;
		DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(B.GetCenter()),SimUnits::MetersToUnreal(B.GetExtent()),FColor(255,165,0),false,-1,0,1);
		const FVector V[]={FVector(P.MinXYM.X,P.MinXYM.Y,P.HeightM),FVector(P.MaxXYM.X,P.MinXYM.Y,P.HeightM),
			FVector(P.MaxXYM.X,P.MaxXYM.Y,P.HeightM),FVector(P.MinXYM.X,P.MaxXYM.Y,P.HeightM)};
		for(int32 J=0;J<4;++J) DrawDebugLine(GetWorld(),SimUnits::MetersToUnreal(V[J]),SimUnits::MetersToUnreal(V[(J+1)%4]),FColor::Cyan,false,-1,0,1);
	}
	const int32 Selected=GetSelectedRoomCeilingIndex();
	if(Plans.IsValidIndex(Selected))
	{
		const auto& B=Plans[Selected].ActualBoundsM;
		// Selected box last/thicker, so identical unselected plates do not hide the highlight. Display only.
		DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(B.GetCenter()),SimUnits::MetersToUnreal(B.GetExtent()),FColor::Green,false,-1,0,3);
		const auto& P=CeilingDragCorner==INDEX_NONE ? Sim->GetRoomCeilingPlates()[Selected] : CeilingDragPreview;
		const FColor Color=CeilingDragCorner==INDEX_NONE ? FColor::Green : bCeilingPreviewValid ? FColor(255,165,0) : FColor::Red;
		const int32 Order[]={0,1,3,2,0};
		for(int32 I=0;I<4;++I)
		{
			DrawDebugLine(GetWorld(),CeilingCornerCm(P,Order[I]),CeilingCornerCm(P,Order[I+1]),Color,false,0,1,3);
			DrawDebugPoint(GetWorld(),CeilingCornerCm(P,I),16,Color,false,0,1); // Existing room handle UI pixels.
		}
		const FVector Top=CeilingCornerCm(P,RoomHeightHandle), Bottom(Top.X,Top.Y,0);
		DrawDebugDirectionalArrow(GetWorld(),Bottom,Top,20.f,Color,false,0,1,2.f); // Existing room UI arrow20cm, not geometry.
		DrawDebugPoint(GetWorld(),Top,16,Color,false,0,1);
		TArray<FRoomCeilingPlan> PreviewPlans;
		if(CeilingDragCorner!=INDEX_NONE && VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),{P},GP,Bounds,PreviewPlans,Reason))
		{
			const auto& Actual=PreviewPlans[0].ActualBoundsM;
			DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(Actual.GetCenter()),SimUnits::MetersToUnreal(Actual.GetExtent()),Color,false,0,1,3);
		}
	}
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
	const FVector Top=RoomCornerCm(S,RoomHeightHandle), Bottom(Top.X,Top.Y,0);
	DrawDebugDirectionalArrow(GetWorld(),Bottom,Top,20.f,Color,false,0,1,2.f); // UI arrow20cm, not physical geometry.
	DrawDebugPoint(GetWorld(),Top,16.f,Color,false,0,1);
	for (int32 I=0;I<4;++I)
	{
		FVector A=RoomCornerCm(S,Order[I]), B=RoomCornerCm(S,Order[I+1]); A.Z=B.Z=Top.Z;
		DrawDebugLine(GetWorld(),A,B,Color,false,0,1,2.f); // Ceiling range guide only; no solid plate.
	}
}
