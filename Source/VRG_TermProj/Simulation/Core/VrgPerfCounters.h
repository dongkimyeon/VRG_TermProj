// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

// S7-0: 프레임 비용을 코드에서 잰다.
//
// --- 왜 필요한가 ---
// D10은 "측정 전에는 보장값으로 표현하지 않는다"고 못 박는다. 그런데 지금까지
// 프레임 비용은 한 번도 측정된 적이 없다. 솔버가 느린지, 디버그 드로우가 느린지
// 모르는 채로 최적화를 시작하면 아닌 곳을 고치게 된다.
//
// --- 왜 stat game만으로는 부족한가 ---
// `stat game`은 사람이 콘솔에 쳐서 눈으로 읽어야 한다. 단계마다 전후를 비교해
// 파일로 남기려면 코드에서 값을 꺼낼 수 있어야 한다. 그래서 두 벌을 둔다.
//   - DECLARE_CYCLE_STAT: 엔진 프로파일러에 항목이 뜨게 (사람이 볼 때)
//   - 아래 누산기: 값을 코드로 읽어 오버레이와 CSV에 쓰기 위해
// 같은 스코프에 둘 다 건다. 두 값이 어긋나면 계측 자체가 잘못된 것이다.
//
// --- 규약 5 ---
// 이 누산기는 **게임 스레드 전용**이다. 워커 스레드에서 건드리지 않는다.
// 지금 솔버가 전부 게임 스레드에서 돌기 때문에 성립하는 전제이며,
// 솔버를 스레드로 빼면 여기도 같이 바꿔야 한다.

DECLARE_STATS_GROUP(TEXT("VRG"), STATGROUP_VRG, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("VRG Air Solver Step"), STAT_VrgAirSolverStep, STATGROUP_VRG, VRG_TERMPROJ_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VRG Pressure Solve"), STAT_VrgPressureSolve, STATGROUP_VRG, VRG_TERMPROJ_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VRG Debug Draw"), STAT_VrgDebugDraw, STATGROUP_VRG, VRG_TERMPROJ_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("VRG Coupling Step"), STAT_VrgCouplingStep, STATGROUP_VRG, VRG_TERMPROJ_API);

namespace VrgPerf
{
	// 한 프레임 동안 모은 비용. 단위는 ms와 개수다.
	// PressureMs는 SolverMs 안에 포함된 값이라 둘을 더하면 안 된다.
	struct FFrameCosts
	{
		double SolverMs = 0.0;
		double PressureMs = 0.0;
		double CouplingMs = 0.0;
		double DebugDrawMs = 0.0;
		int32 SolverSteps = 0;
		int32 PressureIterations = 0;
		int32 DrawnPrimitives = 0;
	};

	// 이번 프레임에 쌓는 중인 값. 스코프 타이머와 카운터가 여기에 더한다.
	VRG_TERMPROJ_API FFrameCosts& Accumulator();

	// 직전 프레임의 확정값. 오버레이와 CSV가 읽는다.
	VRG_TERMPROJ_API const FFrameCosts& LastFrame();

	// 누산기를 확정값으로 넘기고 비운다. 프레임 끝에 한 번만 부른다.
	VRG_TERMPROJ_API void EndFrame();

	// 아직 남은 예열 스텝 수. 예열 중에 수집을 시작하면 표본이 전부
	// 과도 상태라 대표값이 되지 않으므로, 수집이 이 값을 보고 기다린다.
	VRG_TERMPROJ_API int32& PendingWarmupSteps();

	// 스코프 동안 흐른 시간을 FFrameCosts의 한 필드에 더한다.
	// 중첩되면 바깥과 안쪽에 둘 다 더해지므로, 포함 관계를 주석으로 밝혀 둔다.
	class VRG_TERMPROJ_API FScopeTimer
	{
	public:
		explicit FScopeTimer(double FFrameCosts::* InField)
			: Field(InField)
			, StartCycles(FPlatformTime::Cycles64())
		{
		}

		~FScopeTimer()
		{
			Accumulator().*Field += FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);
		}

	private:
		double FFrameCosts::* Field;
		uint64 StartCycles;
	};
}

// 엔진 프로파일러 항목과 코드용 누산기를 같은 스코프에 함께 건다.
#define VRG_SCOPE_COST(StatName, Field) \
	SCOPE_CYCLE_COUNTER(StatName); \
	VrgPerf::FScopeTimer UE_JOIN(VrgScopeTimer_, __LINE__)(&VrgPerf::FFrameCosts::Field)
