// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DebugKnob.generated.h"

// 디버그 키로 런타임에 돌리는 값 하나. CPU 발열, GPU 발열, 펌프 회전수비,
// 라디에이터 UA가 전부 같은 방식으로 움직이므로 여기에 모았다.
//
// 입력 처리(어느 키가 어느 손잡이를 돌리는지)와 값의 규칙(범위, 증분)을 나눠 둔 것이다.
// 이렇게 두면 월드 없이 값 규칙만 테스트할 수 있다.
//
// 단위는 이 구조체가 모른다. 부르는 쪽이 W든 W/K든 무차원이든 맞춰 쓴다 (규약 1·3).
USTRUCT(BlueprintType)
struct FDebugKnob
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	double Value = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	double MinValue = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	double MaxValue = 1.0;

	// bMultiplicative가 false면 더할 양, true면 곱할 배율이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	double StepSize = 0.1;

	// 라디에이터 UA처럼 몇 자릿수를 훑어야 하는 값은 곱셈으로 움직이는 게 편하다.
	// 이때 StepSize는 1보다 커야 하고 MinValue는 0보다 커야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bMultiplicative = false;

	// Direction이 양수면 올리고 음수면 내린다. 0이면 아무 일도 하지 않는다.
	// 결과는 항상 [MinValue, MaxValue] 안이다.
	void Nudge(int32 Direction)
	{
		if (Direction == 0)
		{
			return;
		}

		if (bMultiplicative)
		{
			// 배율이 1 이하면 방향이 뒤집히거나 값이 멈춘다. 그런 설정은 무시한다.
			if (StepSize <= 1.0)
			{
				return;
			}
			Value = Direction > 0 ? Value * StepSize : Value / StepSize;
		}
		else
		{
			Value += Direction > 0 ? StepSize : -StepSize;
		}

		Value = FMath::Clamp(Value, MinValue, MaxValue);
	}
};
