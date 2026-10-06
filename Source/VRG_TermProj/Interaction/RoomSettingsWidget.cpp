#include "Interaction/RoomSettingsWidget.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/LayoutRackPalette.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/DefaultValueHelper.h"

bool URoomSettingsWidget::ParseDraft(bool bManual, const TArray<FString>& Fields, FRoomAirDomainSettings& InOutSettings, FString& Reason)
{
	Reason.Reset();
	FRoomAirDomainSettings Settings = InOutSettings;
	Settings.bManual = bManual;
	if (bManual)
	{
		if (Fields.Num() != 5) { Reason = TEXT("방 설정의 숫자 입력 5개가 필요합니다."); return false; }
		double Values[5];
		const TCHAR* Names[] = {TEXT("최소 X"), TEXT("최대 X"), TEXT("최소 Y"), TEXT("최대 Y"), TEXT("천장 높이")};
		for (int32 I = 0; I < 5; ++I)
			if (!FDefaultValueHelper::ParseDouble(Fields[I].TrimStartAndEnd(), Values[I]) || !FMath::IsFinite(Values[I]))
			{ Reason = FString::Printf(TEXT("%s: 유한한 숫자를 m 단위로 입력하세요 (소수점은 .)."), Names[I]); return false; }
		Settings.MinXYM = FVector2D(Values[0], Values[2]); Settings.MaxXYM = FVector2D(Values[1], Values[3]);
		Settings.CeilingHeightM = Values[4];
	}
	InOutSettings = Settings;
	return true;
}

void URoomSettingsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized(); SetIsFocusable(true);
	auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(); WidgetTree->RootWidget = Canvas;
	// Full-screen hit-tested backdrop; the controller also gates all world input while open.
	auto* Backdrop = WidgetTree->ConstructWidget<UBorder>(); Backdrop->SetBrushColor(FLinearColor(0, 0, 0, .65f));
	auto* BackSlot = Canvas->AddChildToCanvas(Backdrop); BackSlot->SetAnchors(FAnchors(0, 0, 1, 1)); BackSlot->SetOffsets(FMargin(0));
	auto* Border = WidgetTree->ConstructWidget<UBorder>(); Border->SetBrushColor(FLinearColor(.035f, .045f, .055f, 1)); Border->SetPadding(FMargin(18));
	auto* PanelSlot = Canvas->AddChildToCanvas(Border); PanelSlot->SetAnchors(FAnchors(.5f, .5f)); PanelSlot->SetAlignment(FVector2D(.5, .5)); PanelSlot->SetSize(FVector2D(600, 600));
	auto* Scroll = WidgetTree->ConstructWidget<UScrollBox>(); Border->SetContent(Scroll);
	auto* Body = WidgetTree->ConstructWidget<UVerticalBox>(); Scroll->AddChild(Body);
	auto Text = [&](const FString& Value)
	{
		auto* Label = WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(FText::FromString(Value));
		Label->SetAutoWrapText(true); auto Font = Label->GetFont(); Font.Size = 16; Label->SetFont(Font);
		Body->AddChildToVerticalBox(Label)->SetPadding(FMargin(0, 0, 0, 10)); return Label;
	};
	Title = Text(TEXT("방 설정 (m)")); Intro = Text(TEXT("바닥 Z=0m. 입력은 초안이며 적용 버튼으로 확정합니다.\n공기 격자는 시뮬레이션 진입 때 적용됩니다."));
	Manual = WidgetTree->ConstructWidget<UCheckBox>();
	auto* ModeLabel = WidgetTree->ConstructWidget<UTextBlock>(); ModeLabel->SetText(FText::FromString(TEXT("수동 방 (해제하면 장비에 자동 맞춤)")));
	Manual->AddChild(ModeLabel); Manual->OnCheckStateChanged.AddDynamic(this, &URoomSettingsWidget::OnModeChanged);
	Body->AddChildToVerticalBox(Manual)->SetPadding(FMargin(0, 0, 0, 12));
	const TCHAR* Names[] = {TEXT("최소 X (m)"), TEXT("최대 X (m)"), TEXT("최소 Y (m)"), TEXT("최대 Y (m)"), TEXT("천장 높이 (m)")};
	for (const auto* Name : Names)
	{
		auto* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		auto* Label = WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(FText::FromString(Name));
		if (Inputs.Num()==4) HeightLabel=Label;
		Row->AddChildToHorizontalBox(Label)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		auto* Input = WidgetTree->ConstructWidget<UEditableTextBox>(); Input->SetMinDesiredWidth(300);
		Input->OnTextChanged.AddDynamic(this, &URoomSettingsWidget::OnDraftChanged); Inputs.Add(Input);
		Row->AddChildToHorizontalBox(Input); Body->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 8));
	}
	auto* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto Button = [&](const TCHAR* Name)
	{
		auto* B = WidgetTree->ConstructWidget<ULayoutRackButton>();
		auto* Label = WidgetTree->ConstructWidget<UTextBlock>(); Label->SetText(FText::FromString(Name)); B->AddChild(Label);
		Buttons->AddChildToHorizontalBox(B)->SetPadding(FMargin(0, 8, 20, 12)); return B;
	};
	auto* ApplyButton = Button(TEXT("적용")); ApplyLabel = Cast<UTextBlock>(ApplyButton->GetChildAt(0));
	ApplyButton->OnClicked.AddDynamic(this, &URoomSettingsWidget::OnApply);
	Button(TEXT("닫기 (Esc)"))->OnClicked.AddDynamic(this, &URoomSettingsWidget::OnClose);
	Body->AddChildToVerticalBox(Buttons); Status = Text(TEXT("")); Plan = Text(TEXT(""));
}

void URoomSettingsWidget::InitializeDraft(const FRoomAirDomainSettings& Settings, bool bCeiling)
{
	bCeilingMode = bCeiling;
	if (bCeiling)
	{
		Title->SetText(FText::FromString(TEXT("부분 천장 추가 (m)")));
		Intro->SetText(FText::FromString(TEXT("XY 범위와 방 천장보다 낮은 판 높이를 입력하세요. 추가 버튼만 확정합니다.\n한 셀 두께 판 · 공기 격자는 다음 진입에 적용 · 판 저장 미지원.")));
		ApplyLabel->SetText(FText::FromString(TEXT("추가"))); Manual->SetVisibility(ESlateVisibility::Collapsed);
		HeightLabel->SetText(FText::FromString(TEXT("판 높이 (m)")));
	}
	DraftSeed = Settings; bInitializing = true;
	const double Values[] = {Settings.MinXYM.X, Settings.MaxXYM.X, Settings.MinXYM.Y, Settings.MaxXYM.Y, Settings.CeilingHeightM};
	for (int32 I = 0; I < Inputs.Num(); ++I) Inputs[I]->SetText(FText::FromString(FString::Printf(TEXT("%.17g"), Values[I])));
	Manual->SetIsChecked(Settings.bManual); OnModeChanged(Settings.bManual); bInitializing = false;
	Status->SetText(FText::FromString(TEXT("미확정 초안은 닫으면 버립니다. 적용된 변경은 유지됩니다.")));
	if (auto* C = Cast<ADataCenterDebugController>(GetOwningPlayer()))
		Plan->SetText(FText::FromString(bCeiling ? TEXT("초안은 추가할 때 검증합니다. 방의 천장 높이는 변경하지 않습니다.") : TEXT("현재 설정의 계획:\n") + C->DescribeRoomSettingsPlan(Settings)));
}

void URoomSettingsWidget::OnDraftChanged(const FText& Text)
{
	if (bInitializing || !Status) return;
	Status->SetColorAndOpacity(FLinearColor(1, .8f, .3f));
	Status->SetText(FText::FromString(bCeilingMode ? TEXT("초안 변경됨 · 아직 미추가. 아래 계획은 마지막 검증 결과입니다.") : TEXT("초안 변경됨 · 아직 미적용. 아래 계획은 마지막 적용 설정입니다.")));
}

void URoomSettingsWidget::OnModeChanged(bool bManual)
{
	for (const auto& Input : Inputs) Input->SetIsEnabled(bManual);
	OnDraftChanged(FText::GetEmpty());
}

void URoomSettingsWidget::OnApply()
{
	auto* C = Cast<ADataCenterDebugController>(GetOwningPlayer()); if (!C) return;
	TArray<FString> Fields; for (const auto& Input : Inputs) Fields.Add(Input->GetText().ToString());
	auto Settings = DraftSeed; FString Message;
	bool bOk = ParseDraft(bCeilingMode || Manual->IsChecked(), Fields, Settings, Message);
	if (bOk && bCeilingMode)
	{
		FRoomCeilingPlate P; P.MinXYM=Settings.MinXYM; P.MaxXYM=Settings.MaxXYM; P.HeightM=Settings.CeilingHeightM;
		bOk = C->AddRoomCeiling(P,Message);
	}
	else if (bOk) bOk = C->ApplyRoomSettings(Settings,Message);
	if (!bOk)
	{ Status->SetColorAndOpacity(FLinearColor(1, .3f, .3f)); Status->SetText(FText::FromString(Message)); Plan->SetText(FText::FromString(Message)); return; }
	DraftSeed = Settings;
	Status->SetColorAndOpacity(FLinearColor(.3f, 1, .4f));
	Status->SetText(FText::FromString(bCeilingMode ? TEXT("판 한 장 추가됨. 닫고 Ctrl+Z/Y. 고체는 다음 진입에 적용 · 판 저장 미지원.") : TEXT("설정 적용됨. 공기 격자는 시뮬레이션 진입 때 적용됩니다.")));
	Plan->SetText(FText::FromString(TEXT("적용된 설정의 계획:\n") + Message));
}

void URoomSettingsWidget::OnClose()
{
	if (auto* C = Cast<ADataCenterDebugController>(GetOwningPlayer())) C->CloseRoomSettings(this);
}

void URoomSettingsWidget::NativeDestruct()
{
	OnClose(); Super::NativeDestruct();
}

FReply URoomSettingsWidget::NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
	if (Event.GetKey() == EKeys::Escape) { OnClose(); return FReply::Handled(); }
	return Super::NativeOnPreviewKeyDown(Geometry, Event);
}
