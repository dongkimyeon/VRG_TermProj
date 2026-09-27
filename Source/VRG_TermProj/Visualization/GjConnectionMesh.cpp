#include "Visualization/GjConnectionMesh.h"
#include "Components/SplineMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

UStaticMesh* GjConnectionMesh::Mesh(const TCHAR* Name, bool bRack)
{
    const FString Path=FString::Printf(TEXT("/Game/%s/%s.%s"),bRack?TEXT("ExistingGB200"):TEXT("Connections"),Name,Name);
    return LoadObject<UStaticMesh>(nullptr,*Path);
}
UMaterialInterface* GjConnectionMesh::Material(const TCHAR* Name)
{
    return LoadObject<UMaterialInterface>(nullptr,*FString::Printf(TEXT("/Game/Connections/Materials/%s.%s"),Name,Name));
}
void GjConnectionMesh::AddPath(AActor* Owner, const TArray<FVector>& Points, UStaticMesh* Body,
    UMaterialInterface* Surface, double BendCm, TArray<TObjectPtr<UStaticMeshComponent>>& Out, double DiameterScale)
{
    if (!Owner || !Body) return;
    TArray<FSplineSegment> Segments;
    BuildSegments(Points, BendCm, Segments);
    for (const FSplineSegment& S:Segments) if (USplineMeshComponent* M=AddSegment(Owner,S,Body,Surface,DiameterScale)) Out.Add(M);
}

void GjConnectionMesh::BuildSegments(const TArray<FVector>& Points, double BendCm, TArray<FSplineSegment>& Out)
{
    Out.Reset();
    if (Points.Num()<2) return;
    struct FKnot { FVector P, Left, Right; };
    TArray<FVector> Clean;
    for (const FVector& P:Points) if(Clean.IsEmpty() || !P.Equals(Clean.Last(),.001)) Clean.Add(P);
    if(Clean.Num()<2) return;
    TArray<FKnot> Knots;
    auto Add=[&](FVector P) { Knots.Add({P,P,P}); };
    Add(Clean[0]);
    // Rounded-corner construction adapted from Art/Infrastructure/build_infrastructure.py.
    for (int32 I=1;I<Clean.Num()-1;++I)
    {
        const FVector In=(Clean[I]-Clean[I-1]).GetSafeNormal(), Next=(Clean[I+1]-Clean[I]).GetSafeNormal();
        const double Angle=FMath::Acos(FMath::Clamp(FVector::DotProduct(In,Next),-1.0,1.0));
        if (Angle<.0001 || Angle>PI-.001) { Add(Clean[I]); continue; }
        const double Cut=FMath::Min3(BendCm,.4*FVector::Distance(Clean[I],Clean[I-1]),.4*FVector::Distance(Clean[I+1],Clean[I]));
        const double Handle=4.0/3.0*FMath::Tan(Angle/4)*Cut/FMath::Tan(Angle/2);
        Add(Clean[I]-In*Cut); Knots.Last().Right=Knots.Last().P+In*Handle;
        Add(Clean[I]+Next*Cut); Knots.Last().Left=Knots.Last().P-Next*Handle;
    }
    Add(Clean.Last());
    for (int32 I=0;I<Knots.Num()-1;++I)
    {
        FKnot& A=Knots[I]; FKnot& B=Knots[I+1];
        if (A.P.Equals(A.Right,.0001) && B.P.Equals(B.Left,.0001))
        { const FVector D=(B.P-A.P)/3; A.Right=A.P+D; B.Left=B.P-D; }
        if (A.P.Equals(B.P,.001)) continue;
        Out.Add({A.P,(A.Right-A.P)*3,B.P,(B.P-B.Left)*3});
    }
}

USplineMeshComponent* GjConnectionMesh::AddSegment(AActor* Owner, const FSplineSegment& S, UStaticMesh* Body,
    UMaterialInterface* Surface, double DiameterScale)
{
    if (!Owner || !Body) return nullptr;
    {
        auto* M=NewObject<USplineMeshComponent>(Owner,NAME_None,RF_Transient);
        M->SetupAttachment(Owner->GetRootComponent()); M->SetMobility(EComponentMobility::Movable);
        M->SetStaticMesh(Body); M->SetCollisionEnabled(ECollisionEnabled::NoCollision); M->SetCastShadow(false);
        M->SetForwardAxis(ESplineMeshAxis::X,false); M->SetBoundaryMin(0,false); M->SetBoundaryMax(100,false);
        const FVector Direction=(S.End-S.Start).GetSafeNormal();
        M->SetSplineUpDir(FMath::Abs(Direction.Z)>.85?FVector::RightVector:FVector::UpVector,false);
        M->SetStartScale(FVector2D(DiameterScale),false); M->SetEndScale(FVector2D(DiameterScale),false);
        M->SetStartAndEnd(S.Start,S.StartTangent,S.End,S.EndTangent,false);
        if(Surface) M->SetMaterial(0,Surface);
        // Registration builds the render state from the spline parameters set above; a following UpdateMesh()
        // rebuilt it a second time (a third of a room rebuild, measured).
        M->RegisterComponent();
        if(Surface) M->CreateDynamicMaterialInstance(0,Surface);
        return M;
    }
}

UInstancedStaticMeshComponent* GjConnectionMesh::AddBlankPanels(AActor* Owner,
    const TArray<TPair<double, double>>& Slots, const FVector& RackScale)
{
    UStaticMesh* Blank=Mesh(TEXT("SM_GJ_Blank1U"));
    if (!Owner || !Blank || Slots.IsEmpty()) return nullptr;
    auto* C=NewObject<UInstancedStaticMeshComponent>(Owner,MakeUniqueObjectName(Owner,UInstancedStaticMeshComponent::StaticClass(),TEXT("GJ_BlankPanels")));
    C->SetupAttachment(Owner->GetRootComponent()); C->SetStaticMesh(Blank); C->SetMobility(EComponentMobility::Movable);
    C->SetCollisionEnabled(ECollisionEnabled::NoCollision); C->SetCastShadow(false); C->RegisterComponent();
    for (const TPair<double, double>& S:Slots)
    {
        // Metres -> cm here only (rule 1). The mesh is exactly 1U tall, so Z scale stretches a partial gap.
        const FVector Local(GjRackLayout::BlankFaceXM,0,S.Key);
        C->AddInstance(FTransform(FRotator::ZeroRotator,Local*RackScale*100.0,
            FVector(RackScale.X,RackScale.Y,RackScale.Z*S.Value/GjRackLayout::UnitM)));
    }
    return C;
}
