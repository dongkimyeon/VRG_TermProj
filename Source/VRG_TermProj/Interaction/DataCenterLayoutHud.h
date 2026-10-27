#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DataCenterLayoutHud.generated.h"

// Passive viewport-pixel outline only; input stays with the controller.
UCLASS()
class VRG_TERMPROJ_API ADataCenterLayoutHud : public AHUD
{
	GENERATED_BODY()
public:
	virtual void DrawHUD() override;
private:
	void DrawTemperatureLabels();
	void DrawAirLegend();
};
