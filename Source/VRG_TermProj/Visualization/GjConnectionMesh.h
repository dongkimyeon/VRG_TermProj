#pragma once
#include "CoreMinimal.h"
class AActor;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class UInstancedStaticMeshComponent;
class USplineMeshComponent;

// GJ source meshes have +X length, 0..100 cm. Game-thread visual operations only.
namespace GjConnectionMesh
{
    VRG_TERMPROJ_API UStaticMesh* Mesh(const TCHAR* Name, bool bRack = false);
    VRG_TERMPROJ_API UMaterialInterface* Material(const TCHAR* Name);
    // One spline mesh span (owner-local cm). BuildSegments is cheap maths; AddSegment creates and registers the
    // component, which is the expensive part (a few ms each), so callers may spread AddSegment over frames.
    struct FSplineSegment { FVector Start, StartTangent, End, EndTangent; };
    VRG_TERMPROJ_API void BuildSegments(const TArray<FVector>& LocalPointsCm, double BendCm, TArray<FSplineSegment>& Out);
    VRG_TERMPROJ_API USplineMeshComponent* AddSegment(AActor* Owner, const FSplineSegment& Segment, UStaticMesh* Body,
        UMaterialInterface* Surface, double DiameterScale);
    VRG_TERMPROJ_API void AddPath(AActor* Owner, const TArray<FVector>& LocalPointsCm,
        UStaticMesh* Body, UMaterialInterface* Surface, double BendCm,
        TArray<TObjectPtr<UStaticMeshComponent>>& Out, double DiameterScale = 1.0);
    // One instanced component holding a 1U blank plate per free slot (one draw call per cabinet).
    // Slots are rack-local metres (see GjRackLayout); RackScale is the actor's size ratio to the authored rack.
    VRG_TERMPROJ_API UInstancedStaticMeshComponent* AddBlankPanels(AActor* Owner,
        const TArray<TPair<double, double>>& SlotBottomAndHeightM, const FVector& RackScale);
}
