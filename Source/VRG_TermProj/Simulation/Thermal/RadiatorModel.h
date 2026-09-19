// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "RadiatorModel.generated.h"

// CDU 라디에이터(열교환기). 랙에서 데워져 돌아온 냉각수를 외부(설비수/외기)로 버린다.
//
// --- 크기는 UA [W/K]로 받는다 ---
// "라디에이터를 키운다"는 곧 전열 면적이 늘어 UA가 커진다는 뜻이다.
// 단면적이나 배수(x2) 같은 무차원 배율로 받으면 단위가 없어 검증할 수 없다.
//
// --- epsilon-NTU라 효과가 포화한다 ---
// 외부 쪽 열용량률이 훨씬 크다고 보고 단류체(Cr -> 0)로 다룬다:
//   NTU = UA / (MassFlowKgs * Cp)
//   eps = 1 - exp(-NTU)                      <- UA -> 무한대에서 1로 포화
//   T_out = T_in - eps * (T_in - T_ambient)
//   Heat  = MassFlowKgs * Cp * eps * (T_in - T_ambient)
//
// UA를 두 배로 키워도 반출열은 두 배가 되지 않는다. 선형으로 좋아지면 식이 틀린 것이다.
// 아무리 키워도 출구 온도는 외기 온도 아래로 내려가지 않는다.
//
// D2 준수: 유량이 분모에 오는 곳은 NTU뿐이고 유량 0은 분기로 처리한다.
// 유량 0이면 열용량률이 0이라 반출열도 0이 된다 (HeatW/(MassFlow*Cp)를 만들지 않는다).
//
// 이 구조체에는 UObject 참조가 없다. 워커 스레드에서 그대로 쓸 수 있다.
USTRUCT(BlueprintType)
struct FRadiatorParams
{
	GENERATED_BODY()

	// 라디에이터 크기 [W/K]. 디버그 키로 조절하는 값이 이것이다.
	// 기본값은 15 kW 랙의 냉각수 반출(약 12.5 kW)을 외기 대비 약 8 K 접근온도로
	// 버리는 운전점에서 역산했다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator", meta = (ClampMin = "0.0"))
	double UAWK = 1.6e3;

	// 열을 버리는 쪽 온도 [K]. 설비수나 외기.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator")
	double AmbientTempK = 295.15;
};

namespace RadiatorModel
{
	// 단류체 epsilon-NTU 효과도. 0~1.
	FORCEINLINE double Effectiveness(double UAWK, double CapacityRateWK)
	{
		if (UAWK <= 0.0)
		{
			return 0.0;
		}
		// D2: 유량 0에서 나눗셈을 하지 않는다. 흐름이 멈추면 냉각수는 외기 온도까지
		// 접근하지만 열용량률이 0이라 실어 나가는 열은 0이다.
		if (CapacityRateWK <= UE_DOUBLE_SMALL_NUMBER)
		{
			return 1.0;
		}
		return 1.0 - FMath::Exp(-UAWK / CapacityRateWK);
	}

	// 라디에이터를 빠져나온 냉각수 온도 [K]. 외기 온도 아래로 내려가지 않는다.
	FORCEINLINE double OutletTempK(const FRadiatorParams& Params, double InletTempK,
		double MassFlowKgs, double CpJKgK)
	{
		const double CapacityRateWK = MassFlowKgs * CpJKgK;
		const double Eps = Effectiveness(Params.UAWK, CapacityRateWK);
		return InletTempK - Eps * (InletTempK - Params.AmbientTempK);
	}

	// 외부로 버린 열 [W]. 냉각수가 외기보다 차가우면 음수가 된다.
	FORCEINLINE double HeatRejectedW(const FRadiatorParams& Params, double InletTempK,
		double MassFlowKgs, double CpJKgK)
	{
		const double CapacityRateWK = MassFlowKgs * CpJKgK;
		const double Eps = Effectiveness(Params.UAWK, CapacityRateWK);
		return CapacityRateWK * Eps * (InletTempK - Params.AmbientTempK);
	}
}
