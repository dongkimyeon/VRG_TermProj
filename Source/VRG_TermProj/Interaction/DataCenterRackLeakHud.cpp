#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"

void ADataCenterLayoutHud::DrawRackLeakTool()
{
	const auto* PC = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	if (!PC || !Canvas || !GEngine) return;
	const int32 Site = PC->GetAimedInternalLeakSite(); if (Site == INDEX_NONE) return;
	for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It)
	{
		ADataCenterSimulationActor::FRackLeakStatus S;
		if (!It->GetRackLeakStatus(Site, S)) return;
		const auto& P = It->GetLiveLeakSites()[Site];
		auto Alarm = [](bool Ready, bool Raised, double TimeS) { return !Ready ? FString(TEXT("준비 중")) : Raised ? FString::Printf(TEXT("경보 %.2f s (유지)"), TimeS) : FString(TEXT("미감지")); };
		const TArray<FString> Lines = {
			FString::Printf(TEXT("%s | 랙 ID %s"), *P.Label, *S.RackId),
			FString::Printf(TEXT("구멍 %.3f mm | 계산 누수 %.5f L/s | t=%.2f s"), It->GetLiveLeakHoleDiameterM(Site) * 1e3, S.FlowM3s * 1e3, It->GetSnapshot().SimTimeS),
			FString::Printf(TEXT("선택 슬롯: 현재 보유 %.5f L | 뒤쪽 누적 노출 %.5f L (합산 재고 아님)"), S.SlotHeldM3 * 1e3, S.SlotRearExposureM3 * 1e3),
			FString::Printf(TEXT("랙 현재 보유 %.5f L + 이동 중 %.5f L | 바닥 누적 도착 %.5f L"), S.RackHeldM3 * 1e3, S.RackTransitM3 * 1e3, S.BottomM3 * 1e3),
			FString::Printf(TEXT("바닥 도착 = W3 전달 %.5f L + 대기 %.5f L"), S.DeliveredM3 * 1e3, S.PendingM3 * 1e3),
			FString::Printf(TEXT("트레이 냉각수 %.2f °C | 입/출구 %+0.5f / %+0.5f L/s"), SimUnits::KelvinToCelsius(S.TrayTempK), S.TrayInM3s * 1e3, S.TrayOutM3s * 1e3),
			FString::Printf(TEXT("드립: %s | 뒤 매니폴드 로프: %s"), *Alarm(S.bDripReady, S.bDripAlarm, S.DripTimeS), *Alarm(S.bRopeReady, S.bRopeAlarm, S.RopeTimeS)),
			TEXT("D9 모의 계측값 · 설치/문턱/지연 추정 · 청록=보유, 보라=뒤 누적 노출 · 실제 수막 아님")
		};
		UFont* Font = GEngine->GetSmallFont(); const float Top = 90.f;
		DrawRect(FLinearColor(0, 0, 0, .8f), 10.f, Top - 5.f, FMath::Max(0.f, FMath::Min(Canvas->ClipX - 20.f, 1100.f)), Lines.Num() * 20.f + 10.f);
		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			float Width = 0, Height = 0; Canvas->TextSize(Font, Lines[I], Width, Height);
			const float Scale = FMath::Min(1.f, FMath::Max(Canvas->ClipX - 40.f, 1.f) / FMath::Max(Width, 1.f));
			DrawText(Lines[I], I == 6 && (S.bDripAlarm || S.bRopeAlarm) ? FLinearColor(1, .35f, .3f) : FLinearColor::White, 18.f, Top + I * 20.f, Font, Scale);
		}
		return;
	}
}
