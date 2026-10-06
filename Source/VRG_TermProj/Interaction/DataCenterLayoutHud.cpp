#include "Interaction/DataCenterLayoutHud.h"
#include "Interaction/DataCenterDebugController.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

void ADataCenterLayoutHud::DrawHUD()
{
	Super::DrawHUD();
	const auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayerController());
	if (!Canvas || !Controller) return;

	// Key hint on the canvas: the engine's on-screen message list would also print its "DisableAllScreenMessages" hint.
	TArray<FString> Lines;
	Controller->GetHelpLines(Lines);
	if (UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr)
	{
		const float LineHeight = Font->GetMaxCharHeight() + 4.f;
		float Y = 40.f;
		for (const FString& Line : Lines)
		{
			DrawText(Line, FLinearColor(0, 0, 0, .8f), 51.f, Y + 1.f, Font);  // shadow for light backgrounds
			DrawText(Line, FLinearColor(.71f, .86f, 1.f), 50.f, Y, Font);
			Y += LineHeight;
		}
		TArray<FString> RoomLines;
		Controller->GetRoomBoundsMessage().ParseIntoArrayLines(RoomLines);
		FRoomAirDomainSettings Preview; bool bValid = false;
		Controller->GetRoomBoundsPreview(Preview, bValid);
		for (const auto& Line : RoomLines)
		{
			DrawText(Line, FLinearColor(0, 0, 0, .8f), 51.f, Y + 1.f, Font);
			DrawText(Line, bValid ? FLinearColor(1, .65f, 0) : FLinearColor::Red, 50.f, Y, Font);
			Y += LineHeight;
		}
		RoomLines.Reset(); Controller->GetRoomCeilingEditMessage().ParseIntoArrayLines(RoomLines);
		FRoomCeilingPlate CeilingPreview; bValid=false;
		Controller->GetRoomCeilingPreview(CeilingPreview,bValid);
		for(const auto& Line:RoomLines)
		{
			DrawText(Line,FLinearColor(0,0,0,.8f),51.f,Y+1.f,Font);
			DrawText(Line,bValid ? FLinearColor(1,.65f,0) : FLinearColor::Red,50.f,Y,Font);
			Y+=LineHeight;
		}
	}

	FBox2D Rect;
	if (!Controller->GetLayoutMarqueeRect(Rect)) return;
	const FLinearColor Color(.2f, .8f, 1.f);
	DrawLine(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Min.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Max.X, Rect.Max.Y, Rect.Min.X, Rect.Max.Y, Color, 2.f);
	DrawLine(Rect.Min.X, Rect.Max.Y, Rect.Min.X, Rect.Min.Y, Color, 2.f);
}
