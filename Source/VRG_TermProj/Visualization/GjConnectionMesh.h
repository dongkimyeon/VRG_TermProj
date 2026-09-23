#pragma once
#include "CoreMinimal.h"
class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class UInstancedStaticMeshComponent;

// GJ source meshes have +X length, 0..100 cm. Game-thread visual operations only.
namespace GjConnectionMesh
{
    VRG_TERMPROJ_API UStaticMesh* Mesh(const TCHAR* Name, bool bRack = false);
    VRG_TERMPROJ_API UMaterialInterface* Material(const TCHAR* Name);
    VRG_TERMPROJ_API void AddPath(AActor* Owner, const TArray<FVector>& LocalPointsCm,
        UStaticMesh* Body, UMaterialInterface* Surface, double BendCm,
        TArray<TObjectPtr<UStaticMeshComponent>>& Out, double DiameterScale = 1.0);
    // One instanced component holding a 1U blank plate per free slot (one draw call per cabinet).
    // Slots are rack-local metres (see GjRackLayout); RackScale is the actor's size ratio to the authored rack.
    VRG_TERMPROJ_API UInstancedStaticMeshComponent* AddBlankPanels(AActor* Owner,
        const TArray<TPair<double, double>>& SlotBottomAndHeightM, const FVector& RackScale);
}
