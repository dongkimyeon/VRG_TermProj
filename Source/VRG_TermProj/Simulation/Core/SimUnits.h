// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// 규약 1·2: 솔버 내부 단위는 m·s·kg·W·Pa, 온도는 K.
// UE 좌표(cm)와 표시용 °C 변환은 액터/UI 경계에서만, 반드시 이 헤더의 함수로 한다.
// 솔버 내부에 cm나 °C가 들어오면 안 된다.
namespace SimUnits
{
	constexpr double CentimetersPerMeter = 100.0;
	constexpr double KelvinAtZeroCelsius = 273.15;

	FORCEINLINE constexpr double CelsiusToKelvin(double Celsius)
	{
		return Celsius + KelvinAtZeroCelsius;
	}

	FORCEINLINE constexpr double KelvinToCelsius(double Kelvin)
	{
		return Kelvin - KelvinAtZeroCelsius;
	}

	FORCEINLINE constexpr double MetersToCentimeters(double Meters)
	{
		return Meters * CentimetersPerMeter;
	}

	FORCEINLINE constexpr double CentimetersToMeters(double Centimeters)
	{
		return Centimeters / CentimetersPerMeter;
	}

	// 솔버 좌표(m) <-> UE 월드 좌표(cm).
	FORCEINLINE FVector MetersToUnreal(const FVector& PositionM)
	{
		return PositionM * CentimetersPerMeter;
	}

	FORCEINLINE FVector UnrealToMeters(const FVector& PositionCm)
	{
		return PositionCm / CentimetersPerMeter;
	}
}
