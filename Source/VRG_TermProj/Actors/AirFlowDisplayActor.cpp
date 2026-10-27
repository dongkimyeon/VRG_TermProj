// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/AirFlowDisplayActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"

AAirFlowDisplayActor::AAirFlowDisplayActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AAirFlowDisplayActor::Show(const TArray<FPlantInstance>& List)
{
	NumShown = PlantInstances::Apply(this, RootComponent, Sets, List, false);
}

void AAirFlowDisplayActor::Clear()
{
	if (NumShown == 0) return;
	PlantInstances::Apply(this, RootComponent, Sets, {}, false);
	NumShown = 0;
}
