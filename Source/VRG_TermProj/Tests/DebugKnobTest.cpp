// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Interaction/DebugKnob.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgKnobTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;
}

// 키 입력과 값의 규칙을 나눠 둔 덕분에 월드 없이 규칙만 검증할 수 있다.
// 실제 키 바인딩이 도는지는 에디터에서 눌러 봐야 안다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDebugKnobLinearTest,
	"VRG.Interaction.DebugKnob.LinearStepAndClamp", VrgKnobTestFlags)

bool FDebugKnobLinearTest::RunTest(const FString& Parameters)
{
	// 펌프 회전수비 손잡이와 같은 설정.
	FDebugKnob Knob;
	Knob.Value = 1.0;
	Knob.MinValue = 0.0;
	Knob.MaxValue = 1.5;
	Knob.StepSize = 0.05;

	Knob.Nudge(1);
	TestTrue(TEXT("한 번 올리면 StepSize만큼 올라야 한다"), FMath::IsNearlyEqual(Knob.Value, 1.05, 1.0e-12));

	Knob.Nudge(-1);
	TestTrue(TEXT("한 번 내리면 제자리로 돌아와야 한다"), FMath::IsNearlyEqual(Knob.Value, 1.0, 1.0e-12));

	Knob.Nudge(0);
	TestTrue(TEXT("방향 0이면 값이 그대로여야 한다"), FMath::IsNearlyEqual(Knob.Value, 1.0, 1.0e-12));

	// 위로 계속 밀면 최대에서 멈춰야 한다.
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Knob.Nudge(1);
	}
	AddInfo(FString::Printf(TEXT("100번 올린 뒤 %.6f (최대 %.6f)"), Knob.Value, Knob.MaxValue));
	TestTrue(TEXT("최대값을 넘지 않아야 한다"), Knob.Value <= Knob.MaxValue);
	TestTrue(TEXT("최대값에 도달해야 한다"), FMath::IsNearlyEqual(Knob.Value, Knob.MaxValue, 1.0e-12));

	// 아래로 계속 밀면 최소에서 멈춰야 한다.
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Knob.Nudge(-1);
	}
	AddInfo(FString::Printf(TEXT("100번 내린 뒤 %.6f (최소 %.6f)"), Knob.Value, Knob.MinValue));
	TestTrue(TEXT("최소값 아래로 내려가지 않아야 한다"), Knob.Value >= Knob.MinValue);
	TestTrue(TEXT("최소값에 도달해야 한다"), FMath::IsNearlyEqual(Knob.Value, Knob.MinValue, 1.0e-12));

	return true;
}

// 라디에이터 UA처럼 자릿수를 훑어야 하는 값은 곱셈으로 움직인다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDebugKnobMultiplicativeTest,
	"VRG.Interaction.DebugKnob.MultiplicativeStep", VrgKnobTestFlags)

bool FDebugKnobMultiplicativeTest::RunTest(const FString& Parameters)
{
	FDebugKnob Knob;
	Knob.Value = 1.6e3;
	Knob.MinValue = 1.0e2;
	Knob.MaxValue = 2.0e4;
	Knob.StepSize = 1.25;
	Knob.bMultiplicative = true;

	Knob.Nudge(1);
	TestTrue(TEXT("한 번 올리면 StepSize배가 돼야 한다"), FMath::IsNearlyEqual(Knob.Value, 2.0e3, 1.0e-9));

	Knob.Nudge(-1);
	TestTrue(TEXT("한 번 내리면 제자리로 돌아와야 한다"), FMath::IsNearlyEqual(Knob.Value, 1.6e3, 1.0e-9));

	// 범위 양 끝에서 멈춰야 한다. 곱셈이라 0이나 무한으로 달아나기 쉬운 자리다.
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Knob.Nudge(1);
	}
	AddInfo(FString::Printf(TEXT("200번 올린 뒤 %.1f W/K (최대 %.1f)"), Knob.Value, Knob.MaxValue));
	TestTrue(TEXT("최대값에서 멈춰야 한다"), FMath::IsNearlyEqual(Knob.Value, Knob.MaxValue, 1.0e-9));

	for (int32 Index = 0; Index < 200; ++Index)
	{
		Knob.Nudge(-1);
	}
	AddInfo(FString::Printf(TEXT("200번 내린 뒤 %.1f W/K (최소 %.1f)"), Knob.Value, Knob.MinValue));
	TestTrue(TEXT("최소값에서 멈춰야 한다"), FMath::IsNearlyEqual(Knob.Value, Knob.MinValue, 1.0e-9));
	TestTrue(TEXT("곱셈 손잡이가 0으로 내려가면 안 된다"), Knob.Value > 0.0);

	// 기본 설정으로 100 -> 20000 W/K를 몇 번 눌러 훑는지 적어 둔다.
	// 너무 많으면 디버그 키로 쓸모가 없다.
	int32 PressCount = 0;
	while (Knob.Value < Knob.MaxValue - 1.0e-9 && PressCount < 1000)
	{
		Knob.Nudge(1);
		++PressCount;
	}
	AddInfo(FString::Printf(TEXT("최소에서 최대까지 %d번 누르면 된다 (배율 %.2f)"), PressCount, Knob.StepSize));
	TestTrue(TEXT("손으로 훑을 수 있는 횟수여야 한다"), PressCount <= 40);

	// 배율이 1 이하인 잘못된 설정은 값을 움직이지 않아야 한다 (방향이 뒤집히면 안 된다).
	FDebugKnob BadKnob;
	BadKnob.Value = 100.0;
	BadKnob.MinValue = 1.0;
	BadKnob.MaxValue = 1000.0;
	BadKnob.StepSize = 0.5;
	BadKnob.bMultiplicative = true;
	BadKnob.Nudge(1);
	TestTrue(TEXT("배율이 1 이하면 값을 움직이지 않아야 한다"), FMath::IsNearlyEqual(BadKnob.Value, 100.0, 1.0e-12));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
