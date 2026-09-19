// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Visualization/SimMeshBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

// 메쉬 배치 자체(상자 크기, 관 방향, 유닛 슬라이드)는 월드가 있어야 하므로
// 자동화 테스트로 판정하지 않고 눈으로 확인한다.
// 여기서는 월드 없이 검증할 수 있는 순수 함수인 온도 색상표만 다룬다.
//
// 색상표를 검증하는 이유: 온도를 잘못된 방향으로 매핑하면 화면은 멀쩡해 보이지만
// 결과를 거꾸로 읽게 된다. "화면이 자연스럽다"로 판정하지 않는다는 규칙의 최소 적용이다.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimMeshBuilderTemperatureColorMapTest,
	"VRG.Visualization.TemperatureColorMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FSimMeshBuilderTemperatureColorMapTest::RunTest(const FString& Parameters)
{
	const double MinTempK = 293.15;  // 20 C
	const double MaxTempK = 363.15;  // 90 C

	const FLinearColor ColdColor = SimMeshBuilder::TemperatureToColor(MinTempK, MinTempK, MaxTempK);
	const FLinearColor HotColor = SimMeshBuilder::TemperatureToColor(MaxTempK, MinTempK, MaxTempK);

	AddInfo(FString::Printf(TEXT("20 C -> (%.3f, %.3f, %.3f) / 90 C -> (%.3f, %.3f, %.3f)"),
		ColdColor.R, ColdColor.G, ColdColor.B, HotColor.R, HotColor.G, HotColor.B));

	// 뜨거울수록 빨강이 세고 파랑이 약해야 한다. 방향이 뒤집히면 결과를 거꾸로 읽는다.
	TestTrue(TEXT("뜨거운 쪽이 더 빨개야 한다"), HotColor.R > ColdColor.R);
	TestTrue(TEXT("찬 쪽이 더 파래야 한다"), ColdColor.B > HotColor.B);

	// 빨강 성분이 온도에 대해 단조 증가해야 한다.
	bool bRedMonotonic = true;
	double PreviousRed = -1.0;
	for (int32 StepIndex = 0; StepIndex <= 20; ++StepIndex)
	{
		const double TempK = FMath::Lerp(MinTempK, MaxTempK, StepIndex / 20.0);
		const double Red = SimMeshBuilder::TemperatureToColor(TempK, MinTempK, MaxTempK).R;
		if (Red < PreviousRed - 1.0e-6)
		{
			bRedMonotonic = false;
		}
		PreviousRed = Red;
	}
	TestTrue(TEXT("빨강 성분이 온도에 대해 단조 증가해야 한다"), bRedMonotonic);

	// 범위 밖은 양 끝 색으로 고정돼야 한다. 색이 튀면 범위를 벗어난 값을 못 알아본다.
	const FLinearColor BelowColor = SimMeshBuilder::TemperatureToColor(MinTempK - 100.0, MinTempK, MaxTempK);
	const FLinearColor AboveColor = SimMeshBuilder::TemperatureToColor(MaxTempK + 100.0, MinTempK, MaxTempK);
	TestTrue(TEXT("범위 아래는 찬 쪽 색으로 고정돼야 한다"), BelowColor.Equals(ColdColor, 1.0e-6f));
	TestTrue(TEXT("범위 위는 뜨거운 쪽 색으로 고정돼야 한다"), AboveColor.Equals(HotColor, 1.0e-6f));

	// 범위 폭이 0이어도 나누기로 터지면 안 된다.
	const FLinearColor DegenerateColor = SimMeshBuilder::TemperatureToColor(310.0, 300.0, 300.0);
	TestTrue(TEXT("범위 폭 0에서도 유한한 색이 나와야 한다"),
		FMath::IsFinite(DegenerateColor.R) && FMath::IsFinite(DegenerateColor.G)
		&& FMath::IsFinite(DegenerateColor.B));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
