#pragma once

#include "CoreMinimal.h"
#include <limits>

class FVrgFaultMonitor;
class FInstrumentBank;

// 2026-11-24 UI 데모(사용자 요청 "지금 화면을 갈아엎지 말고 데모로"): 기존 HUD·팔레트 옆에 새 HUD·팔레트를 둔다.
// vrg.Ui.Style 0 = 기존(기본), 1 = 새 UI. F2로도 바꾼다. 표시만 바꾸며 계산·입력 경로는 그대로다(규약 5: 표시는
// 게임 스레드에서 이미 있는 값을 읽기만 한다).
namespace VrgUi
{
	VRG_TERMPROJ_API bool IsNewStyle();
	VRG_TERMPROJ_API void SetNewStyle(bool bNew);

	// 표시 색(UI 전용, 물리 값 아님). sRGB로 적고 FLinearColor(FColor)로 바꾼다.
	namespace Theme
	{
		inline FLinearColor Panel()      { return FLinearColor(FColor(13, 16, 21, 228)); }
		inline FLinearColor PanelHi()    { return FLinearColor(FColor(22, 27, 35, 236)); }
		inline FLinearColor Line()       { return FLinearColor(FColor(255, 255, 255, 30)); }
		inline FLinearColor LineSoft()   { return FLinearColor(FColor(255, 255, 255, 16)); }
		inline FLinearColor Text()       { return FLinearColor(FColor(229, 233, 238)); }
		inline FLinearColor Text2()      { return FLinearColor(FColor(150, 160, 173)); }
		inline FLinearColor Text3()      { return FLinearColor(FColor(98, 108, 121)); }
		inline FLinearColor Accent()     { return FLinearColor(FColor(55, 138, 221)); }
		inline FLinearColor AccentDim()  { return FLinearColor(FColor(24, 95, 165)); }
		inline FLinearColor Teal()       { return FLinearColor(FColor(29, 158, 117)); }
		inline FLinearColor Amber()      { return FLinearColor(FColor(239, 159, 39)); }
		inline FLinearColor Red()        { return FLinearColor(FColor(226, 75, 74)); }
		inline FLinearColor Cyan()       { return FLinearColor(FColor(93, 190, 229)); }
		inline FLinearColor Purple()     { return FLinearColor(FColor(127, 119, 221)); }
		inline FLinearColor Gray()       { return FLinearColor(FColor(136, 135, 128)); }
		inline FLinearColor WithAlpha(FLinearColor C, float A) { C.A = A; return C; }
	}

	// 사건 기록 한 줄. 심각도 0 정보, 1 주의, 2 위험.
	struct FEventRow
	{
		double TimeS = 0.0;
		int32 Severity = 0;
		FString Text;
	};

	// 장애 판정·누수·밸브·W4 침수·누수 감지기 경보를 시각 내림차순으로 모은다(기존 기록을 읽기만 한다).
	// OutTotal은 자르기 전 전체 수.
	VRG_TERMPROJ_API void BuildEventRows(const FVrgFaultMonitor& Faults, const FInstrumentBank* Instruments,
		int32 MaxRows, TArray<FEventRow>& Out, int32& OutTotal);

	// 추이선 세로 범위: 값의 최소·최대에 여백을 붙이고, 범위가 MinSpan보다 좁으면 가운데를 두고 넓힌다.
	// 기준선(Threshold)이 있으면 그것도 범위 안에 넣는다(NaN이면 무시).
	VRG_TERMPROJ_API void SparkRange(const TArray<double>& Values, double MinSpan, double Threshold, double& OutLo, double& OutHi);

	// "02:14.3" 형식 시각(초).
	VRG_TERMPROJ_API FString FormatClock(double TimeS, bool bTenths = true);

	// 표시용 이름 줄이기: "rack/placed/" 같은 ID 접두어를 떼고, 액터 이름(RackThermalActor_UAID_...)을 Names의 짧은 이름
	// ("랙 #3")으로 바꾼다. "랙 <액터 이름>"은 "랙 랙 #3"이 되지 않게 함께 바꾼다.
	VRG_TERMPROJ_API FString ShortenNames(const FString& Text, const TArray<TPair<FString, FString>>& Names);

	// "기준선 없음" (SparkRange의 Threshold).
	inline double NoThreshold() { return std::numeric_limits<double>::quiet_NaN(); }
}
