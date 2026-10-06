#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Simulation/Air/RoomAirDomain.h"
#include "RoomSettingsWidget.generated.h"

class UCheckBox;
class UEditableTextBox;
class UTextBlock;
class UHorizontalBox;

// Draft text is deliberately separate from the committed SI settings and live air grid.
UCLASS()
class VRG_TERMPROJ_API URoomSettingsWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	void InitializeDraft(const FRoomAirDomainSettings& Settings, bool bCeiling = false);
	void InitializeHoleDraft(const FRoomCeilingPlate& Plate);
	static bool ParseDraft(bool bManual, const TArray<FString>& Fields, FRoomAirDomainSettings& InOutSettings, FString& Reason);
	static bool ParseHoleDraft(const TArray<FString>& Fields, FRoomCeilingHole& InOutHole, FString& Reason);
protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
	UPROPERTY() TObjectPtr<UCheckBox> Manual;
	UPROPERTY() TArray<TObjectPtr<UEditableTextBox>> Inputs;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> InputLabels;
	UPROPERTY() TObjectPtr<UHorizontalBox> HeightRow;
	UPROPERTY() TObjectPtr<UTextBlock> Status;
	UPROPERTY() TObjectPtr<UTextBlock> Plan;
	UPROPERTY() TObjectPtr<UTextBlock> Title;
	UPROPERTY() TObjectPtr<UTextBlock> Intro;
	UPROPERTY() TObjectPtr<UTextBlock> ApplyLabel;
	UPROPERTY() TObjectPtr<UTextBlock> HeightLabel;
	bool bCeilingMode = false;
	bool bHoleMode = false;
	FRoomAirDomainSettings DraftSeed;
	bool bInitializing = false;
	UFUNCTION() void OnApply();
	UFUNCTION() void OnClose();
	UFUNCTION() void OnModeChanged(bool bManual);
	UFUNCTION() void OnDraftChanged(const FText& Text);
};
