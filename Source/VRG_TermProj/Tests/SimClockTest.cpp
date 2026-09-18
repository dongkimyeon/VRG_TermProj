// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Core/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;
}

// 규약 4: 시계에 넣은 실시간은 어디로도 사라지지 않는다.
// SimTime + 아직 안 쓴 누적 시간 + 버린 시간 = 넣은 시간 총합.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockNoTimeLostTest,
	"VRG.Core.SimClock.NoTimeLost", VrgTestFlags)

bool FSimClockNoTimeLostTest::RunTest(const FString& Parameters)
{
	FSimClock Clock(0.01);

	// 스텝 크기의 배수가 아닌 프레임 간격을 일부러 섞는다.
	const double FrameDeltas[] = { 0.0163, 0.0331, 0.0089, 0.0207, 0.0154, 0.0442, 0.0071 };

	double TotalFedS = 0.0;
	for (int32 Repeat = 0; Repeat < 50; ++Repeat)
	{
		for (const double Delta : FrameDeltas)
		{
			TotalFedS += Delta;
			const int32 NumSteps = Clock.BeginFrame(Delta);
			for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
			{
				Clock.CompleteStep();
			}
		}
	}

	const double Accounted = Clock.GetSimTimeS() + Clock.GetPendingTimeS() + Clock.GetDiscardedTimeS();
	const double AbsErrorS = FMath::Abs(Accounted - TotalFedS);

	AddInfo(FString::Printf(TEXT("넣은 시간 %.9f s / 정산 %.9f s / 절대 오차 %.3e s"),
		TotalFedS, Accounted, AbsErrorS));
	AddInfo(FString::Printf(TEXT("SimTime %.6f s, 누적기 잔여 %.6f s, 버린 시간 %.6f s"),
		Clock.GetSimTimeS(), Clock.GetPendingTimeS(), Clock.GetDiscardedTimeS()));

	TestTrue(TEXT("정상 프레임에서는 버리는 시간이 없어야 한다"), Clock.GetDiscardedTimeS() == 0.0);
	TestTrue(TEXT("넣은 시간과 정산 시간이 일치해야 한다"), AbsErrorS < 1.0e-9);
	TestTrue(TEXT("누적기 잔여는 한 스텝 미만이어야 한다"), Clock.GetPendingTimeS() < Clock.GetFixedStepS());

	return true;
}

// 서브스텝 상한에 걸려 시간을 버릴 때, 버린 양이 숨지 않고 그대로 보고돼야 한다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockClampReportsDiscardedTest,
	"VRG.Core.SimClock.ClampReportsDiscardedTime", VrgTestFlags)

bool FSimClockClampReportsDiscardedTest::RunTest(const FString& Parameters)
{
	FSimClock Clock(0.01);
	Clock.SetMaxSubStepsPerFrame(5);

	const double HitchDeltaS = 1.0;
	const int32 NumSteps = Clock.BeginFrame(HitchDeltaS);
	for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
	{
		Clock.CompleteStep();
	}

	const double Accounted = Clock.GetSimTimeS() + Clock.GetPendingTimeS() + Clock.GetDiscardedTimeS();

	AddInfo(FString::Printf(TEXT("1.0 s 히치 -> 실행 스텝 %d, 버린 시간 %.6f s"),
		NumSteps, Clock.GetDiscardedTimeS()));

	TestEqual(TEXT("상한만큼만 실행해야 한다"), NumSteps, 5);
	TestTrue(TEXT("클램프 사실이 보고돼야 한다"), Clock.WasClampedLastFrame());
	TestTrue(TEXT("버린 시간이 0보다 커야 한다"), Clock.GetDiscardedTimeS() > 0.0);
	TestTrue(TEXT("버린 시간까지 합치면 총량이 맞아야 한다"),
		FMath::Abs(Accounted - HitchDeltaS) < 1.0e-9);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
