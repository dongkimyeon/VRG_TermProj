// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GPUProfiler.h"
#include "Simulation/Core/VrgPerfCounters.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "VrgPerfSubsystem.generated.h"

// 한 프레임의 측정값. CSV 한 줄이기도 하다.
struct FVrgPerfSample
{
	double TimeS = 0.0;
	double FrameMs = 0.0;
	double GameMs = 0.0;
	double DrawMs = 0.0;
	double GpuMs = 0.0;
	VrgPerf::FFrameCosts Costs;
};

// S7-0: 프레임 비용을 화면에 띄우고 파일로 남긴다.
//
// 액터가 아니라 GameInstance 서브시스템이다. 레벨에 무언가를 놓지 않아도
// PIE와 -game 실행 양쪽에서 항상 뜬다. 측정 도구가 레벨 편집 상태에 따라
// 있다 없다 하면 단계 간 비교가 안 된다.
//
// Frame/Game/Draw/GPU는 엔진이 이미 재고 있는 값(GGameThreadTime 등)을 읽는다.
// 솔버·드로우 ms는 VrgPerfCounters의 누산기에서 읽는다. 둘을 같은 줄에 적어야
// "게임 스레드 안에서 솔버가 몇 %인가"를 바로 볼 수 있다.
UCLASS()
class VRG_TERMPROJ_API UVrgPerfSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void SetOverlayEnabled(bool bEnabled) { bOverlayEnabled = bEnabled; }
	bool IsOverlayEnabled() const { return bOverlayEnabled; }

	// DurationS 동안 프레임을 모아 CSV와 요약을 Saved/Profiling에 쓴다.
	// 이미 수집 중이면 무시한다.
	void BeginCapture(double DurationS, const FString& Label, bool bQuitWhenDone);

private:
	bool Tick(float DeltaTime);
	void OnBeginFrame();
	void OnEndFrame();
	void DrawOverlay(const FVrgPerfSample& Sample) const;
	void FinishCapture();

	// UE 5.8에는 GGPUFrameTime 전역이 없다. RHI가 밀어 넣는 프레임 히스토리를
	// 상태를 들고 하나씩 꺼내 쓴다. 새 값이 없으면 직전 값을 유지한다.
	FRHIGPUFrameTimeHistory::FState GpuHistoryState;
	double LastGpuMs = 0.0;

	// 게임 스레드 시간은 직접 잰다. 에디터 PIE에서 GGameThreadTime이 갱신되지 않아
	// 23000 ms 같은 쓰레기 값이 나왔다. 프레임 시작~끝을 우리가 재면 항상 맞는다.
	uint64 FrameStartCycles = 0;
	double LastGameThreadMs = 0.0;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle BeginFrameHandle;
	FDelegateHandle EndFrameHandle;

	bool bOverlayEnabled = true;
	bool bCapturing = false;
	bool bQuitAfterCapture = false;

	// 스크린샷은 요청한 프레임에 바로 저장되지 않는다. 몇 프레임 기다렸다 종료한다.
	int32 PendingQuitFrames = 0;
	double CaptureRemainingS = 0.0;
	double ElapsedS = 0.0;
	FString CaptureLabel;
	TArray<FVrgPerfSample> Samples;
};
