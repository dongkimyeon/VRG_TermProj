#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Visualization/PatchManager.h"
#include "PatchPlanWidget.generated.h"

class ADataCenterRoomActor;

// Game-thread snapshot; paint does not read simulation actors or mutate solver state.
UCLASS()
class UPatchPlanWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	void SetRoom(ADataCenterRoomActor* InRoom);
protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& Geometry, float DeltaSeconds) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
		FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool ParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
private:
	TWeakObjectPtr<ADataCenterRoomActor> Room;
	Patch::FPlan Snapshot;
	TArray<Patch::FDevice> Devices;
	FBox BoundsM{ForceInit};
	float RefreshSeconds=1;
	int32 Selected=0;
	FVector2D Project(const FVector& PointM, const FVector2D& Size) const;
};
