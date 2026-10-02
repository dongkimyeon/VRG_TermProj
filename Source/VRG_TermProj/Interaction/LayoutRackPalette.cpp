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
}

void ULayoutRackPalette::OnAddRack()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->BeginRackAddition();
}
