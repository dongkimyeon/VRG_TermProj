#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "RoomAirDomain.generated.h"

// Authored world metres; floor Z=0. Layout UI/history and optional v2 persistence share this value type.
USTRUCT()
struct VRG_TERMPROJ_API FRoomAirDomainSettings
{
	GENERATED_BODY()
	UPROPERTY() bool bManual = false;
	UPROPERTY() FVector2D MinXYM = FVector2D::ZeroVector;
	UPROPERTY() FVector2D MaxXYM = FVector2D::ZeroVector;
	UPROPERTY() double CeilingHeightM = 0.0;
	bool operator==(const FRoomAirDomainSettings& Other) const
	{ return bManual == Other.bManual && MinXYM == Other.MinXYM && MaxXYM == Other.MaxXYM && CeilingHeightM == Other.CeilingHeightM; }
};

namespace VrgRoomAirDomain
{
	// Existing FitToRacks allocation policy, shared rather than a second resource limit.
	inline constexpr int32 MaxFitCellsPerAxis = 128;
	inline constexpr double MinFitCellSizeM = 0.01;
	// A manual request is rejected instead of the legacy auto-fit clamping its counts.
	VRG_TERMPROJ_API bool BuildGrid(const FRoomAirDomainSettings& Settings, const FAirGridParams& Source,
		FAirGridParams& OutGrid, FBox& OutActualBoundsM, FString& Reason);
	// Transform contains metres, including translation. Actor/UE conversion belongs to the caller.
	VRG_TERMPROJ_API FBox TransformedBoxM(const FVector& LocalMinM, const FVector& LocalMaxM, const FTransform& TransformM);
}

// User decision 2026-10-06: a one-cell horizontal plate, NOT a filled volume above HeightM.
// Hole metres are relative to the owning plate's MinXYM; no exterior outlet boundary.
USTRUCT()
struct VRG_TERMPROJ_API FRoomCeilingHole
{
	GENERATED_BODY()
	UPROPERTY() FVector2D LocalMinXYM = FVector2D::ZeroVector;
	UPROPERTY() FVector2D LocalMaxXYM = FVector2D::ZeroVector;
	bool operator==(const FRoomCeilingHole& Other) const
	{ return LocalMinXYM == Other.LocalMinXYM && LocalMaxXYM == Other.LocalMaxXYM; }
};

// World metres; optional v2 complete list, absent field retains the current list.
USTRUCT()
struct VRG_TERMPROJ_API FRoomCeilingPlate
{
	GENERATED_BODY()
	UPROPERTY() FVector2D MinXYM = FVector2D::ZeroVector;
	UPROPERTY() FVector2D MaxXYM = FVector2D::ZeroVector;
	UPROPERTY() double HeightM = 0.0;
	UPROPERTY() TArray<FRoomCeilingHole> Holes;
	bool operator==(const FRoomCeilingPlate& Other) const
	{ return MinXYM == Other.MinXYM && MaxXYM == Other.MaxXYM && HeightM == Other.HeightM && Holes == Other.Holes; }
};

struct VRG_TERMPROJ_API FRoomCeilingPlan
{
	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector MaxCellExclusive = FIntVector::ZeroValue;
	FBox ActualBoundsM = FBox(ForceInit);
	// Global grid XY indices [Min,Max), only cells whose whole footprint lies in a requested hole.
	TArray<FIntRect> OpenCells;
};

namespace VrgRoomAirDomain
{
	// Conservative XY overlap, containing Z layer [floor(height/h), floor(height/h)+1).
	// Bounds are reported, never silently clipped; caller supplies the validated manual grid.
	VRG_TERMPROJ_API bool BuildCeilingPlans(const FRoomAirDomainSettings& Room, const TArray<FRoomCeilingPlate>& Plates,
		const FAirGridParams& Grid, const FBox& ActualRoomM, TArray<FRoomCeilingPlan>& OutPlans, FString& Reason);
	VRG_TERMPROJ_API bool IsCeilingCellOpen(const FRoomCeilingPlan& Plan, int32 X, int32 Y);
	// Positive volume overlap with remaining plate cells, for the existing equipment/duct validator.
	VRG_TERMPROJ_API bool CeilingIntersectsBox(const FRoomCeilingPlan& Plan, const FBox& BoxM, double CellSizeM);
	// Union into the mask; return only newly marked cell indices for actor ownership/diagnostics.
	// Plans must have been built for this grid. Caller notifies the solver once after all solids are ready.
	VRG_TERMPROJ_API TArray<int32> MarkCeilingCells(FUniformAirGrid& Grid, const TArray<FRoomCeilingPlan>& Plans);
}
