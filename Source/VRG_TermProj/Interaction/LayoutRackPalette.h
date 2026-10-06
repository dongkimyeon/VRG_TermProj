#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "LayoutRackPalette.generated.h"

UCLASS()
class ULayoutRackButton : public UButton
{
	GENERATED_BODY()
public:
	ULayoutRackButton() { InitIsFocusable(false); }
};

// Layout-only, non-focusable button: keyboard shortcuts stay with the viewport.
UCLASS()
class ULayoutRackPalette : public UUserWidget
{
	GENERATED_BODY()
protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;
private:
	UPROPERTY() TObjectPtr<ULayoutRackButton> CeilingButton;
	UPROPERTY() TObjectPtr<ULayoutRackButton> CeilingEditButton;
	UFUNCTION()
	void OnAddRack();
	UFUNCTION() void OnAddSpine();
	UFUNCTION() void OnAddCdu();
 UFUNCTION() void OnAddCrah();
 UFUNCTION() void OnAddPerimeterCrah();
	UFUNCTION() void OnRoomSettings();
	UFUNCTION() void OnRoomCeiling();
	UFUNCTION() void OnRoomCeilingEdit();
	UFUNCTION() void OnRoomBounds();
	UFUNCTION() void OnAddWall();
	UFUNCTION() void OnAddCurtain();
};
