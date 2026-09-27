#include "Actors/SpineCabinetActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/GjConnectionMesh.h"
#include "Visualization/GjRackLayout.h"
#include "Visualization/SimMeshBuilder.h"

ASpineCabinetActor::ASpineCabinetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ASpineCabinetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisuals();
}

void ASpineCabinetActor::EnsureThermalModel()
{
	if (ThermalSwitchCount == NumSwitches && ThermalFirstSlot == FirstSwitchSlot) return;
	TArray<double> Heights;
	for (int32 S = 0; S < FMath::Clamp(NumSwitches, 1, 8); ++S) Heights.Add(GetSwitchZM(S));
	const FRackThermalParams Params = FRackThermalModel::MakeQuantumSpineParams(Heights);
	Input = FRackThermalInput();
	Input.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(Params);
	Input.SledDocked.Init(true, Heights.Num());
	Input.CoolantMassFlowKgs = 0.0;
	// Air flow comes from the switch's own fans (QuantumSpineSwitch, sources in RackThermalModel.cpp).
	Model.Initialize(Params, FRackThermalState());
	ThermalSwitchCount = NumSwitches;
	ThermalFirstSlot = FirstSwitchSlot;
}

double ASpineCabinetActor::GetSwitchZM(int32 Switch) const
{
	return GjRackLayout::NodeZ(FirstSwitchSlot + Switch);
}

int32 ASpineCabinetActor::GetNumPorts() const
{
	return NumSwitches * GjRackLayout::PortsPerSwitch;
}

FVector ASpineCabinetActor::GetPortWorldM(int32 Port) const
{
	const int32 Switch = Port / GjRackLayout::PortsPerSwitch, InSwitch = Port % GjRackLayout::PortsPerSwitch;
	const bool bUpper = InSwitch >= GjRackLayout::PortsPerSwitchRow;
	const FVector LocalM = GjRackLayout::SwitchPortM(GetSwitchZM(Switch), InSwitch % GjRackLayout::PortsPerSwitchRow + 1, bUpper);
	// Rule 1: cm only at this actor boundary.
	return SimUnits::UnrealToMeters(GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(LocalM)));
}

void ASpineCabinetActor::RefreshVisuals()
{
	for (UStaticMeshComponent* M : Meshes) if (M) M->DestroyComponent();
	Meshes.Reset();
	UStaticMesh* Frame = GjConnectionMesh::Mesh(TEXT("SM_GB200_RackFrame"), true);
	UStaticMesh* Switch = GjConnectionMesh::Mesh(TEXT("SM_QuantumIB_Switch_Photo"), true);
	if (!Frame || !Switch)
	{
		UE_LOG(LogTemp, Error, TEXT("SpineCabinet: GJ rack assets missing. Run Tools/install_gj_models.py."));
		return;
	}
	Meshes.Add(SimMeshBuilder::CreateMeshComponent(this, Frame, nullptr, TEXT("Spine_Frame")));
	TArray<TPair<double, double>> Occupied;
	for (int32 S = 0; S < NumSwitches; ++S)
	{
		UStaticMeshComponent* C = SimMeshBuilder::CreateMeshComponent(this, Switch, nullptr, TEXT("Spine_Switch"));
		C->SetRelativeLocation(SimUnits::MetersToUnreal(FVector(GjRackLayout::SwitchXM, 0, GetSwitchZM(S))));
		C->SetRelativeRotation(FRotator(0, 180, 0));
		Meshes.Add(C);
		Occupied.Add({GetSwitchZM(S), GetSwitchZM(S) + GjRackLayout::UnitM});
	}
	TArray<TPair<double, double>> Blanks;
	for (const GjRackLayout::FBlankSlot& Slot : GjRackLayout::BlankSlots(Occupied)) Blanks.Add({Slot.BottomM, Slot.HeightM});
	if (UInstancedStaticMeshComponent* Panels = GjConnectionMesh::AddBlankPanels(this, Blanks, FVector::OneVector)) Meshes.Add(Panels);
}
