#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Visualization/AirFlowView.h"
#include "Visualization/AirFlowView.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Simulation/Core/SimUnits.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarHudTemps(TEXT("vrg.Hud.Temps"), 1,
		TEXT("Simulation mode temperature labels in the viewport (2026-10-22 user request): over each cabinet its hottest part, at each inlet sensor the sensor reading and the rack coolant in -> out. 0 = off."));
	// UI choice, not a physical range: beyond it only the cabinet temperature shows.
	TAutoConsoleVariable<float> CVarHudTempsRange(TEXT("vrg.Hud.TempsRange"), 15.f,
		TEXT("Camera distance [m] within which the inlet sensor labels (air + coolant) show."));

	// Display colour bands (UI only, not limits the model enforces): parts below 60 / 75 C, inlets below the ASHRAE
	// recommended 27 C / allowable 32 C.
	FLinearColor PartColor(double C) { return C < 60.0 ? FLinearColor(.45f, 1.f, .45f) : C < 75.0 ? FLinearColor(1.f, .85f, .2f) : FLinearColor(1.f, .3f, .25f); }
	FLinearColor InletColor(double C) { return C <= 27.0 ? FLinearColor(.7f, .9f, 1.f) : C <= 32.0 ? FLinearColor(1.f, .65f, .2f) : FLinearColor(1.f, .3f, .25f); }
}

// Labels at world points [cm], centred, with a shadow; skipped behind the camera.
void ADataCenterLayoutHud::DrawTemperatureLabels()
{
	if (CVarHudTemps.GetValueOnGameThread() == 0 || !Canvas || !GetWorld()) return;
	ADataCenterSimulationActor* Sim = nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It) { Sim = *It; break; }
	if (!Sim || Sim->GetSimMode() != ESimMode::Simulation) return;   // layout mode has no computed state to show
	const FVrgDashboardRacks Racks = Sim->BuildDashboardRacks(INDEX_NONE);
	if (Racks.Rows.IsEmpty()) return;
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	APlayerController* PC = GetOwningPlayerController();
	if (!Font || !PC) return;
	FVector EyeCm; FRotator ViewRot;
	PC->GetPlayerViewPoint(EyeCm, ViewRot);
	const FVector ViewDir = ViewRot.Vector();
	TMap<FString, AActor*> Cabinets;
	for (TActorIterator<ARackThermalActor> It(GetWorld()); It; ++It) Cabinets.Add(It->GetName(), *It);
	for (TActorIterator<ASpineCabinetActor> It(GetWorld()); It; ++It) Cabinets.Add(It->GetName(), *It);
	// Collected first, then drawn nearest first; a label that would overlap one already drawn is skipped, so the
	// overview stays readable and moving closer reveals the rest.
	// Priority first (0 leak alarm, 1 cabinet, 2 inlet sensor / unit, 3 pipe point), then distance.
	struct FLabel { float X, Y, W, H; int32 Priority; double DistCm; FString Text; FLinearColor Color; };
	TArray<FLabel> Labels;
	auto Label = [&](const FVector& WorldCm, const FString& Text, const FLinearColor& Color, int32 Priority = 1)
	{
		const double Dist = FVector::DotProduct(WorldCm - EyeCm, ViewDir);
		if (Dist <= 0.0) return;
		const FVector Screen = Canvas->Project(WorldCm);
		float W = 0.f, H = 0.f;
		Canvas->TextSize(Font, Text, W, H);
		const float X = Screen.X - 0.5f * W, Y = Screen.Y - 0.5f * H;
		if (X + W < 0.f || Y + H < 0.f || X > Canvas->ClipX || Y > Canvas->ClipY) return;
		Labels.Add({ X, Y, W, H, Priority, Dist, Text, Color });
	};
	const double SensorLabelRangeCm = SimUnits::MetersToCentimeters(FMath::Max(CVarHudTempsRange.GetValueOnGameThread(), 0.f));
	const FVrgEvaluation& Eval = Sim->GetEvaluation();
	for (const FVrgDashboardRackRow& R : Racks.Rows)
	{
		AActor* const* Found = Cabinets.Find(R.Name);
		FVector SizeM;
		if (!Found || !ADataCenterDebugController::PlaceableBoxM(*Found, SizeM)) continue;
		const FTransform T = (*Found)->GetActorTransform();
		// Rule 2: K -> C only here. The cabinet's "current temperature" is its hottest part (the limit that matters).
		const double PartC = SimUnits::KelvinToCelsius(R.HottestPartK);
		Label(T.TransformPosition(FVector(0, 0, SizeM.Z * 100.0 + 25.0)), FString::Printf(TEXT("%.1f °C"), PartC), PartColor(PartC), 1);
		// The inlet sensor sits on the front face (+X, the side the cabinet takes its air from), mid height.
		const FVector SensorCm = T.TransformPosition(FVector(SizeM.X * 50.0 + 15.0, 0, SizeM.Z * 50.0));
		if (FVector::Dist(SensorCm, EyeCm) > SensorLabelRangeCm) continue;
		const FVrgEvaluation::FInletStats In = Eval.GetInlet(R.Inlet);
		const double AirC = SimUnits::KelvinToCelsius(In.bSensorHasReading ? In.SensorLastK : R.InletNowK);
		const FString Coolant = R.Kind == TEXT("rack")
			? FString::Printf(TEXT("수냉 %.1f -> %.1f °C"), SimUnits::KelvinToCelsius(R.CoolantInK), SimUnits::KelvinToCelsius(R.CoolantOutK))
			: FString(TEXT("공랭 전용"));
		Label(SensorCm, FString::Printf(TEXT("흡기 %.1f °C | %s"), AirC, *Coolant), InletColor(AirC), 2);
	}
	// 2026-10-22 instruments: units / CDUs and pipe points within the range (a dot marks every pipe point there), leak
	// alarms everywhere.
	const FInstrumentBank& Instruments = Sim->GetInstruments();
	for (int32 I = 0; I < Instruments.Num(); ++I)
	{
		const FInstrumentPoint& P = Instruments.GetPoint(I);
		const FVector WorldCm = SimUnits::MetersToUnreal(P.PositionM);
		if (Instruments.IsLeakAlarm(I))
		{
			Label(WorldCm, FString::Printf(TEXT("누수 감지 %.0f s | %s"), Instruments.GetLeakAlarmTimeS(I), *P.Label), FLinearColor(1.f, .2f, .2f), 0);
			continue;
		}
		if (FVector::Dist(WorldCm, EyeCm) > SensorLabelRangeCm || FVector::DotProduct(WorldCm - EyeCm, ViewDir) <= 0.0) continue;
		const bool bPipe = P.Site == EInstrumentSite::PlantPipe || P.Site == EInstrumentSite::RoomPipe;
		if (bPipe)
		{
			const FVector Screen = Canvas->Project(WorldCm);
			DrawRect(FLinearColor(.3f, 1.f, .8f), Screen.X - 2.f, Screen.Y - 2.f, 4.f, 4.f);
		}
		const FString Text = Sim->DescribeInstrument(I);
		if (!Text.IsEmpty()) Label(WorldCm + FVector(0, 0, bPipe ? 12.0 : 0.0), Text, bPipe ? FLinearColor(.75f, .95f, .9f) : FLinearColor(.95f, .9f, .7f), bPipe ? 3 : 2);
	}
	Labels.Sort([](const FLabel& A, const FLabel& B) { return A.Priority != B.Priority ? A.Priority < B.Priority : A.DistCm < B.DistCm; });
	TArray<FBox2f> Drawn;
	for (const FLabel& L : Labels)
	{
		const FBox2f Box(FVector2f(L.X - 5.f, L.Y - 3.f), FVector2f(L.X + L.W + 5.f, L.Y + L.H + 3.f));
		bool bOverlap = false;
		for (const FBox2f& D : Drawn) bOverlap |= D.Intersect(Box);
		if (bOverlap) continue;
		Drawn.Add(Box);
		DrawRect(FLinearColor(0, 0, 0, .55f), L.X - 3.f, L.Y - 1.f, L.W + 6.f, L.H + 2.f);
		DrawText(L.Text, L.Color, L.X, L.Y, Font);
	}
}

// The air view's colour bar (bottom right): mode, average window, gradient with five values.
void ADataCenterLayoutHud::DrawAirLegend()
{
	if (!Canvas || !GetWorld()) return;
	ADataCenterSimulationActor* Sim = nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It) { Sim = *It; break; }
	if (!Sim) return;
	TArray<FAirFlowLegend> Legends;
	FString Note;
	Sim->GetAirFlowLegends(Legends, Note);
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (Legends.IsEmpty() || !Font) return;
	const float BarW = 320.f, BarH = 14.f, Pad = 10.f, LineH = Font->GetMaxCharHeight() + 4.f;
	// As wide as the longest text (the note can be longer than the bar).
	float TextW = 0.f, TextH = 0.f;
	Canvas->TextSize(Font, Note, TextW, TextH);
	for (const FAirFlowLegend& L : Legends) { float W = 0.f; Canvas->TextSize(Font, FString::Printf(TEXT("%s [%s]"), *L.Title, *L.Unit), W, TextH); TextW = FMath::Max(TextW, W); }
	const float BoxW = FMath::Max(BarW, TextW) + 2.f * Pad, BoxH = Pad + LineH + Legends.Num() * (LineH + BarH + LineH + 4.f) + Pad;
	const float X0 = Canvas->ClipX - BoxW - 20.f, Y0 = Canvas->ClipY - BoxH - 20.f;
	DrawRect(FLinearColor(0, 0, 0, .6f), X0, Y0, BoxW, BoxH);
	float Y = Y0 + Pad;
	DrawText(Note, FLinearColor(.85f, .9f, 1.f), X0 + Pad, Y, Font);
	Y += LineH;
	for (const FAirFlowLegend& L : Legends)
	{
		DrawText(FString::Printf(TEXT("%s [%s]"), *L.Title, *L.Unit), FLinearColor::White, X0 + Pad, Y, Font);
		Y += LineH;
		constexpr int32 Steps = 64;
		for (int32 S = 0; S < Steps; ++S)
			DrawRect(AirFlowView::Colormap((S + .5) / Steps), X0 + Pad + BarW * S / Steps, Y, BarW / Steps + 1.f, BarH);
		Y += BarH + 2.f;
		for (int32 K = 0; K <= 4; ++K)
		{
			const FString V = FString::Printf(TEXT("%g"), FMath::RoundToDouble((L.Min + (L.Max - L.Min) * K / 4.0) * 10.0) / 10.0);
			float W = 0.f, Hh = 0.f;
			Canvas->TextSize(Font, V, W, Hh);
			DrawText(V, FLinearColor(.9f, .9f, .9f), FMath::Clamp(X0 + Pad + BarW * K / 4.f - .5f * W, X0 + Pad, X0 + Pad + BarW - W), Y, Font);
		}
		Y += LineH + 4.f;
	}
}

void ADataCenterLayoutHud::DrawHUD()
{
	Super::DrawHUD();
	const auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	if (!Canvas || !Controller) return;
	DrawTemperatureLabels();   // under the key hint
	DrawAirLegend();

	// Key hint on the canvas: the engine's on-screen message list would also print its "DisableAllScreenMessages" hint.
	TArray<FString> Lines;
	Controller->GetHelpLines(Lines);
	if (UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr)
	{
		const float LineHeight = Font->GetMaxCharHeight() + 4.f;
		float Y = 40.f;
		for (const FString& Line : Lines)
		{
			DrawText(Line, FLinearColor(0, 0, 0, .8f), 51.f, Y + 1.f, Font);  // shadow for light backgrounds
			DrawText(Line, FLinearColor(.71f, .86f, 1.f), 50.f, Y, Font);
			Y += LineHeight;
		}
		TArray<FString> RoomLines;
		Controller->GetRoomBoundsMessage().ParseIntoArrayLines(RoomLines);
		FRoomAirDomainSettings Preview; bool bValid = false;
		Controller->GetRoomBoundsPreview(Preview, bValid);
		for (const auto& Line : RoomLines)
		{
			DrawText(Line, FLinearColor(0, 0, 0, .8f), 51.f, Y + 1.f, Font);
			DrawText(Line, bValid ? FLinearColor(1, .65f, 0) : FLinearColor::Red, 50.f, Y, Font);
			Y += LineHeight;
		}
		RoomLines.Reset(); Controller->GetRoomCeilingEditMessage().ParseIntoArrayLines(RoomLines);
		FRoomCeilingPlate CeilingPreview; bValid=false;
		Controller->GetRoomCeilingPreview(CeilingPreview,bValid);
		for(const auto& Line:RoomLines)
		{
			DrawText(Line,FLinearColor(0,0,0,.8f),51.f,Y+1.f,Font);
			DrawText(Line,bValid ? FLinearColor(1,.65f,0) : FLinearColor::Red,50.f,Y,Font);
			Y+=LineHeight;
		}
	}

	FBox2D Rect;
	if (!Controller->GetLayoutMarqueeRect(Rect)) return;
	const FLinearColor Color(.2f, .8f, 1.f);
	DrawLine(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Min.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Max.Y, Rect.Min.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Min.X, Rect.Max.Y, Rect.Min.X, Rect.Min.Y, Color, 2.f);
}
