#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DataCenterLayoutHud.generated.h"

struct FVrgHudV2State;

// Passive viewport-pixel outline only; input stays with the controller.
// 2026-11-24 UI demo: vrg.Ui.Style 1 draws the new HUD (DataCenterHudV2.cpp) instead of the key-hint text and W4 box.
UCLASS()
class VRG_TERMPROJ_API ADataCenterLayoutHud : public AHUD
{
	GENERATED_BODY()
public:
	virtual void DrawHUD() override;
	// New HUD: true when Screen (viewport pixels) is over a panel drawn last frame; the controller then keeps the click
	// out of the scene (2026-11-25). Always false in the previous style.
	bool IsPointerOverPanel(const FVector2D& Screen) const;
private:
	void DrawTemperatureLabels();
	void DrawAirLegend(float BottomMargin = 20.f);
	void DrawRackLeakTool(float Left = 10.f, float Top = 90.f, float MaxWidth = 1100.f);
	void DrawHudV2();
	void DrawAirLegendV2(float BottomMargin);
	void DrawNoiseOverlay();
	void DrawMarquee();
	TSharedPtr<FVrgHudV2State> V2;
	TArray<FBox2D> PanelRects;   // panels drawn this frame, viewport pixels (display only)
};
