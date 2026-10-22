// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "Visualization/PlantVisual.h"

// S8 F7 (Tools/설비냉각_계획.md): what the facility plant looks like on screen, built from the plant's own networks.
//
// "보이는 것 = 계산하는 것": one display pipe per network pipe and per pump of every built loop (FWS / CHW / CW), with
// that edge's solved flow; a topology draws the pipes its network has, nothing else. Equipment boxes are the units the
// plant model counts (pumps incl. standby, dry cooler units of the thermal bank, chillers incl. standby, trim chillers,
// the economizer). Leak markers sit on the leaking nodes with the solved leak flow.
//
// Layout (2026-10-21, "patch" routing like Visualization/PatchManager): a header node of the network (supply / return /
// middle header, plant supply / return, pump suction) is drawn as a header pipe, and every pipe leaves it at its own
// anchor and runs orthogonally in its own lane: no diagonal lines. Branches into the room run in the room's lanes over
// the ceiling (one lane per branch, shared by all loops, so no two runs overlap), supply in the lower layer and return
// in the upper one, and drop onto the consumer; the yard coils and condenser branches run in aisle lanes beside their
// unit row and drop to the unit's connections. Each loop has its own heights, so loops never cross in a layer.
//
// The positions are a SCHEMATIC: the plant has no piping geometry, so the layout puts a plant yard beside the room
// (one lane per loop) and runs each branch to the consumer it feeds (the CDU of the group, the CRAH unit, the chiller
// whose condenser it is). Lengths and sizes on screen are not the modelled pressure drops.
// Plain C++ in metres (rule 1: the actor converts to cm); built on the game thread from the plant state (rule 5).

// S8 F8: a unit placed by hand (APlantEquipmentActor): box centre, size and rotation [m].
struct FPlantDisplayUnit
{
	FVector CenterM = FVector::ZeroVector, SizeM = FVector::OneVector;
	FQuat Rotation = FQuat::Identity;
};

struct FPlantDisplayInput
{
	FBox RoomM = FBox(ForceInit);         // air domain (or rack) bounds; the yard goes beyond its +X side
	TArray<FVector> CduPositionsM;        // per CDU group (FWS branch / combined CHW branch order)
	TArray<FVector> CrahPositionsM;       // per CRAH unit (CHW branch order)
	// F8: placed units per kind, in the model's unit order: model unit K is drawn at placed unit K (placed units set that
	// count at the next simulation entry, FacilityPlantLayout); model units beyond the placed ones keep the yard positions
	// (layout mode right after an addition / deletion, before the plant is rebuilt).
	TArray<FPlantDisplayUnit> FwsDryCoolers, CwDryCoolers, Chillers, TrimChillers;
	// 2026-10-22: placed pumps per loop (model pump K at placed pump K) and the placed economizer (at most one).
	TArray<FPlantDisplayUnit> FwsPumps, ChwPumps, CwPumps, Economizers;
	void ClearPlaced()
	{
		FwsDryCoolers.Reset(); CwDryCoolers.Reset(); Chillers.Reset(); TrimChillers.Reset();
		FwsPumps.Reset(); ChwPumps.Reset(); CwPumps.Reset(); Economizers.Reset();
	}
	// The plant is computed and coupled now (simulation mode with vrg.Plant.Enable 1). Otherwise the yard shows the
	// structure only: loop colours, no flow brightness, no running / failed state.
	bool bLive = true;
};

enum class EPlantPipeRole : uint8 { Pump, CheckValve, SupplyMain, SupplyRun, Valve, Exchanger, ReturnRun, ReturnMain, ReturnCoil, Bypass, Economizer, Other };

struct FPlantDisplayPipe
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	EPlantPipeRole Role = EPlantPipeRole::Other;
	int32 Edge = INDEX_NONE;              // network pipe index, or pump index for Role == Pump
	int32 Branch = INDEX_NONE;            // branch of the loop for branch pipes
	int32 FromNode = INDEX_NONE, ToNode = INDEX_NONE;   // network nodes (pump: suction -> discharge)
	TArray<FVector> PointsM;              // orthogonal polyline: starts on the From node's shape, ends on the To node's
	double FlowM3s = 0.0;                 // solved flow of that edge (sign: From -> To)
	double TempK = 0.0;                   // the loop volume this pipe belongs to (supply / return / series middle)
	uint8 Volume = 0;                     // that volume: 0 supply, 1 return, 2 series middle (PlantDisplay::VolumeTempK)
	bool bClosed = false, bReversed = false, bHeader = false;   // bHeader: mains and bypass (drawn thicker)
};

// A network node drawn as a header pipe (supply / return / middle header, plant supply / return, pump suction).
struct FPlantDisplayHeader
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	int32 Node = INDEX_NONE;
	TArray<FVector> PointsM;
	double TempK = 0.0;
};

// Display-only supports: a tray under parallel lanes (box) and a pipe rack post (floor to TopM).
struct FPlantDisplayTray { EPlantLoopId Loop = EPlantLoopId::Fws; FVector CenterM = FVector::ZeroVector, SizeM = FVector::ZeroVector; };

enum class EPlantEquipmentKind : uint8 { Pump, DryCooler, Chiller, TrimChiller, Economizer };

struct FPlantDisplayEquipment
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	EPlantEquipmentKind Kind = EPlantEquipmentKind::Pump;
	int32 Index = 0;
	FVector CenterM = FVector::ZeroVector, SizeM = FVector::OneVector;
	FQuat Rotation = FQuat::Identity;     // F8: a placed unit's rotation (schematic units are axis aligned)
	bool bPlaced = false;                 // F8: drawn where a placed unit is
	bool bRunning = false, bFailed = false;
};

struct FPlantDisplayLeak
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	int32 Leak = INDEX_NONE;
	FVector PositionM = FVector::ZeroVector;
	double FlowM3s = 0.0;
};

struct FPlantDisplaySnapshot
{
	TArray<FPlantDisplayPipe> Pipes;
	TArray<FPlantDisplayHeader> Headers;
	TArray<FPlantDisplayTray> Trays;
	TArray<FVector> PostTopsM;
	TArray<FPlantDisplayEquipment> Equipment;
	TArray<FPlantDisplayLeak> Leaks;
	// Structure key: rebuild meshes only when this changes (counts and topology), recolour otherwise.
	uint32 StructureHash = 0;
	// Network node positions per loop (a header's midpoint; leak markers sit here) and every node's drawn shape
	// (one point, or the header polyline the pipes attach to; tests check the pipe ends against these).
	TArray<FVector> NodePositionsM[3];
	TArray<TArray<FVector>> NodeShapesM[3];
	bool bLive = true;
};

// Drawn diameters [m] (schematic, not the modelled pipe sizes).
namespace PlantDisplayStyle
{
	constexpr double HeaderDiameterM = 0.16, MainDiameterM = 0.14, PipeDiameterM = 0.08;
}

// Two drawn pipes / headers closer than touching (centre lines nearer than the mean diameter), outside the ends where
// they meet at a shared node. A display check like Patch::FindInterferences, not a routing constraint.
struct FPlantDisplayClash { int32 A = INDEX_NONE, B = INDEX_NONE; bool bAHeader = false, bBHeader = false; double GapM = 0.0; };

namespace PlantDisplay
{
	VRG_TERMPROJ_API FPlantDisplaySnapshot Build(const FFacilityPlant& Plant, const FPlantDisplayInput& In);
	// Display colour of a pipe: temperature mode uses the fixed pipe range (PipeTemperatureDisplay), flow mode the loop's
	// hue with brightness by |flow| relative to the loop's largest flow. Closed / zero flow = grey, reversed = magenta.
	VRG_TERMPROJ_API FLinearColor PipeColor(const FPlantDisplaySnapshot& S, const FPlantDisplayPipe& P, bool bTemperatureMode);
	VRG_TERMPROJ_API FLinearColor HeaderColor(const FPlantDisplaySnapshot& S, const FPlantDisplayHeader& H, bool bTemperatureMode);
	// Accent (status) colour of a unit: failed red, stopped grey, running its kind's colour; structure-only = kind colour.
	VRG_TERMPROJ_API FLinearColor EquipmentColor(const FPlantDisplayEquipment& E);
	VRG_TERMPROJ_API FLinearColor KindColor(EPlantLoopId Loop, EPlantEquipmentKind Kind);
	VRG_TERMPROJ_API TArray<FPlantDisplayClash> FindClashes(const FPlantDisplaySnapshot& S);
	VRG_TERMPROJ_API EPlantVisualKind VisualKind(EPlantEquipmentKind Kind);
	// World position of a unit's connection [m] (its centre, size and rotation).
	VRG_TERMPROJ_API FVector PortM(const FPlantDisplayEquipment& E, EPlantPort Port);
	// Temperature of a loop volume now (0 supply, 1 return, 2 series middle) [K]: what a pipe of that volume carries.
	VRG_TERMPROJ_API double VolumeTempK(const FFacilityPlant& Plant, EPlantLoopId Loop, uint8 Volume);
}
