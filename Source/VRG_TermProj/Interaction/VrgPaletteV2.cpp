#include "Interaction/VrgPaletteV2.h"
#include "Interaction/LayoutRackPalette.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgUiStyle.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"

// UI pixels below are layout choices of the demo (1080p reference, UMG DPI scaling applies), not equipment dimensions.
namespace
{
	using namespace VrgUi::Theme;

	FButtonStyle MakeButtonStyle(const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Pressed, float Radius = 6.f)
	{
		FButtonStyle S;
		S.SetNormal(FSlateRoundedBoxBrush(Normal, Radius));
		S.SetHovered(FSlateRoundedBoxBrush(Hover, Radius));
		S.SetPressed(FSlateRoundedBoxBrush(Pressed, Radius));
		S.SetDisabled(FSlateRoundedBoxBrush(Normal, Radius));
		S.SetNormalPadding(FMargin(0.f));
		S.SetPressedPadding(FMargin(0.f));
		return S;
	}
	FButtonStyle ItemStyle() { return MakeButtonStyle(WithAlpha(Text(), 0.f), WithAlpha(Text(), .07f), WithAlpha(Text(), .13f)); }
	FButtonStyle TabStyle(bool bOn) { return bOn ? MakeButtonStyle(AccentDim(), Accent(), AccentDim()) : MakeButtonStyle(WithAlpha(Text(), .04f), WithAlpha(Text(), .09f), WithAlpha(Text(), .14f)); }

	UTextBlock* MakeText(UWidgetTree* Tree, const FString& S, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		auto* T = Tree->ConstructWidget<UTextBlock>();
		T->SetText(FText::FromString(S));
		T->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		T->SetColorAndOpacity(FSlateColor(Color));
		return T;
	}
	UWidget* MakeRule(UWidgetTree* Tree)
	{
		auto* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetHeightOverride(1.f);
		auto* B = Tree->ConstructWidget<UBorder>();
		B->SetBrush(FSlateColorBrush(Line()));
		Box->AddChild(B);
		return Box;
	}
	ADataCenterDebugController* Ctrl(const UUserWidget* W) { return Cast<ADataCenterDebugController>(W->GetOwningPlayer()); }
	void Show(UWidget* W, bool bShow) { if (W) W->SetVisibility(bShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
}

ULayoutRackButton* UVrgPaletteV2::AddItem(UVerticalBox* Page, const FLinearColor& Swatch, const TCHAR* Title, const TCHAR* Sub, TObjectPtr<UTextBlock>* OutTitle)
{
	auto* Button = WidgetTree->ConstructWidget<ULayoutRackButton>();
	Button->SetStyle(ItemStyle());
	auto* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	auto* SwatchBox = WidgetTree->ConstructWidget<USizeBox>();
	SwatchBox->SetWidthOverride(3.f); SwatchBox->SetHeightOverride(30.f);
	auto* SwatchFill = WidgetTree->ConstructWidget<UBorder>();
	SwatchFill->SetBrush(FSlateRoundedBoxBrush(Swatch, 1.5f));
	SwatchBox->AddChild(SwatchFill);
	if (auto* S = Row->AddChildToHorizontalBox(SwatchBox)) { S->SetVerticalAlignment(VAlign_Center); S->SetPadding(FMargin(0, 0, 10, 0)); }
	auto* Texts = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* TitleText = MakeText(WidgetTree, Title, 11, Text());
	Texts->AddChildToVerticalBox(TitleText);
	if (Sub && *Sub) Texts->AddChildToVerticalBox(MakeText(WidgetTree, Sub, 9, Text2()));
	if (auto* S = Row->AddChildToHorizontalBox(Texts)) { S->SetVerticalAlignment(VAlign_Center); S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); }
	Button->AddChild(Row);
	if (auto* S = Cast<UButtonSlot>(Button->GetContentSlot())) { S->SetPadding(FMargin(8, 6, 8, 6)); S->SetHorizontalAlignment(HAlign_Fill); }
	if (auto* S = Page->AddChildToVerticalBox(Button)) S->SetPadding(FMargin(0, 0, 0, 2));
	if (OutTitle) *OutTitle = TitleText;
	return Button;
}

void UVrgPaletteV2::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(false);
	auto* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	auto* Width = WidgetTree->ConstructWidget<USizeBox>();
	Width->SetWidthOverride(272.f);
	auto* PanelBorder = WidgetTree->ConstructWidget<UBorder>();
	PanelBorder->SetBrush(FSlateRoundedBoxBrush(Panel(), 10.f, Line(), 1.f));
	PanelBorder->SetPadding(FMargin(12, 12, 12, 10));
	PanelBorder->OnMouseButtonDownEvent.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UVrgPaletteV2, OnPanelMouseDown));
	Width->AddChild(PanelBorder);
	if (auto* S = Root->AddChildToCanvas(Width))
	{
		S->SetAnchors(FAnchors(0, 0)); S->SetAlignment(FVector2D(0, 0));
		S->SetPosition(FVector2D(16, 60)); S->SetAutoSize(true);
	}
	auto* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	PanelBorder->AddChild(Col);
	auto Pad = [](UVerticalBoxSlot* S, float T, float B) { if (S) S->SetPadding(FMargin(0, T, 0, B)); };

	Pad(Col->AddChildToVerticalBox(MakeText(WidgetTree, TEXT("장비 배치"), 12, Text(), true)), 0, 1);
	Pad(Col->AddChildToVerticalBox(MakeText(WidgetTree, TEXT("항목을 고르고 바닥을 눌러 놓습니다"), 9, Text2())), 0, 10);

	// Category tabs.
	const TCHAR* TabNames[] = { TEXT("IT 장비"), TEXT("냉각"), TEXT("설비 마당"), TEXT("방·천장") };
	auto* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
	for (int32 I = 0; I < 4; ++I)
	{
		auto* B = WidgetTree->ConstructWidget<ULayoutRackButton>();
		B->SetStyle(TabStyle(I == 0));
		UTextBlock* L = MakeText(WidgetTree, TabNames[I], 10, I == 0 ? FLinearColor::White : Text2());
		B->AddChild(L);
		if (auto* S = Cast<UButtonSlot>(B->GetContentSlot())) { S->SetPadding(FMargin(4, 5, 4, 5)); S->SetHorizontalAlignment(HAlign_Center); }
		if (auto* S = Tabs->AddChildToHorizontalBox(B)) { S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S->SetPadding(FMargin(I == 0 ? 0 : 2, 0, I == 3 ? 0 : 2, 0)); }
		TabButtons.Add(B); TabLabels.Add(L);
	}
	TabButtons[0]->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnTabIt);
	TabButtons[1]->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnTabCooling);
	TabButtons[2]->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnTabPlant);
	TabButtons[3]->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnTabRoom);
	Pad(Col->AddChildToVerticalBox(Tabs), 0, 10);

	for (int32 I = 0; I < 4; ++I)
	{
		auto* Page = WidgetTree->ConstructWidget<UVerticalBox>();
		Pages.Add(Page);
		Pad(Col->AddChildToVerticalBox(Page), 0, 4);
	}
	// Category colours: IT blue, air cooling teal, yard plant purple, room structure gray (display only).
	AddItem(Pages[0], Accent(), TEXT("GB200 NVL72 랙"), TEXT("수랭 + 공랭 · 장비 37대"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddRack);
	AddItem(Pages[0], Accent(), TEXT("스파인 캐비닛"), TEXT("Quantum 스위치 · 공랭 전용"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddSpine);
	AddItem(Pages[0], Accent(), TEXT("CDU"), TEXT("줄 단위 열교환기 · 펌프 1+1"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddCdu);
	AddItem(Pages[1], Teal(), TEXT("인로우 쿨러"), TEXT("랙 줄 사이 · 뒤 흡입"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddInRow);
	AddItem(Pages[1], Teal(), TEXT("벽면 CRAH"), TEXT("벽에 붙임 · 위 흡입"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddPerimeter);
	AddItem(Pages[2], Purple(), TEXT("FWS 드라이쿨러"), TEXT("CDU 1차측 열 방출"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddFwsDryCooler);
	AddItem(Pages[2], Purple(), TEXT("CW 드라이쿨러"), TEXT("칠러 응축수 열 방출"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddCwDryCooler);
	AddItem(Pages[2], Purple(), TEXT("냉수 칠러"), TEXT("CRAH 코일 냉수"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddChiller);
	AddItem(Pages[2], Purple(), TEXT("트림 칠러"), TEXT("FWS 보조 냉각"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddTrimChiller);
	AddItem(Pages[2], Purple(), TEXT("FWS 펌프"), TEXT("설비수 순환"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddFwsPump);
	AddItem(Pages[2], Purple(), TEXT("냉수 펌프"), TEXT("CHW 순환"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddChwPump);
	AddItem(Pages[2], Purple(), TEXT("응축수 펌프"), TEXT("CW 순환"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddCwPump);
	AddItem(Pages[2], Purple(), TEXT("이코노마이저"), TEXT("수측 판형 열교환기 · 최대 1대"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddEconomizer);
	AddItem(Pages[3], Gray(), TEXT("방 설정"), TEXT("크기·높이 숫자 입력 (m)"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnRoomSettings);
	RoomBoundsButton = AddItem(Pages[3], Gray(), TEXT("방 범위 편집"), TEXT("모서리·천장 핸들 끌기"), &RoomBoundsTitle);
	RoomBoundsButton->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnRoomBounds);
	AddItem(Pages[3], Gray(), TEXT("벽"), TEXT("공기가 지나지 못하는 고체"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddWall);
	AddItem(Pages[3], Gray(), TEXT("차단막"), TEXT("얇은 공기 차단막"))->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnAddCurtain);
	CeilingButton = AddItem(Pages[3], Gray(), TEXT("부분 천장"), TEXT("천장 판 한 장 추가"));
	CeilingButton->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnCeiling);
	CeilingEditButton = AddItem(Pages[3], Gray(), TEXT("천장 선택·삭제"), TEXT("판 고르기·옮기기"), &CeilingEditTitle);
	CeilingEditButton->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnCeilingEdit);
	HoleButton = AddItem(Pages[3], Amber(), TEXT("선택 판에 구멍 추가"), TEXT("판 위아래 공기 연결"));
	HoleButton->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnHole);
	HoleEditButton = AddItem(Pages[3], Amber(), TEXT("구멍 선택·삭제"), TEXT("구멍 고르기·옮기기"), &HoleEditTitle);
	HoleEditButton->OnClicked.AddDynamic(this, &UVrgPaletteV2::OnHoleEdit);
	CeilingNote = MakeText(WidgetTree, TEXT("부분 천장은 방 설정을 '수동'으로 바꾸면 쓸 수 있습니다"), 9, Text3());
	CeilingNote->SetAutoWrapText(true);
	if (auto* S = Pages[3]->AddChildToVerticalBox(CeilingNote)) S->SetPadding(FMargin(8, 4, 8, 2));

	PlacingNote = MakeText(WidgetTree, TEXT("배치 중 · 클릭 확정 · R 회전 · Esc 취소"), 10, Amber());
	Pad(Col->AddChildToVerticalBox(PlacingNote), 4, 6);
	Pad(Col->AddChildToVerticalBox(MakeRule(WidgetTree)), 4, 6);
	Pad(Col->AddChildToVerticalBox(MakeText(WidgetTree, TEXT("F2 기존 UI로 보기"), 9, Text3())), 0, 0);
	SetTab(0);
}

void UVrgPaletteV2::SetTab(int32 Tab)
{
	ActiveTab = FMath::Clamp(Tab, 0, 3);
	for (int32 I = 0; I < Pages.Num(); ++I)
	{
		Pages[I]->SetVisibility(I == ActiveTab ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		TabButtons[I]->SetStyle(TabStyle(I == ActiveTab));
		TabLabels[I]->SetColorAndOpacity(FSlateColor(I == ActiveTab ? FLinearColor::White : Text2()));
	}
}

void UVrgPaletteV2::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
	Super::NativeTick(Geometry, DeltaTime);
	ADataCenterDebugController* C = Ctrl(this);
	if (!C) return;
	// Same availability rules as the existing palette (ULayoutRackPalette::NativeTick); unavailable items are hidden.
	const bool bManual = C->GetRoomSettingsDraft().bManual;
	const bool bDragging = C->IsLayoutDragging();
	Show(CeilingButton, bManual);
	Show(CeilingEditButton, bManual);
	Show(CeilingNote, !bManual);
	if (CeilingEditTitle) CeilingEditTitle->SetText(FText::FromString(C->IsRoomCeilingEditing() ? TEXT("천장 편집 끝내기") : TEXT("천장 선택·삭제")));
	if (RoomBoundsTitle) RoomBoundsTitle->SetText(FText::FromString(C->IsRoomBoundsEditing() ? TEXT("방 범위 편집 끝내기") : TEXT("방 범위 편집")));
	Show(HoleButton, C->IsRoomCeilingEditing() && C->GetSelectedRoomCeilingIndices().Num() == 1 && !bDragging);
	FRoomCeilingPlate P; bool bValid = false;
	Show(HoleEditButton, !bDragging && (C->IsRoomCeilingHoleEditing() || (C->GetSelectedRoomCeilingIndices().Num() == 1 && C->GetRoomCeilingPreview(P, bValid) && !P.Holes.IsEmpty())));
	if (HoleEditTitle) HoleEditTitle->SetText(FText::FromString(C->IsRoomCeilingHoleEditing() ? TEXT("구멍 편집 끝내기") : TEXT("구멍 선택·삭제")));
	if (PlacingNote) PlacingNote->SetVisibility(C->IsAddingRack() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if ((C->IsRoomCeilingEditing() || C->IsRoomCeilingHoleEditing() || C->IsRoomBoundsEditing()) && ActiveTab != 3) SetTab(3);
}

FEventReply UVrgPaletteV2::OnPanelMouseDown(FGeometry MyGeometry, const FPointerEvent& MouseEvent)
{
	return FEventReply(true);
}

void UVrgPaletteV2::OnTabIt() { SetTab(0); }
void UVrgPaletteV2::OnTabCooling() { SetTab(1); }
void UVrgPaletteV2::OnTabPlant() { SetTab(2); }
void UVrgPaletteV2::OnTabRoom() { SetTab(3); }
void UVrgPaletteV2::OnAddRack() { if (auto* C = Ctrl(this)) C->BeginRackAddition(); }
void UVrgPaletteV2::OnAddSpine() { if (auto* C = Ctrl(this)) C->BeginSpineAddition(); }
void UVrgPaletteV2::OnAddCdu() { if (auto* C = Ctrl(this)) C->BeginCduAddition(); }
void UVrgPaletteV2::OnAddInRow() { if (auto* C = Ctrl(this)) C->BeginInRowCrahAddition(); }
void UVrgPaletteV2::OnAddPerimeter() { if (auto* C = Ctrl(this)) C->BeginPerimeterCrahAddition(); }
void UVrgPaletteV2::OnAddFwsDryCooler() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::FwsDryCooler); }
void UVrgPaletteV2::OnAddCwDryCooler() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::CwDryCooler); }
void UVrgPaletteV2::OnAddChiller() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::Chiller); }
void UVrgPaletteV2::OnAddTrimChiller() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::TrimChiller); }
void UVrgPaletteV2::OnAddFwsPump() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::FwsPump); }
void UVrgPaletteV2::OnAddChwPump() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::ChwPump); }
void UVrgPaletteV2::OnAddCwPump() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::CwPump); }
void UVrgPaletteV2::OnAddEconomizer() { if (auto* C = Ctrl(this)) C->BeginPlantAddition(EPlantUnitKind::Economizer); }
void UVrgPaletteV2::OnRoomSettings() { if (auto* C = Ctrl(this)) C->OpenRoomSettings(); }
void UVrgPaletteV2::OnRoomBounds() { if (auto* C = Ctrl(this)) C->ToggleRoomBoundsEditing(); }
void UVrgPaletteV2::OnAddWall() { if (auto* C = Ctrl(this)) C->BeginWallAddition(); }
void UVrgPaletteV2::OnAddCurtain() { if (auto* C = Ctrl(this)) C->BeginCurtainAddition(); }
void UVrgPaletteV2::OnCeiling() { if (auto* C = Ctrl(this)) C->OpenRoomCeilingSettings(); }
void UVrgPaletteV2::OnCeilingEdit() { if (auto* C = Ctrl(this)) C->ToggleRoomCeilingEditing(); }
void UVrgPaletteV2::OnHole() { if (auto* C = Ctrl(this)) C->OpenRoomCeilingHoleSettings(); }
void UVrgPaletteV2::OnHoleEdit() { if (auto* C = Ctrl(this)) C->ToggleRoomCeilingHoleEditing(); }
