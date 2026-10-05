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
