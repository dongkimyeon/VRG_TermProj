#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"
#include "RoomAirDomain.generated.h"

// Authored world metres; floor Z=0. No walls, heat exchange, UI or layout-file persistence yet.
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
