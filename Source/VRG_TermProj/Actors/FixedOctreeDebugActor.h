// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Air/FixedOctreeGrid.h"
#include "FixedOctreeDebugActor.generated.h"

class AAirFieldDebugActor;

// S5-A 미리보기 전용. 균일 공기 솔버를 교체하거나 적분하지 않는다.
// 랙 이동/격자 변경 후 RebuildOctree로 고정 토폴로지를 다시 만든다.
UCLASS()
class VRG_TERMPROJ_API AFixedOctreeDebugActor : public AActor
{
	GENERATED_BODY()

public:
	AFixedOctreeDebugActor();
	virtual void Tick(float DeltaTime) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Octree")
	void RebuildOctree();

	// 지정하지 않으면 월드에 공기 액터가 정확히 한 개일 때만 자동 선택한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree")
	TObjectPtr<AAirFieldDebugActor> AirField;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree", meta = (ClampMin = "0", ClampMax = "6"))
	int32 RefinementDepth = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree", meta = (ClampMin = "0.0"))
	double RackPaddingM = 0.25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree", meta = (ClampMin = "1", ClampMax = "1000000"))
	int32 MaxLeaves = 60000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Display")
	bool bDrawWireframe = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Display")
	bool bDrawCoarseCells = false;

	// 표시만 제한한다. 계산 토폴로지를 잘라내지 않는다. 잘린 개수는 요약에 표시한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Display", meta = (ClampMin = "1", ClampMax = "60000"))
	int32 MaxDrawnLeaves = 3000;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Octree")
	int32 ActiveLeafCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Octree")
	FString BuildStatus;

protected:
	virtual void BeginPlay() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FFixedOctreeGrid Octree;
	FVector BuiltOriginCm = FVector::ZeroVector;
	bool bNeedsBuild = true;
	void BuildPreview();
};
