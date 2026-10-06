// Barrier gizmo of ADataCenterDebugController (2026-10-06).
//
// View-side geometry in UE cm (actor boundary). Moves, rotations and size changes are layout transforms only;
// the air grid rasterises the barrier again at the next simulation entry (MarkBarrierSolidCells).

#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/LayoutBarrierActor.h"

#include "DrawDebugHelpers.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "UnrealClient.h"

namespace
{
	// Parameter s of the point on line P + s*A (A unit) closest to the ray E + t*D (D unit); false when nearly parallel.
	bool ClosestLineParam(const FVector& P, const FVector& A, const FVector& E, const FVector& D, double& OutS)
	{
		const FVector W0 = P - E;
		const double B = FVector::DotProduct(A, D), Dd = FVector::DotProduct(A, W0), Ee = FVector::DotProduct(D, W0);
		const double Denom = 1.0 - B * B;
		if (Denom < 1e-4) return false;
		OutS = (B * Ee - Dd) / Denom;
		return FMath::IsFinite(OutS);
	}

	double PointSegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double L2 = AB.SizeSquared();
		const double T = L2 > 0.0 ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / L2, 0.0, 1.0) : 0.0;
		return FVector2D::Distance(P, A + AB * T);
	}
}

ALayoutBarrierActor* ADataCenterDebugController::GetLayoutGizmoTarget() const
{
	if (GizmoDrag.bActive) return GizmoDrag.Target.Get();
	if (bRoomBoundsEditing || !IsLayoutMode() || bAddingRack || bMarqueeActive || !DragMembers.IsEmpty() || Grabbed.IsValid()) return nullptr;
	const TArray<AActor*> Selected = GetSelectedLayoutActors();
	return Selected.Num() == 1 ? Cast<ALayoutBarrierActor>(Selected[0]) : nullptr;
}

bool ADataCenterDebugController::SetLayoutGizmoMode(ELayoutGizmoMode Mode)
{
	if (GizmoDrag.bActive || !GetLayoutGizmoTarget()) return false;
	GizmoMode = Mode;
	return true;
}

bool ADataCenterDebugController::GetLayoutGizmoFrame(FVector& PivotCm, double& LengthCm) const
{
	const ALayoutBarrierActor* Target = GetLayoutGizmoTarget();
	if (!Target) return false;
	PivotCm = GizmoDrag.bActive && GizmoMode == ELayoutGizmoMode::Rotate ? GizmoDrag.PivotCm
		: Target->GetActorTransform().TransformPosition(FVector(0, 0, 50.0));  // unit-box centre
	FVector EyeCm; FRotator View;
	GetPlayerViewPoint(EyeCm, View);
	// Constant on-screen size: a UI choice, not a dimension.
	LengthCm = FMath::Clamp(FVector::Dist(EyeCm, PivotCm) * 0.12, 20.0, 400.0);
	return true;
}

FVector ADataCenterDebugController::GetLayoutGizmoAxis(int32 Axis) const
{
	if (Axis < 0 || Axis > 2) return FVector::ZeroVector;
	if (GizmoMode == ELayoutGizmoMode::Scale)
	{
		// Scale is always along the barrier's own axes (as in Unreal).
		const ALayoutBarrierActor* Target = GetLayoutGizmoTarget();
		const FQuat Q = GizmoDrag.bActive ? GizmoDrag.Start.GetRotation() : Target ? Target->GetActorQuat() : FQuat::Identity;
		return Axis == 0 ? Q.GetAxisX() : Axis == 1 ? Q.GetAxisY() : Q.GetAxisZ();
	}
	return Axis == 0 ? FVector::XAxisVector : Axis == 1 ? FVector::YAxisVector : FVector::ZAxisVector;
}

int32 ADataCenterDebugController::PickLayoutGizmoAxis(const FVector2D& Screen) const
{
	FVector PivotCm; double LengthCm = 0.0;
	if (!GetLayoutGizmoFrame(PivotCm, LengthCm)) return INDEX_NONE;
	int32 Best = INDEX_NONE;
	double BestPx = 10.0;  // UI pick radius in pixels
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FVector Dir = GetLayoutGizmoAxis(Axis);
		double Distance = TNumericLimits<double>::Max();
		if (GizmoMode == ELayoutGizmoMode::Rotate)
		{
			FVector U, V;
			Dir.FindBestAxisVectors(U, V);
			FVector2D Previous = FVector2D::ZeroVector;
			bool bHasPrevious = false;
			for (int32 K = 0; K <= 48; ++K)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * K / 48.0;
				FVector2D Point;
				const bool bOk = ProjectWorldLocationToScreen(PivotCm + (U * FMath::Cos(Angle) + V * FMath::Sin(Angle)) * LengthCm * .8, Point);
				if (bOk && bHasPrevious) Distance = FMath::Min(Distance, PointSegmentDistance(Screen, Previous, Point));
				Previous = Point;
				bHasPrevious = bOk;
			}
		}
		else
		{
			FVector2D A, B;
			if (ProjectWorldLocationToScreen(PivotCm, A) && ProjectWorldLocationToScreen(PivotCm + Dir * LengthCm, B))
				Distance = PointSegmentDistance(Screen, A, B);
		}
		if (Distance < BestPx) { BestPx = Distance; Best = Axis; }
	}
	return Best;
}

bool ADataCenterDebugController::BeginLayoutGizmoDrag(int32 Axis, const FVector& EyeCm, const FVector& Direction, const FVector2D& Screen)
{
	ALayoutBarrierActor* Target = GetLayoutGizmoTarget();
	if (!Target || GizmoDrag.bActive || Axis < 0 || Axis > 2 || !IsEquipmentInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)
		|| !Target->GetRootComponent() || Target->GetRootComponent()->Mobility != EComponentMobility::Movable
		|| EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return false;
	const FVector D = Direction.GetSafeNormal();
	FGizmoDrag Drag;
	Drag.Axis = Axis;
	Drag.Target = Target;
	Drag.Start = Target->GetActorTransform();
	Drag.PivotCm = Drag.Start.TransformPosition(FVector(0, 0, 50.0));
	Drag.AxisDir = GetLayoutGizmoAxis(Axis);
	Drag.StartScreen = Screen;
	if (GizmoMode == ELayoutGizmoMode::Rotate)
	{
		const double Facing = FVector::DotProduct(D, Drag.AxisDir);
		const double T = FMath::Abs(Facing) > 1e-6 ? FVector::DotProduct(Drag.PivotCm - EyeCm, Drag.AxisDir) / Facing : -1.0;
		const FVector Hit = EyeCm + D * T - Drag.PivotCm;
		Drag.StartVector = Hit - Drag.AxisDir * FVector::DotProduct(Hit, Drag.AxisDir);
		// Seen edge-on, the ring plane gives an unstable angle: use horizontal mouse motion instead.
		Drag.bScreenRotate = FMath::Abs(Facing) < 0.1 || T <= 0.0 || Drag.StartVector.IsNearlyZero(1.0);
	}
	else if (!ClosestLineParam(Drag.PivotCm, Drag.AxisDir, EyeCm, D, Drag.StartParamCm))
	{
		return false;  // looking straight down the handle
	}
	EndLayoutMarquee(false);
	Drag.bActive = true;
	GizmoDrag = Drag;
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 기즈모 %s %c축 시작: %s"),
		GizmoMode == ELayoutGizmoMode::Translate ? TEXT("이동") : GizmoMode == ELayoutGizmoMode::Rotate ? TEXT("회전") : TEXT("크기"),
		TCHAR('X' + Axis), *Target->GetName());
	return true;
}

void ADataCenterDebugController::UpdateLayoutGizmoDrag(const FVector& EyeCm, const FVector& Direction, const FVector2D& Screen, bool bNoSnap)
{
	if (!GizmoDrag.bActive) return;
	ALayoutBarrierActor* Target = GizmoDrag.Target.Get();
	if (!IsValid(Target) || !IsLayoutMode() || !IsEquipmentInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton))
	{
		EndLayoutGizmoDrag(false);
		return;
	}
	if (EyeCm.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero()) return;
	const FVector D = Direction.GetSafeNormal();
	const FVector A = GizmoDrag.AxisDir;
	FTransform Proposed = GizmoDrag.Start;
	const bool bSnap = !bNoSnap && FMath::IsFinite(PlaceSnapM) && PlaceSnapM > 0.0;
	if (GizmoMode == ELayoutGizmoMode::Rotate)
	{
		double AngleDeg = 0.0;
		if (GizmoDrag.bScreenRotate)
		{
			AngleDeg = (Screen.X - GizmoDrag.StartScreen.X) * 0.5;  // UI: half a degree per pixel
		}
		else
		{
			const double Facing = FVector::DotProduct(D, A);
			if (FMath::Abs(Facing) < 1e-6) return;
			const double T = FVector::DotProduct(GizmoDrag.PivotCm - EyeCm, A) / Facing;
			if (T <= 0.0) return;
			FVector V = EyeCm + D * T - GizmoDrag.PivotCm;
			V -= A * FVector::DotProduct(V, A);
			if (V.IsNearlyZero(1.0)) return;
			AngleDeg = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(FVector::CrossProduct(GizmoDrag.StartVector, V), A),
				FVector::DotProduct(GizmoDrag.StartVector, V)));
		}
		if (!bNoSnap && GizmoRotateSnapDeg > 0.0) AngleDeg = FMath::RoundToDouble(AngleDeg / GizmoRotateSnapDeg) * GizmoRotateSnapDeg;
		const FQuat Turn(A, FMath::DegreesToRadians(AngleDeg));
		Proposed.SetRotation((Turn * GizmoDrag.Start.GetRotation()).GetNormalized());
		Proposed.SetLocation(GizmoDrag.PivotCm + Turn.RotateVector(GizmoDrag.Start.GetLocation() - GizmoDrag.PivotCm));
	}
	else
	{
		double S = 0.0;
		if (!ClosestLineParam(GizmoDrag.PivotCm, A, EyeCm, D, S)) return;
		const double DeltaCm = S - GizmoDrag.StartParamCm;
		if (GizmoMode == ELayoutGizmoMode::Translate)
		{
			const double StepCm = PlaceSnapM * 100.0;
			Proposed.SetLocation(GizmoDrag.Start.GetLocation() + A * (bSnap ? FMath::RoundToDouble(DeltaCm / StepCm) * StepCm : DeltaCm));
		}
		else
		{
			// The origin is the bottom centre: thickness/width grow on both sides (2x), height grows upward (1x).
			const int32 I = GizmoDrag.Axis;
			double DeltaSizeM = (I == 2 ? 1.0 : 2.0) * DeltaCm / 100.0;
			if (bSnap) DeltaSizeM = FMath::RoundToDouble(DeltaSizeM / PlaceSnapM) * PlaceSnapM;
			FVector Scale = GizmoDrag.Start.GetScale3D();
			Scale[I] = FMath::Max(0.01, Scale[I] + DeltaSizeM);  // 1 cm floor keeps the box valid (UI limit)
			Proposed.SetScale3D(Scale);
		}
	}
	if (!Proposed.ContainsNaN()) Target->SetActorTransform(Proposed, false);
}

bool ADataCenterDebugController::IsLayoutGizmoBlocked() const
{
	const ALayoutBarrierActor* Target = GizmoDrag.bActive ? GizmoDrag.Target.Get() : nullptr;
	return Target && IsLayoutPlacementBlocked(Target, Target->GetActorTransform());
}

void ADataCenterDebugController::EndLayoutGizmoDrag(bool bCommit)
{
	if (!GizmoDrag.bActive) return;
	ALayoutBarrierActor* Target = GizmoDrag.Target.Get();
	const FTransform Start = GizmoDrag.Start;
	const bool bBlocked = IsLayoutGizmoBlocked();
	GizmoDrag = FGizmoDrag();
	if (!IsValid(Target)) return;
	if (!bCommit || bBlocked)
	{
		Target->SetActorTransform(Start, false);  // the live preview moved the actor; put it back
		if (bCommit) UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 기즈모 거절: %s가 장비와 겹침 (1 cm 여유)"), *Target->GetName());
		return;
	}
	if (Target->GetActorTransform().Equals(Start, 0.0)) return;
	RecordLayoutTransform(Target, Start);
	if (ADataCenterSimulationActor* Sim = FindSimActor()) Sim->RebuildCoupling();
	UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 기즈모 확정: %s 위치 %s cm, 회전 %s, 크기 %s m"), *Target->GetName(),
		*Target->GetActorLocation().ToCompactString(), *Target->GetActorRotation().ToCompactString(), *Target->GetSizeM().ToCompactString());
}

bool ADataCenterDebugController::MouseRay(FVector& EyeCm, FVector& Direction, FVector2D& Screen) const
{
	const UGameViewportClient* Client = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	float X = 0, Y = 0;
	int32 Width = 0, Height = 0;
	GetViewportSize(Width, Height);
	if (!Client || !Client->Viewport || !Client->Viewport->HasFocus() || !GetMousePosition(X, Y)
		|| X < 0 || Y < 0 || X >= Width || Y >= Height || !DeprojectMousePositionToWorld(EyeCm, Direction)) return false;
	Screen = FVector2D(X, Y);
	return true;
}

void ADataCenterDebugController::OnGizmoTranslateKey()
{
	if (bRoomSettingsOpen || !IsLayoutMode() || !IsEquipmentInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) return;
	SetLayoutGizmoMode(ELayoutGizmoMode::Translate);
}

void ADataCenterDebugController::OnGizmoCycleKey()
{
	if (bRoomSettingsOpen || !IsLayoutMode() || !IsEquipmentInputAvailable() || IsInputKeyDown(EKeys::RightMouseButton)) return;
	SetLayoutGizmoMode(static_cast<ELayoutGizmoMode>((uint8(GizmoMode) + 1) % 3));
}

void ADataCenterDebugController::DrawLayoutGizmo() const
{
	FVector PivotCm; double LengthCm = 0.0;
	if (!GetWorld() || !GetLayoutGizmoFrame(PivotCm, LengthCm)) return;
	const uint8 Front = SDPG_Foreground;  // drawn over the meshes, like the editor gizmo
	const FColor AxisColors[3] = { FColor(230, 60, 60), FColor(80, 210, 80), FColor(70, 120, 255) };
	const int32 Highlight = GizmoDrag.bActive ? GizmoDrag.Axis : GizmoHoverAxis;
	const ALayoutBarrierActor* Target = GetLayoutGizmoTarget();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FColor Color = Axis == Highlight ? FColor::Yellow : AxisColors[Axis];
		const FVector Dir = GetLayoutGizmoAxis(Axis);
		if (GizmoMode == ELayoutGizmoMode::Rotate)
		{
			FVector U, V;
			Dir.FindBestAxisVectors(U, V);
			DrawDebugCircle(GetWorld(), PivotCm, LengthCm * .8, 48, Color, false, 0.0f, Front, 2.5f, U, V, false);
		}
		else if (GizmoMode == ELayoutGizmoMode::Translate)
		{
			DrawDebugDirectionalArrow(GetWorld(), PivotCm, PivotCm + Dir * LengthCm, LengthCm * .2, Color, false, 0.0f, Front, 3.0f);
		}
		else
		{
			DrawDebugLine(GetWorld(), PivotCm, PivotCm + Dir * LengthCm, Color, false, 0.0f, Front, 3.0f);
			DrawDebugBox(GetWorld(), PivotCm + Dir * LengthCm, FVector(LengthCm * .06), Target ? Target->GetActorQuat() : FQuat::Identity,
				Color, false, 0.0f, Front, 3.0f);
		}
	}
}
