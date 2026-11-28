// 2026-11-28 소음 표시를 새 UI(vrg.Ui.Style 1)에 맞춘 모양. 이전 UI는 DrawNoiseOverlay(소음 브랜치 N5·N6) 그대로.
// 값은 수신점 묶음의 최신 LpA·LAeq·추이를 읽기만 하고(규약 5), 색은 소음 모델의 NoiseLevelColor 그대로 쓴다.
// 크기·위치·간격은 UI 선택이며 물리 값이 아니다.
#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/VrgHudPaint.h"
#include "Interaction/VrgNoiseView.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Simulation/Acoustics/NoiseMonitoring.h"
#include "Simulation/Core/SimUnits.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

using namespace VrgUi::Theme;

namespace
{
	const TCHAR* GroupName(VrgNoise::EReceiverSet Set)
	{
		switch (Set)
		{
		case VrgNoise::EReceiverSet::Indoor: return TEXT("실내");
		case VrgNoise::EReceiverSet::Probe: return TEXT("실외 수신점");
		case VrgNoise::EReceiverSet::Boundary: return TEXT("부지 경계");
		default: return TEXT("");
		}
	}
}

bool ADataCenterLayoutHud::GatherNoiseView(FVrgNoiseView& Out) const
{
	using namespace VrgNoise;
	Out = FVrgNoiseView();
	const IConsoleVariable* Show = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Noise.Show"));
	Out.bShow = Show && Show->GetInt() != 0;
	APlayerController* PC = GetOwningPlayerController();
	if (!Out.bShow || !Canvas || !PC || !GetWorld()) return false;
	const ADataCenterSimulationActor* Sim = nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It) { Sim = *It; break; }
	if (!Sim || Sim->GetSimMode() != ESimMode::Simulation) { Out.bShow = false; return false; }
	const FVector2D Center(Canvas->ClipX * .5, Canvas->ClipY * .5);
	double Best = FMath::Square(48.0);   // UI pick radius [px], same as the previous overlay
	for (int32 G = 0; G < int32(EReceiverSet::Count); ++G)
	{
		const FNoiseReceiverGroup& Group = Sim->GetNoiseReceiverGroup(static_cast<EReceiverSet>(G));
		Out.Receivers += Group.Tracks.Num();
		for (const FNoiseTrack& Track : Group.Tracks)
		{
			const FLevel& L = Track.Latest.Pressure;
			if (L.Db.IsSet()) Out.MaxDb = FMath::Max(Out.MaxDb.Get(-1e300), *L.Db);
			Out.PartialPoints += L.MissingSources > 0 ? 1 : 0;
			FVector2D Screen;
			if (!PC->ProjectWorldLocationToScreen(SimUnits::MetersToUnreal(Track.Latest.PositionM), Screen, true)
				|| Screen.X < 0 || Screen.X > Canvas->ClipX || Screen.Y < 0 || Screen.Y > Canvas->ClipY) continue;
			++Out.OnScreen;
			Out.Points.Add({ Screen, NoiseLevelColor(L) });
			const double D = FVector2D::DistSquared(Screen, Center);
			if (D < Best) { Best = D; Out.Selected = &Track; Out.SelectedScreen = Screen; Out.SelectedGroup = GroupName(static_cast<EReceiverSet>(G)); Out.SourceTimeS = Group.LatestSourceS; }
		}
	}
	Out.bAny = Out.Receivers > 0;
	return true;
}

// Receiver dots, the picked point's callout and the colour bar (bottom right, above the air legend when it is shown).
void ADataCenterLayoutHud::DrawNoiseV2(float LegendBottomMargin)
{
	using namespace VrgNoise;
	FVrgNoiseView V;
	if (!GatherNoiseView(V)) return;
	VrgUi::FHudPaint P;
	P.C = Canvas;
	P.Hits = &PanelRects;
	P.S = VrgUi::HudScale(Canvas->ClipX, Canvas->ClipY);
	const float S = P.S;
	for (const TPair<FVector2D, FColor>& Pt : V.Points)
	{
		P.Rect(Pt.Key.X - 4.f * S, Pt.Key.Y - 4.f * S, 8.f * S, 8.f * S, FLinearColor(0, 0, 0, .55f));
		P.Rect(Pt.Key.X - 3.f * S, Pt.Key.Y - 3.f * S, 6.f * S, 6.f * S, FLinearColor(Pt.Value));
	}
	if (V.Selected)
	{
		const FLevel& L = V.Selected->Latest.Pressure;
		const FLinearColor Col(NoiseLevelColor(L));
		const FVector2D A = V.SelectedScreen;
		P.Frame(A.X - 8.f * S, A.Y - 8.f * S, 16.f * S, 16.f * S, FLinearColor::White);
		const TOptional<double> Eq = V.Selected->Average.GetDb();
		const FString Head = L.Db.IsSet() ? FString::Printf(TEXT("%.1f dBA"), *L.Db) : FString(NoiseLevelState(L));
		FString Rest = FString::Printf(TEXT("LAeq %s"), Eq.IsSet() ? *FString::Printf(TEXT("%.1f dBA"), *Eq) : V.Selected->Average.GetState());
		if (L.MissingSources > 0) Rest += FString::Printf(TEXT("  ·  누락 %d (부분 합계)"), L.MissingSources);
		Rest += TEXT("  ·  ") + V.SelectedGroup;
		const float Pad = 8.f * S, H = 22.f * S;
		const float W = 3.f * S + Pad + P.Measure(Head, 11.f, true).X + 10.f * S + P.Measure(Rest, 9.5f).X + Pad;
		const float X0 = A.X - .5f * W, Y0 = A.Y - 16.f * S - H;
		P.Rect(A.X - .5f, Y0 + H, 1.f, 8.f * S, WithAlpha(Col, .8f));
		P.Rect(X0, Y0, W, H, WithAlpha(Panel(), .92f));
		P.Frame(X0, Y0, W, H, Line());
		P.Rect(X0, Y0, 3.f * S, H, Col);
		const float Hw = P.TextMid(Head, X0 + 3.f * S + Pad, Y0 + .5f * H, 11.f, Col, true);
		P.TextMid(Rest, X0 + 3.f * S + Pad + Hw + 10.f * S, Y0 + .5f * H, 9.5f, Text2());
	}
	// Colour bar.
	const float Pad = 14.f * S, BarW = 320.f * S, BarH = 10.f * S;
	const float Bw = BarW + 2.f * Pad, Bh = (V.bAny ? 92.f : 58.f) * S;
	const float X0 = Canvas->ClipX - 16.f * S - Bw, Y0 = Canvas->ClipY - LegendBottomMargin - Bh;
	P.Panel(X0, Y0, Bw, Bh);
	P.Text(TEXT("소음 LpA · A 보정"), X0 + Pad, Y0 + 11.f * S, 10.f, VrgUi::Theme::Text(), true);
	P.TextRight(TEXT("dBA · 추정 음원 자료"), X0 + Pad + BarW, Y0 + 12.f * S, 9.f, Text3());
	if (!V.bAny)
	{
		P.Text(TEXT("수신점이 없습니다 · N을 다시 누르면 실내 지도를 만듭니다"), X0 + Pad, Y0 + 32.f * S, 9.5f, Text2());
		return;
	}
	const float By = Y0 + 32.f * S;
	constexpr int32 Steps = 64;
	for (int32 K = 0; K < Steps; ++K)
	{
		FLevel Lv; Lv.Db = FMath::Lerp(NoiseLegendMinDb, NoiseLegendMaxDb, (K + .5) / Steps);
		P.Rect(X0 + Pad + BarW * K / Steps, By, BarW / Steps + 1.f, BarH, FLinearColor(NoiseLevelColor(Lv)));
	}
	for (int32 K = 0; K <= 4; ++K)
	{
		const FString T = FString::Printf(TEXT("%.0f"), FMath::Lerp(NoiseLegendMinDb, NoiseLegendMaxDb, K / 4.0));
		const float Tw = P.Measure(T, 9.f).X;
		P.Text(T, FMath::Clamp(X0 + Pad + BarW * K / 4.f - .5f * Tw, X0 + Pad, X0 + Pad + BarW - Tw), By + BarH + 4.f * S, 9.f, Text2());
	}
	FLevel Off; Off.State = ELevelState::Silent;
	FLevel Miss; Miss.State = ELevelState::MissingData;
	float Kx = X0 + Pad;
	const float Ky = By + BarH + 26.f * S;
	P.Rect(Kx, Ky + 2.f * S, 8.f * S, 8.f * S, FLinearColor(NoiseLevelColor(Off))); Kx += 12.f * S;
	Kx += P.Text(TEXT("정지"), Kx, Ky - 1.f * S, 9.f, Text2()) + 14.f * S;
	P.Rect(Kx, Ky + 2.f * S, 8.f * S, 8.f * S, FLinearColor(NoiseLevelColor(Miss))); Kx += 12.f * S;
	Kx += P.Text(TEXT("자료 없음"), Kx, Ky - 1.f * S, 9.f, Text2()) + 14.f * S;
	P.TextRight(FString::Printf(TEXT("수신점 %d · 화면 %d"), V.Receivers, V.OnScreen), X0 + Pad + BarW, Ky - 1.f * S, 9.f, Text3());
}
