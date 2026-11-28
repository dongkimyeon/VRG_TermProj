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

	// 2026-11-27: vrg.Ui.Panels 1(기본, 같은 날 사용자 결정)이면 새 UI(Style 1)에서 3D 온도 라벨·방 설정 창·패치 평면도도
	// 새 모양으로 그린다. 0이면 이전 모양. 표시만 바뀐다.
	VRG_TERMPROJ_API bool IsNewPanels();
	VRG_TERMPROJ_API void SetNewPanels(bool bNew);

	// 2026-11-28 UI 테마(사용자: "다른 버전 데모 3개, 골라서 확정"): 색과 스타일 값 묶음. vrg.Ui.Theme 0 = 지금 모양
	// (값 그대로), 1 = 관제실 앰버, 2 = 라이트, 3 = 네온 미니멀. 표시 전용(물리 값 아님). 색은 sRGB.
	struct FTheme
	{
		const TCHAR* Name;
		FColor Panel, PanelHi, Line, LineSoft, Text, Text2, Text3;
		FColor Accent, AccentDim, OnAccent;            // OnAccent = 강조색 바탕 위 글자
		FColor Teal, Amber, Red, Cyan, Purple, Gray;   // 상태·분류 색
		FColor Scrim, KeyFill, KeyEdge;                // 오버레이 어둡게, 키캡
		FColor PlanBg, PlanArea, SpineLine, SelectLine; // 배선 평면도
		float RadiusScale;      // UMG 둥근 모서리 배율(0 = 각진 모서리)
		bool bFloatingBar;      // 상단 바를 화면 가장자리에서 띄운 패널로
		bool bCardStripTop;     // 지표 카드 상태 띠를 위쪽에(아니면 왼쪽)
		bool bLight;            // 밝은 바탕: 3D 말풍선 글자를 값 색 대신 본문 색으로
	};
	inline constexpr int32 NumThemes = 4;
	VRG_TERMPROJ_API int32 ThemeIndex();
	VRG_TERMPROJ_API void SetThemeIndex(int32 Index);
	VRG_TERMPROJ_API const FTheme& GetTheme(int32 Index);
	inline const FTheme& ActiveTheme() { return GetTheme(ThemeIndex()); }
	// 상단 바가 차지하는 아래쪽 끝(뷰포트 픽셀, 배율 S): 그 아래부터 패널을 놓는다.
	inline float BarBottom(float S) { return ActiveTheme().bFloatingBar ? (10.f + 46.f) * S : 46.f * S; }

	namespace Theme
	{
		inline FLinearColor Panel()      { return FLinearColor(ActiveTheme().Panel); }
		inline FLinearColor PanelHi()    { return FLinearColor(ActiveTheme().PanelHi); }
		inline FLinearColor Line()       { return FLinearColor(ActiveTheme().Line); }
		inline FLinearColor LineSoft()   { return FLinearColor(ActiveTheme().LineSoft); }
		inline FLinearColor Text()       { return FLinearColor(ActiveTheme().Text); }
		inline FLinearColor Text2()      { return FLinearColor(ActiveTheme().Text2); }
		inline FLinearColor Text3()      { return FLinearColor(ActiveTheme().Text3); }
		inline FLinearColor Accent()     { return FLinearColor(ActiveTheme().Accent); }
		inline FLinearColor AccentDim()  { return FLinearColor(ActiveTheme().AccentDim); }
		inline FLinearColor OnAccent()   { return FLinearColor(ActiveTheme().OnAccent); }
		inline FLinearColor Teal()       { return FLinearColor(ActiveTheme().Teal); }
		inline FLinearColor Amber()      { return FLinearColor(ActiveTheme().Amber); }
		inline FLinearColor Red()        { return FLinearColor(ActiveTheme().Red); }
		inline FLinearColor Cyan()       { return FLinearColor(ActiveTheme().Cyan); }
		inline FLinearColor Purple()     { return FLinearColor(ActiveTheme().Purple); }
		inline FLinearColor Gray()       { return FLinearColor(ActiveTheme().Gray); }
		inline FLinearColor Scrim()      { return FLinearColor(ActiveTheme().Scrim); }
		inline FLinearColor KeyFill()    { return FLinearColor(ActiveTheme().KeyFill); }
		inline FLinearColor KeyEdge()    { return FLinearColor(ActiveTheme().KeyEdge); }
		inline FLinearColor PlanBg()     { return FLinearColor(ActiveTheme().PlanBg); }
		inline FLinearColor PlanArea()   { return FLinearColor(ActiveTheme().PlanArea); }
		inline FLinearColor SpineLine()  { return FLinearColor(ActiveTheme().SpineLine); }
		inline FLinearColor SelectLine() { return FLinearColor(ActiveTheme().SelectLine); }
		inline float Radius(float Base)  { return Base * ActiveTheme().RadiusScale; }
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

	// 새 HUD의 크기 배율: UMG와 같은 DPI 곡선(프로젝트 UI 설정)을 화면 크기에 적용한 값, [0.5, 3]. Canvas 패널과
	// UMG 팔레트·누수 도구 패널이 같은 배율을 쓰게 한다(2026-11-25).
	VRG_TERMPROJ_API float HudScale(float ViewportX, float ViewportY);

	// 화면 좌표(뷰포트 픽셀)가 사각형 중 하나 안에 있는지(경계 포함). 새 HUD 패널 위 클릭을 장면에 넘기지 않는 데 쓴다.
	VRG_TERMPROJ_API bool HitAnyRect(const TArray<FBox2D>& Rects, const FVector2D& Point);

	// "기준선 없음" (SparkRange의 Threshold).
	inline double NoThreshold() { return std::numeric_limits<double>::quiet_NaN(); }
}
