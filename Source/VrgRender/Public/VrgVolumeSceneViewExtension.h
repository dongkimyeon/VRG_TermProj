// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "SceneViewExtension.h"

// S7-1: 온도장을 3D 텍스처 + 레이마칭으로 그리는 뷰 확장.
//
// --- 왜 디버그 박스를 대체하나 ---
// `DrawDebugSolidBox`는 셀 하나가 드로우 제출 하나다. 기준선에서 28130개를 그려
// Draw 스레드가 44.6 ms였다. 이건 그 자체로 30fps 선(33.3 ms)을 넘는다.
// 레이마칭은 전체 화면 패스 하나라 **드로우 호출이 셀 수와 무관하게 1개**다.
//
// --- 규약 5 ---
// 솔버도 액터도 RHI를 직접 건드리지 않는다. 게임 스레드가 스냅샷에서 만든
// 복사본을 `UpdateVolume`에 넘기면, 여기서 렌더 명령으로 감싸 렌더 스레드가 올린다.
// 아래 멤버는 전부 렌더 스레드 전용이다.
//
// --- 값의 의미 ---
// 텍스처에 담는 값은 **실제 절대온도 K**다 (규약 2). 고체 셀은 음수로 표시한다.
// 표시 범위(MinK/MaxK)는 셰이더 상수라서, 색상 범위를 바꿔도 텍스처를 다시 올리지 않는다.
class VRGRENDER_API FVrgVolumeSceneViewExtension : public FSceneViewExtensionBase
{
public:
	explicit FVrgVolumeSceneViewExtension(const FAutoRegister& AutoRegister);

	// 게임 스레드에서 부른다. Voxels는 X가 가장 빠른 순서이며 길이는 Size의 곱이다.
	// 고체 셀은 음수 K로 넣는다. 렌더 스레드로 넘겨 복사하므로 호출 뒤 원본을 재사용해도 된다.
	void UpdateVolume_GameThread(const FIntVector& InSize, TArray<float>&& InVoxelsK,
		const FBox& InWorldBoundsCm, float InMinTempK, float InMaxTempK,
		float InOpacityPerCm, int32 InStepCount);

	// 꺼 두면 패스를 아예 추가하지 않는다. 드로우 호출도 0이다.
	void SetEnabled_GameThread(bool bInEnabled);

	// 직전 프레임에 이 확장이 추가한 드로우 호출 수. 1단계 완료 조건 확인용이다.
	int32 GetLastPassCount() const { return LastPassCount.load(std::memory_order_relaxed); }

	//~ ISceneViewExtension
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

private:
	FScreenPassTexture AddVolumePass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	// ---- 렌더 스레드 전용 ----
	FTextureRHIRef VolumeTexture;
	FIntVector VolumeSize = FIntVector::ZeroValue;
	FVector3f BoundsMinCm = FVector3f::ZeroVector;
	FVector3f BoundsMaxCm = FVector3f::ZeroVector;
	float MinTempK = 295.0f;
	float MaxTempK = 320.0f;
	float OpacityPerCm = 0.004f;
	int32 StepCount = 128;
	bool bRenderEnabled = false;

	// 게임 스레드가 쓰고 아무 스레드나 읽는다.
	std::atomic<int32> LastPassCount{ 0 };
};
