#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VrgPaletteV2.generated.h"

class ULayoutRackButton;
class UTextBlock;
class UVerticalBox;
class UBorder;

// 2026-11-24 UI 데모(vrg.Ui.Style 1): 배치 모드 왼쪽 세로 팔레트. 분류 탭 4개(IT 장비 / 냉각 / 설비 마당 / 방·천장)와
// 항목 목록. 버튼은 기존 팔레트와 같은 컨트롤러 함수를 부른다(새 배치 경로 없음). 지금 못 쓰는 항목은 회색으로 두지 않고
// 조건이 맞을 때만 보인다. 버튼은 포커스를 받지 않아 단축키는 뷰포트에 남는다.
UCLASS()
class UVrgPaletteV2 : public UUserWidget
{
	GENERATED_BODY()
protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;
private:
	int32 ActiveTab = 0;
	UPROPERTY() TArray<TObjectPtr<ULayoutRackButton>> TabButtons;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> TabLabels;
	UPROPERTY() TArray<TObjectPtr<UVerticalBox>> Pages;
	UPROPERTY() TObjectPtr<ULayoutRackButton> CeilingButton;
	UPROPERTY() TObjectPtr<ULayoutRackButton> CeilingEditButton;
	UPROPERTY() TObjectPtr<UTextBlock> CeilingEditTitle;
	UPROPERTY() TObjectPtr<ULayoutRackButton> RoomBoundsButton;
	UPROPERTY() TObjectPtr<UTextBlock> RoomBoundsTitle;
	UPROPERTY() TObjectPtr<ULayoutRackButton> HoleButton;
	UPROPERTY() TObjectPtr<ULayoutRackButton> HoleEditButton;
	UPROPERTY() TObjectPtr<UTextBlock> HoleEditTitle;
	UPROPERTY() TObjectPtr<UTextBlock> CeilingNote;
	UPROPERTY() TObjectPtr<UTextBlock> PlacingNote;

	void SetTab(int32 Tab);
	ULayoutRackButton* AddItem(UVerticalBox* Page, const FLinearColor& Swatch, const TCHAR* Title, const TCHAR* Sub, TObjectPtr<UTextBlock>* OutTitle = nullptr);

	// The panel background takes clicks between buttons so they do not select equipment behind it (2026-11-25).
	UFUNCTION() FEventReply OnPanelMouseDown(FGeometry MyGeometry, const FPointerEvent& MouseEvent);
	UFUNCTION() void OnTabIt();
	UFUNCTION() void OnTabCooling();
	UFUNCTION() void OnTabPlant();
	UFUNCTION() void OnTabRoom();
	UFUNCTION() void OnAddRack();
	UFUNCTION() void OnAddSpine();
	UFUNCTION() void OnAddCdu();
	UFUNCTION() void OnAddInRow();
	UFUNCTION() void OnAddPerimeter();
	UFUNCTION() void OnAddFwsDryCooler();
	UFUNCTION() void OnAddCwDryCooler();
	UFUNCTION() void OnAddChiller();
	UFUNCTION() void OnAddTrimChiller();
	UFUNCTION() void OnAddFwsPump();
	UFUNCTION() void OnAddChwPump();
	UFUNCTION() void OnAddCwPump();
	UFUNCTION() void OnAddEconomizer();
	UFUNCTION() void OnRoomSettings();
	UFUNCTION() void OnRoomBounds();
	UFUNCTION() void OnAddWall();
	UFUNCTION() void OnAddCurtain();
	UFUNCTION() void OnCeiling();
	UFUNCTION() void OnCeilingEdit();
	UFUNCTION() void OnHole();
	UFUNCTION() void OnHoleEdit();
};
