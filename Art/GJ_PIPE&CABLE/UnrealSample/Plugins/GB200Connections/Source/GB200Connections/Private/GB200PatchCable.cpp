#include "GB200PatchCable.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"

AGB200PatchCable::AGB200PatchCable()
{
    PrimaryActorTick.bCanEverTick=true;
    PrimaryActorTick.TickInterval=.05f;
    Path=CreateDefaultSubobject<USplineComponent>(TEXT("EditablePath"));
    SetRootComponent(Path);
    Path->bSplineHasBeenEdited=true;
    StartPlug=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StartPlug")); StartPlug->SetupAttachment(Path);
    EndPlug=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EndPlug")); EndPlug->SetupAttachment(Path);
    StartPlug->SetCollisionEnabled(ECollisionEnabled::NoCollision); EndPlug->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}
void AGB200PatchCable::ResolveEnds(FVector& A,FVector& B,FVector& DA,FVector& DB) const
{
    // Port offsets must match the visual actor scale. CDU's project API uses rotation only;
    // integration should supply its world-m values after multiplying by 100.
    const FTransform S=IsValid(StartAnchor)?StartAnchor->GetActorTransform():GetActorTransform();
    const FTransform E=IsValid(EndAnchor)?EndAnchor->GetActorTransform():GetActorTransform();
    A=S.TransformPosition(StartOffsetCm); B=E.TransformPosition(EndOffsetCm);
    DA=S.TransformVectorNoScale(StartOutward).GetSafeNormal(); DB=E.TransformVectorNoScale(EndOutward).GetSafeNormal();
    if (DA.IsNearlyZero()) DA=FVector::ForwardVector;
    if (DB.IsNearlyZero()) DB=-FVector::ForwardVector;
}
void AGB200PatchCable::OnConstruction(const FTransform& T)
{
    Super::OnConstruction(T); RebuildConnection();
}
void AGB200PatchCable::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // Generated meshes are transient; reconstruct when reopening the preview map.
    if (Path) RebuildConnection();
}
void AGB200PatchCable::Tick(float Dt)
{
    Super::Tick(Dt);
    if (!bAutoPath) return;
    FVector A,B,DA,DB; ResolveEnds(A,B,DA,DB);
    if (!A.Equals(LastStart,.01)||!B.Equals(LastEnd,.01)||!DA.Equals(LastStartDir,.0001)||!DB.Equals(LastEndDir,.0001)) RebuildConnection();
}
void AGB200PatchCable::RebuildConnection()
{
    for (USplineMeshComponent* Segment:Segments) if (IsValid(Segment)) Segment->DestroyComponent();
    Segments.Reset();
    FVector A,B,DA,DB; ResolveEnds(A,B,DA,DB);
    LastStart=A; LastEnd=B; LastStartDir=DA; LastEndDir=DB;
    if (bAutoPath)
    {
        const FVector S=A+DA*StartPlugLengthCm, E=B+DB*EndPlugLengthCm;
        const float Lead=FMath::Max(LeadCm,1.f);
        Path->ClearSplinePoints(false);
        const FVector Points[]={S,S+DA*Lead,(S+E)*.5-FVector(0,0,FMath::Max(SagCm,0.f)),E+DB*Lead,E};
        for (const FVector& P:Points) Path->AddSplinePoint(P,ESplineCoordinateSpace::World,false);
        Path->SetTangentAtSplinePoint(0,DA*Lead,ESplineCoordinateSpace::World,false);
        Path->SetTangentAtSplinePoint(4,-DB*Lead,ESplineCoordinateSpace::World,false);
        Path->UpdateSpline();
    }
    if (Path->GetNumberOfSplinePoints()<2) return;
    if (!bAutoPath)
    {
        DA=Path->GetDirectionAtSplinePoint(0,ESplineCoordinateSpace::World);
        DB=-Path->GetDirectionAtSplinePoint(Path->GetNumberOfSplinePoints()-1,ESplineCoordinateSpace::World);
        A=Path->GetLocationAtSplinePoint(0,ESplineCoordinateSpace::World)-DA*StartPlugLengthCm;
        B=Path->GetLocationAtSplinePoint(Path->GetNumberOfSplinePoints()-1,ESplineCoordinateSpace::World)-DB*EndPlugLengthCm;
    }
    StartPlug->SetStaticMesh(StartPlugMesh); EndPlug->SetStaticMesh(EndPlugMesh);
    StartPlug->SetWorldLocationAndRotation(A,DA.Rotation()); EndPlug->SetWorldLocationAndRotation(B,DB.Rotation());
    if (!BodyMesh) return;
    // Subdivide each original Hermite span in parameter space, preserving its curve exactly.
    // A per-span stable up direction avoids the parallel-to-Z singularity on vertical runs.
    for (int32 I=0;I<Path->GetNumberOfSplinePoints()-1;++I)
    {
        const float D0=Path->GetDistanceAlongSplineAtSplinePoint(I),D1=Path->GetDistanceAlongSplineAtSplinePoint(I+1);
        const int32 Count=FMath::Clamp(FMath::CeilToInt((D1-D0)/FMath::Max(MaxSegmentCm,1.f)),1,256);
        FVector MidDir=Path->GetTangentAtSplineInputKey(I+.5f,ESplineCoordinateSpace::Local).GetSafeNormal();
        FVector Up=FMath::Abs(MidDir.Z)>.85f?FVector::RightVector:FVector::UpVector;
        const FVector V0=Path->GetLocationAtSplinePoint(I,ESplineCoordinateSpace::Local);
        const FVector V1=Path->GetLocationAtSplinePoint(I+1,ESplineCoordinateSpace::Local);
        const bool bLinear=Path->GetSplinePointType(I)==ESplinePointType::Linear;
        const FVector T0=bLinear?V1-V0:Path->GetLeaveTangentAtSplinePoint(I,ESplineCoordinateSpace::Local);
        const FVector T1=bLinear?V1-V0:Path->GetArriveTangentAtSplinePoint(I+1,ESplineCoordinateSpace::Local);
        for (int32 J=0;J<Count;++J)
        {
            const float K0=float(J)/Count,K1=float(J+1)/Count,Step=1.f/Count;
            FVector P0=FMath::CubicInterp(V0,T0,V1,T1,K0);
            FVector P1=FMath::CubicInterp(V0,T0,V1,T1,K1);
            if (P0.Equals(P1,.001f)) continue;
            auto* M=NewObject<USplineMeshComponent>(this,NAME_None,RF_Transient);
            M->CreationMethod=EComponentCreationMethod::UserConstructionScript;
            M->SetupAttachment(Path); M->SetMobility(EComponentMobility::Movable);
            M->SetStaticMesh(BodyMesh); if (BodyMaterial) M->SetMaterial(0,BodyMaterial);
            M->SetCollisionEnabled(ECollisionEnabled::NoCollision); M->SetForwardAxis(ESplineMeshAxis::X,false);
            M->SetBoundaryMin(0,false); M->SetBoundaryMax(100,false); M->SetSplineUpDir(Up,false);
            M->SetStartAndEnd(P0,FMath::CubicInterpDerivative(V0,T0,V1,T1,K0)*Step,
                P1,FMath::CubicInterpDerivative(V0,T0,V1,T1,K1)*Step,false);
            M->RegisterComponent(); M->UpdateMesh(); Segments.Add(M);
        }
    }
}
