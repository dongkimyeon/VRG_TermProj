#pragma once
#include "CoreMinimal.h"
class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;

// GJ source meshes have +X length, 0..100 cm. Game-thread visual operations only.
namespace GjConnectionMesh
{
    VRG_TERMPROJ_API UStaticMesh* Mesh(const TCHAR* Name, bool bRack = false);
    VRG_TERMPROJ_API UMaterialInterface* Material(const TCHAR* Name);
    VRG_TERMPROJ_API void AddPath(AActor* Owner, const TArray<FVector>& LocalPointsCm,
        UStaticMesh* Body, UMaterialInterface* Surface, double BendCm,
        TArray<TObjectPtr<UStaticMeshComponent>>& Out, double DiameterScale = 1.0);
}
