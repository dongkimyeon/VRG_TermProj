#include "Visualization/GjConnectionMesh.h"
#include "Components/SplineMeshComponent.h"
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
    if (!Owner || !Body || Points.Num()<2) return;
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
        auto* M=NewObject<USplineMeshComponent>(Owner,NAME_None,RF_Transient);
        M->SetupAttachment(Owner->GetRootComponent()); M->SetMobility(EComponentMobility::Movable);
        M->SetStaticMesh(Body); M->SetCollisionEnabled(ECollisionEnabled::NoCollision); M->SetCastShadow(false);
        M->SetForwardAxis(ESplineMeshAxis::X,false); M->SetBoundaryMin(0,false); M->SetBoundaryMax(100,false);
        const FVector Direction=(B.P-A.P).GetSafeNormal();
        M->SetSplineUpDir(FMath::Abs(Direction.Z)>.85?FVector::RightVector:FVector::UpVector,false);
        M->SetStartScale(FVector2D(DiameterScale),false); M->SetEndScale(FVector2D(DiameterScale),false);
        M->SetStartAndEnd(A.P,(A.Right-A.P)*3,B.P,(B.P-B.Left)*3,false);
        if(Surface) M->SetMaterial(0,Surface);
        M->RegisterComponent(); M->UpdateMesh();
        if(Surface) M->CreateDynamicMaterialInstance(0,Surface);
        Out.Add(M);
    }
}
