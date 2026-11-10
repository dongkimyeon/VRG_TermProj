// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FFloorWater;
class FLeakSpray;

// W6 (Tools/누수_실시간_계획.md L6, 2026-11-10): the computed water drawn as water.
//
// A render snapshot (rule 5) built from the W2 spray and the W3 floor water, plain C++ in metres (rule 1); the actor
// that draws it converts to cm. What is drawn is what is computed:
//  - Floor: one flat water prism per wet cell, top at bed + depth, side skirts where the neighbour is lower. Its volume
//    read back from the mesh equals the floor model's stored volume (SurfaceVolumeM3, checked by VRG.Water.Display).
//    The whole surface is lifted LiftM above the bed (display only: a translation, the volume does not change) so a
//    0.01 mm puddle does not fight the floor's depth. Heights are not exaggerated (plan L6).
//  - Drip trays: the tray's stored volume as a sheet over its footprint (stored / area deep).
//  - Air: every parcel is one sphere of its computed diameter at its computed position (a parcel is a group of drops of
//    that size; one sphere stands for it). The path each parcel travelled in the last step is a tube of the same
//    diameter (a jet is its own path; for a drop it is the streak a camera with a 1/50 s shutter would see).
//  - Films: a sheet on the wetted face, as wide as the film and as high as its wet height, thickness = volume / area.
//  - Wet marks: floor cells that were wet once and are dry now (display memory, there is no drying model), and every
//    box the spray or the floor water touched, from its bottom up to the highest wet point.
// Not drawn: water waiting at a hole, water in rack interiors (W3.5, inside the cabinet), the outdoor ground.

struct VRG_TERMPROJ_API FWaterDisplaySettings
{
	double MinDepthM = 1.0e-6;          // thinner floor cells are not drawn (their volume is reported as hidden)
	double LiftM = 1.0e-3;              // display-only lift of the whole floor surface above the bed
	double WetMarkLiftM = 0.5e-3;       // wet (dry now) floor marks
	double DepthColorRangeM = 0.01;     // vertex colour R = depth / this (material: absorption)
	double SpeedColorRangeMs = 1.0;     // vertex colour G, B = 0.5 + 0.5 u / this (material: flow direction)
	double SkinOffsetM = 3.0e-3;        // wet skin stands this far off the box faces
	int32 MaxSurfaceCells = 300000;     // performance cap (the rest is reported as hidden)
};

struct VRG_TERMPROJ_API FWaterSurfaceMesh
{
	TArray<FVector> VerticesM;
	TArray<int32> Triangles;
	TArray<FVector> NormalsM;
	TArray<FLinearColor> Colors;
	int32 Cells = 0;
	void Reset() { VerticesM.Reset(); Triangles.Reset(); NormalsM.Reset(); Colors.Reset(); Cells = 0; }
};

// A thin box: centre, rotation, size (X = through the sheet, Y = width, Z = height for a wall sheet).
struct VRG_TERMPROJ_API FWaterSheet
{
	FVector CenterM = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector SizeM = FVector::ZeroVector;
	double VolumeM3 = 0.0;
	bool bTray = false;
};

struct VRG_TERMPROJ_API FWaterDropShape
{
	FVector CenterM = FVector::ZeroVector;
	double DiameterM = 0.0;
	double VolumeM3 = 0.0;
	bool bJet = false;
};

struct VRG_TERMPROJ_API FWaterStreakShape
{
	FVector FromM = FVector::ZeroVector, ToM = FVector::ZeroVector;
	double DiameterM = 0.0;
	bool bJet = false;
};

struct VRG_TERMPROJ_API FWetSkin
{
	FVector CenterM = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector SizeM = FVector::ZeroVector;
	int32 Obstacle = INDEX_NONE;
};

struct VRG_TERMPROJ_API FWaterDisplayTotals
{
	double FloorShownM3 = 0.0, FloorComputedM3 = 0.0, FloorHiddenM3 = 0.0;
	double TrayShownM3 = 0.0, TrayComputedM3 = 0.0;
	double FilmShownM3 = 0.0, FilmComputedM3 = 0.0;
	double AirShownM3 = 0.0, AirComputedM3 = 0.0;
	int32 ParcelsShown = 0, ParcelsComputed = 0, StreaksShown = 0, TrailsComputed = 0;
	int32 SurfaceCells = 0, WetMarkCells = 0, WetSkins = 0;
	// max(|shown + hidden - computed|) / computed over floor, trays, films and air (0 when nothing is computed)
	double Residual = 0.0;
};

struct VRG_TERMPROJ_API FWaterDisplaySnapshot
{
	FWaterSurfaceMesh Surface;
	FWaterSurfaceMesh WetFloor;
	TArray<FWaterSheet> Sheets;          // films and drip trays
	TArray<FWaterDropShape> Drops;
	TArray<FWaterStreakShape> Streaks;
	TArray<FWetSkin> Skins;
	FWaterDisplayTotals Totals;
};

namespace WaterDisplay
{
	// Remembers the floor cells that have been wet (display memory only; sized to the floor grid, cleared when it changes).
	VRG_TERMPROJ_API void UpdateEverWet(const FFloorWater& Floor, TArray<uint8>& EverWet, double MinDepthM);
	VRG_TERMPROJ_API void Build(const FFloorWater& Floor, const FLeakSpray& Spray, const TArray<uint8>* EverWet,
		const FWaterDisplaySettings& Settings, FWaterDisplaySnapshot& Out);
	// Volume read back from the drawn floor surface: every upward triangle's plan area times its height above the bed
	// under it (minus the display lift). Skirts are vertical and add nothing.
	VRG_TERMPROJ_API double SurfaceVolumeM3(const FWaterSurfaceMesh& Mesh, const FFloorWater& Floor, double LiftM);
	// The drawn sheets' volume (size product): equals the films' and trays' stored volume by construction.
	VRG_TERMPROJ_API double SheetVolumeM3(const TArray<FWaterSheet>& Sheets, bool bTrays);
}
