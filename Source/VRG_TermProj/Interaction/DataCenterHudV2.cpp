// 2026-11-24 UI 데모(vrg.Ui.Style 1): 새 HUD. 계산 상태를 바꾸지 않고, 게임 스레드에서 이미 있는 값(대시보드 요약·
// 평가 시계열·장애 기록·계측)을 읽어 그린다(규약 5). 온도는 여기서만 °C로 바꾼다(규약 2).
// 화면 크기·간격·색은 UI 선택이며 물리 값이 아니다. 1080p 기준 픽셀에 UMG와 같은 DPI 배율을 곱한다.
#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgUiStyle.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Visualization/AirFlowView.h"
#include "Simulation/Core/SimUnits.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/UserInterfaceSettings.h"
#include "CanvasItem.h"
#include "TextureResource.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

using namespace VrgUi::Theme;

struct FVrgHudV2State
{
	double NextSampleRealS = -1.0;
	FVrgDashboardSummary Summary;
	bool bHaveSummary = false;
	TArray<FVrgSeriesPoint> Series;
	TArray<double> PartT, PartK;                 // own history of the hottest part (the series has no such channel)
	double HottestPartK = 0.0, MaxInletNowK = 0.0;
	FString HottestPartRack;
	TArray<TPair<FString, FString>> ShortNames;   // actor name -> "랙 #n" (dashboard row order), display only
	int32 Racks = 0, Spines = 0, Cdus = 0, Crahs = 0, Plants = 0;
	TArray<VrgUi::FEventRow> Events;
	int32 EventTotal = 0, AlarmCount = 0;
	double LastSimT = -1.0;
};

namespace
{
	// Canvas drawing helpers in DPI-scaled pixels.
	struct FPaint
	{
		UCanvas* C = nullptr;
		float S = 1.f;

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
		void Panel(float X, float Y, float W, float H) const { Rect(X, Y, W, H, VrgUi::Theme::Panel()); Frame(X, Y, W, H, Line()); }
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

	double ToC(double K) { return SimUnits::KelvinToCelsius(K); }

	ADataCenterSimulationActor* FindSim(UWorld* World)
	{
		if (!World) return nullptr;
		for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It) return *It;
		return nullptr;
	}

	FString PowerText(double W, FString& OutUnit)
	{
		if (FMath::Abs(W) >= 1.0e6) { OutUnit = TEXT("MW"); return FString::Printf(TEXT("%.2f"), W * 1e-6); }
		OutUnit = TEXT("kW"); return FString::Printf(TEXT("%.1f"), W * 1e-3);
	}

	const TCHAR* LeakToolText(int32 Mode)
	{
		return Mode == 1 ? TEXT("5 mm") : Mode == 2 ? TEXT("30 mm") : Mode == 3 ? TEXT("파단") : Mode == 4 ? TEXT("수리") : TEXT("끔");
	}

	// Sparkline of (T, V) over [T0, T1]: soft fill to the bottom, line, last-value dot, optional threshold.
	void Spark(const FPaint& P, float X, float Y, float W, float H, const TArray<double>& T, const TArray<double>& V,
		double T0, double T1, double MinSpan, double Threshold, const FLinearColor& Col)
	{
		P.Rect(X, Y + H, W, 1.f, LineSoft());
		TArray<double> Vis;
		for (int32 I = 0; I < V.Num(); ++I) if (T[I] >= T0 && FMath::IsFinite(V[I])) Vis.Add(V[I]);
		double Lo, Hi;
		VrgUi::SparkRange(Vis, MinSpan, Threshold, Lo, Hi);
		auto Px = [&](double Tt) { return X + float((Tt - T0) / FMath::Max(T1 - T0, 1e-9)) * W; };
		auto Py = [&](double Vv) { return Y + H - float((Vv - Lo) / FMath::Max(Hi - Lo, 1e-12)) * H; };
		if (FMath::IsFinite(Threshold))
		{
			const float Ty = Py(Threshold);
			for (float Dx = 0.f; Dx < W; Dx += 6.f * P.S) P.Rect(X + Dx, Ty, FMath::Min(3.f * P.S, W - Dx), 1.f, WithAlpha(Amber(), .7f));
		}
		if (Vis.Num() < 2)
		{
			P.TextMid(TEXT("자료 모으는 중"), X, Y + .5f * H, 9.f, Text3());
			return;
		}
		FVector2D Prev(0, 0); bool bHave = false;
		for (int32 I = 0; I < V.Num(); ++I)
		{
			if (T[I] < T0 || !FMath::IsFinite(V[I])) continue;
			const FVector2D Cur(Px(T[I]), FMath::Clamp(Py(V[I]), Y, Y + H));
			if (bHave)
			{
				const FVector2D B0(Prev.X, Y + H), B1(Cur.X, Y + H);
				P.Tri(Prev, Cur, B1, WithAlpha(Col, .16f));
				P.Tri(Prev, B1, B0, WithAlpha(Col, .16f));
				P.Line2(Prev.X, Prev.Y, Cur.X, Cur.Y, Col, 1.5f * P.S);
			}
			Prev = Cur; bHave = true;
		}
		if (bHave) { P.Rect(Prev.X - 2.5f * P.S, Prev.Y - 2.5f * P.S, 5.f * P.S, 5.f * P.S, Col); }
	}

	FString KindLabel(const AActor* A, FLinearColor& OutCol)
	{
		OutCol = Gray();
		if (Cast<ARackThermalActor>(A)) { OutCol = Accent(); return TEXT("GB200 NVL72 랙"); }
		if (Cast<ASpineCabinetActor>(A)) { OutCol = Accent(); return TEXT("스파인 캐비닛"); }
		if (Cast<ACoolingDistributionUnitActor>(A)) { OutCol = Accent(); return TEXT("CDU"); }
		if (const auto* Crah = Cast<ACrahUnitActor>(A)) { OutCol = Teal(); return Crah->Kind == ECrahUnitKind::Perimeter ? TEXT("벽면 CRAH") : TEXT("인로우 쿨러"); }
		if (const auto* Plant = Cast<APlantEquipmentActor>(A)) { OutCol = Purple(); return APlantEquipmentActor::KindName(Plant->Kind); }
		if (const auto* Bar = Cast<ALayoutBarrierActor>(A)) return Bar->Kind == EBarrierKind::Curtain ? TEXT("차단막") : TEXT("벽");
		return A ? A->GetClass()->GetName() : FString();
	}
}

void ADataCenterLayoutHud::DrawMarquee()
{
	const auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	FBox2D Rect;
	if (!Controller || !Controller->GetLayoutMarqueeRect(Rect)) return;
	const FLinearColor Color(.2f, .8f, 1.f);
	if (VrgUi::IsNewStyle()) DrawRect(FLinearColor(.2f, .55f, .9f, .10f), Rect.Min.X, Rect.Min.Y, Rect.Max.X - Rect.Min.X, Rect.Max.Y - Rect.Min.Y);
	DrawLine(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Min.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Max.Y, Rect.Min.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Min.X, Rect.Max.Y, Rect.Min.X, Rect.Min.Y, Color, 2.f);
}

// The air view colour bar in the new panel style (same legends and colormap as DrawAirLegend).
void ADataCenterLayoutHud::DrawAirLegendV2(float BottomMargin)
{
	ADataCenterSimulationActor* Sim = FindSim(GetWorld());
	if (!Canvas || !Sim) return;
	TArray<FAirFlowLegend> Legends;
	FString Note;
	Sim->GetAirFlowLegends(Legends, Note);
	if (Legends.IsEmpty()) return;
	FPaint P;
	P.C = Canvas;
	P.S = FMath::Clamp(GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(int32(Canvas->ClipX), int32(Canvas->ClipY))), .5f, 3.f);
	const float S = P.S, Pad = 14.f * S, BarW = 320.f * S, BarH = 10.f * S;
	const float Bw = BarW + 2.f * Pad, Bh = 38.f * S + Legends.Num() * 54.f * S;
	const float X0 = Canvas->ClipX - 16.f * S - Bw, Y0 = Canvas->ClipY - BottomMargin - Bh;
	P.Panel(X0, Y0, Bw, Bh);
	P.Text(P.Fit(Note, BarW, 9.f), X0 + Pad, Y0 + 12.f * S, 9.f, Text2());
	float Y = Y0 + 34.f * S;
	for (const FAirFlowLegend& L : Legends)
	{
		P.Text(L.Title, X0 + Pad, Y, 10.f, VrgUi::Theme::Text(), true);
		P.TextRight(L.Unit, X0 + Pad + BarW, Y + 1.f * S, 9.f, Text3());
		Y += 19.f * S;
		constexpr int32 Steps = 96;
		for (int32 K = 0; K < Steps; ++K)
			P.Rect(X0 + Pad + BarW * K / Steps, Y, BarW / Steps + 1.f, BarH, AirFlowView::Colormap((K + .5) / Steps));
		Y += BarH + 4.f * S;
		for (int32 K = 0; K <= 4; ++K)
		{
			const FString V = FString::Printf(TEXT("%g"), FMath::RoundToDouble((L.Min + (L.Max - L.Min) * K / 4.0) * 10.0) / 10.0);
			const float Tw = P.Measure(V, 9.f).X;
			const float Tx = FMath::Clamp(X0 + Pad + BarW * K / 4.f - .5f * Tw, X0 + Pad, X0 + Pad + BarW - Tw);
			P.Rect(X0 + Pad + BarW * K / 4.f - (K == 4 ? 1.f : 0.f), Y - 4.f * S, 1.f, 3.f * S, Text3());
			P.Text(V, Tx, Y, 9.f, Text2());
		}
		Y += 31.f * S - 4.f * S;
	}
}

void ADataCenterLayoutHud::DrawHudV2()
{
	auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	UWorld* World = GetWorld();
	if (!Canvas || !Controller || !World) return;
	if (!V2) V2 = MakeShared<FVrgHudV2State>();
	FVrgHudV2State& St = *V2;
	FPaint P;
	P.C = Canvas;
	P.S = FMath::Clamp(GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(int32(Canvas->ClipX), int32(Canvas->ClipY))), .5f, 3.f);
	const float S = P.S, W = Canvas->ClipX, H = Canvas->ClipY;
	ADataCenterSimulationActor* Sim = FindSim(World);
	const bool bSim = Sim && Sim->GetSimMode() == ESimMode::Simulation;
	const double RealS = World->GetRealTimeSeconds();

	// ---- sample (4 Hz of real time; the drawn numbers then hold still between samples) ----
	if (RealS >= St.NextSampleRealS)
	{
		St.NextSampleRealS = RealS + .25;
		St.Racks = St.Spines = St.Cdus = St.Crahs = St.Plants = 0;
		for (TActorIterator<ARackThermalActor> It(World); It; ++It) ++St.Racks;
		for (TActorIterator<ASpineCabinetActor> It(World); It; ++It) ++St.Spines;
		for (TActorIterator<ACoolingDistributionUnitActor> It(World); It; ++It) ++St.Cdus;
		for (TActorIterator<ACrahUnitActor> It(World); It; ++It) ++St.Crahs;
		for (TActorIterator<APlantEquipmentActor> It(World); It; ++It) ++St.Plants;
		St.bHaveSummary = false;
		if (Sim)
		{
			St.Summary = Sim->BuildDashboardSummary();
			St.bHaveSummary = true;
			const double SimT = Sim->GetSimClockTimeS();
			if (!bSim || SimT < St.LastSimT) { St.PartT.Reset(); St.PartK.Reset(); }
			St.LastSimT = SimT;
			St.Series.Reset();
			if (bSim && St.Summary.bEvalReady) Sim->GetEvaluation().GetSeries(-1.0, St.Series);
			St.HottestPartK = St.MaxInletNowK = 0.0; St.HottestPartRack.Reset();
			if (bSim)
			{
				const FVrgDashboardRacks Racks = Sim->BuildDashboardRacks(INDEX_NONE);
				St.ShortNames.Reset();
				int32 RackNo = 0, SpineNo = 0;
				for (const FVrgDashboardRackRow& R : Racks.Rows)
				{
					const FString Short = R.Kind == TEXT("rack") ? FString::Printf(TEXT("랙 #%d"), ++RackNo) : FString::Printf(TEXT("스파인 #%d"), ++SpineNo);
					St.ShortNames.Add({ R.Name, Short });
					if (R.HottestPartK > St.HottestPartK) { St.HottestPartK = R.HottestPartK; St.HottestPartRack = Short; }
					St.MaxInletNowK = FMath::Max(St.MaxInletNowK, R.InletNowK);
				}
				if (St.HottestPartK > 0.0 && (St.PartT.IsEmpty() || SimT > St.PartT.Last()))
				{
					St.PartT.Add(SimT); St.PartK.Add(St.HottestPartK);
					if (St.PartT.Num() > 2400) { St.PartT.RemoveAt(0, 400); St.PartK.RemoveAt(0, 400); }
				}
			}
			VrgUi::BuildEventRows(Sim->GetFaultMonitor(), &Sim->GetInstruments(), 9, St.Events, St.EventTotal);
			// Instrument labels carry actor names (RackThermalActor_UAID_...); show the short rack number instead.
			for (VrgUi::FEventRow& E : St.Events) E.Text = VrgUi::ShortenNames(E.Text, St.ShortNames);
			St.AlarmCount = Sim->GetFaultMonitor().GetFaults().Num();
			const FInstrumentBank& Ins = Sim->GetInstruments();
			for (int32 I = 0; I < Ins.Num(); ++I) St.AlarmCount += Ins.IsLeakAlarm(I) ? 1 : 0;
		}
	}

	const float Margin = 16.f * S, BarH = 46.f * S;
	const float LeftW = 288.f * S, RightW = 380.f * S;

	// ---- help overlay (H) ----
	TArray<FString> HelpLines;
	Controller->GetHelpLines(HelpLines);
	const bool bHelp = Controller->bShowHelp && HelpLines.Num() > 1;

	// ---- top status bar ----
	{
		P.Rect(0, 0, W, BarH, PanelHi());
		P.Rect(0, BarH - 1.f, W, 1.f, Line());
		float X = Margin;
		const float Cy = .5f * BarH;
		X += P.TextMid(TEXT("VRG"), X, Cy, 13.f, Text(), true) + 8.f * S;
		X += P.TextMid(TEXT("AI 데이터센터 냉각 디지털 트윈"), X, Cy, 10.f, Text2()) + 18.f * S;
		float PillW = 0.f;
		const float Pulse = .55f + .45f * FMath::Sin(float(RealS) * 4.f);
		P.Pill(bSim ? TEXT("시뮬레이션") : TEXT("배치 모드"), X, Cy, bSim ? Teal() : Accent(), PillW, bSim, Pulse);
		X += PillW + 14.f * S;
		auto Seg = [&](const FString& Label, const FString& Value, const FLinearColor& ValueCol)
		{
			P.Rect(X, 10.f * S, 1.f, BarH - 20.f * S, Line());
			X += 12.f * S;
			const float Lw = P.Text(Label, X, 7.f * S, 8.5f, Text3());
			const float Vw = P.Text(Value, X, 21.f * S, 11.f, ValueCol, true);
			X += FMath::Max(Lw, Vw) + 14.f * S;
		};
		FString MapName = World->GetMapName();
		MapName.RemoveFromStart(World->StreamingLevelsPrefix);
		if (!bSim)
		{
			Seg(TEXT("레벨"), MapName, Text());
			Seg(TEXT("IT 장비"), FString::Printf(TEXT("랙 %d · 스파인 %d · CDU %d"), St.Racks, St.Spines, St.Cdus), Text());
			Seg(TEXT("냉각 장비"), St.Plants > 0 ? FString::Printf(TEXT("CRAH %d · 설비 마당 %d"), St.Crahs, St.Plants) : FString::Printf(TEXT("CRAH %d · 설비 마당 자동"), St.Crahs), Text());
			Seg(TEXT("카메라 속도"), FString::Printf(TEXT("%.1f m/s"), Controller->GetLayoutCameraSpeedMs()), Text());
		}
		else
		{
			const double Req = ADataCenterSimulationActor::GetRequestedSimSpeed();
			const double Got = St.bHaveSummary ? St.Summary.AchievedSimSpeed : 0.0;
			Seg(TEXT("시뮬레이션 시각"), VrgUi::FormatClock(Sim->GetSimClockTimeS()), Text());
			Seg(TEXT("배속 (요청 · 실제)"), FString::Printf(TEXT("x%g · x%.2f"), Req, Got), Got > 0.0 && Got < .9 * Req ? Amber() : Text());
			FString Unit; const FString It = PowerText(St.bHaveSummary ? St.Summary.ItNowW : 0.0, Unit);
			Seg(TEXT("IT 부하"), It + TEXT(" ") + Unit, Text());
			Seg(TEXT("공기 보기"), AirFlowView::Name(Sim->GetAirView()), Text());
			Seg(TEXT("레벨"), MapName, Text2());
		}
		// Right side: alarms, help, classic UI.
		float R = W - Margin;
		R -= P.KeyHintWidth(TEXT("F2"), TEXT("기존 UI"));
		P.KeyHint(TEXT("F2"), TEXT("기존 UI"), R, Cy);
		R -= 16.f * S + P.KeyHintWidth(TEXT("H"), TEXT("단축키"));
		P.KeyHint(TEXT("H"), TEXT("단축키"), R, Cy);
		R -= 16.f * S;
		if (bSim)
		{
			const FString A = St.AlarmCount > 0 ? FString::Printf(TEXT("경보 %d"), St.AlarmCount) : FString(TEXT("경보 없음"));
			const float Aw = P.Measure(A, 10.f, true).X + 20.f * S;
			if (St.AlarmCount > 0) { float Pw; P.Pill(A, R - Aw, Cy, Red(), Pw); }
			else P.TextMid(A, R - Aw + 10.f * S, Cy, 10.f, Text3());
		}
	}

	const float Top = BarH + 14.f * S;

	if (!bSim)
	{
		// ---- edit banner (room bounds / ceiling / hole editing) ----
		float BannerBottom = Top;
		{
			TArray<FString> Lines;
			if ((Controller->IsRoomBoundsEditing() || Controller->IsRoomCeilingEditing() || Controller->IsRoomCeilingHoleEditing()) && HelpLines.Num() == 1)
				Lines.Add(HelpLines[0].Replace(TEXT(" | "), TEXT("   ·   ")));
			TArray<FString> More;
			Controller->GetRoomBoundsMessage().ParseIntoArrayLines(More);
			FString CeilingMsg = Controller->GetRoomCeilingEditMessage();
			TArray<FString> More2; CeilingMsg.ParseIntoArrayLines(More2);
			More.Append(More2);
			const int32 NumHead = Lines.Num();
			Lines.Append(More);
			if (!Lines.IsEmpty())
			{
				const float X0 = Margin + 272.f * S + 16.f * S, X1 = W - Margin - 300.f * S - 16.f * S;
				const float Bw = FMath::Max(X1 - X0, 200.f * S), LineH = 20.f * S;
				const float Bh = 16.f * S + Lines.Num() * LineH;
				P.Rect(X0, Top, Bw, Bh, Panel());
				P.Frame(X0, Top, Bw, Bh, WithAlpha(Amber(), .55f));
				P.Rect(X0, Top, 3.f * S, Bh, Amber());
				FRoomAirDomainSettings Rp; bool bValidRoom = true; Controller->GetRoomBoundsPreview(Rp, bValidRoom);
				for (int32 I = 0; I < Lines.Num(); ++I)
				{
					const bool bHead = I < NumHead;
					P.Text(P.Fit(Lines[I], Bw - 28.f * S, 10.f), X0 + 14.f * S, Top + 8.f * S + I * LineH, 10.f, bHead ? Text() : (bValidRoom ? Amber() : Red()), bHead);
				}
				BannerBottom = Top + Bh;
			}
		}
		(void)BannerBottom;

		// ---- inspector (selection) ----
		const TArray<AActor*> Sel = Controller->GetSelectedLayoutActors();
		if (!Sel.IsEmpty())
		{
			const float Iw = 300.f * S, X0 = W - Margin - Iw;
			float Y = Top;
			const float Pad = 14.f * S;
			if (Sel.Num() == 1)
			{
				AActor* A = Sel[0];
				FLinearColor KCol; const FString Kind = KindLabel(A, KCol);
				FVector SizeM = FVector::ZeroVector;
				const bool bSize = ADataCenterDebugController::PlaceableBoxM(A, SizeM);
				const FVector PosM = SimUnits::UnrealToMeters(A->GetActorLocation());   // rule 1: cm -> m at the boundary
				const float Ih = 176.f * S;
				P.Panel(X0, Y, Iw, Ih);
				P.Rect(X0, Y, Iw, 3.f * S, KCol);
				P.Text(Kind, X0 + Pad, Y + 14.f * S, 12.f, Text(), true);
				P.Text(P.Fit(A->GetName(), Iw - 2.f * Pad, 9.f), X0 + Pad, Y + 34.f * S, 9.f, Text3());
				float Ry = Y + 58.f * S;
				auto Row = [&](const FString& K, const FString& V)
				{
					P.Text(K, X0 + Pad, Ry, 10.f, Text2());
					P.TextRight(V, X0 + Iw - Pad, Ry, 10.f, Text(), true);
					Ry += 21.f * S;
				};
				Row(TEXT("위치 X · Y"), FString::Printf(TEXT("%.2f · %.2f m"), PosM.X, PosM.Y));
				if (bSize) Row(TEXT("크기 W × D × H"), FString::Printf(TEXT("%.2f × %.2f × %.2f m"), SizeM.X, SizeM.Y, SizeM.Z));
				Row(TEXT("방향"), FString::Printf(TEXT("%.0f°"), FRotator::NormalizeAxis(A->GetActorRotation().Yaw)));
				P.Rect(X0 + Pad, Y + Ih - 38.f * S, Iw - 2.f * Pad, 1.f, LineSoft());
				float Kx = X0 + Pad; const float Ky = Y + Ih - 19.f * S;
				Kx += P.KeyHint(TEXT("R"), TEXT("회전"), Kx, Ky) + 12.f * S;
				Kx += P.KeyHint(TEXT("Ctrl+D"), TEXT("복제"), Kx, Ky) + 12.f * S;
				P.KeyHint(TEXT("Del"), TEXT("삭제"), Kx, Ky);
			}
			else
			{
				TMap<FString, int32> Counts; TMap<FString, FLinearColor> Cols;
				for (AActor* A : Sel) { FLinearColor C; const FString K = KindLabel(A, C); Counts.FindOrAdd(K)++; Cols.Add(K, C); }
				const float Ih = (92.f + 21.f * Counts.Num()) * S;
				P.Panel(X0, Y, Iw, Ih);
				P.Rect(X0, Y, Iw, 3.f * S, Accent());
				P.Text(FString::Printf(TEXT("%d개 선택"), Sel.Num()), X0 + Pad, Y + 14.f * S, 12.f, Text(), true);
				float Ry = Y + 42.f * S;
				for (const TPair<FString, int32>& C : Counts)
				{
					P.Rect(X0 + Pad, Ry + 4.f * S, 3.f * S, 11.f * S, Cols[C.Key]);
					P.Text(C.Key, X0 + Pad + 10.f * S, Ry, 10.f, Text2());
					P.TextRight(FString::Printf(TEXT("%d"), C.Value), X0 + Iw - Pad, Ry, 10.f, Text(), true);
					Ry += 21.f * S;
				}
				float Kx = X0 + Pad; const float Ky = Y + Ih - 19.f * S;
				Kx += P.KeyHint(TEXT("드래그"), TEXT("함께 이동"), Kx, Ky) + 12.f * S;
				P.KeyHint(TEXT("Del"), TEXT("모두 삭제"), Kx, Ky);
			}
		}

		// ---- context hint bar ----
		TArray<TPair<FString, FString>> Hints;
		if (Controller->IsAddingRack())
			Hints = { { TEXT("클릭"), TEXT("놓기") }, { TEXT("R"), TEXT("90° 회전") }, { TEXT("Esc"), TEXT("취소") } };
		else if (Controller->IsRoomBoundsEditing() || Controller->IsRoomCeilingEditing() || Controller->IsRoomCeilingHoleEditing())
			Hints = { { TEXT("드래그"), TEXT("핸들 끌기") }, { TEXT("Ctrl+Z"), TEXT("되돌리기") }, { TEXT("Esc"), TEXT("끝내기") }, { TEXT("Enter"), TEXT("시뮬레이션") } };
		else if (Controller->GetLayoutGizmoTarget())
			Hints = { { TEXT("W"), TEXT("이동") }, { TEXT("E"), TEXT("회전") }, { TEXT("R"), TEXT("크기") }, { TEXT("Space"), TEXT("기즈모 전환") }, { TEXT("Alt"), TEXT("스냅 끔") }, { TEXT("Del"), TEXT("삭제") } };
		else if (!Sel.IsEmpty())
			Hints = { { TEXT("드래그"), TEXT("이동") }, { TEXT("R"), TEXT("회전") }, { TEXT("Ctrl+D"), TEXT("복제") }, { TEXT("Del"), TEXT("삭제") }, { TEXT("F"), TEXT("선택 보기") }, { TEXT("Shift+클릭"), TEXT("추가 선택") } };
		else
			Hints = { { TEXT("클릭"), TEXT("선택") }, { TEXT("빈 곳 드래그"), TEXT("사각형 선택") }, { TEXT("우클릭+WASD"), TEXT("카메라") }, { TEXT("Ctrl+Z"), TEXT("되돌리기") }, { TEXT("C"), TEXT("컨테인먼트") }, { TEXT("Enter"), TEXT("시뮬레이션 시작") } };
		float Total = 0.f;
		for (const auto& Hn : Hints) Total += P.KeyHintWidth(Hn.Key, Hn.Value) + 20.f * S;
		Total += 8.f * S;
		const float Hh = 40.f * S, X0 = .5f * (W - Total), Y0 = H - Margin - Hh;
		P.Panel(X0, Y0, Total, Hh);
		float Kx = X0 + 14.f * S;
		for (const auto& Hn : Hints) Kx += P.KeyHint(Hn.Key, Hn.Value, Kx, Y0 + .5f * Hh) + 20.f * S;
	}
	else
	{
		// ---- KPI cards (left) ----
		const FVrgDashboardSummary& Su = St.Summary;
		const double TNow = Sim->GetSimClockTimeS();
		// Display window [s], UI choice: the last 120 s; early in a run the line starts at the left edge.
		const double T0 = FMath::Max(TNow - 120.0, 0.0), T1 = FMath::Max(TNow, T0 + 30.0);
		TArray<double> ST, SIt, SIn, SPue, SPueT;
		for (const FVrgSeriesPoint& Pt : St.Series)
		{
			ST.Add(Pt.TimeS); SIt.Add(Pt.ItW); SIn.Add(ToC(Pt.InletMaxK));
			if (Pt.bPartialPueValid) { SPueT.Add(Pt.TimeS); SPue.Add(Pt.PartialPue); }
		}
		TArray<double> SPartC; for (double K : St.PartK) SPartC.Add(ToC(K));
		const float X0 = Margin, Cw = LeftW, Ch = 92.f * S, Gap = 8.f * S, Pad = 14.f * S;
		float Y = Top;
		auto Card = [&](const FString& Label, const FString& Qual, const FString& Value, const FString& Unit, const FLinearColor& StatusCol,
			const TArray<double>& T, const TArray<double>& V, double MinSpan, double Thresh)
		{
			P.Panel(X0, Y, Cw, Ch);
			P.Rect(X0, Y, 3.f * S, Ch, StatusCol);
			const float Lw = P.Text(Label, X0 + Pad, Y + 10.f * S, 10.f, Text2());
			P.TextRight(P.Fit(Qual, Cw - 2.f * Pad - Lw - 10.f * S, 8.5f), X0 + Cw - Pad, Y + 11.f * S, 8.5f, Text3());
			const float Vw = P.Text(Value, X0 + Pad, Y + 26.f * S, 19.f, Text(), true);
			P.Text(Unit, X0 + Pad + Vw + 5.f * S, Y + 35.f * S, 10.f, Text2());
			Spark(P, X0 + Pad, Y + 60.f * S, Cw - 2.f * Pad, 22.f * S, T, V, T0, T1, MinSpan, Thresh, StatusCol);
			Y += Ch + Gap;
		};
		FString Unit; const FString ItV = PowerText(Su.ItNowW, Unit);
		Card(TEXT("IT 부하"), TEXT("서버 팬·PSU 손실 포함"), ItV, Unit, Accent(), ST, SIt, 2000.0, VrgUi::NoThreshold());
		const double InletC = St.Series.Num() ? ToC(St.Series.Last().InletMaxK) : ToC(St.MaxInletNowK);
		const FLinearColor InCol = InletC <= 27.0 ? Teal() : InletC <= 32.0 ? Amber() : Red();   // ASHRAE recommended / allowable (display bands)
		Card(TEXT("최고 흡기 온도"), TEXT("1 s 평균 · 권장 27 °C"), FString::Printf(TEXT("%.1f"), InletC), TEXT("°C"), InCol, ST, SIn, .5, 27.0);
		const double PartC = ToC(St.HottestPartK);
		const FLinearColor PartCol = PartC < 60.0 ? Teal() : PartC < 75.0 ? Amber() : Red();     // same bands as the cabinet labels
		Card(TEXT("최고 부품 온도"), St.HottestPartRack, St.HottestPartK > 0.0 ? FString::Printf(TEXT("%.1f"), PartC) : FString(TEXT("-")), TEXT("°C"), PartCol,
			St.PartT, SPartC, .5, PartC > 75.0 ? 85.0 : VrgUi::NoThreshold());
		Card(TEXT("부분 PUE"), TEXT("(IT + CRAH 팬 + 펌프) / IT"), Su.bPartialPueValid ? FString::Printf(TEXT("%.3f"), Su.PartialPueWindow) : FString(TEXT("-")), TEXT(""),
			Accent(), SPueT, SPue, .004, VrgUi::NoThreshold());
		// Heat removal split.
		{
			const float Sh = 92.f * S;
			P.Panel(X0, Y, Cw, Sh);
			P.Text(TEXT("열 제거 분담"), X0 + Pad, Y + 10.f * S, 10.f, Text2());
			P.TextRight(TEXT("지금 스텝"), X0 + Cw - Pad, Y + 11.f * S, 8.5f, Text3());
			const double Cool = FMath::Max(Su.CoolantRemovedNowW, 0.0), Air = FMath::Max(Su.AirRemovedNowW, 0.0), Sum = Cool + Air;
			const float Bx = X0 + Pad, By = Y + 32.f * S, Bw = Cw - 2.f * Pad, Bh = 8.f * S;
			P.Rect(Bx, By, Bw, Bh, LineSoft());
			if (Sum > 0.0)
			{
				const float Cwid = float(Cool / Sum) * Bw;
				P.Rect(Bx, By, Cwid, Bh, Accent());
				P.Rect(Bx + Cwid + 1.f, By, Bw - Cwid - 1.f, Bh, Teal());
				auto SplitRow = [&](float Ly, const FLinearColor& C, const TCHAR* Name, double V)
				{
					FString U; const FString Vt = PowerText(V, U);
					P.Rect(Bx, Ly + 4.f * S, 8.f * S, 8.f * S, C);
					P.Text(Name, Bx + 14.f * S, Ly, 10.f, Text2());
					P.TextRight(FString::Printf(TEXT("%s %s  ·  %.1f%%"), *Vt, *U, 100.0 * V / Sum), Bx + Bw, Ly, 10.f, VrgUi::Theme::Text(), true);
				};
				SplitRow(By + 18.f * S, Accent(), TEXT("수랭 (CDU·라디에이터)"), Cool);
				SplitRow(By + 38.f * S, Teal(), TEXT("공기 (CRAH 코일)"), Air);
			}
			else P.Text(TEXT("첫 결합 스텝을 기다리는 중"), Bx, By + 16.f * S, 9.5f, Text3());
		}

		// ---- event log (right) ----
		{
			const float X1 = W - Margin - RightW, Rh = 24.f * S, Hh = 40.f * S;
			const int32 Rows = FMath::Max(St.Events.Num(), 1);
			const float Ph = Hh + Rows * Rh + (St.Events.IsEmpty() ? 22.f * S : 8.f * S);
			P.Panel(X1, Top, RightW, Ph);
			P.Text(TEXT("사건 기록"), X1 + 14.f * S, Top + 12.f * S, 11.f, Text(), true);
			P.TextRight(FString::Printf(TEXT("전체 %d건"), St.EventTotal), X1 + RightW - 14.f * S, Top + 14.f * S, 9.f, Text3());
			P.Rect(X1 + 14.f * S, Top + Hh - 4.f * S, RightW - 28.f * S, 1.f, LineSoft());
			float Ry = Top + Hh;
			if (St.Events.IsEmpty())
			{
				P.Text(TEXT("기록된 사건이 없습니다"), X1 + 14.f * S, Ry + 2.f * S, 10.f, Text2());
				P.Text(TEXT("L 누수 도구나 콘솔 vrg.Demo leak 으로 상황을 만들어 보세요"), X1 + 14.f * S, Ry + 20.f * S, 9.f, Text3());
			}
			for (int32 I = 0; I < St.Events.Num(); ++I)
			{
				const VrgUi::FEventRow& E = St.Events[I];
				const FLinearColor Sev = E.Severity >= 2 ? Red() : E.Severity == 1 ? Amber() : Cyan();
				const float Cy = Ry + .5f * Rh;
				if (I == 0 && TNow - E.TimeS < 5.0) P.Rect(X1 + 1.f, Ry, RightW - 2.f, Rh, WithAlpha(Sev, .10f));   // fresh event
				P.Rect(X1 + 14.f * S, Cy - 3.f * S, 6.f * S, 6.f * S, Sev);
				P.TextMid(VrgUi::FormatClock(E.TimeS), X1 + 28.f * S, Cy, 9.5f, Text3());
				P.TextMid(P.Fit(E.Text, RightW - 98.f * S, 10.f), X1 + 84.f * S, Cy, 10.f, Text());
				Ry += Rh;
			}
		}

		// ---- tool strip (bottom) ----
		{
			struct FTool { FString Key, Label, Value; bool bActive; };
			const int32 LeakMode = Controller->GetLeakToolMode();
			const double Req = ADataCenterSimulationActor::GetRequestedSimSpeed();
			const TArray<FTool> Tools = {
				{ TEXT("L"), TEXT("누수 도구"), LeakToolText(LeakMode), LeakMode != 0 },
				{ TEXT("V"), TEXT("공기 보기"), AirFlowView::Name(Sim->GetAirView()), Sim->GetAirView() != EAirView::Volume },
				{ TEXT("[  ]"), TEXT("배속"), FString::Printf(TEXT("x%g"), Req), Req != 1.0 },
				{ TEXT("T"), TEXT("유닛 탈착"), TEXT(""), false },
				{ TEXT("R"), TEXT("처음부터"), TEXT(""), false },
				{ TEXT("Enter"), TEXT("배치 모드"), TEXT(""), false },
			};
			const float Th = 48.f * S, Gp = 10.f * S;
			TArray<float> Widths; float Total = 10.f * S;
			for (const FTool& T : Tools)
			{
				float Wd = P.Measure(T.Key, 9.5f, true).X + 12.f * S + 8.f * S + P.Measure(T.Label, 10.f).X;
				if (!T.Value.IsEmpty()) Wd += 8.f * S + P.Measure(T.Value, 10.5f, true).X;
				Wd += 20.f * S;
				Widths.Add(Wd); Total += Wd + Gp;
			}
			const float X0t = .5f * (W - Total), Y0 = H - Margin - Th;
			P.Panel(X0t, Y0, Total, Th);
			float Tx = X0t + 10.f * S;
			for (int32 I = 0; I < Tools.Num(); ++I)
			{
				const FTool& T = Tools[I];
				const float Iy = Y0 + 7.f * S, Ih = Th - 14.f * S, Cy = Y0 + .5f * Th;
				if (T.bActive) { P.Rect(Tx, Iy, Widths[I], Ih, WithAlpha(Accent(), .22f)); P.Frame(Tx, Iy, Widths[I], Ih, WithAlpha(Accent(), .8f)); }
				float Ix = Tx + 10.f * S;
				Ix += P.Key(T.Key, Ix, Cy, T.bActive ? WithAlpha(Accent(), .55f) : FLinearColor(1, 1, 1, .07f)) + 8.f * S;
				Ix += P.TextMid(T.Label, Ix, Cy, 10.f, Text2());
				if (!T.Value.IsEmpty()) P.TextMid(T.Value, Ix + 8.f * S, Cy, 10.5f, T.bActive ? FLinearColor::White : Text(), true);
				Tx += Widths[I] + Gp;
				if (I + 1 < Tools.Num() && I == 2) P.Rect(Tx - .5f * Gp, Y0 + 12.f * S, 1.f, Th - 24.f * S, Line());
			}
		}
	}

	if (bHelp)
	{
		P.Rect(0, BarH, W, H - BarH, FLinearColor(0, 0, 0, .45f));
		struct FGroup { FString Name; TArray<FString> Items; };
		TArray<FGroup> Groups;
		for (const FString& L : HelpLines)
		{
			FGroup& G = Groups.AddDefaulted_GetRef();
			FString Rest = L;
			if (L.StartsWith(TEXT("[")))
			{
				int32 Close = INDEX_NONE;
				if (L.FindChar(TEXT(']'), Close)) { G.Name = L.Mid(1, Close - 1); Rest = L.Mid(Close + 1).TrimStartAndEnd(); }
			}
			Rest.ParseIntoArray(G.Items, TEXT(" · "));
		}
		const float Pw = FMath::Min(1000.f * S, W - 4.f * Margin), ColW = .5f * (Pw - 3.f * 20.f * S), LineH = 19.f * S;
		float LeftH = 0.f, RightH = 0.f;
		TArray<int32> Side;
		for (const FGroup& G : Groups)
		{
			const float Gh = 24.f * S + G.Items.Num() * LineH + 10.f * S;
			if (LeftH <= RightH) { Side.Add(0); LeftH += Gh; } else { Side.Add(1); RightH += Gh; }
		}
		const float Ph = 70.f * S + FMath::Max(LeftH, RightH) + 30.f * S;
		const float X0 = .5f * (W - Pw), Y0 = FMath::Max(BarH + 20.f * S, .5f * (H - Ph));
		P.Panel(X0, Y0, Pw, Ph);
		P.Rect(X0, Y0, Pw, 3.f * S, bSim ? Teal() : Accent());
		P.Text(TEXT("단축키"), X0 + 20.f * S, Y0 + 18.f * S, 14.f, Text(), true);
		P.Text(bSim ? TEXT("시뮬레이션 모드") : TEXT("배치 모드"), X0 + 90.f * S, Y0 + 22.f * S, 10.f, Text2());
		P.TextRight(TEXT("H 닫기"), X0 + Pw - 20.f * S, Y0 + 22.f * S, 10.f, Text3());
		float Yc[2] = { Y0 + 60.f * S, Y0 + 60.f * S };
		for (int32 I = 0; I < Groups.Num(); ++I)
		{
			const int32 Sd = Side[I];
			const float Gx = X0 + 20.f * S + Sd * (ColW + 20.f * S);
			float& Gy = Yc[Sd];
			P.Text(Groups[I].Name, Gx, Gy, 10.f, bSim ? Teal() : Accent(), true);
			P.Rect(Gx, Gy + 19.f * S, ColW, 1.f, LineSoft());
			Gy += 24.f * S;
			for (const FString& Item : Groups[I].Items) { P.Text(P.Fit(Item, ColW, 10.f), Gx, Gy, 10.f, Text()); Gy += LineH; }
			Gy += 10.f * S;
		}
	}
}
