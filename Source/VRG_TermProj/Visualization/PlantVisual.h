// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class AActor;
class USceneComponent;
class UStaticMesh;
class UInstancedStaticMeshComponent;
struct FPlantInstance;

// Authored plant exteriors (Art/Plant); the following parts remain the fallback.
// S8 plant equipment look (2026-10-21): every unit is a few engine boxes and cylinders (dry cooler: legs, coil block,
// fans on top; chiller: two shells, compressor, control panel; pump: motor and volute; plate exchanger: plate pack
// between frame plates) instead of one plain cube. Display only: nothing here is a model input. The parts stay inside
// the unit's box (origin = bottom centre, +X = length, +Y = width, +Z = up), so selection and overlap keep using it.
// Shapes and the nominal sizes are a schematic, not catalog dimensions (Art/Plant/모델링_요청.md replaces them with
// authored meshes later; the ports below must then move with the mesh).
enum class EPlantVisualKind : uint8 { DryCooler, Chiller, Pump, Economizer, Valve2Way, CheckValve, LeakMarker };

// Colour role of a part. Accent carries the unit's identity / status colour (running, stopped, failed).
enum class EPlantTone : uint8 { Body, Frame, Coil, Fan, Pipe, Panel, Screen, Shell, Accent };

struct FPlantVisualPart
{
	bool bCylinder = false;
	FVector A = FVector::ZeroVector;     // box: centre; cylinder: one end centre [m, unit local]
	FVector B = FVector::ZeroVector;     // cylinder: other end centre
	FVector BoxSizeM = FVector::ZeroVector;
	double DiameterM = 0.0;
	EPlantTone Tone = EPlantTone::Body;
};

// Pipe connection points [m, unit local, on the box face]. Coil = the water that passes the unit in its own loop
// (dry cooler coil, chiller evaporator), Condenser = a chiller's condenser water (CW loop). "In" is the -X side one.
enum class EPlantPort : uint8 { CoilIn, CoilOut, CondenserIn, CondenserOut };

namespace PlantVisual
{
	// Authored cm mesh, nominal metre envelope; null selects the primitive fallback.
	VRG_TERMPROJ_API UStaticMesh* ReferenceMesh(EPlantVisualKind Kind);
	VRG_TERMPROJ_API void AppendEquipment(EPlantVisualKind Kind, const FVector& SizeM, const FLinearColor& Accent,
		TArray<FPlantInstance>& Out, const FTransform& Unit = FTransform::Identity);
	VRG_TERMPROJ_API FVector NominalSizeM(EPlantVisualKind Kind);
	// Parts scaled from the nominal size to SizeM per axis.
	VRG_TERMPROJ_API void BuildParts(EPlantVisualKind Kind, const FVector& SizeM, TArray<FPlantVisualPart>& Out);
	VRG_TERMPROJ_API FVector PortLocalM(EPlantVisualKind Kind, const FVector& SizeM, EPlantPort Port);
	VRG_TERMPROJ_API FLinearColor ToneColor(EPlantTone Tone, const FLinearColor& Accent);

	// Engine shape transforms in cm (the basic cube and cylinder are 100 units, centred; the cylinder stands on Z).
	VRG_TERMPROJ_API FTransform BoxTransformCm(const FVector& CenterCm, const FVector& SizeCm);
	VRG_TERMPROJ_API FTransform TubeTransformCm(const FVector& StartCm, const FVector& EndCm, double DiameterCm);
	VRG_TERMPROJ_API FTransform PartTransformCm(const FPlantVisualPart& Part);
}

// One drawn shape: engine mesh, colour, transform relative to the owner's parent component [cm].
struct FPlantInstance
{
	UStaticMesh* Mesh = nullptr;
	FLinearColor Color = FLinearColor::White;
	FTransform Transform;
	bool bUseMeshMaterials = false; // preserve PBR slots; tint only MC_Plant_Accent
};

// Shapes grouped into one instanced component per (mesh, colour): a few draw calls for hundreds of parts and pipe
// segments. Colours are quantised to 1/64 per channel (the key), close enough for display. Game thread only (rule 5).
namespace PlantInstances
{
	using FSets = TMap<uint64, TObjectPtr<UInstancedStaticMeshComponent>>;
	// Replaces every set's instances with the list (sets that get none are emptied and kept). Returns instances drawn.
	VRG_TERMPROJ_API int32 Apply(AActor* Owner, USceneComponent* Parent, FSets& Sets, const TArray<FPlantInstance>& List, bool bCastShadow);
	// Order-sensitive content key of a list (skip the upload when nothing changed).
	VRG_TERMPROJ_API uint32 Hash(const TArray<FPlantInstance>& List);
	VRG_TERMPROJ_API UStaticMesh* Cube();
	VRG_TERMPROJ_API UStaticMesh* Cylinder();
}
