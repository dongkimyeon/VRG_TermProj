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
private:
	UFUNCTION()
	void OnAddRack();
	UFUNCTION() void OnAddSpine();
};
