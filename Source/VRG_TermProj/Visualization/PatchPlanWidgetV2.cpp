// 2026-11-27 (vrg.Ui.Panels 1, 2026-11-27부터 기본): 패치 평면도를 새 UI 모양으로 그린다. 그리는 자료(스냅샷)와 투영(Project),
// 클릭·키 처리는 UPatchPlanWidget 그대로이고 모양만 다르다(표시 전용, 진단 도구).
#include "Visualization/PatchPlanWidget.h"
#include "Interaction/VrgUiStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace
{
	using namespace VrgUi::Theme;

	// One brush per theme (corner radius differs); kept alive for the draw lists.
	const FSlateBrush* FillBrush()
	{
		static const FSlateRoundedBoxBrush B[VrgUi::NumThemes] = { { FLinearColor::White, 8.f * VrgUi::GetTheme(0).RadiusScale }, { FLinearColor::White, 8.f * VrgUi::GetTheme(1).RadiusScale },
			{ FLinearColor::White, 8.f * VrgUi::GetTheme(2).RadiusScale }, { FLinearColor::White, 8.f * VrgUi::GetTheme(3).RadiusScale } };
		return &B[VrgUi::ThemeIndex()];
	}
	const FSlateBrush* OutlineBrush()
	{
		static const FSlateRoundedBoxBrush B[VrgUi::NumThemes] = {
			{ FLinearColor::Transparent, 8.f * VrgUi::GetTheme(0).RadiusScale, FLinearColor::White, 1.f }, { FLinearColor::Transparent, 8.f * VrgUi::GetTheme(1).RadiusScale, FLinearColor::White, 1.f },
			{ FLinearColor::Transparent, 8.f * VrgUi::GetTheme(2).RadiusScale, FLinearColor::White, 1.f }, { FLinearColor::Transparent, 8.f * VrgUi::GetTheme(3).RadiusScale, FLinearColor::White, 1.f } };
		return &B[VrgUi::ThemeIndex()];
	}
	const FSlateBrush* Solid() { return FCoreStyle::Get().GetBrush("WhiteBrush"); }

	FVector2D Measure(const FString& S, const FSlateFontInfo& F)
	{
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
			return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(S, F);
		return FVector2D(S.Len() * F.Size * .6f, F.Size * 1.4f);
	}
}

int32 UPatchPlanWidget::PaintV2(const FGeometry& Geometry, FSlateWindowElementList& Elements, int32 Layer) const
{
	const FVector2D Size = Geometry.GetLocalSize();
	// Short device names ("랙 #3", "스파인 #1") in device order; the patch labels are class names.
	TArray<FString> Short;
	{
		int32 Racks = 0, Spines = 0;
		for (const Patch::FDevice& D : Devices)
			Short.Add(D.Label.Contains(TEXT("Rack")) ? FString::Printf(TEXT("랙 #%d"), ++Racks)
				: D.Label.Contains(TEXT("Spine")) ? FString::Printf(TEXT("스파인 #%d"), ++Spines) : D.Label);
	}
	auto Box = [&](int32 L, const FVector2D& Pos, const FVector2D& Sz, const FSlateBrush* Brush, const FLinearColor& Color)
	{
		FSlateDrawElement::MakeBox(Elements, Layer + L, Geometry.ToPaintGeometry(FVector2f(Sz), FSlateLayoutTransform(FVector2f(Pos))), Brush, ESlateDrawEffect::None, Color);
	};
	auto Card = [&](const FVector2D& Pos, const FVector2D& Sz, const FLinearColor& Fill, const FLinearColor& Edge)
	{
		if (VrgUi::ThemeIndex() == 0)
		{
			// The approved default look (2026-11-27): fill brush plus an outline brush whose tint also lightens the inside.
			Box(1, Pos, Sz, FillBrush(), Fill);
			Box(1, Pos, Sz, OutlineBrush(), Edge);
			return;
		}
		// Themes: one brush; Slate fills a rounded box with the element colour and draws the brush outline colour.
		const FSlateRoundedBoxBrush B(FLinearColor::White, Radius(8.f), Edge, 1.f);
		Box(1, Pos, Sz, &B, Fill);   // the element colour fills; the brush keeps the outline colour
	};
	auto Font = [](int32 Pt, bool bBold) { return FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Pt); };
	auto Write = [&](const FVector2D& Pos, const FString& S, int32 Pt, const FLinearColor& Color, bool bBold = false)
	{
		const FSlateFontInfo F = Font(Pt, bBold);
		FSlateDrawElement::MakeText(Elements, Layer + 4, Geometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(Pos))), S, F, ESlateDrawEffect::None, Color);
		return Measure(S, F).X;
	};
	auto Poly = [&](const TArray<FVector>& Points, const FLinearColor& Color, float Width, int32 L = 2)
	{
		TArray<FVector2D> P; for (const FVector& Pt : Points) P.Add(Project(Pt, Size));
		if (P.Num() > 1) FSlateDrawElement::MakeLines(Elements, Layer + L, Geometry.ToPaintGeometry(), P, ESlateDrawEffect::None, Color, true, Width);
	};
	// Key cap + description; returns the width used.
	auto KeyHint = [&](float X, float Y, const FString& Key, const FString& Desc)
	{
		const float Kw = Measure(Key, Font(9, true)).X + 12.f;
		Card(FVector2D(X, Y), FVector2D(Kw, 20.f), WithAlpha(Text(), .07f), WithAlpha(Text(), .22f));
		Write(FVector2D(X + 6.f, Y + 2.f), Key, 9, VrgUi::Theme::Text(), true);
		return Kw + 6.f + Write(FVector2D(X + Kw + 6.f, Y + 2.f), Desc, 10, Text2()) + 18.f;
	};

	// Backdrop and header.
	Box(0, FVector2D::ZeroVector, Size, Solid(), PlanBg());
	Box(1, FVector2D::ZeroVector, FVector2D(Size.X, 52.f), Solid(), PanelHi());
	Box(1, FVector2D(0.f, 51.f), FVector2D(Size.X, 1.f), Solid(), Line());
	float X = 24.f;
	X += Write(FVector2D(X, 14.f), TEXT("배선 평면도"), 15, VrgUi::Theme::Text(), true) + 12.f;
	Write(FVector2D(X, 18.f), TEXT("위에서 본 세계 XY (m) · 진단용"), 10, Text2());
	{
		const TArray<TPair<FString, FString>> Keys = { { TEXT("클릭"), TEXT("케이블 선택") }, { TEXT("← →"), TEXT("이전·다음") }, { TEXT("P"), TEXT("설비 마당") }, { TEXT("Esc"), TEXT("닫기") } };
		float W = 0.f;
		for (const auto& K : Keys) W += Measure(K.Key, Font(9, true)).X + 12.f + 6.f + Measure(K.Value, Font(10, false)).X + 18.f;
		float Kx = Size.X - 24.f - W + 18.f;
		for (const auto& K : Keys) Kx += KeyHint(Kx, 16.f, K.Key, K.Value);
	}

	// Stat cards.
	struct FStat { FString Label; int32 Value; bool bBad; };
	TArray<FStat> Stats = {
		{ TEXT("케이블"), Snapshot.Cables.Num(), false },
		{ TEXT("미연결 포트"), Snapshot.Unconnected.Num(), Snapshot.Unconnected.Num() > 0 },
		{ TEXT("거절된 포트 요청"), Snapshot.RejectedClaims, Snapshot.RejectedClaims > 0 },
		{ TEXT("3D 간섭 쌍"), Snapshot.Interferences.Num(), Snapshot.Interferences.Num() > 0 },
	};
	if (bShowPlant) { Stats.Add({ TEXT("설비 배관"), PlantPipes, false }); Stats.Add({ TEXT("그림 겹침"), PlantClashes, PlantClashes > 0 }); }
	float Sx = 24.f;
	for (const FStat& St : Stats)
	{
		const FString V = FString::FromInt(St.Value);
		const float W = FMath::Max(Measure(St.Label, Font(9, false)).X, Measure(V, Font(15, true)).X) + 28.f;
		Card(FVector2D(Sx, 64.f), FVector2D(W, 50.f), Panel(), St.bBad ? WithAlpha(Red(), .6f) : Line());
		Box(2, FVector2D(Sx, 64.f), FVector2D(3.f, 50.f), Solid(), St.bBad ? Red() : Accent());
		Write(FVector2D(Sx + 14.f, 70.f), St.Label, 9, Text3());
		Write(FVector2D(Sx + 14.f, 84.f), V, 15, St.bBad ? Red() : VrgUi::Theme::Text(), true);
		Sx += W + 8.f;
	}
	// Legend (right of the stats).
	{
		struct FKey { FString Name; FLinearColor Color; float Width; };
		TArray<FKey> Keys = { { TEXT("업링크"), FLinearColor(0, .7f, 1), 2.f }, { TEXT("스파인 간"), SpineLine(), 2.f },
			{ TEXT("간섭"), FLinearColor(1, .15f, .1f), 2.f }, { TEXT("선택"), SelectLine(), 3.f } };
		if (bShowPlant)
		{
			Keys.Add({ TEXT("FWS"), FLinearColor(1, .45f, .1f), 3.f });
			Keys.Add({ TEXT("CHW"), FLinearColor(.2f, .5f, 1), 3.f });
			Keys.Add({ TEXT("CW"), FLinearColor(.25f, .85f, .35f), 3.f });
		}
		float Lx = Sx + 16.f;
		for (const FKey& K : Keys)
		{
			if (Lx + 80.f > Size.X - 24.f) break;
			Box(2, FVector2D(Lx, 88.f), FVector2D(16.f, K.Width), Solid(), K.Color);
			Lx += 22.f + Write(FVector2D(Lx + 22.f, 80.f), K.Name, 10, Text2()) + 18.f;
		}
	}

	// Plan area: panel behind the projection with a 1 m grid inside the drawn bounds.
	const float PlanTop = 124.f, PlanBottom = Size.Y - 96.f;
	Card(FVector2D(24.f, PlanTop), FVector2D(Size.X - 48.f, PlanBottom - PlanTop), PlanArea(), Line());
	if (BoundsM.IsValid)
	{
		const FVector Lo = BoundsM.Min, Hi = BoundsM.Max;
		for (double Gx = FMath::CeilToDouble(Lo.X); Gx <= Hi.X; Gx += 1.0)
			Poly({ FVector(Gx, Lo.Y, 0), FVector(Gx, Hi.Y, 0) }, FMath::IsNearlyZero(FMath::Fmod(Gx, 5.0)) ? Line() : LineSoft(), 1.f, 1);
		for (double Gy = FMath::CeilToDouble(Lo.Y); Gy <= Hi.Y; Gy += 1.0)
			Poly({ FVector(Lo.X, Gy, 0), FVector(Hi.X, Gy, 0) }, FMath::IsNearlyZero(FMath::Fmod(Gy, 5.0)) ? Line() : LineSoft(), 1.f, 1);
		// 5 m scale bar (bottom left of the plan).
		const FVector2D A = Project(FVector(Lo.X, Lo.Y, 0), Size), B = Project(FVector(Lo.X + 5.0, Lo.Y, 0), Size);
		const FVector2D Bar(40.f, PlanBottom - 22.f);
		Box(3, Bar, FVector2D(FMath::Max(B.X - A.X, 4.0), 2.f), Solid(), Text2());
		Write(Bar + FVector2D(0, -16.f), TEXT("5 m"), 9, Text2());
	}

	// Plant yard, devices, cables (same data and colours as the existing map).
	for (const TArray<FVector>& Unit : PlantUnits) Poly(Unit, WithAlpha(Text(), .35f), 1.f);
	for (const auto& P : PlantLines) Poly(P.Key, WithAlpha(P.Value, .8f), 1.f);
	for (const auto& P : PlantHeaders) Poly(P.Key, P.Value, 3.f);
	for (int32 Di = 0; Di < Devices.Num(); ++Di)
	{
		const Patch::FDevice& Device = Devices[Di];
		TArray<FVector> Corners;
		for (FVector Pt : { FVector(-1, -1, 0), FVector(1, -1, 0), FVector(1, 1, 0), FVector(-1, 1, 0), FVector(-1, -1, 0) })
			Corners.Add(Device.ToWorld.TransformPosition(Pt * Device.HalfExtentM));
		const FVector2D C0 = Project(Corners[0], Size), C2 = Project(Corners[2], Size);
		const FVector2D Min(FMath::Min(C0.X, C2.X), FMath::Min(C0.Y, C2.Y)), Max(FMath::Max(C0.X, C2.X), FMath::Max(C0.Y, C2.Y));
		Box(2, Min, Max - Min, Solid(), WithAlpha(Teal(), .14f));
		Poly(Corners, WithAlpha(Teal(), .85f), 1.5f);
		const FVector2D At = Project(Device.ToWorld.GetLocation(), Size);
		// The name only where it fits inside the footprint (dense rows stay readable; the selection card names both ends).
		const float Lw = Measure(Short[Di], Font(8, false)).X;
		if (Lw + 4.f <= Max.X - Min.X || Lw + 4.f <= Max.Y - Min.Y)
			Write(At - FVector2D(.5f * Lw, 6.f), Short[Di], 8, Text2());
	}
	TSet<int32> Interfering;
	for (const Patch::FInterference& Hit : Snapshot.Interferences) { Interfering.Add(Hit.CableA); Interfering.Add(Hit.CableB); }
	for (int32 I = 0; I < Snapshot.Cables.Num(); ++I)
	{
		const Patch::FCable& C = Snapshot.Cables[I];
		Poly(C.PointsM, Interfering.Contains(I) ? FLinearColor(1, .15f, .1f) : C.bSpineToSpine ? SpineLine() : FLinearColor(0, .7f, 1, .85f), 1.f, 3);
	}

	// Selection card.
	const float By = Size.Y - 84.f;
	Card(FVector2D(24.f, By), FVector2D(Size.X - 48.f, 60.f), Panel(), Line());
	if (Snapshot.Cables.IsValidIndex(Selected))
	{
		const Patch::FCable& C = Snapshot.Cables[Selected];
		Poly(C.PointsM, SelectLine(), 3.f, 3);
		if (C.PointsM.Num()) { const FVector2D E = Project(C.PointsM[0], Size); Box(3, E - FVector2D(4, 4), FVector2D(8, 8), Solid(), SelectLine()); }
		double LengthM = 0.0;
		for (int32 I = 1; I < C.PointsM.Num(); ++I) LengthM += FVector::Distance(C.PointsM[I - 1], C.PointsM[I]);
		auto Name = [&](int32 I) { return Short.IsValidIndex(I) ? Short[I] : FString(TEXT("?")); };
		Box(2, FVector2D(24.f, By), FVector2D(3.f, 60.f), Solid(), SelectLine());
		Write(FVector2D(40.f, By + 9.f), FString::Printf(TEXT("케이블 %d / %d"), Selected + 1, Snapshot.Cables.Num()), 9, Text3());
		Write(FVector2D(40.f, By + 25.f), FString::Printf(TEXT("%s  포트 %d   →   %s  포트 %d"), *Name(C.FromDevice), C.FromPort + 1, *Name(C.ToDevice), C.ToPort + 1), 12, VrgUi::Theme::Text(), true);
		const FString Len = FString::Printf(TEXT("%.2f m"), LengthM);
		const float Lw = Measure(Len, Font(15, true)).X;
		Write(FVector2D(Size.X - 40.f - Lw, By + 9.f), TEXT("꺾인선 길이"), 9, Text3());
		Write(FVector2D(Size.X - 40.f - Lw, By + 23.f), Len, 15, VrgUi::Theme::Text(), true);
	}
	else Write(FVector2D(40.f, By + 20.f), TEXT("케이블이 없습니다"), 11, Text2());
	Write(FVector2D(24.f, Size.Y - 20.f), TEXT("진단용 표시: 둥근 굽힘·커넥터·장애물·굽힘 반경은 검증하지 않습니다."), 9, Text3());
	return Layer + 5;
}
