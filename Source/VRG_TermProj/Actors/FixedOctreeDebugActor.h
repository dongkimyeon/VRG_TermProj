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

	// S5-B: 면 인덱스와 비균일 Poisson 연산자를 현재 토폴로지에서 점검한다.
	// 물리를 푸는 것이 아니라 연산자가 보존·대칭을 지키는지 보는 버튼이다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Octree")
	void CheckPoissonOperator();

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

	// S5-B: 깊이가 다른 두 리프가 맞닿은 면. 비균일 압력 연산자가 1차로 떨어지는 자리다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Display")
	bool bDrawHangingFaces = false;

	// 표시만 제한한다. 계산 토폴로지를 잘라내지 않는다. 잘린 개수는 요약에 표시한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Display", meta = (ClampMin = "1", ClampMax = "60000"))
	int32 MaxDrawnLeaves = 3000;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Octree")
	int32 ActiveLeafCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Octree")
	FString BuildStatus;

	// D5: 점검 버튼의 종료 조건. 목표와 상한을 함께 둔다.
	// 실시간이 아니라 버튼이므로 테스트보다 느슨하되 화면에서 수렴 여부를 읽을 수 있게 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Operator", meta = (ClampMin = "0.000000000001"))
	double PreviewResidualTarget = 1.0e-5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree|Operator", meta = (ClampMin = "1", ClampMax = "100000"))
	int32 PreviewMaxIterations = 2000;

	// CheckPoissonOperator의 결과. 누르기 전에는 비어 있다.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Octree")
	FString OperatorStatus;

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
