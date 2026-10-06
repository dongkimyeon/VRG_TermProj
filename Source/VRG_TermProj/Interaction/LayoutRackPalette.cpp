#include "Interaction/LayoutRackPalette.h"
#include "Interaction/DataCenterDebugController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

void ULayoutRackPalette::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(false);
	auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Canvas;
	Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	auto* Button = WidgetTree->ConstructWidget<ULayoutRackButton>();
	Button->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnAddRack);
	auto* Label = WidgetTree->ConstructWidget<UTextBlock>();
	Label->SetText(FText::FromString(TEXT("GB200 NVL72 랙 추가")));
	FSlateFontInfo Font = Label->GetFont(); Font.Size = 16; Label->SetFont(Font);
	Button->AddChild(Label);
	auto* ButtonSlot = Canvas->AddChildToCanvas(Button);
	ButtonSlot->SetAnchors(FAnchors(0, 1));
	ButtonSlot->SetAlignment(FVector2D(0, 1));
	ButtonSlot->SetPosition(FVector2D(20, -20));
	ButtonSlot->SetSize(FVector2D(240, 44)); // UI pixels, not equipment dimensions.
 auto* SpineButton = WidgetTree->ConstructWidget<ULayoutRackButton>();
 SpineButton->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnAddSpine);
 auto* SpineLabel = WidgetTree->ConstructWidget<UTextBlock>();
 SpineLabel->SetText(FText::FromString(TEXT("스파인 캐비닛 추가"))); SpineLabel->SetFont(Font);
 SpineButton->AddChild(SpineLabel);
 auto* SpineSlot = Canvas->AddChildToCanvas(SpineButton);
 SpineSlot->SetAnchors(FAnchors(0, 1)); SpineSlot->SetAlignment(FVector2D(0, 1));
 SpineSlot->SetPosition(FVector2D(270, -20)); SpineSlot->SetSize(FVector2D(240, 44));
 auto* CduButton = WidgetTree->ConstructWidget<ULayoutRackButton>();
 CduButton->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnAddCdu);
 auto* CduLabel = WidgetTree->ConstructWidget<UTextBlock>();
 CduLabel->SetText(FText::FromString(TEXT("CDU 추가"))); CduLabel->SetFont(Font); CduButton->AddChild(CduLabel);
 auto* CduSlot = Canvas->AddChildToCanvas(CduButton);
 CduSlot->SetAnchors(FAnchors(0, 1)); CduSlot->SetAlignment(FVector2D(0, 1));
 CduSlot->SetPosition(FVector2D(520, -20)); CduSlot->SetSize(FVector2D(180, 44));
 auto* CrahButton=WidgetTree->ConstructWidget<ULayoutRackButton>();
 CrahButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnAddCrah);
 auto* CrahLabel=WidgetTree->ConstructWidget<UTextBlock>();
 CrahLabel->SetText(FText::FromString(TEXT("인로우 쿨러 추가")));
 CrahLabel->SetFont(Font); CrahButton->AddChild(CrahLabel);
 auto* CrahSlot=Canvas->AddChildToCanvas(CrahButton);
 CrahSlot->SetAnchors(FAnchors(0,1)); CrahSlot->SetAlignment(FVector2D(0,1));
 CrahSlot->SetPosition(FVector2D(710,-20)); CrahSlot->SetSize(FVector2D(200,44));
 auto* PerimeterButton=WidgetTree->ConstructWidget<ULayoutRackButton>();
 PerimeterButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnAddPerimeterCrah);
 auto* PerimeterLabel=WidgetTree->ConstructWidget<UTextBlock>();
 PerimeterLabel->SetText(FText::FromString(TEXT("벽면 CRAH 추가")));
 PerimeterLabel->SetFont(Font); PerimeterButton->AddChild(PerimeterLabel);
 auto* PerimeterSlot=Canvas->AddChildToCanvas(PerimeterButton);
 PerimeterSlot->SetAnchors(FAnchors(0,1)); PerimeterSlot->SetAlignment(FVector2D(0,1));
 PerimeterSlot->SetPosition(FVector2D(920,-20)); PerimeterSlot->SetSize(FVector2D(200,44));
	auto* RoomButton = WidgetTree->ConstructWidget<ULayoutRackButton>();
	RoomButton->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnRoomSettings);
	auto* RoomLabel = WidgetTree->ConstructWidget<UTextBlock>();
	RoomLabel->SetText(FText::FromString(TEXT("방 설정 (m)"))); RoomLabel->SetFont(Font); RoomButton->AddChild(RoomLabel);
	auto* RoomSlot = Canvas->AddChildToCanvas(RoomButton);
	RoomSlot->SetAnchors(FAnchors(0, 1)); RoomSlot->SetAlignment(FVector2D(0, 1));
	RoomSlot->SetPosition(FVector2D(20, -74)); RoomSlot->SetSize(FVector2D(240, 44));
	// Walls / air curtains: impermeable, adiabatic solids in the air grid (second row, next to the room settings).
	auto AddBarrierButton = [&](const TCHAR* Text, float X)
	{
		auto* BarrierButton = WidgetTree->ConstructWidget<ULayoutRackButton>();
		auto* BarrierLabel = WidgetTree->ConstructWidget<UTextBlock>();
		BarrierLabel->SetText(FText::FromString(Text)); BarrierLabel->SetFont(Font); BarrierButton->AddChild(BarrierLabel);
		auto* BarrierSlot = Canvas->AddChildToCanvas(BarrierButton);
		BarrierSlot->SetAnchors(FAnchors(0, 1)); BarrierSlot->SetAlignment(FVector2D(0, 1));
		BarrierSlot->SetPosition(FVector2D(X, -74)); BarrierSlot->SetSize(FVector2D(180, 44));
		return BarrierButton;
	};
	AddBarrierButton(TEXT("벽 추가"), 270)->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnAddWall);
	AddBarrierButton(TEXT("차단막 추가"), 460)->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnAddCurtain);
	AddBarrierButton(TEXT("방 범위 편집"), 650)->OnClicked.AddDynamic(this, &ULayoutRackPalette::OnRoomBounds);
	CeilingButton=AddBarrierButton(TEXT("부분 천장 추가"),840);
	CeilingButton->SetIsEnabled(false); CeilingButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnRoomCeiling);
	CeilingEditButton=AddBarrierButton(TEXT("천장 선택·삭제"),1030);
	CeilingEditButton->SetIsEnabled(false); CeilingEditButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnRoomCeilingEdit);
	// Third row uses existing UI pixel sizes, not a new physical dimension or a focusable world input.
	CeilingHoleButton=WidgetTree->ConstructWidget<ULayoutRackButton>();
	auto* HoleLabel=WidgetTree->ConstructWidget<UTextBlock>(); HoleLabel->SetText(FText::FromString(TEXT("선택 판 구멍 추가"))); HoleLabel->SetFont(Font); CeilingHoleButton->AddChild(HoleLabel);
	auto* HoleSlot=Canvas->AddChildToCanvas(CeilingHoleButton); HoleSlot->SetAnchors(FAnchors(0,1)); HoleSlot->SetAlignment(FVector2D(0,1));
	HoleSlot->SetPosition(FVector2D(20,-128)); HoleSlot->SetSize(FVector2D(240,44));
	CeilingHoleButton->SetIsEnabled(false); CeilingHoleButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnRoomCeilingHole);
	CeilingHoleEditButton=WidgetTree->ConstructWidget<ULayoutRackButton>();
	auto* EditLabel=WidgetTree->ConstructWidget<UTextBlock>(); EditLabel->SetText(FText::FromString(TEXT("구멍 선택·삭제"))); EditLabel->SetFont(Font); CeilingHoleEditButton->AddChild(EditLabel);
	auto* EditSlot=Canvas->AddChildToCanvas(CeilingHoleEditButton); EditSlot->SetAnchors(FAnchors(0,1)); EditSlot->SetAlignment(FVector2D(0,1));
	EditSlot->SetPosition(FVector2D(270,-128)); EditSlot->SetSize(FVector2D(240,44));
	CeilingHoleEditButton->SetIsEnabled(false); CeilingHoleEditButton->OnClicked.AddDynamic(this,&ULayoutRackPalette::OnRoomCeilingHoleEdit);
}

void ULayoutRackPalette::OnAddRack()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginRackAddition();
}

void ULayoutRackPalette::OnAddSpine()
{
 if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginSpineAddition();
}

void ULayoutRackPalette::OnAddCdu()
{
 if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginCduAddition();
}

void ULayoutRackPalette::OnAddCrah()
{
 if(auto* Controller=Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginInRowCrahAddition();
}

void ULayoutRackPalette::OnAddPerimeterCrah()
{
 if(auto* Controller=Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginPerimeterCrahAddition();
}

void ULayoutRackPalette::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
	Super::NativeTick(Geometry,DeltaTime);
	if(CeilingHoleButton)
	{
		auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer());
		CeilingHoleButton->SetIsEnabled(C && C->IsRoomCeilingEditing() && C->GetSelectedRoomCeilingIndices().Num()==1 && !C->IsLayoutDragging());
	}
	if(CeilingHoleEditButton)
	{
		auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer()); FRoomCeilingPlate P; bool bValid=false;
		CeilingHoleEditButton->SetIsEnabled(C && !C->IsLayoutDragging() && (C->IsRoomCeilingHoleEditing() || (C->GetSelectedRoomCeilingIndices().Num()==1 && C->GetRoomCeilingPreview(P,bValid) && !P.Holes.IsEmpty())));
		if(auto* Label=Cast<UTextBlock>(CeilingHoleEditButton->GetChildAt(0))) Label->SetText(FText::FromString(C && C->IsRoomCeilingHoleEditing()?TEXT("구멍 편집 끄기"):TEXT("구멍 선택·삭제")));
	}
	if (CeilingButton) { auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer()); CeilingButton->SetIsEnabled(C && C->GetRoomSettingsDraft().bManual); }
	if(CeilingEditButton)
	{
		auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer()); CeilingEditButton->SetIsEnabled(C && C->GetRoomSettingsDraft().bManual);
		if(auto* Label=Cast<UTextBlock>(CeilingEditButton->GetChildAt(0))) Label->SetText(FText::FromString(C && C->IsRoomCeilingEditing()?TEXT("천장 편집 끄기"):TEXT("천장 선택·삭제")));
	}
}

void ULayoutRackPalette::OnRoomCeilingHoleEdit()
{
	if(auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer())) C->ToggleRoomCeilingHoleEditing();
}

void ULayoutRackPalette::OnRoomCeilingHole()
{
	if(auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer())) C->OpenRoomCeilingHoleSettings();
}

void ULayoutRackPalette::OnRoomCeilingEdit()
{
	if(auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer())) C->ToggleRoomCeilingEditing();
}

void ULayoutRackPalette::OnRoomCeiling()
{
	if (auto* C=Cast<ADataCenterDebugController>(GetOwningPlayer())) C->OpenRoomCeilingSettings();
}

void ULayoutRackPalette::OnRoomSettings()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->OpenRoomSettings();
}

void ULayoutRackPalette::OnRoomBounds()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->ToggleRoomBoundsEditing();
}

void ULayoutRackPalette::OnAddWall()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginWallAddition();
}

void ULayoutRackPalette::OnAddCurtain()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginCurtainAddition();
}
