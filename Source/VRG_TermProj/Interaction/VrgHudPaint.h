#pragma once

// 2026-11-24 새 HUD의 Canvas 그리기 도구(DataCenterHudV2.cpp에서 옮김, 2026-11-27 3D 라벨 데모도 같이 쓴다).
// 크기는 DPI 배율 S를 곱한 뷰포트 픽셀, 색은 VrgUi::Theme. 표시 전용.
#include "CoreMinimal.h"
#include "Interaction/VrgUiStyle.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "CanvasItem.h"
#include "TextureResource.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace VrgUi
{
using namespace VrgUi::Theme;

// Canvas drawing helpers in DPI-scaled pixels.
struct FHudPaint
{
	UCanvas* C = nullptr;
	float S = 1.f;
	TArray<FBox2D>* Hits = nullptr;   // panel rectangles that take clicks away from the scene

	void Block(float X, float Y, float W, float H) const { if (Hits && W > 0.f && H > 0.f) Hits->Add(FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H))); }

	void Rect(float X, float Y, float W, float H, const FLinearColor& Col) const
	{
		if (W <= 0.f || H <= 0.f) return;
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(W, H), Col);
		T.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(T);
	}
	void Frame(float X, float Y, float W, float H, const FLinearColor& Col) const
	{
		Rect(X, Y, W, 1.f, Col); Rect(X, Y + H - 1.f, W, 1.f, Col);
		Rect(X, Y + 1.f, 1.f, H - 2.f, Col); Rect(X + W - 1.f, Y + 1.f, 1.f, H - 2.f, Col);
	}
	void Panel(float X, float Y, float W, float H) const { Rect(X, Y, W, H, VrgUi::Theme::Panel()); Frame(X, Y, W, H, Line()); Block(X, Y, W, H); }
	void Line2(float X0, float Y0, float X1, float Y1, const FLinearColor& Col, float Thick) const
	{
		FCanvasLineItem L(FVector2D(X0, Y0), FVector2D(X1, Y1));
		L.SetColor(Col); L.LineThickness = Thick; L.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(L);
	}
	void Tri(const FVector2D& A, const FVector2D& B, const FVector2D& D, const FLinearColor& Col) const
	{
		FCanvasTriangleItem T(A, B, D, GWhiteTexture);
		T.SetColor(Col); T.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(T);
	}
	FSlateFontInfo Font(float Size, bool bBold) const
	{
		// The canvas draws a Slate font only through a runtime-cached UFont (FCanvasSimpleTextItem::HasValidText needs
		// Font != null), so the font object is the engine's small font (Roboto composite, also used by the existing labels).
		const float Px = FMath::Max(1.f, FMath::RoundToFloat(Size * S));
		const UFont* Obj = GEngine ? GEngine->GetSmallFont() : nullptr;
		if (Obj && Obj->FontCacheType == EFontCacheType::Runtime) return FSlateFontInfo(Obj, Px, bBold ? FName("Bold") : FName("Regular"));
		FSlateFontInfo F = FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", 10);
		F.Size = Px;
		return F;
	}
	FVector2D Measure(const FString& T, float Size, bool bBold = false) const
	{
		const FSlateFontInfo F = Font(Size, bBold);
		if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
			return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(T, F);
		return FVector2D(T.Len() * F.Size * .6f, F.Size * 1.35f);
	}
	// Draws T with its box's top-left at (X, Y); returns the width.
	float Text(const FString& T, float X, float Y, float Size, const FLinearColor& Col, bool bBold = false) const
	{
		FCanvasTextItem I(FVector2D(FMath::RoundToFloat(X), FMath::RoundToFloat(Y)), FText::FromString(T), Font(Size, bBold), Col);
		C->DrawItem(I);
		return Measure(T, Size, bBold).X;
	}
	// Text vertically centred on Cy.
	float TextMid(const FString& T, float X, float Cy, float Size, const FLinearColor& Col, bool bBold = false) const
	{
		return Text(T, X, Cy - .5f * Measure(T, Size, bBold).Y, Size, Col, bBold);
	}
	float TextRight(const FString& T, float Right, float Y, float Size, const FLinearColor& Col, bool bBold = false) const
	{
		const float W = Measure(T, Size, bBold).X;
		Text(T, Right - W, Y, Size, Col, bBold);
		return W;
	}
	FString Fit(const FString& T, float MaxW, float Size) const
	{
		if (Measure(T, Size).X <= MaxW) return T;
		FString Cut = T;
		while (Cut.Len() > 1 && Measure(Cut + TEXT("…"), Size).X > MaxW) Cut.LeftChopInline(1);
		return Cut + TEXT("…");
	}
	// A key cap centred on Cy; returns its width.
	float Key(const FString& K, float X, float Cy, const FLinearColor& Fill = FLinearColor(1, 1, 1, .07f)) const
	{
		const FVector2D Sz = Measure(K, 9.5f, true);
		const float W = Sz.X + 12.f * S, H = 19.f * S;
		Rect(X, Cy - .5f * H, W, H, Fill);
		Frame(X, Cy - .5f * H, W, H, FLinearColor(1, 1, 1, .22f));
		Rect(X + 1.f, Cy + .5f * H - 2.f, W - 2.f, 1.f, FLinearColor(0, 0, 0, .35f));   // bottom edge of the cap
		Text(K, X + 6.f * S, Cy - .5f * Sz.Y - .5f * S, 9.5f, VrgUi::Theme::Text(), true);
		return W;
	}
	// Key + description pair; returns the width.
	float KeyHint(const FString& K, const FString& Desc, float X, float Cy) const
	{
		float W = Key(K, X, Cy);
		W += 6.f * S;
		W += TextMid(Desc, X + W, Cy, 10.f, Text2());
		return W;
	}
	float KeyHintWidth(const FString& K, const FString& Desc) const
	{
		return Measure(K, 9.5f, true).X + 12.f * S + 6.f * S + Measure(Desc, 10.f).X;
	}
	void Pill(const FString& T, float X, float Cy, const FLinearColor& Col, float& OutW, bool bDot = false, float DotAlpha = 1.f) const
	{
		const FVector2D Sz = Measure(T, 10.f, true);
		const float Dot = bDot ? 12.f * S : 0.f;
		const float W = Sz.X + 20.f * S + Dot, H = 22.f * S;
		Rect(X, Cy - .5f * H, W, H, WithAlpha(Col, .20f));
		Frame(X, Cy - .5f * H, W, H, WithAlpha(Col, .65f));
		if (bDot) Rect(X + 10.f * S, Cy - 3.f * S, 6.f * S, 6.f * S, WithAlpha(Col, DotAlpha));
		Text(T, X + 10.f * S + Dot, Cy - .5f * Sz.Y, 10.f, FLinearColor::White, true);
		OutW = W;
	}
};
}
