// 2026-11-27 (vrg.Ui.Panels 1, 2026-11-27부터 기본): 방 설정·부분 천장·구멍 창을 새 UI 모양으로 다시 칠한다.
// 위젯 구성·입력·검증·적용 경로는 URoomSettingsWidget 그대로이고 브러시·글꼴·색·여백만 바꾼다(표시 전용).
#include "Interaction/RoomSettingsWidget.h"
#include "Interaction/LayoutRackPalette.h"
#include "Interaction/VrgUiStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Styling/CoreStyle.h"

namespace
{
	using namespace VrgUi::Theme;

	void Restyle(UTextBlock* T, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		if (!T) return;
		T->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		T->SetColorAndOpacity(FSlateColor(Color));
	}

	FButtonStyle ButtonStyle(const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Outline)
	{
		FButtonStyle S;
		S.SetNormal(FSlateRoundedBoxBrush(Normal, Radius(6.f), Outline, 1.f));
		S.SetHovered(FSlateRoundedBoxBrush(Hover, Radius(6.f), Outline, 1.f));
		S.SetPressed(FSlateRoundedBoxBrush(Normal, Radius(6.f), Outline, 1.f));
		S.SetDisabled(FSlateRoundedBoxBrush(WithAlpha(Normal, .4f), Radius(6.f)));
		S.SetNormalPadding(FMargin(0.f));
		S.SetPressedPadding(FMargin(0.f));
		return S;
	}

	void RestyleButton(ULayoutRackButton* B, const FButtonStyle& Style, const FLinearColor& TextColor, bool bBold)
	{
		if (!B) return;
		B->SetStyle(Style);
		if (auto* S = Cast<UButtonSlot>(B->GetContentSlot())) S->SetPadding(FMargin(18, 8, 18, 8));
		Restyle(Cast<UTextBlock>(B->GetChildAt(0)), 14, TextColor, bBold);
	}
}

void URoomSettingsWidget::ApplyNewLook()
{
	if (BackdropBorder) BackdropBorder->SetBrushColor(Scrim());
	if (PanelBorder)
	{
		PanelBorder->SetBrush(FSlateRoundedBoxBrush(PanelHi(), Radius(10.f), Line(), 1.f));
		// The base dialog tints its border dark; the themes need their own panel colour (theme 0 keeps the approved look).
		if (VrgUi::ThemeIndex() != 0) PanelBorder->SetBrushColor(FLinearColor::White);
		PanelBorder->SetPadding(FMargin(26, 22, 26, 18));
		if (auto* PanelSlot = Cast<UCanvasPanelSlot>(PanelBorder->Slot)) PanelSlot->SetSize(FVector2D(620, 560));   // UI pixels
	}
	Restyle(Title, 19, Text(), true);
	Restyle(Intro, 13, Text2());
	Restyle(ModeText, 14, Text());
	for (UTextBlock* L : InputLabels) Restyle(L, 14, Text2());
	FEditableTextBoxStyle Field = Inputs.IsEmpty() ? FEditableTextBoxStyle() : Inputs[0]->GetWidgetStyle();
	Field.SetBackgroundImageNormal(FSlateRoundedBoxBrush(WithAlpha(Text(), .05f), Radius(5.f), Line(), 1.f));
	Field.SetBackgroundImageHovered(FSlateRoundedBoxBrush(WithAlpha(Text(), .08f), Radius(5.f), WithAlpha(Text(), .25f), 1.f));
	Field.SetBackgroundImageFocused(FSlateRoundedBoxBrush(WithAlpha(Accent(), .10f), Radius(5.f), Accent(), 1.f));
	Field.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(WithAlpha(Text(), .02f), Radius(5.f), LineSoft(), 1.f));
	Field.SetForegroundColor(FSlateColor(Text()));
	Field.SetFocusedForegroundColor(FSlateColor(Text()));
	Field.SetPadding(FMargin(10, 6, 10, 6));
	Field.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	for (UEditableTextBox* Box : Inputs) { Box->SetWidgetStyle(Field); Box->SetMinDesiredWidth(260); }
	RestyleButton(ApplyButtonRef, ButtonStyle(AccentDim(), Accent(), WithAlpha(Accent(), .9f)), OnAccent(), true);
	RestyleButton(CloseButtonRef, ButtonStyle(WithAlpha(Text(), .03f), WithAlpha(Text(), .08f), Line()), Text2(), false);
	Restyle(Status, 13, Amber());
	Restyle(Plan, 13, Text2());
}

// Opens the room settings dialog from the console (same path as the palette button; layout mode only). Added with the
// 2026-11-27 demo so the dialog can be captured without mouse input; also handy on any setup.
#include "Interaction/DataCenterDebugController.h"
#include "HAL/IConsoleManager.h"
static FAutoConsoleCommandWithWorld GVrgRoomSettingsCommand(TEXT("vrg.Room.Settings"),
	TEXT("Open the room settings dialog (layout mode), as the palette button does."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		auto* C = World ? Cast<ADataCenterDebugController>(World->GetFirstPlayerController()) : nullptr;
		const bool bOk = C && C->OpenRoomSettings();
		UE_LOG(LogTemp, Display, TEXT("[Room] 방 설정 창 열기: %s"), bOk ? TEXT("성공") : TEXT("거절(배치 모드·다른 편집 중이 아닌지 확인)"));
	}));
