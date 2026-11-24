#include "Interaction/VrgUiStyle.h"
#include "Simulation/Evaluation/VrgFaultMonitor.h"
#include "Simulation/Sensors/InstrumentBank.h"
#include "HAL/IConsoleManager.h"
#include "Algo/StableSort.h"

namespace
{
	TAutoConsoleVariable<int32> CVarUiStyle(TEXT("vrg.Ui.Style"), 0,
		TEXT("2026-11-24 UI demo: 0 = existing HUD and bottom palette (default), 1 = new HUD (status bar, KPI cards, event log, tool strip) and side palette. F2 toggles. Display only."));

	const TCHAR* PhaseName(const FString& P)
	{
		if (P == TEXT("wet")) return TEXT("젖음");
		if (P == TEXT("tripped")) return TEXT("침수 트립");
		if (P == TEXT("damaged")) return TEXT("침수 손상");
		if (P == TEXT("normal")) return TEXT("정상");
		return nullptr;
	}
}

bool VrgUi::IsNewStyle() { return CVarUiStyle.GetValueOnGameThread() != 0; }
void VrgUi::SetNewStyle(bool bNew) { CVarUiStyle->Set(bNew ? 1 : 0, ECVF_SetByConsole); }

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
