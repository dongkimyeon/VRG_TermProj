#pragma once
#include "CoreMinimal.h"

// Lightweight game-thread display copy; no component arrays or solver mutation.
struct FRackCoolantDisplaySnapshot
{
	double InletTempK = 0;
	double OutletTempK = 0;
	bool bValid = false;
};

namespace PipeTemperatureDisplay
{
	inline FLinearColor UnknownColor() { return FLinearColor(.35f, .35f, .35f); }
	inline FLinearColor Color(double TempK, bool bValid)
	{
		if (!bValid || !FMath::IsFinite(TempK) || TempK <= 0) return UnknownColor();
		// 20..60 C is a fixed visualization range, NOT an equipment limit or measured specification.
		// Fixed across runs so equal temperatures retain equal colours; solver temperatures stay in K.
		const float Alpha = static_cast<float>(FMath::Clamp((TempK - 293.15) / 40.0, 0.0, 1.0));
		return FMath::Lerp(FLinearColor(.1f, .3f, 1.f), FLinearColor(1.f, .15f, .05f), Alpha);
	}
}
