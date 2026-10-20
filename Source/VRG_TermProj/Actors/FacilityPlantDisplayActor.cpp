// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/FacilityPlantDisplayActor.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/SimMeshBuilder.h"

AFacilityPlantDisplayActor::AFacilityPlantDisplayActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFacilityPlantDisplayActor::ShowSnapshot(const FPlantDisplaySnapshot& Snapshot)
{
	Shown = Snapshot;
	if (!bBuilt || Shown.StructureHash != BuiltHash) Rebuild();
	else Recolor();
}

void AFacilityPlantDisplayActor::Rebuild()
{
	for (TArray<TObjectPtr<UStaticMeshComponent>>* Set : { &PipeMeshes, &EquipmentMeshes, &LeakMeshes })
	{
		for (UStaticMeshComponent* C : *Set) if (C != nullptr) C->DestroyComponent();
		Set->Reset();
	}
	PipeMeshOwner.Reset();
	bBuilt = true;
	BuiltHash = Shown.StructureHash;
	++Rebuilds;
	UStaticMesh* Cylinder = SimMeshBuilder::LoadCylinderMesh();
	UStaticMesh* Cube = SimMeshBuilder::LoadCubeMesh();
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();
	if (Cylinder == nullptr || Cube == nullptr) return;
	// Metres -> world cm only here (rule 1).
	for (int32 I = 0; I < Shown.Pipes.Num(); ++I)
	{
		const FPlantDisplayPipe& P = Shown.Pipes[I];
		const double DiameterCm = SimUnits::MetersToCentimeters(P.bHeader ? HeaderDiameterM : BranchDiameterM);
		for (int32 K = 0; K + 1 < P.PointsM.Num(); ++K)
		{
			UStaticMeshComponent* Tube = SimMeshBuilder::CreateMeshComponent(this, Cylinder, Material, TEXT("PlantPipe"));
			SimMeshBuilder::SetTube(Tube, SimUnits::MetersToUnreal(P.PointsM[K]), SimUnits::MetersToUnreal(P.PointsM[K + 1]), DiameterCm);
			PipeMeshes.Add(Tube);
			PipeMeshOwner.Add(I);
		}
	}
	for (const FPlantDisplayEquipment& E : Shown.Equipment)
	{
		UStaticMeshComponent* Box = SimMeshBuilder::CreateMeshComponent(this, Cube, Material, TEXT("PlantEquipment"));
		SimMeshBuilder::SetBox(Box, SimUnits::MetersToUnreal(E.CenterM), SimUnits::MetersToUnreal(E.SizeM));
		EquipmentMeshes.Add(Box);
	}
	for (const FPlantDisplayLeak& L : Shown.Leaks)
	{
		UStaticMeshComponent* Box = SimMeshBuilder::CreateMeshComponent(this, Cube, Material, TEXT("PlantLeak"));
		SimMeshBuilder::SetBox(Box, SimUnits::MetersToUnreal(L.PositionM), SimUnits::MetersToUnreal(FVector(0.35)));
		LeakMeshes.Add(Box);
	}
	Recolor();
}

void AFacilityPlantDisplayActor::Recolor()
{
	static IConsoleVariable* TempMode = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Pipes.Temperature"));
	const bool bTemperature = TempMode != nullptr && TempMode->GetInt() != 0;
	for (int32 M = 0; M < PipeMeshes.Num(); ++M)
	{
		if (Shown.Pipes.IsValidIndex(PipeMeshOwner[M])) SimMeshBuilder::SetMeshColor(PipeMeshes[M], PlantDisplay::PipeColor(Shown, Shown.Pipes[PipeMeshOwner[M]], bTemperature));
	}
	for (int32 E = 0; E < EquipmentMeshes.Num() && E < Shown.Equipment.Num(); ++E) SimMeshBuilder::SetMeshColor(EquipmentMeshes[E], PlantDisplay::EquipmentColor(Shown.Equipment[E]));
	for (UStaticMeshComponent* L : LeakMeshes) SimMeshBuilder::SetMeshColor(L, FLinearColor(1.f, 0.f, 0.f));
}
