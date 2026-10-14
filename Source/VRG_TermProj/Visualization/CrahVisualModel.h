#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

// Display only: the physical box/ducts continue to use the caller's SizeM and Kind.
// Both authored meshes have a floor-centre pivot, +X supply and centimetre vertices.
namespace CrahVisualModel
{
	inline FVector NominalSizeM(bool bPerimeter)
	{
		return bPerimeter ? FVector(.9, 2.0, 2.0) : FVector(1.068, .6, 2.236);
	}

	inline UStaticMesh* LoadReference(bool bPerimeter)
	{
		// /Game/Connections is already AlwaysCook. Missing assets deliberately use the cube.
		return LoadObject<UStaticMesh>(nullptr, bPerimeter
			? TEXT("/Game/Connections/SM_CRAH_Perimeter.SM_CRAH_Perimeter")
			: TEXT("/Game/Connections/SM_CRAH_InRow.SM_CRAH_InRow"), nullptr, LOAD_NoWarn);
	}

	struct FBody
	{
		UStaticMesh* Mesh = nullptr;
		FVector OffsetCm = FVector::ZeroVector;
		FVector Scale = FVector::OneVector;
	};

	inline FBody Resolve(const FVector& SizeM, bool bPerimeter, UStaticMesh* Reference, UStaticMesh* Cube)
	{
		FBody Result;
		Result.Mesh = Reference ? Reference : Cube;
		Result.OffsetCm = Reference ? FVector::ZeroVector : FVector(0, 0, SizeM.Z * 50.0);
		Result.Scale = Reference ? SizeM / NominalSizeM(bPerimeter) : SizeM;
		return Result;
	}
}
