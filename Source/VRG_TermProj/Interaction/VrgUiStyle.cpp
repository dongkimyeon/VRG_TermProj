#include "Interaction/VrgUiStyle.h"
#include "Simulation/Evaluation/VrgFaultMonitor.h"
#include "Simulation/Sensors/InstrumentBank.h"
#include "HAL/IConsoleManager.h"
#include "Algo/StableSort.h"
#include "Engine/UserInterfaceSettings.h"

namespace
{
	// 2026-11-25 user decision: the new UI is the default; 0 keeps the previous HUD and bottom palette.
	TAutoConsoleVariable<int32> CVarUiStyle(TEXT("vrg.Ui.Style"), 1,
		TEXT("UI style (default 1 since 2026-11-25): 0 = previous HUD and bottom palette, 1 = new HUD (status bar, KPI cards, event log, tool strip) and side palette. F2 toggles. Display only."));

	// 2026-11-27 user decision: the new look is the default; 0 keeps the previous labels/dialog/patch map.
	TAutoConsoleVariable<int32> CVarUiPanels(TEXT("vrg.Ui.Panels"), 1,
		TEXT("With the new UI (vrg.Ui.Style 1): 1 = 3D temperature labels, room settings dialog and patch map in the new look (default since 2026-11-27), 0 = their previous look. Display only."));

	const TCHAR* PhaseName(const FString& P)
	{
		if (P == TEXT("wet")) return TEXT("젖음");
		if (P == TEXT("tripped")) return TEXT("침수 트립");
		if (P == TEXT("damaged")) return TEXT("침수 손상");
		if (P == TEXT("normal")) return TEXT("정상");
		return nullptr;
	}
}

namespace
{
	TAutoConsoleVariable<int32> CVarUiTheme(TEXT("vrg.Ui.Theme"), 0,
		TEXT("2026-11-28 UI theme demo for the new UI: 0 = current look (default), 1 = control-room amber, 2 = light, 3 = neon minimal. Display only."));

	// UI colours and style switches (display choices, not physical values). Theme 0 holds the exact values of the
	// look before themes existed.
	const VrgUi::FTheme GThemes[VrgUi::NumThemes] = {
		{ TEXT("기본"),
			FColor(13, 16, 21, 228), FColor(22, 27, 35, 236), FColor(255, 255, 255, 30), FColor(255, 255, 255, 16),
			FColor(229, 233, 238), FColor(150, 160, 173), FColor(98, 108, 121),
			FColor(55, 138, 221), FColor(24, 95, 165), FColor(255, 255, 255),
			FColor(29, 158, 117), FColor(239, 159, 39), FColor(226, 75, 74), FColor(93, 190, 229), FColor(127, 119, 221), FColor(136, 135, 128),
			FColor(0, 0, 0, 115), FColor(255, 255, 255, 18), FColor(255, 255, 255, 56),
			FColor(9, 11, 15, 248), FColor(13, 17, 22, 255), FColor(255, 255, 255, 217), FColor(255, 255, 0, 255),
			1.f, false, false, false },
		{ TEXT("관제실 앰버"),
			FColor(7, 8, 9, 238), FColor(12, 13, 14, 244), FColor(255, 176, 0, 70), FColor(255, 176, 0, 28),
			FColor(244, 236, 220), FColor(190, 168, 128), FColor(128, 112, 86),
			FColor(255, 170, 0), FColor(170, 105, 0), FColor(24, 16, 0),
			FColor(80, 205, 120), FColor(250, 214, 60), FColor(240, 64, 48), FColor(120, 200, 230), FColor(170, 140, 255), FColor(140, 132, 118),
			FColor(0, 0, 0, 140), FColor(255, 176, 0, 22), FColor(255, 176, 0, 110),
			FColor(6, 7, 8, 250), FColor(11, 12, 13, 255), FColor(244, 236, 220, 220), FColor(255, 214, 0, 255),
			.15f, false, true, false },   // near-square corners (a zero radius makes Slate fill the outline colour)
		{ TEXT("라이트"),
			FColor(248, 249, 251, 236), FColor(255, 255, 255, 244), FColor(20, 30, 45, 46), FColor(20, 30, 45, 22),
			FColor(22, 28, 36), FColor(78, 88, 102), FColor(128, 136, 148),
			FColor(37, 99, 235), FColor(29, 78, 216), FColor(255, 255, 255),
			FColor(13, 148, 110), FColor(217, 119, 6), FColor(220, 38, 38), FColor(8, 145, 178), FColor(124, 58, 237), FColor(120, 116, 108),
			FColor(10, 15, 25, 80), FColor(20, 30, 45, 12), FColor(20, 30, 45, 70),
			FColor(238, 241, 245, 250), FColor(255, 255, 255, 255), FColor(60, 70, 85, 230), FColor(217, 119, 6, 255),
			1.3f, true, false, true },
		{ TEXT("네온 미니멀"),
			FColor(4, 9, 18, 165), FColor(4, 9, 18, 190), FColor(0, 229, 255, 0), FColor(120, 220, 255, 30),
			FColor(236, 246, 255), FColor(150, 192, 216), FColor(96, 128, 152),
			FColor(0, 229, 255), FColor(0, 140, 170), FColor(0, 22, 30),
			FColor(0, 235, 160), FColor(255, 204, 0), FColor(255, 64, 116), FColor(0, 229, 255), FColor(196, 128, 255), FColor(130, 150, 165),
			FColor(0, 4, 12, 150), FColor(0, 229, 255, 20), FColor(0, 229, 255, 90),
			FColor(3, 7, 14, 250), FColor(6, 12, 22, 255), FColor(236, 246, 255, 210), FColor(255, 204, 0, 255),
			1.7f, true, true, false },
	};
}

int32 VrgUi::ThemeIndex() { return FMath::Clamp(CVarUiTheme.GetValueOnGameThread(), 0, NumThemes - 1); }
void VrgUi::SetThemeIndex(int32 Index) { CVarUiTheme->Set(FMath::Clamp(Index, 0, NumThemes - 1), ECVF_SetByConsole); }
const VrgUi::FTheme& VrgUi::GetTheme(int32 Index) { return GThemes[FMath::Clamp(Index, 0, NumThemes - 1)]; }

bool VrgUi::IsNewStyle() { return CVarUiStyle.GetValueOnGameThread() != 0; }
void VrgUi::SetNewStyle(bool bNew) { CVarUiStyle->Set(bNew ? 1 : 0, ECVF_SetByConsole); }
bool VrgUi::IsNewPanels() { return IsNewStyle() && CVarUiPanels.GetValueOnGameThread() != 0; }
void VrgUi::SetNewPanels(bool bNew) { CVarUiPanels->Set(bNew ? 1 : 0, ECVF_SetByConsole); }

FString VrgUi::ShortenNames(const FString& Text, const TArray<TPair<FString, FString>>& Names)
{
	FString Out = Text;
	// ID prefixes "<kind>/placed/" or "<kind>/auto/" (W4 equipment ids).
	for (const TCHAR* Mid : { TEXT("/placed/"), TEXT("/auto/") })
	{
		int32 At = Out.Find(Mid, ESearchCase::CaseSensitive);
		while (At != INDEX_NONE)
		{
			int32 Start = At;
			while (Start > 0 && !FChar::IsWhitespace(Out[Start - 1]) && Out[Start - 1] != TEXT('|')) --Start;
			Out.RemoveAt(Start, At + FCString::Strlen(Mid) - Start);
			At = Out.Find(Mid, ESearchCase::CaseSensitive);
		}
	}
	for (const TPair<FString, FString>& N : Names)
	{
		if (N.Key.IsEmpty()) continue;
		Out.ReplaceInline(*(FString(TEXT("랙 ")) + N.Key), *N.Value, ESearchCase::CaseSensitive);
		Out.ReplaceInline(*N.Key, *N.Value, ESearchCase::CaseSensitive);
	}
	return Out;
}

float VrgUi::HudScale(float ViewportX, float ViewportY)
{
	const UUserInterfaceSettings* Ui = GetDefault<UUserInterfaceSettings>();
	const float Dpi = Ui ? Ui->GetDPIScaleBasedOnSize(FIntPoint(FMath::Max(1, int32(ViewportX)), FMath::Max(1, int32(ViewportY)))) : ViewportY / 1080.f;
	return FMath::Clamp(Dpi, .5f, 3.f);
}

bool VrgUi::HitAnyRect(const TArray<FBox2D>& Rects, const FVector2D& Point)
{
	for (const FBox2D& R : Rects)
		if (R.bIsValid && Point.X >= R.Min.X && Point.X <= R.Max.X && Point.Y >= R.Min.Y && Point.Y <= R.Max.Y) return true;
	return false;
}

FString VrgUi::FormatClock(double TimeS, bool bTenths)
{
	const double T = FMath::Max(TimeS, 0.0);
	const int32 Min = FMath::FloorToInt32(T / 60.0);
	const double Sec = T - 60.0 * Min;
	return bTenths ? FString::Printf(TEXT("%02d:%04.1f"), Min, FMath::FloorToDouble(Sec * 10.0) / 10.0)
	               : FString::Printf(TEXT("%02d:%02d"), Min, FMath::FloorToInt32(Sec));
}

void VrgUi::SparkRange(const TArray<double>& Values, double MinSpan, double Threshold, double& OutLo, double& OutHi)
{
	double Lo = TNumericLimits<double>::Max(), Hi = TNumericLimits<double>::Lowest();
	for (double V : Values) if (FMath::IsFinite(V)) { Lo = FMath::Min(Lo, V); Hi = FMath::Max(Hi, V); }
	if (Lo > Hi) { Lo = 0.0; Hi = 1.0; }
	if (FMath::IsFinite(Threshold)) { Lo = FMath::Min(Lo, Threshold); Hi = FMath::Max(Hi, Threshold); }
	const double Pad = 0.12 * (Hi - Lo);
	Lo -= Pad; Hi += Pad;
	if (Hi - Lo < MinSpan) { const double Mid = 0.5 * (Lo + Hi); Lo = Mid - 0.5 * MinSpan; Hi = Mid + 0.5 * MinSpan; }
	OutLo = Lo; OutHi = Hi;
}

void VrgUi::BuildEventRows(const FVrgFaultMonitor& Faults, const FInstrumentBank* Instruments, int32 MaxRows, TArray<FEventRow>& Out, int32& OutTotal)
{
	Out.Reset();
	auto RackName = [](int32 Rack) { return Rack == INDEX_NONE ? FString(TEXT("전체 망")) : FString::Printf(TEXT("랙 #%d"), Rack + 1); };
	for (const FVrgFaultRecord& F : Faults.GetFaults())
	{
		FEventRow& R = Out.AddDefaulted_GetRef();
		R.TimeS = F.TimeS;
		switch (F.Kind)
		{
		case EVrgFaultKind::LowCoolantFlow:        R.Severity = 1; R.Text = FString::Printf(TEXT("%s 냉각수 유량 부족 (설계의 %.0f%%)"), *RackName(F.Rack), F.Value * 100.0); break;
		case EVrgFaultKind::OverTemperature:       R.Severity = 2; R.Text = FString::Printf(TEXT("%s 부품 과열 %.1f °C"), *RackName(F.Rack), F.Value - 273.15); break;   // rule 2: UI only
		case EVrgFaultKind::InventoryDepleted:     R.Severity = 2; R.Text = FString::Printf(TEXT("%s 냉각수 재고 고갈"), *RackName(F.Rack)); break;
		case EVrgFaultKind::HydraulicNotConverged: R.Severity = 2; R.Text = TEXT("수력 계산 수렴 실패"); break;
		case EVrgFaultKind::WaterTrip:             R.Severity = 2; R.Text = FString::Printf(TEXT("%s 침수 트립 (슬롯 %d)"), *RackName(F.Rack), int32(F.Value)); break;
		case EVrgFaultKind::WaterDamage:           R.Severity = 2; R.Text = FString::Printf(TEXT("%s 침수 손상 (슬롯 %d)"), *RackName(F.Rack), int32(F.Value)); break;
		}
	}
	for (const FVrgLeakRecord& L : Faults.GetLeaks())
	{
		FEventRow& R = Out.AddDefaulted_GetRef();
		R.TimeS = L.TimeS;
		const FString Where = L.Site.IsEmpty() ? FString::Printf(TEXT("%s %s 분기"), *RackName(L.Rack), L.bReturnBranch ? TEXT("회수") : TEXT("공급")) : L.Site;
		if (L.HoleDiameterM > 0.0) { R.Severity = 1; R.Text = FString::Printf(TEXT("누수 %.1f mm · %s"), L.HoleDiameterM * 1e3, *Where); }
		else { R.Severity = 0; R.Text = FString::Printf(TEXT("누수 지점 닫힘 · %s"), *Where); }
	}
	for (const FVrgValveRecord& V : Faults.GetValves())
	{
		FEventRow& R = Out.AddDefaulted_GetRef();
		R.TimeS = V.TimeS; R.Severity = 0;
		R.Text = V.bClosed ? FString::Printf(TEXT("%s 밸브 닫힘 · 격리 %d대, 영향 %d대"), *RackName(V.Rack), V.IsolatedRacks.Num(), V.ImpactedRacks.Num())
		                   : FString::Printf(TEXT("%s 밸브 열림"), *RackName(V.Rack));
	}
	for (const FVrgWaterImpactRecord& W : Faults.GetWaterImpacts())
	{
		FEventRow& R = Out.AddDefaulted_GetRef();
		R.TimeS = W.TimeS;
		const TCHAR* To = PhaseName(W.To);
		R.Severity = W.To == TEXT("wet") ? 1 : W.To == TEXT("normal") ? 0 : 2;
		R.Text = W.Slot != INDEX_NONE ? FString::Printf(TEXT("%s 슬롯 %d → %s"), *W.EquipmentId, W.Slot, To ? To : *W.To)
		                              : FString::Printf(TEXT("%s → %s"), *W.EquipmentId, To ? To : *W.To);
	}
	if (Instruments)
	{
		for (const FLeakAlarm& A : Instruments->GetAlarmLog())
		{
			if (A.Point < 0 || A.Point >= Instruments->Num()) continue;
			FEventRow& R = Out.AddDefaulted_GetRef();
			R.TimeS = A.TimeS; R.Severity = 2;
			R.Text = FString::Printf(TEXT("누수 감지 · %s"), *Instruments->GetPoint(A.Point).Label);
		}
	}
	// Latest first; equal times keep the more severe first, then insertion order (stable sort).
	Algo::StableSort(Out, [](const FEventRow& A, const FEventRow& B) { return A.TimeS != B.TimeS ? A.TimeS > B.TimeS : A.Severity > B.Severity; });
	OutTotal = Out.Num();
	if (MaxRows >= 0 && Out.Num() > MaxRows) Out.SetNum(MaxRows);
}
