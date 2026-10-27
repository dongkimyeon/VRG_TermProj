// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/AirFlowView.h"

FLinearColor AirFlowView::Colormap(double T)
{
	T = FMath::Clamp(T, 0.0, 1.0);
	static const FLinearColor Stops[5] = { FLinearColor(0.05f, 0.1f, 0.9f), FLinearColor(0.2f, 0.75f, 1.0f), FLinearColor(0.3f, 0.9f, 0.3f),
		FLinearColor(1.0f, 0.9f, 0.1f), FLinearColor(0.9f, 0.08f, 0.05f) };
	const double S = T * 4.0;
	const int32 I = FMath::Min(int32(S), 3);
	return FMath::Lerp(Stops[I], Stops[I + 1], float(S - I));
}

FLinearColor AirFlowView::Banded(double T, int32 Bands)
{
	const double Q = (FMath::FloorToDouble(FMath::Clamp(T, 0.0, 1.0) * (Bands - 1) + .5)) / (Bands - 1);
	return Colormap(Q);
}

const TCHAR* AirFlowView::Name(EAirView View)
{
	switch (View)
	{
	case EAirView::Volume: return TEXT("온도 볼륨");
	case EAirView::StreamSpeed: return TEXT("유선 · 속도");
	case EAirView::StreamTemperature: return TEXT("유선 · 온도");
	case EAirView::StreamSpeedVolume: return TEXT("유선(속도) + 온도 볼륨");
	case EAirView::Plan: return TEXT("평면 보기");
	case EAirView::Arrows: return TEXT("화살표 · 속도");
	default: return TEXT("?");
	}
}
