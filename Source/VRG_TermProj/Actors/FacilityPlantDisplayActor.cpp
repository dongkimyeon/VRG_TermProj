// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/FacilityPlantDisplayActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
#include "Simulation/Core/SimUnits.h"

AFacilityPlantDisplayActor::AFacilityPlantDisplayActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AFacilityPlantDisplayActor::ShowSnapshot(const FPlantDisplaySnapshot& Snapshot)
{
	using namespace PlantDisplayStyle;
	Shown = Snapshot;
	static IConsoleVariable* TempMode = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Pipes.Temperature"));
	const bool bTemperature = TempMode != nullptr && TempMode->GetInt() != 0;
	UStaticMesh* Cube = PlantInstances::Cube();
	UStaticMesh* Cylinder = PlantInstances::Cylinder();
	TArray<FPlantInstance> List;
	// Metres -> world cm only here (rule 1).
	auto Box = [&](const FVector& CenterM, const FVector& SizeM, const FLinearColor& Color)
	{
		List.Add({ Cube, Color, PlantVisual::BoxTransformCm(SimUnits::MetersToUnreal(CenterM), SimUnits::MetersToUnreal(SizeM)) });
	};
	// A polyline: one tube per segment and a cube at every bend so the corners close.
	auto Polyline = [&](const TArray<FVector>& Pts, double DiameterM, const FLinearColor& Color)
	{
		int32 N = 0;
		for (int32 K = 0; K + 1 < Pts.Num(); ++K)
		{
			List.Add({ Cylinder, Color, PlantVisual::TubeTransformCm(SimUnits::MetersToUnreal(Pts[K]), SimUnits::MetersToUnreal(Pts[K + 1]), SimUnits::MetersToCentimeters(DiameterM)) });
			++N;
			if (K > 0) Box(Pts[K], FVector(DiameterM), Color);
		}
		return N;
	};

	PipeSegments = HeaderSegments = EquipmentDrawn = LeakMarkers = 0;
	for (const FPlantDisplayHeader& H : Shown.Headers) HeaderSegments += Polyline(H.PointsM, HeaderDiameterM, PlantDisplay::HeaderColor(Shown, H, bTemperature));
	const FLinearColor Fitting(.17f, .18f, .19f), Shut(.75f, .1f, .1f);
	for (const FPlantDisplayPipe& P : Shown.Pipes)
	{
		PipeSegments += Polyline(P.PointsM, P.bHeader ? MainDiameterM : PipeDiameterM, PlantDisplay::PipeColor(Shown, P, bTemperature));
		if (P.PointsM.Num() < 2) continue;
		// Valve bodies (control valve on its drop, check valve on the pump discharge), red when shut.
		if (P.Role == EPlantPipeRole::Valve) Box(0.5 * (P.PointsM[0] + P.PointsM[1]), FVector(0.18, 0.18, 0.14), P.bClosed ? Shut : Fitting);
		if (P.Role == EPlantPipeRole::CheckValve) Box(0.5 * (P.PointsM.Last(1) + P.PointsM.Last()), FVector(0.16, 0.14, 0.14), P.bClosed ? Shut : Fitting);
	}
	// Units the yard places itself (placed ones are their own actors).
	TArray<FPlantVisualPart> Parts;
	for (const FPlantDisplayEquipment& E : Shown.Equipment)
	{
		if (E.bPlaced) continue;
		const FLinearColor Accent = Shown.bLive ? PlantDisplay::EquipmentColor(E) : PlantDisplay::KindColor(E.Loop, E.Kind);
		const FTransform Unit(E.Rotation, SimUnits::MetersToUnreal(E.CenterM + E.Rotation.RotateVector(FVector(0, 0, -0.5 * E.SizeM.Z))));
		PlantVisual::BuildParts(PlantDisplay::VisualKind(E.Kind), E.SizeM, Parts);
		for (const FPlantVisualPart& Part : Parts)
			List.Add({ Part.bCylinder ? Cylinder : Cube, PlantVisual::ToneColor(Part.Tone, Accent), PlantVisual::PartTransformCm(Part) * Unit });
		++EquipmentDrawn;
	}
	// Supports: trays and crossbeams galvanised grey, posts dark.
	for (const FPlantDisplayTray& T : Shown.Trays) Box(T.CenterM, T.SizeM, FLinearColor(.48f, .5f, .52f));
	for (const FVector& Top : Shown.PostTopsM) Box(FVector(Top.X, Top.Y, 0.5 * Top.Z), FVector(0.08, 0.08, Top.Z), Fitting);
	for (const FPlantDisplayLeak& L : Shown.Leaks) { Box(L.PositionM, FVector(0.35), FLinearColor(1.f, 0.f, 0.f)); ++LeakMarkers; }

	const uint32 Hash = PlantInstances::Hash(List);
	if (bUploaded && Hash == UploadedHash) return;
	PlantInstances::Apply(this, RootComponent, Sets, List, true);
	UploadedHash = Hash;
	bUploaded = true;
	++Uploads;
}
