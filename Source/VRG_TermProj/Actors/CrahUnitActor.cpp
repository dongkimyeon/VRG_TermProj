// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/CrahUnitActor.h"
#include "Visualization/CrahVisualModel.h"

#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACrahUnitActor::ACrahUnitActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Referenced from the CDO so the cooker packages it.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	Body->SetStaticMesh(Cube.Object);

	SupplyArrow = CreateDefaultSubobject<UArrowComponent>(TEXT("SupplyArrow"));
	SupplyArrow->SetupAttachment(RootComponent);
	SupplyArrow->ArrowColor = FColor(80, 170, 255);
	SupplyArrow->ArrowSize = 2.0f;
}

void ACrahUnitActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisuals();
}

void ACrahUnitActor::RefreshVisuals()
{
	const bool bPerimeter = Kind == ECrahUnitKind::Perimeter;
	const auto Visual = CrahVisualModel::Resolve(SizeM, bPerimeter,
		bUseReferenceModel ? CrahVisualModel::LoadReference(bPerimeter) : nullptr,
		LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Body->SetStaticMesh(Visual.Mesh);
	Body->SetRelativeLocation(Visual.OffsetCm);
	Body->SetRelativeScale3D(Visual.Scale);
	// The arrow leaves the supply face: the front for both kinds, low for a perimeter unit.
	const double ArrowZCm = Kind == ECrahUnitKind::Perimeter ? SizeM.Z * 25.0 : SizeM.Z * 50.0;
	SupplyArrow->SetRelativeLocation(FVector(SizeM.X * 50.0, 0.0, ArrowZCm));
}

FCrahLayoutSettings ACrahUnitActor::CaptureLayoutSettings() const
{
	FCrahLayoutSettings S; S.Kind = Kind; S.SizeM = SizeM; S.VolumeFlowM3s = VolumeFlowM3s; return S;
}
void ACrahUnitActor::ApplyLayoutSettings(const FCrahLayoutSettings& S)
{
	Kind = S.Kind; SizeM = S.SizeM; VolumeFlowM3s = S.VolumeFlowM3s;
}
