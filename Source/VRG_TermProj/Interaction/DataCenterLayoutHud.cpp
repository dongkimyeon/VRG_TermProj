#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/DataCenterDebugController.h"

void ADataCenterLayoutHud::DrawHUD()
{
	Super::DrawHUD();
	const auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	FBox2D Rect;
	if (!Canvas || !Controller || !Controller->GetLayoutMarqueeRect(Rect)) return;
	const FLinearColor Color(.2f, .8f, 1.f);
	DrawLine(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Min.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Max.Y, Rect.Min.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Min.X, Rect.Max.Y, Rect.Min.X, Rect.Min.Y, Color, 2.f);
}
