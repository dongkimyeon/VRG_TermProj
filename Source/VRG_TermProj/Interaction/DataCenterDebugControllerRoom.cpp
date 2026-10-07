// Room handle geometry is view-side UE cm; only authored room settings cross into SI metres.
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Simulation/Core/SimUnits.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"

namespace
{
	// Same forward slab/parallel tolerance as equipment picking. Keep every hit, including exact duplicates.
	TArray<int32> CeilingRayHits(const TArray<FBox>& Boxes, const FVector& EyeCm, const FVector& Direction)
	{
		const FVector Eye=SimUnits::UnrealToMeters(EyeCm),Dir=Direction.GetSafeNormal();
		TArray<TPair<int32,double>> Hits;
		for(int32 I=0;I<Boxes.Num();++I)
		{
			const auto& B=Boxes[I]; double Lo=0,Hi=TNumericLimits<double>::Max(); bool bHit=true;
			for(int32 Axis=0;Axis<3 && bHit;++Axis)
			{
				if(FMath::Abs(Dir[Axis])<1e-9) { bHit=Eye[Axis]>=B.Min[Axis] && Eye[Axis]<=B.Max[Axis]; continue; }
				double A=(B.Min[Axis]-Eye[Axis])/Dir[Axis],Z=(B.Max[Axis]-Eye[Axis])/Dir[Axis]; if(A>Z) Swap(A,Z);
				Lo=FMath::Max(Lo,A); Hi=FMath::Min(Hi,Z); bHit=Lo<=Hi;
			}
			if(bHit) Hits.Emplace(I,Lo);
		}
		Hits.Sort([](const auto& A,const auto& B) { return A.Value==B.Value ? A.Key<B.Key : A.Value<B.Value; });
		TArray<int32> Result; for(const auto& Hit:Hits) Result.Add(Hit.Key); return Result;
	}
	int32 CeilingRaySelection(const TArray<int32>& Hits,int32 Current,bool bCycle,bool bKeepSelected)
	{
		if(Hits.IsEmpty()) return INDEX_NONE;
		const int32 Position=Hits.Find(Current);
		return Position==INDEX_NONE ? Hits[0] : bCycle ? Hits[(Position+1)%Hits.Num()] : bKeepSelected ? Current : Hits[0];
	}
	void SetCeilingSelection(TArray<int32>& Selection,int32 Hit,bool bToggle,int32& Primary)
	{
		if(Hit==INDEX_NONE) Selection.Reset();
		else if(bToggle) { if(Selection.Contains(Hit)) Selection.Remove(Hit); else Selection.Add(Hit); }
		else Selection={Hit};
		Primary=Selection.IsEmpty() ? INDEX_NONE : Selection.Last();
	}
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
	FRoomCeilingPlate HoleHandlePlate(const FRoomCeilingPlate& P, int32 Hole)
	{
		FRoomCeilingPlate Handle; Handle.MinXYM=P.MinXYM+P.Holes[Hole].LocalMinXYM;
		Handle.MaxXYM=P.MinXYM+P.Holes[Hole].LocalMaxXYM; Handle.HeightM=P.HeightM; return Handle;
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
	if (!Plan.OpenCells.IsEmpty())
	{
		int32 OpenCount=0;
		for(int32 Y=Plan.MinCell.Y;Y<Plan.MaxCellExclusive.Y;++Y) for(int32 X=Plan.MinCell.X;X<Plan.MaxCellExclusive.X;++X)
			if(VrgRoomAirDomain::IsCeilingCellOpen(Plan,X,Y)) ++OpenCount;
		Message+=FString::Printf(TEXT("해당 판 제외 %d셀 · 남은 고체 %d셀 (다른 고체 유지)\n"),OpenCount,N.X*N.Y*N.Z-OpenCount);
		for(int32 I=0;I<Plan.OpenCells.Num();++I)
		{
			const auto& Open=Plan.OpenCells[I];
			const FVector Min=Bounds.Min+FVector(Open.Min.X,Open.Min.Y,Plan.MinCell.Z)*GP.CellSizeM;
			const FVector Max=Bounds.Min+FVector(Open.Max.X,Open.Max.Y,Plan.MaxCellExclusive.Z)*GP.CellSizeM;
			Message+=FString::Printf(TEXT("구멍 #%d 개방 계산 %s ~ %s m · %d셀\n"),I+1,*Min.ToString(),*Max.ToString(),(Open.Max.X-Open.Min.X)*(Open.Max.Y-Open.Min.Y));
		}
	}
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

bool ADataCenterDebugController::AddRoomCeilingHole(const FRoomCeilingHole& Hole, FString& Message)
{
	const int32 Index=GetSelectedRoomCeilingIndex(); auto* Sim=FindSimActor();
	if (!bRoomSettingsOpen || !bRoomCeilingHoleOpen || !bRoomCeilingEditing || OpenPatchMaps>0 || !IsLayoutMode() || !Sim || IsLayoutDragging())
	{ Message=TEXT("천장 편집의 선택 판 구멍 추가 창에서만 추가할 수 있습니다."); return false; }
	if (Index==INDEX_NONE || GetSelectedRoomCeilingIndices().Num()!=1)
	{ Message=TEXT("선택 판 또는 목록이 변경되었습니다. 창을 닫고 판을 다시 선택하세요."); return false; }
	const auto Before=SelectedCeilingList; auto After=Before; auto& P=After[Index]; P.Holes.Add(Hole);
	const FVector2D WorldMin=P.MinXYM+Hole.LocalMinXYM,WorldMax=P.MinXYM+Hole.LocalMaxXYM;
	Message=FString::Printf(TEXT("구멍 로컬 XY %s ~ %s m\n요청 월드 XY %s ~ %s m · 판 높이 %.6g m\n"),
		*Hole.LocalMinXYM.ToString(),*Hole.LocalMaxXYM.ToString(),*WorldMin.ToString(),*WorldMax.ToString(),P.HeightM);
	Message+=DescribeRoomCeilingCandidate(P,After);
	FString Reason;
	if (!Sim->TrySetRoomCeilingPlates(After,Reason)) { Message+=TEXT("\n거절: ")+Reason; return false; }
	FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
	Command.CeilingsBefore=Before; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
	SelectedRoomCeilingHole=INDEX_NONE; SelectedRoomCeilingHoles.Reset();
	SelectedCeilingList=MoveTemp(After); // Same plate can receive another confirmed hole; stale external lists cannot.
	Message+=TEXT("\n개방은 해당 판에서만 제외하는 계획입니다. 다른 판·벽·장비·콜드아일 지붕이 겹치면 통로는 막힐 수 있습니다.");
	return true;
}

void ADataCenterDebugController::ClearRoomCeilingSelection()
{
	SetRoomCeilingHoleEditing(false);
	EndRoomCeilingCornerDrag(false);
	SelectedRoomCeiling=INDEX_NONE; SelectedRoomCeilings.Reset(); SelectedCeilingList.Reset(); SelectedCeilingTarget.Reset();
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

TArray<int32> ADataCenterDebugController::GetSelectedRoomCeilingIndices() const
{
	return GetSelectedRoomCeilingIndex()==INDEX_NONE ? TArray<int32>{} : SelectedRoomCeilings;
}

bool ADataCenterDebugController::SelectRoomCeilingAlongRay(const FVector& EyeCm, const FVector& Direction, bool bCycle, bool bKeepSelected, bool bToggle)
{
	if(bRoomCeilingHoleEditing) return false;
	if (!bRoomCeilingEditing || !IsLayoutMode() || !IsCameraInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| bAddingRack || bRoomBoundsEditing || IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid()
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
	auto* Sim=FindSimActor(); AAirFieldDebugActor* Air=nullptr;
	for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Air=*It; break; }
	FAirGridParams GP; FBox Bounds(ForceInit); FString Reason; TArray<FRoomCeilingPlan> Plans;
	if (!Sim || !Air || !VrgRoomAirDomain::BuildGrid(Sim->GetRoomAirDomain(),Air->GridParams,GP,Bounds,Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),Sim->GetRoomCeilingPlates(),GP,Bounds,Plans,Reason)) return false;
	TArray<FBox> Boxes; for(const auto& Plan:Plans) Boxes.Add(Plan.ActualBoundsM);
	const auto Hits=CeilingRayHits(Boxes,EyeCm,Direction);
	const int32 Best=CeilingRaySelection(Hits,GetSelectedRoomCeilingIndex(),bCycle,bKeepSelected);
	if(!bToggle || GetSelectedRoomCeilingIndex()==INDEX_NONE) ClearRoomCeilingSelection();
	if(Best==INDEX_NONE) { ClearRoomCeilingSelection(); return false; }
	if(!bKeepSelected || !SelectedRoomCeilings.Contains(Best)) SetCeilingSelection(SelectedRoomCeilings,Best,bKeepSelected?false:bToggle,SelectedRoomCeiling);
	else SelectedRoomCeiling=Best; // The clicked selected item anchors a group without changing its membership.
	SelectedCeilingTarget=Sim; SelectedCeilingList=Sim->GetRoomCeilingPlates(); bCeilingPreviewValid=true;
	CeilingEditMessage=FString::Printf(TEXT("판 #%d 클릭 · 겹침 순서 %d/%d · 선택 %d개 · Alt 순환 / Shift 추가·제거"),Best+1,Hits.Find(Best)+1,Hits.Num(),SelectedRoomCeilings.Num()); return true;
}

bool ADataCenterDebugController::TryDeleteRoomCeiling()
{
	if(bRoomCeilingHoleEditing) return false;
	if (!bRoomCeilingEditing || !IsLayoutMode() || !IsCameraInputAvailable() || bRoomBoundsEditing || bAddingRack
		|| IsLayoutDragging() || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	const int32 Index=GetSelectedRoomCeilingIndex(); if(Index==INDEX_NONE) { ClearRoomCeilingSelection(); return false; }
	auto* Sim=FindSimActor(); const auto Before=Sim->GetRoomCeilingPlates(); auto After=Before; auto Selected=GetSelectedRoomCeilingIndices(); Selected.Sort();
	for(int32 I=Selected.Num()-1;I>=0;--I) After.RemoveAt(Selected[I]);
	FString Reason;
	if(!Sim->TrySetRoomCeilingPlates(After,Reason))
	{
		bCeilingPreviewValid=false; CeilingEditMessage=TEXT("천장 삭제 거절: ")+Reason;
		UE_LOG(LogTemp,Display,TEXT("[VrgLayout] 천장 삭제 거절: %s"),*Reason);
		if(GEngine) GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID())+23,6,FColor::Red,Reason);
		return false;
	}
	FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
	Command.CeilingsBefore=Before; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
	ClearRoomCeilingSelection(); return true;
}

bool ADataCenterDebugController::SetRoomCeilingHoleEditing(bool bEnable)
{
	if(!bEnable) { SelectedRoomCeilingHoles.Reset(); if(CeilingDragHole!=INDEX_NONE) EndRoomCeilingCornerDrag(false); bRoomCeilingHoleEditing=false; SelectedRoomCeilingHole=INDEX_NONE; CeilingEditMessage.Reset(); return true; }
	const int32 Plate=GetSelectedRoomCeilingIndex();
	if(Plate==INDEX_NONE || GetSelectedRoomCeilingIndices().Num()!=1 || !IsCameraInputAvailable() || IsLayoutDragging() || bAddingRack || bMarqueeActive || Grabbed.IsValid()
		|| IsInputKeyDown(EKeys::RightMouseButton) || SelectedCeilingList[Plate].Holes.IsEmpty()) return false;
	bRoomCeilingHoleEditing=true; SelectedRoomCeilingHole=INDEX_NONE; SelectedRoomCeilingHoles.Reset();
	CeilingEditMessage=TEXT("구멍 편집: 보라색 계산 개방 범위를 클릭 · Shift 추가·제거 · Delete 선택 삭제 · Esc 구멍 편집 종료. 다른 고체가 겹치면 통로는 막힐 수 있습니다.");
	return true;
}

int32 ADataCenterDebugController::GetSelectedRoomCeilingHoleIndex() const
{
	const int32 Plate=GetSelectedRoomCeilingIndex();
	// The parent captures the complete value list: any external change invalidates this index too.
	return bRoomCeilingHoleEditing && GetSelectedRoomCeilingIndices().Num()==1 && Plate!=INDEX_NONE && SelectedCeilingList[Plate].Holes.IsValidIndex(SelectedRoomCeilingHole)
		? SelectedRoomCeilingHole : INDEX_NONE;
}

TArray<int32> ADataCenterDebugController::GetSelectedRoomCeilingHoleIndices() const
{
	return GetSelectedRoomCeilingHoleIndex()==INDEX_NONE ? TArray<int32>{} : SelectedRoomCeilingHoles;
}

bool ADataCenterDebugController::SelectRoomCeilingHoleAlongRay(const FVector& EyeCm, const FVector& Direction, bool bCycle, bool bKeepSelected, bool bToggle)
{
	const int32 Plate=GetSelectedRoomCeilingIndex();
	if(!bRoomCeilingHoleEditing || Plate==INDEX_NONE || !IsCameraInputAvailable() || IsLayoutDragging() || bAddingRack || bMarqueeActive
		|| Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton) || EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
	auto* Sim=FindSimActor(); AAirFieldDebugActor* Air=nullptr;
	for(TActorIterator<AAirFieldDebugActor> It(GetWorld());It;++It) { Air=*It; break; }
	FAirGridParams GP; FBox Bounds(ForceInit); FString Reason; TArray<FRoomCeilingPlan> Plans;
	if(!Air || !VrgRoomAirDomain::BuildGrid(Sim->GetRoomAirDomain(),Air->GridParams,GP,Bounds,Reason)
		|| !VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),SelectedCeilingList,GP,Bounds,Plans,Reason)) return false;
	const auto& Plan=Plans[Plate]; TArray<FBox> Boxes;
	for(const auto& O:Plan.OpenCells) Boxes.Emplace(Bounds.Min+FVector(O.Min.X,O.Min.Y,Plan.MinCell.Z)*GP.CellSizeM,
		Bounds.Min+FVector(O.Max.X,O.Max.Y,Plan.MaxCellExclusive.Z)*GP.CellSizeM);
	const auto Hits=CeilingRayHits(Boxes,EyeCm,Direction);
	const int32 Best=CeilingRaySelection(Hits,GetSelectedRoomCeilingHoleIndex(),bCycle,bKeepSelected);
	if(Best==INDEX_NONE || !bKeepSelected || !SelectedRoomCeilingHoles.Contains(Best)) SetCeilingSelection(SelectedRoomCeilingHoles,Best,bKeepSelected?false:bToggle,SelectedRoomCeilingHole);
	bCeilingPreviewValid=true;
	CeilingEditMessage=Best==INDEX_NONE ? TEXT("구멍 선택 해제 · Esc 구멍 편집 종료")
		: FString::Printf(TEXT("구멍 #%d 클릭 · 겹침 순서 %d/%d · 선택 %d개 · Alt 순환 / Shift 추가·제거\n%s"),Best+1,Hits.Find(Best)+1,Hits.Num(),SelectedRoomCeilingHoles.Num(),*DescribeRoomCeilingCandidate(SelectedCeilingList[Plate],SelectedCeilingList));
	return Best!=INDEX_NONE;
}

bool ADataCenterDebugController::TryDeleteRoomCeilingHole()
{
	const int32 Hole=GetSelectedRoomCeilingHoleIndex(),Plate=GetSelectedRoomCeilingIndex();
	if(Hole==INDEX_NONE || !IsCameraInputAvailable() || IsLayoutDragging() || bAddingRack || bMarqueeActive || Grabbed.IsValid()
		|| IsInputKeyDown(EKeys::RightMouseButton)) return false;
	auto* Sim=FindSimActor(); const auto Before=SelectedCeilingList; auto After=Before; auto Selected=GetSelectedRoomCeilingHoleIndices(); Selected.Sort();
	for(int32 I=Selected.Num()-1;I>=0;--I) After[Plate].Holes.RemoveAt(Selected[I]);
	FString Reason;
	if(!Sim->TrySetRoomCeilingPlates(After,Reason))
	{
		bCeilingPreviewValid=false; CeilingEditMessage=TEXT("구멍 삭제 거절: ")+Reason;
		UE_LOG(LogTemp,Display,TEXT("[VrgLayout] %s"),*CeilingEditMessage); return false;
	}
	FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
	Command.CeilingsBefore=Before; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
	SelectedCeilingList=MoveTemp(After); SelectedRoomCeilingHole=INDEX_NONE; SelectedRoomCeilingHoles.Reset(); bCeilingPreviewValid=true;
	CeilingEditMessage=TEXT("선택 구멍 전체 삭제 완료 · 실제 셀은 다음 시뮬레이션 진입 때 적용 · Esc 구멍 편집 종료");
	return true;
}

bool ADataCenterDebugController::GetRoomCeilingPreview(FRoomCeilingPlate& Out, bool& bValid) const
{
	const int32 Index=GetSelectedRoomCeilingIndex();
	if(Index==INDEX_NONE) return false;
	Out=CeilingDragCorner==INDEX_NONE ? SelectedCeilingList[Index] : CeilingDragPreview;
	bValid=bCeilingPreviewValid; return true;
}

bool ADataCenterDebugController::GetRoomCeilingListPreview(TArray<FRoomCeilingPlate>& Out,bool& bValid) const
{
	if(GetSelectedRoomCeilingIndex()==INDEX_NONE) return false;
	Out=CeilingDragCorner==INDEX_NONE ? SelectedCeilingList : CeilingDragPlatesPreview;
	bValid=bCeilingPreviewValid; return true;
}

int32 ADataCenterDebugController::PickRoomCeilingCorner(const FVector2D& Screen) const
{
	if(bRoomCeilingHoleEditing || GetSelectedRoomCeilingIndices().Num()!=1) return INDEX_NONE;
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

int32 ADataCenterDebugController::PickRoomCeilingHoleCorner(const FVector2D& Screen) const
{
	const int32 Hole=GetSelectedRoomCeilingHoleIndex(); FRoomCeilingPlate P; bool bValid;
	if(Hole==INDEX_NONE || GetSelectedRoomCeilingHoleIndices().Num()!=1 || !GetRoomCeilingPreview(P,bValid) || !IsCameraInputAvailable() || Screen.ContainsNaN()) return INDEX_NONE;
	const auto Handle=HoleHandlePlate(P,Hole); int32 Best=INDEX_NONE; double RadiusPx=10; // Existing handle UI radius.
	for(int32 I=0;I<4;++I)
	{
		FVector2D XY; if(!ProjectWorldLocationToScreen(CeilingCornerCm(Handle,I),XY)) continue;
		const double Distance=FVector2D::Distance(Screen,XY); if(Distance<=RadiusPx) { RadiusPx=Distance; Best=I; }
	}
	return Best;
}

bool ADataCenterDebugController::BeginRoomCeilingHolePointerDrag(int32 PickedHandle, const FVector& EyeCm, const FVector& Direction)
{
	// A picked hole handle owns an invalid ray too; never reselect a duplicate or move the parent.
	if(PickedHandle!=INDEX_NONE) return BeginRoomCeilingHoleCornerDrag(PickedHandle,EyeCm,Direction);
	const bool bMultiple=GetSelectedRoomCeilingHoleIndices().Num()>1;
	if(!SelectRoomCeilingHoleAlongRay(EyeCm,Direction,false,true,bMultiple)) return false;
	return !bMultiple && BeginRoomCeilingHoleMoveDrag(EyeCm,Direction);
}

bool ADataCenterDebugController::BeginRoomCeilingHoleMoveDrag(const FVector& EyeCm, const FVector& Direction)
{
	return GetSelectedRoomCeilingHoleIndex()!=INDEX_NONE && BeginRoomCeilingDrag(CeilingMoveHandle,true,EyeCm,Direction);
}

bool ADataCenterDebugController::BeginRoomCeilingHoleCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction)
{
	if(GetSelectedRoomCeilingHoleIndex()==INDEX_NONE || Corner<0 || Corner>3) return false;
	return BeginRoomCeilingDrag(Corner,true,EyeCm,Direction);
}

bool ADataCenterDebugController::BeginRoomCeilingCornerDrag(int32 Corner, const FVector& EyeCm, const FVector& Direction)
{
	return !bRoomCeilingHoleEditing && BeginRoomCeilingDrag(Corner,false,EyeCm,Direction);
}

bool ADataCenterDebugController::BeginRoomCeilingDrag(int32 Corner, bool bHole, const FVector& EyeCm, const FVector& Direction)
{
	const int32 Index=GetSelectedRoomCeilingIndex(); FVector Point=FVector::ZeroVector;
	if(Index==INDEX_NONE || (GetSelectedRoomCeilingIndices().Num()!=1 && (bHole || Corner!=CeilingMoveHandle)) || (bHole && GetSelectedRoomCeilingHoleIndices().Num()!=1) || Corner<0 || Corner>CeilingMoveHandle || !IsCameraInputAvailable() || IsLayoutDragging()
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()
		|| bAddingRack || bMarqueeActive || Grabbed.IsValid() || IsInputKeyDown(EKeys::RightMouseButton)) return false;
	if(Corner==RoomHeightHandle)
	{
		if(!RoomHeightAxisParam(CeilingHandleSettings(SelectedCeilingList[Index]),EyeCm,Direction,CeilingDragStartHeightCm))
		{ CeilingEditMessage=TEXT("높이 축과 시선이 거의 평행하거나 교점이 시선 뒤에 있어 높이를 정할 수 없습니다. 카메라 각도를 바꾸세요."); return false; }
	}
	else if(!FloorPointSnapped(EyeCm,Direction.GetSafeNormal(),SimUnits::MetersToCentimeters(SelectedCeilingList[Index].HeightM),0,Point)) return false;
	CeilingDragPlatesPreview=SelectedCeilingList; CeilingDragItems=bHole ? GetSelectedRoomCeilingHoleIndices() : GetSelectedRoomCeilingIndices();
	CeilingDragPreview=SelectedCeilingList[Index]; CeilingDragRoom=FindSimActor()->GetRoomAirDomain();
	CeilingDragHole=bHole ? GetSelectedRoomCeilingHoleIndex() : INDEX_NONE;
	CeilingDragCorner=Corner; if(Corner!=RoomHeightHandle) CeilingDragStartCm=Point; bCeilingMouseThresholdPassed=false;
	float X=0,Y=0; GetMousePosition(X,Y); CeilingDragStartMouse=FVector2D(X,Y);
	ValidateRoomCeilingPreview(); return true;
}

bool ADataCenterDebugController::BeginRoomCeilingPointerDrag(int32 PickedHandle, const FVector& EyeCm, const FVector& Direction)
{
	// A picked handle owns the press even if its ray is invalid. Never fall through to body movement.
	if(PickedHandle!=INDEX_NONE) return BeginRoomCeilingCornerDrag(PickedHandle,EyeCm,Direction);
	const bool bMultiple=GetSelectedRoomCeilingIndices().Num()>1;
	if(!SelectRoomCeilingAlongRay(EyeCm,Direction,false,true,bMultiple)) return false;
	return BeginRoomCeilingMoveDrag(EyeCm,Direction);
}

void ADataCenterDebugController::ValidateRoomCeilingPreview()
{
	auto* Sim=FindSimActor(); const int32 Index=GetSelectedRoomCeilingIndex();
	if(!Sim || Index==INDEX_NONE) { bCeilingPreviewValid=false; return; }
	const auto& Plates=CeilingDragPlatesPreview;
	FAirGridParams Grid; FBox Bounds(ForceInit); FString Reason;
	bCeilingPreviewValid=Sim->ValidateRoomAirDomain(Sim->GetRoomAirDomain(),Grid,Bounds,Reason,&Plates);
	CeilingEditMessage=DescribeRoomCeilingCandidate(CeilingDragPreview,Plates);
	if(CeilingDragItems.Num()>1 && CeilingDragHole==INDEX_NONE)
	{
		CeilingEditMessage=FString::Printf(TEXT("판 그룹 %d개 · 기준 판 #%d · 다음은 기준 판 계산 범위\n"),CeilingDragItems.Num(),Index+1)+CeilingEditMessage;
		for(int32 I:CeilingDragItems) { const auto& P=Plates[I]; CeilingEditMessage+=FString::Printf(TEXT("\n판 #%d 요청 XY %s ~ %s m · 높이 %.6g m"),I+1,*P.MinXYM.ToString(),*P.MaxXYM.ToString(),P.HeightM); }
	}
	if(CeilingDragHole!=INDEX_NONE)
	{
		const auto& H=CeilingDragPreview.Holes[CeilingDragHole];
		CeilingEditMessage=FString::Printf(TEXT("선택 구멍 #%d 요청 로컬 %s ~ %s m · 요청 월드 %s ~ %s m\n"),CeilingDragHole+1,
			*H.LocalMinXYM.ToString(),*H.LocalMaxXYM.ToString(),*(CeilingDragPreview.MinXYM+H.LocalMinXYM).ToString(),*(CeilingDragPreview.MinXYM+H.LocalMaxXYM).ToString())+CeilingEditMessage;
	}
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
		CeilingDragPlatesPreview=SelectedCeilingList; CeilingDragPlatesPreview[Index]=Candidate;
		CeilingDragPreview=Candidate; ValidateRoomCeilingPreview(); return; // XY exact; reuse all list/commit/cancel gates.
	}
	FVector Point;
	if(EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()
		|| !FloorPointSnapped(EyeCm,Direction.GetSafeNormal(),SimUnits::MetersToCentimeters(CeilingDragPreview.HeightM),0,Point))
	{ bCeilingPreviewValid=false; CeilingEditMessage=TEXT("커서가 판 높이 평면과 만나지 않아 확정할 수 없습니다."); return; }
	const bool bMove=CeilingDragCorner==CeilingMoveHandle;
	const auto& Original=SelectedCeilingList[Index];
	const auto Handle=CeilingDragHole==INDEX_NONE ? Original : HoleHandlePlate(Original,CeilingDragHole);
	const auto Target=CornerTargetM(CeilingCornerCm(Handle,bMove?0:CeilingDragCorner),Point,CeilingDragStartCm,PlaceSnapM);
	auto Candidate=Original;
	if(CeilingDragHole!=INDEX_NONE)
	{
		auto& H=Candidate.Holes[CeilingDragHole];
		if(bMove)
		{
			// Same minimum-anchor/grab-offset snap as plate movement; translate only this local hole.
			const FVector2D Delta=FVector2D(Target.X,Target.Y)-Candidate.MinXYM-H.LocalMinXYM;
			H.LocalMinXYM+=Delta; H.LocalMaxXYM+=Delta;
		}
		else
		{
			(CeilingDragCorner&1 ? H.LocalMaxXYM.X : H.LocalMinXYM.X)=Target.X-Candidate.MinXYM.X;
			(CeilingDragCorner&2 ? H.LocalMaxXYM.Y : H.LocalMinXYM.Y)=Target.Y-Candidate.MinXYM.Y;
		}
	}
	else if(bMove)
	{
		// Snap the minimum XY anchor with the original grab offset; both edges share one displacement.
		const FVector2D Delta=FVector2D(Target.X,Target.Y)-Candidate.MinXYM;
		Candidate.MinXYM+=Delta; Candidate.MaxXYM+=Delta;
	}
	else
	{
		(CeilingDragCorner&1 ? Candidate.MaxXYM.X : Candidate.MinXYM.X)=Target.X;
		(CeilingDragCorner&2 ? Candidate.MaxXYM.Y : Candidate.MinXYM.Y)=Target.Y;
	}
	// One anchor snap, identical delta for every selected plate; preserve each height/size/local holes.
	CeilingDragPlatesPreview=SelectedCeilingList;
	if(bMove && CeilingDragHole==INDEX_NONE)
	{
		const FVector2D Delta=Candidate.MinXYM-Original.MinXYM;
		for(int32 I:CeilingDragItems) { CeilingDragPlatesPreview[I].MinXYM+=Delta; CeilingDragPlatesPreview[I].MaxXYM+=Delta; }
	}
	else CeilingDragPlatesPreview[Index]=Candidate;
	// Validate again while stationary: equipment may have moved externally without changing the list.
	CeilingDragPreview=CeilingDragPlatesPreview[Index]; ValidateRoomCeilingPreview();
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
		&& IsCameraInputAvailable() && !IsInputKeyDown(EKeys::RightMouseButton) && !(CeilingDragPlatesPreview==SelectedCeilingList))
	{
		const auto After=CeilingDragPlatesPreview; FString Reason;
		if(Sim->TrySetRoomCeilingPlates(After,Reason))
		{
			FLayoutTransformCommand Command; Command.bRoomCeilings=true; Command.RoomSettingsTarget=Sim;
			Command.CeilingsBefore=SelectedCeilingList; Command.CeilingsAfter=After; PushLayoutCommand(MoveTemp(Command));
			SelectedCeilingList=After; bOk=true; // Same index stays selected in our own successful replacement.
		}
		else { bCeilingPreviewValid=false; CeilingEditMessage=TEXT("거절: ")+Reason; }
	}
	CeilingDragCorner=INDEX_NONE; CeilingDragHole=INDEX_NONE; CeilingDragItems.Reset(); CeilingDragPlatesPreview.Reset();
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
	auto DrawHoles=[&](const FRoomCeilingPlate& P,const FRoomCeilingPlan& Plan)
	{
		for(int32 J=0;J<P.Holes.Num();++J)
		{
			const auto Lo=P.MinXYM+P.Holes[J].LocalMinXYM,Hi=P.MinXYM+P.Holes[J].LocalMaxXYM;
			const FVector V[]={FVector(Lo.X,Lo.Y,P.HeightM),FVector(Hi.X,Lo.Y,P.HeightM),FVector(Hi.X,Hi.Y,P.HeightM),FVector(Lo.X,Hi.Y,P.HeightM)};
			for(int32 K=0;K<4;++K) DrawDebugLine(GetWorld(),SimUnits::MetersToUnreal(V[K]),SimUnits::MetersToUnreal(V[(K+1)%4]),FColor::Cyan,false,0,1,2);
			const auto& Open=Plan.OpenCells[J];
			const FBox B(Bounds.Min+FVector(Open.Min.X,Open.Min.Y,Plan.MinCell.Z)*GP.CellSizeM,
				Bounds.Min+FVector(Open.Max.X,Open.Max.Y,Plan.MaxCellExclusive.Z)*GP.CellSizeM);
			// Front-depth lines only: no collision/selection primitive, no clearing another solid owner.
			DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(B.GetCenter()),SimUnits::MetersToUnreal(B.GetExtent()),FColor(220,100,255),false,0,1,3);
		}
	};
	for(int32 I=0;I<Plans.Num();++I)
	{
		const auto& P=Sim->GetRoomCeilingPlates()[I]; const auto& B=Plans[I].ActualBoundsM;
		DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(B.GetCenter()),SimUnits::MetersToUnreal(B.GetExtent()),FColor(255,165,0),false,-1,0,1);
		const FVector V[]={FVector(P.MinXYM.X,P.MinXYM.Y,P.HeightM),FVector(P.MaxXYM.X,P.MinXYM.Y,P.HeightM),
			FVector(P.MaxXYM.X,P.MaxXYM.Y,P.HeightM),FVector(P.MinXYM.X,P.MaxXYM.Y,P.HeightM)};
		for(int32 J=0;J<4;++J) DrawDebugLine(GetWorld(),SimUnits::MetersToUnreal(V[J]),SimUnits::MetersToUnreal(V[(J+1)%4]),FColor::Cyan,false,-1,0,1);
		DrawHoles(P,Plans[I]);
	}
	const auto SelectedPlates=GetSelectedRoomCeilingIndices();
	for(int32 I:SelectedPlates)
	{
		const auto& B=Plans[I].ActualBoundsM;
		DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(B.GetCenter()),SimUnits::MetersToUnreal(B.GetExtent()),FColor::Green,false,0,1,4);
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
			if(!bRoomCeilingHoleEditing && SelectedPlates.Num()==1) DrawDebugPoint(GetWorld(),CeilingCornerCm(P,I),16,Color,false,0,1); // Existing room handle UI pixels.
		}
		const FVector Top=CeilingCornerCm(P,RoomHeightHandle), Bottom(Top.X,Top.Y,0);
		if(!bRoomCeilingHoleEditing && SelectedPlates.Num()==1) DrawDebugDirectionalArrow(GetWorld(),Bottom,Top,20.f,Color,false,0,1,2.f); // Existing room UI arrow20cm, not geometry.
		if(!bRoomCeilingHoleEditing && SelectedPlates.Num()==1) DrawDebugPoint(GetWorld(),Top,16,Color,false,0,1);
		const auto SelectedHoles=GetSelectedRoomCeilingHoleIndices();
		for(int32 I:SelectedHoles)
		{
			const auto& Plan=Plans[Selected]; const auto& O=Plan.OpenCells[I];
			const FBox SelectedHoleBounds(Bounds.Min+FVector(O.Min.X,O.Min.Y,Plan.MinCell.Z)*GP.CellSizeM,
				Bounds.Min+FVector(O.Max.X,O.Max.Y,Plan.MaxCellExclusive.Z)*GP.CellSizeM);
			DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(SelectedHoleBounds.GetCenter()),SimUnits::MetersToUnreal(SelectedHoleBounds.GetExtent()),FColor::Green,false,0,1,5);
		}
		const int32 Hole=GetSelectedRoomCeilingHoleIndex();
		if(Hole!=INDEX_NONE && SelectedHoles.Num()==1)
		{
			const auto Handle=HoleHandlePlate(P,Hole);
			for(int32 I=0;I<4;++I)
			{
				DrawDebugLine(GetWorld(),CeilingCornerCm(Handle,Order[I]),CeilingCornerCm(Handle,Order[I+1]),Color,false,0,1,5);
				DrawDebugPoint(GetWorld(),CeilingCornerCm(Handle,I),16,Color,false,0,1);
			}
			TArray<FRoomCeilingPlan> HolePlans;
			if(VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),{P},GP,Bounds,HolePlans,Reason))
			{
				const auto& Plan=HolePlans[0]; const auto& O=Plan.OpenCells[Hole];
				const FBox Highlight(Bounds.Min+FVector(O.Min.X,O.Min.Y,Plan.MinCell.Z)*GP.CellSizeM,
					Bounds.Min+FVector(O.Max.X,O.Max.Y,Plan.MaxCellExclusive.Z)*GP.CellSizeM);
				DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(Highlight.GetCenter()),SimUnits::MetersToUnreal(Highlight.GetExtent()),Color,false,0,1,5);
			}
		}

		TArray<FRoomCeilingPlan> PreviewPlans;
		if(CeilingDragCorner!=INDEX_NONE && CeilingDragHole==INDEX_NONE && VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),{P},GP,Bounds,PreviewPlans,Reason))
		{
			const auto& Actual=PreviewPlans[0].ActualBoundsM;
			DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(Actual.GetCenter()),SimUnits::MetersToUnreal(Actual.GetExtent()),Color,false,0,1,3);
			DrawHoles(P,PreviewPlans[0]);
		}
	}
	if(CeilingDragCorner==CeilingMoveHandle && CeilingDragHole==INDEX_NONE && CeilingDragItems.Num()>1 && Selected!=INDEX_NONE)
	{
		const FColor Color=bCeilingPreviewValid ? FColor(255,165,0) : FColor::Red;
		TArray<FRoomCeilingPlan> GroupPlans;
		const bool bPlan=VrgRoomAirDomain::BuildCeilingPlans(Sim->GetRoomAirDomain(),CeilingDragPlatesPreview,GP,Bounds,GroupPlans,Reason);
		for(int32 I:CeilingDragItems)
		{
			const auto& P=CeilingDragPlatesPreview[I]; const int32 Order[]={0,1,3,2,0};
			for(int32 J=0;J<4;++J) DrawDebugLine(GetWorld(),CeilingCornerCm(P,Order[J]),CeilingCornerCm(P,Order[J+1]),Color,false,0,1,5);
			if(bPlan) { const auto& Box=GroupPlans[I].ActualBoundsM; DrawDebugBox(GetWorld(),SimUnits::MetersToUnreal(Box.GetCenter()),SimUnits::MetersToUnreal(Box.GetExtent()),Color,false,0,1,5); DrawHoles(P,GroupPlans[I]); }
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
	if(!bRoomCeilingHoleEditing) DrawDebugDirectionalArrow(GetWorld(),Bottom,Top,20.f,Color,false,0,1,2.f); // UI arrow20cm, not physical geometry.
	DrawDebugPoint(GetWorld(),Top,16.f,Color,false,0,1);
	for (int32 I=0;I<4;++I)
	{
		FVector A=RoomCornerCm(S,Order[I]), B=RoomCornerCm(S,Order[I+1]); A.Z=B.Z=Top.Z;
		DrawDebugLine(GetWorld(),A,B,Color,false,0,1,2.f); // Ceiling range guide only; no solid plate.
	}
}
