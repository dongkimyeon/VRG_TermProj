// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Plant/FacilityPlant.h"

// S8 F7 (Tools/설비냉각_계획.md): what the facility plant looks like on screen, built from the plant's own networks.
//
// "보이는 것 = 계산하는 것": one display pipe per network pipe and per pump of every built loop (FWS / CHW / CW), with
// that edge's solved flow; a topology draws the pipes its network has, nothing else. Equipment boxes are the units the
// plant model counts (pumps incl. standby, dry cooler units of the thermal bank, chillers incl. standby, trim chillers,
// the economizer). Leak markers sit on the leaking nodes with the solved leak flow.
//
// The positions are a SCHEMATIC: the plant has no piping geometry, so the layout puts a plant yard beside the room
// (one lane per loop) and runs each branch to the consumer it feeds (the CDU of the group, the CRAH unit, the chiller
// whose condenser it is). Lengths and sizes on screen are not the modelled pressure drops.
// Plain C++ in metres (rule 1: the actor converts to cm); built on the game thread from the plant state (rule 5).

struct FPlantDisplayInput
{
	FBox RoomM = FBox(ForceInit);         // air domain (or rack) bounds; the yard goes beyond its +X side
	TArray<FVector> CduPositionsM;        // per CDU group (FWS branch / combined CHW branch order)
	TArray<FVector> CrahPositionsM;       // per CRAH unit (CHW branch order)
};

enum class EPlantPipeRole : uint8 { Pump, CheckValve, SupplyMain, SupplyRun, Valve, Exchanger, ReturnRun, ReturnMain, ReturnCoil, Bypass, Other };

struct FPlantDisplayPipe
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	EPlantPipeRole Role = EPlantPipeRole::Other;
	int32 Edge = INDEX_NONE;              // network pipe index, or pump index for Role == Pump
	int32 Branch = INDEX_NONE;            // branch of the loop for branch pipes
	TArray<FVector> PointsM;              // polyline (2 points, or 3 for a return coil through its equipment)
	double FlowM3s = 0.0;                 // solved flow of that edge (sign: From -> To)
	double TempK = 0.0;                   // the loop volume this pipe belongs to (supply / return / series middle)
	bool bClosed = false, bReversed = false, bHeader = false;
};

enum class EPlantEquipmentKind : uint8 { Pump, DryCooler, Chiller, TrimChiller, Economizer };

struct FPlantDisplayEquipment
{
	EPlantLoopId Loop = EPlantLoopId::Fws;
	EPlantEquipmentKind Kind = EPlantEquipmentKind::Pump;
	int32 Index = 0;
	FVector CenterM = FVector::ZeroVector, SizeM = FVector::OneVector;
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
	TArray<FPlantDisplayEquipment> Equipment;
	TArray<FPlantDisplayLeak> Leaks;
	// Structure key: rebuild meshes only when this changes (counts and topology), recolour otherwise.
	uint32 StructureHash = 0;
	// Network node positions per loop (the pipe end points come from these; tests compare against them).
	TArray<FVector> NodePositionsM[3];
};

namespace PlantDisplay
{
	VRG_TERMPROJ_API FPlantDisplaySnapshot Build(const FFacilityPlant& Plant, const FPlantDisplayInput& In);
	// Display colour of a pipe: temperature mode uses the fixed pipe range (PipeTemperatureDisplay), flow mode the loop's
	// hue with brightness by |flow| relative to the loop's largest flow. Closed / zero flow = grey, reversed = magenta.
	VRG_TERMPROJ_API FLinearColor PipeColor(const FPlantDisplaySnapshot& S, const FPlantDisplayPipe& P, bool bTemperatureMode);
	VRG_TERMPROJ_API FLinearColor EquipmentColor(const FPlantDisplayEquipment& E);
}
