// 2026-11-24 UI 데모: 화면 없이 확인할 수 있는 새 HUD의 계산 부분(사건 목록 모으기·정렬·자르기, 추이선 범위, 시각 형식,
// 스타일 전환 CVar). 그리는 모양은 화면 확인 대상이다.
#include "Misc/AutomationTest.h"
#include "Interaction/VrgUiStyle.h"
#include "Simulation/Evaluation/VrgFaultMonitor.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/UserInterfaceSettings.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVrgUiStyleModelTest, "VRG.Interaction.UiStyleModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FVrgUiStyleModelTest::RunTest(const FString& Parameters)
{
	// Style switch: default 0 (existing UI), set and restore.
	const bool bWasNew = VrgUi::IsNewStyle();
	VrgUi::SetNewStyle(true);
	TestTrue(TEXT("style 1 after set"), VrgUi::IsNewStyle());
	VrgUi::SetNewStyle(false);
	TestFalse(TEXT("style 0 after reset"), VrgUi::IsNewStyle());
	VrgUi::SetNewStyle(bWasNew);
	// F2 is the style key; the engine debug binding F2 = "viewmode unlit" must be removed (Config/DefaultInput.ini),
	// otherwise each press also turns the scene unlit (2026-11-24 user report).
	bool bF2Debug = false;
	for (const FKeyBind& B : GetDefault<UPlayerInput>()->DebugExecBindings) bF2Debug |= B.Key == EKeys::F2;
	TestFalse(TEXT("no engine debug exec binding on F2"), bF2Debug);

	// Default style: the new UI since 2026-11-25 (user decision); 0 keeps the previous HUD.
	if (IConsoleVariable* Cv = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Ui.Style")))
	{
		const FString Default = Cv->GetDefaultValue();
		TestEqual(TEXT("default style is the new UI"), Default, FString(TEXT("1")));
	}
	else AddError(TEXT("vrg.Ui.Style missing"));

	// Panel hit test (clicks on new-HUD panels stay out of the scene).
	const TArray<FBox2D> Rects = { FBox2D(FVector2D(10, 10), FVector2D(110, 60)), FBox2D(FVector2D(0, 0), FVector2D(1920, 46)) };
	TestTrue(TEXT("inside a panel"), VrgUi::HitAnyRect(Rects, FVector2D(50, 50)));
	TestTrue(TEXT("on a panel edge"), VrgUi::HitAnyRect(Rects, FVector2D(110, 60)));
	TestTrue(TEXT("inside the status bar"), VrgUi::HitAnyRect(Rects, FVector2D(1500, 20)));
	TestFalse(TEXT("outside every panel"), VrgUi::HitAnyRect(Rects, FVector2D(500, 500)));
	TestFalse(TEXT("no panels"), VrgUi::HitAnyRect({}, FVector2D(50, 50)));
	TestFalse(TEXT("invalid box ignored"), VrgUi::HitAnyRect({ FBox2D(ForceInit) }, FVector2D(0, 0)));

	// One scale for every panel: UMG DPI curve, clamped, not shrinking with a larger viewport.
	const float S720 = VrgUi::HudScale(1280, 720), S1080 = VrgUi::HudScale(1920, 1080), S2160 = VrgUi::HudScale(3840, 2160);
	TestTrue(TEXT("scale in [0.5, 3]"), S720 >= .5f && S2160 <= 3.f);
	TestTrue(TEXT("scale non-decreasing with size"), S720 <= S1080 && S1080 <= S2160);
	TestEqual(TEXT("scale equals the UMG DPI curve at 1080p"), S1080,
		FMath::Clamp(GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(1920, 1080)), .5f, 3.f));

	// 2026-11-27 new-look labels/dialog/patch map: on by default (user decision), and only with the new style.
	if (IConsoleVariable* Pv = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Ui.Panels")))
		TestEqual(TEXT("new-look panels on by default"), Pv->GetDefaultValue(), FString(TEXT("1")));
	else AddError(TEXT("vrg.Ui.Panels missing"));
	{
		const bool bStyle = VrgUi::IsNewStyle(), bPanels = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Ui.Panels"))->GetInt() != 0;
		VrgUi::SetNewStyle(true); VrgUi::SetNewPanels(true);
		TestTrue(TEXT("panels on with new style"), VrgUi::IsNewPanels());
		VrgUi::SetNewStyle(false);
		TestFalse(TEXT("panels need the new style"), VrgUi::IsNewPanels());
		VrgUi::SetNewStyle(bStyle); VrgUi::SetNewPanels(bPanels);
	}

	// Clock text.
	TestEqual(TEXT("clock 0"), VrgUi::FormatClock(0.0), FString(TEXT("00:00.0")));
	TestEqual(TEXT("clock 134.37"), VrgUi::FormatClock(134.37), FString(TEXT("02:14.3")));
	TestEqual(TEXT("clock no tenths"), VrgUi::FormatClock(59.99, false), FString(TEXT("00:59")));
	TestEqual(TEXT("clock negative clamps"), VrgUi::FormatClock(-3.0), FString(TEXT("00:00.0")));

	// Spark range: padding, minimum span, threshold inclusion, empty input.
	double Lo = 0, Hi = 0;
	VrgUi::SparkRange({ 20.0, 22.0 }, .5, VrgUi::NoThreshold(), Lo, Hi);
	TestTrue(TEXT("range covers values"), Lo < 20.0 && Hi > 22.0);
	TestEqual(TEXT("range padding 12%"), Hi - Lo, 2.0 * 1.24, 1e-12);
	VrgUi::SparkRange({ 23.7, 23.7, 23.7 }, .5, VrgUi::NoThreshold(), Lo, Hi);
	TestEqual(TEXT("flat series widened to min span"), Hi - Lo, .5, 1e-12);
	TestEqual(TEXT("flat series centred"), .5 * (Lo + Hi), 23.7, 1e-12);
	VrgUi::SparkRange({ 23.0, 24.0 }, .5, 27.0, Lo, Hi);
	TestTrue(TEXT("threshold inside range"), Lo < 23.0 && Hi > 27.0);
	VrgUi::SparkRange({}, .5, VrgUi::NoThreshold(), Lo, Hi);
	TestTrue(TEXT("empty input gives a finite range"), FMath::IsFinite(Lo) && FMath::IsFinite(Hi) && Hi > Lo);

	// Event rows from the existing records: latest first, severity, truncation, total count.
	FVrgFaultMonitor M;
	M.Reset(FVrgFaultSettings());
	FVrgLeakRecord L; L.TimeS = 3.0; L.HoleDiameterM = .003; L.Site = TEXT("rack:0:tray:0:plate");
	M.AddLeak(L);
	FVrgValveRecord V; V.TimeS = 5.0; V.Rack = 0; V.bClosed = true; V.IsolatedRacks = { 0 }; V.ImpactedRacks = { 0, 1 };
	M.AddValve(V);
	FVrgFaultInput In; In.TimeS = 4.0;
	In.CoolantMassFlowKgs = { .1 }; In.DesignMassFlowKgs = { 1.0 }; In.MaxComponentTempK = { 300.0 }; In.LoopDepleted = { false };
	TestEqual(TEXT("one low-flow fault"), M.Update(In), 1);
	FVrgWaterImpactRecord W; W.TimeS = 4.0; W.EquipmentId = TEXT("rack-0"); W.Rack = 0; W.Slot = 3; W.From = TEXT("wet"); W.To = TEXT("tripped");
	M.AddWaterImpact(W);   // also records the rack's first WaterTrip fault at 4 s

	TArray<VrgUi::FEventRow> Rows; int32 Total = 0;
	VrgUi::BuildEventRows(M, nullptr, -1, Rows, Total);
	TestEqual(TEXT("all records gathered"), Total, 5);
	TestEqual(TEXT("rows untruncated"), Rows.Num(), 5);
	if (Rows.Num() == 5)
	{
		TestEqual(TEXT("latest first (valve 5 s)"), Rows[0].TimeS, 5.0);
		TestEqual(TEXT("valve is info"), Rows[0].Severity, 0);
		TestEqual(TEXT("oldest last (leak 3 s)"), Rows[4].TimeS, 3.0);
		TestEqual(TEXT("leak is caution"), Rows[4].Severity, 1);
		TestTrue(TEXT("leak text has the hole size"), Rows[4].Text.Contains(TEXT("3.0 mm")));
		bool bSorted = true;
		for (int32 I = 1; I < Rows.Num(); ++I)
			bSorted &= Rows[I - 1].TimeS > Rows[I].TimeS || (Rows[I - 1].TimeS == Rows[I].TimeS && Rows[I - 1].Severity >= Rows[I].Severity);
		TestTrue(TEXT("time descending, ties by severity"), bSorted);
		bool bTripText = false;
		for (const VrgUi::FEventRow& R : Rows) bTripText |= R.Text.Contains(TEXT("침수 트립")) && R.Severity == 2;
		TestTrue(TEXT("water trip shown as danger"), bTripText);
	}
	// Display names.
	const TArray<TPair<FString, FString>> Names = { { TEXT("RackThermalActor_UAID_74563C_1248868693"), TEXT("랙 #3") } };
	TestEqual(TEXT("prefix and actor name shortened"), VrgUi::ShortenNames(TEXT("rack/placed/RackThermalActor_UAID_74563C_1248868693 슬롯 0 → 침수 손상"), Names),
		FString(TEXT("랙 #3 슬롯 0 → 침수 손상")));
	TestEqual(TEXT("no doubled rack word"), VrgUi::ShortenNames(TEXT("누수 감지 · 랙 RackThermalActor_UAID_74563C_1248868693 트레이 0"), Names),
		FString(TEXT("누수 감지 · 랙 #3 트레이 0")));
	TestEqual(TEXT("after a separator"), VrgUi::ShortenNames(TEXT("침수 손상 | cdu/placed/CDU_2"), {}), FString(TEXT("침수 손상 | CDU_2")));
	TestEqual(TEXT("unrelated text kept"), VrgUi::ShortenNames(TEXT("FWS 펌프 2"), Names), FString(TEXT("FWS 펌프 2")));

	VrgUi::BuildEventRows(M, nullptr, 2, Rows, Total);
	TestEqual(TEXT("truncated rows"), Rows.Num(), 2);
	TestEqual(TEXT("total before truncation"), Total, 5);
	return true;
}
