// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// Air display modes of the simulation view (2026-10-27, the professor's streamtrace figure). Cycled with V.
//  Volume              the temperature volume (as before)
//  StreamSpeed         streamtubes of the time-averaged flow, coloured by speed (volume off)
//  StreamTemperature   the same tubes coloured by air temperature
//  StreamSpeedVolume   speed tubes over the temperature volume
//  Plan                a horizontal slice from above: temperature colours and the in-plane streamlines
//  Arrows              velocity arrows of the averaged flow on three heights, coloured by speed
enum class EAirView : uint8 { Volume = 0, StreamSpeed, StreamTemperature, StreamSpeedVolume, Plan, Arrows, Count };

// A colour bar shown on the HUD for the current mode.
struct FAirFlowLegend
{
	FString Title;
	double Min = 0.0, Max = 1.0;
	FString Unit;
};

namespace AirFlowView
{
	// Blue -> cyan -> green -> yellow -> red over [0, 1] (clamped), the figure's colour map.
	VRG_TERMPROJ_API FLinearColor Colormap(double T);
	// The colour quantised to a fixed number of bands (one instanced component per band).
	VRG_TERMPROJ_API FLinearColor Banded(double T, int32 Bands = 32);
	VRG_TERMPROJ_API const TCHAR* Name(EAirView View);

	// Display ranges (UI choices, not limits the model enforces): speed 0-2 m/s (CRAH jets saturate red),
	// temperature 18-40 °C (supply air to rack exhaust).
	constexpr double SpeedMaxMs = 2.0;
	constexpr double TempMinC = 18.0;
	constexpr double TempMaxC = 40.0;
}
