#include "Interaction/DataCenterLayoutHud.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Simulation/Core/SimUnits.h"

namespace
{
	TAutoConsoleVariable<int32> GShowNoise(
		TEXT("vrg.Noise.Show"), 0, TEXT("Show the indoor, outdoor probe and boundary noise observations in the game HUD."));
}

void ADataCenterLayoutHud::DrawNoiseOverlay()
{
	using namespace VrgNoise;
	if (GShowNoise.GetValueOnGameThread() == 0 || !Canvas || !GetOwningPlayerController()) return;
	const ADataCenterSimulationActor* Sim = nullptr;
	for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It) { Sim = *It; break; }
	if (!Sim || Sim->GetSimMode() != ESimMode::Simulation) return;
	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font) return;

	const FVector2D Center(Canvas->ClipX * .5, Canvas->ClipY * .5);
	const FNoiseTrack* Selected = nullptr;
	const FNoiseReceiverGroup* SelectedGroup = nullptr;
	FVector2D SelectedPosition = FVector2D::ZeroVector;
	double BestDistance = FMath::Square(48.0);
	bool bAny = false;
	for (int32 G = 0; G < int32(EReceiverSet::Count); ++G)
	{
		const auto& Group = Sim->GetNoiseReceiverGroup(static_cast<EReceiverSet>(G));
		bAny |= !Group.Tracks.IsEmpty();
		for (const auto& Track : Group.Tracks)
		{
			FVector2D Screen;
			if (!GetOwningPlayerController()->ProjectWorldLocationToScreen(SimUnits::MetersToUnreal(Track.Latest.PositionM), Screen, true)
				|| Screen.X < 0 || Screen.X > Canvas->ClipX || Screen.Y < 0 || Screen.Y > Canvas->ClipY) continue;
			DrawRect(FLinearColor::Black, Screen.X - 5.f, Screen.Y - 5.f, 10.f, 10.f);
			DrawRect(FLinearColor(NoiseLevelColor(Track.Latest.Pressure)), Screen.X - 4.f, Screen.Y - 4.f, 8.f, 8.f);
			const double Distance = FVector2D::DistSquared(Screen, Center);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Selected = &Track;
				SelectedGroup = &Group;
				SelectedPosition = Screen;
			}
		}
	}

	const float Y = Canvas->ClipY - 110.f;
	const FString Legend = bAny
		? FString::Printf(TEXT("소음 LpA · %.0f~%.0f dBA (추정) | 회색 정지 · 보라 누락"), NoiseLegendMinDb, NoiseLegendMaxDb)
		: TEXT("소음 수신점 없음 · vrg.Noise.IndoorMap 1 1.5");
	float Width = 0.f, Height = 0.f;
	Canvas->TextSize(Font, Legend, Width, Height);
	const float X = FMath::Max(12.f, (Canvas->ClipX - Width) * .5f);
	DrawRect(FLinearColor(0, 0, 0, .76f), X - 8.f, Y - 6.f, Width + 16.f, Height + (bAny ? 21.f : 12.f));
	DrawText(Legend, FLinearColor::White, X, Y, Font);
	if (bAny)
	{
		constexpr int32 Segments = 24;
		for (int32 I = 0; I < Segments; ++I)
		{
			FLevel Level;
			Level.Db = FMath::Lerp(NoiseLegendMinDb, NoiseLegendMaxDb, double(I) / (Segments - 1));
			DrawRect(FLinearColor(NoiseLevelColor(Level)), X + I * Width / Segments, Y + Height + 4.f, Width / Segments, 5.f);
		}
	}
	if (!Selected) return;
	const auto& Level = Selected->Latest.Pressure;
	DrawRect(FLinearColor::White, SelectedPosition.X - 6.f, SelectedPosition.Y - 6.f, 12.f, 12.f);
	DrawRect(FLinearColor(NoiseLevelColor(Level)), SelectedPosition.X - 4.f, SelectedPosition.Y - 4.f, 8.f, 8.f);
	const auto Average = Selected->Average.GetDb();
	const FString Now = Level.Db.IsSet()
		? FString::Printf(TEXT("%.2f dBA%s"), *Level.Db, Level.MissingSources > 0 ? TEXT(" (부분)") : TEXT(""))
		: NoiseLevelState(Level);
	const FString Eq = Average.IsSet()
		? FString::Printf(TEXT("%.2f dBA%s"), *Average, Selected->Average.IncompleteS > 0.0 ? TEXT(" (부분)") : TEXT(""))
		: Selected->Average.GetState();
	const FString Label = FString::Printf(TEXT("LpA %s  ·  LAeq %s  ·  t %.2f s  ·  누락 %d"), *Now, *Eq, SelectedGroup->LatestSourceS, Level.MissingSources);
	Canvas->TextSize(Font, Label, Width, Height);
	const float LabelX = FMath::Max(8.f, (Canvas->ClipX - Width) * .5f);
	const float LabelY = FMath::Max(8.f, Y - Height - 16.f);
	DrawRect(FLinearColor(0, 0, 0, .76f), LabelX - 5.f, LabelY - 4.f, Width + 10.f, Height + 8.f);
	DrawText(Label, FLinearColor::White, LabelX, LabelY, Font);
}
