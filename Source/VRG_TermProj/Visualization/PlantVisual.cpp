// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/PlantVisual.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Crc.h"
#include "HAL/IConsoleManager.h"
#include "Visualization/SimMeshBuilder.h"

namespace
{
	// Nominal parts [m] of each kind in its nominal box (origin bottom centre). Display shapes, not product geometry.
	void NominalParts(EPlantVisualKind Kind, TArray<FPlantVisualPart>& Out)
	{
		auto Box = [&Out](double X, double Y, double Z, double SX, double SY, double SZ, EPlantTone T)
		{
			FPlantVisualPart& P = Out.AddDefaulted_GetRef();
			P.A = FVector(X, Y, Z); P.BoxSizeM = FVector(SX, SY, SZ); P.Tone = T;
		};
		auto Cyl = [&Out](const FVector& A, const FVector& B, double D, EPlantTone T)
		{
			FPlantVisualPart& P = Out.AddDefaulted_GetRef();
			P.bCylinder = true; P.A = A; P.B = B; P.DiameterM = D; P.Tone = T;
		};
		switch (Kind)
		{
		case EPlantVisualKind::DryCooler:   // 2.4 x 1.2 x 1.3: legs, base frame, finned coil block, end frames, two top fans
			for (const double X : { -1.1, 1.1 }) for (const double Y : { -0.5, 0.5 }) Box(X, Y, 0.175, 0.1, 0.1, 0.35, EPlantTone::Frame);
			for (const double Y : { -0.5, 0.5 }) Box(0, Y, 0.4, 2.4, 0.1, 0.1, EPlantTone::Frame);
			for (const double X : { -1.1, 1.1 }) Box(X, 0, 0.4, 0.1, 1.0, 0.1, EPlantTone::Frame);
			Box(0, 0, 0.8, 2.2, 0.96, 0.7, EPlantTone::Coil);
			for (int32 K = 0; K < 11; ++K) for (const double Y : { -0.485, 0.485 }) Box(-1.0 + 0.2 * K, Y, 0.8, 0.03, 0.02, 0.66, EPlantTone::Fan);
			for (const double X : { -1.15, 1.15 }) Box(X, 0, 0.8, 0.1, 1.2, 0.8, EPlantTone::Body);
			Box(0, 0, 1.175, 2.4, 1.2, 0.05, EPlantTone::Body);
			for (const double Y : { -0.57, 0.57 }) Box(0, Y, 1.22, 2.4, 0.06, 0.04, EPlantTone::Accent);
			for (const double X : { -0.6, 0.6 })
			{
				Cyl(FVector(X, 0, 1.2), FVector(X, 0, 1.27), 1.0, EPlantTone::Fan);
				Box(X, 0, 1.28, 0.9, 0.07, 0.02, EPlantTone::Pipe);
				Box(X, 0, 1.28, 0.07, 0.9, 0.02, EPlantTone::Pipe);
				Cyl(FVector(X, 0, 1.2), FVector(X, 0, 1.3), 0.16, EPlantTone::Accent);
			}
			for (const double X : { -0.85, 0.85 }) Cyl(FVector(X, -0.47, 0.65), FVector(X, -0.6, 0.65), 0.1, EPlantTone::Pipe);
			break;
		case EPlantVisualKind::Chiller:     // 2.4 x 1.2 x 1.8: skid, evaporator (-Y) and condenser (+Y) shells, compressor, panel (+X)
			Box(0, 0, 0.05, 2.4, 1.2, 0.1, EPlantTone::Frame);
			for (const double X : { -0.7, 0.7 }) for (const double Y : { -0.3, 0.3 }) Box(X, Y, 0.2, 0.15, 0.4, 0.2, EPlantTone::Frame);
			for (const double Y : { -0.3, 0.3 })
			{
				Cyl(FVector(-1.0, Y, 0.55), FVector(0.85, Y, 0.55), 0.5, EPlantTone::Shell);
				Cyl(FVector(-1.08, Y, 0.55), FVector(-1.0, Y, 0.55), 0.52, EPlantTone::Frame);
				Cyl(FVector(0.85, Y, 0.55), FVector(0.93, Y, 0.55), 0.52, EPlantTone::Frame);
			}
			Box(-0.15, 0, 1.1, 0.9, 0.7, 0.5, EPlantTone::Body);
			Box(-0.15, 0, 1.37, 0.9, 0.7, 0.04, EPlantTone::Accent);
			Cyl(FVector(-0.6, 0, 1.1), FVector(-1.05, 0, 1.1), 0.42, EPlantTone::Shell);
			Cyl(FVector(0.55, 0.2, 0.85), FVector(0.55, 0.2, 1.45), 0.3, EPlantTone::Shell);
			Box(1.08, 0, 0.875, 0.22, 1.1, 1.55, EPlantTone::Panel);
			Box(1.19, -0.2, 1.3, 0.02, 0.36, 0.26, EPlantTone::Screen);
			Box(1.19, 0.15, 0.85, 0.02, 0.02, 1.3, EPlantTone::Frame);
			Box(1.08, 0, 1.69, 0.22, 1.1, 0.08, EPlantTone::Accent);
			for (const double X : { -0.75, 0.6 }) for (const double Y : { -1.0, 1.0 })
				Cyl(FVector(X, 0.45 * Y, 0.55), FVector(X, 0.6 * Y, 0.55), 0.12, EPlantTone::Pipe);
			break;
		case EPlantVisualKind::Pump:        // 0.6 cube: base, motor (-X) with fins, coupling, volute and suction flange (+X)
			Box(0, 0, 0.03, 0.6, 0.44, 0.06, EPlantTone::Frame);
			Box(-0.12, 0, 0.1, 0.3, 0.26, 0.08, EPlantTone::Frame);
			Cyl(FVector(-0.3, 0, 0.28), FVector(0.0, 0, 0.28), 0.3, EPlantTone::Body);
			for (const double X : { -0.22, -0.14, -0.06 }) Cyl(FVector(X - 0.01, 0, 0.28), FVector(X + 0.01, 0, 0.28), 0.32, EPlantTone::Frame);
			Cyl(FVector(-0.3, 0, 0.28), FVector(-0.26, 0, 0.28), 0.28, EPlantTone::Fan);
			Cyl(FVector(0.0, 0, 0.28), FVector(0.06, 0, 0.28), 0.1, EPlantTone::Pipe);
			Box(0.13, 0, 0.11, 0.14, 0.2, 0.1, EPlantTone::Frame);
			Cyl(FVector(0.06, 0, 0.3), FVector(0.2, 0, 0.3), 0.4, EPlantTone::Accent);
			Cyl(FVector(0.2, 0, 0.3), FVector(0.3, 0, 0.3), 0.14, EPlantTone::Pipe);
			Cyl(FVector(0.13, 0, 0.5), FVector(0.13, 0, 0.6), 0.12, EPlantTone::Pipe);
			break;
		case EPlantVisualKind::Economizer:  // 0.8 x 0.5 x 1.6: plate pack between frame plates, guide bars, tie bolts, nozzles (+X)
			Box(0, 0, 0.03, 0.8, 0.5, 0.06, EPlantTone::Frame);
			for (const double Y : { -0.215, 0.215 }) Box(0, Y, 0.81, 0.8, 0.07, 1.5, EPlantTone::Body);
			Box(0, 0, 0.81, 0.7, 0.36, 1.3, EPlantTone::Coil);
			for (int32 K = 0; K < 9; ++K) Box(0, -0.16 + 0.04 * K, 0.81, 0.72, 0.008, 1.3, EPlantTone::Fan);
			Cyl(FVector(0, -0.25, 1.56), FVector(0, 0.25, 1.56), 0.06, EPlantTone::Accent);
			Cyl(FVector(0, -0.25, 0.12), FVector(0, 0.25, 0.12), 0.05, EPlantTone::Frame);
			for (const double X : { -0.37, 0.37 }) for (const double Z : { 0.4, 1.2 }) Cyl(FVector(X, -0.25, Z), FVector(X, 0.25, Z), 0.03, EPlantTone::Pipe);
			for (const double Y : { -0.1, 0.1 }) for (const double Z : { 0.35, 1.3 }) Cyl(FVector(0.35, Y, Z), FVector(0.4, Y, Z), 0.08, EPlantTone::Pipe);
			break;
		}
	}

	FVector Scale(const FVector& SizeM, EPlantVisualKind Kind)
	{
		const FVector N = PlantVisual::NominalSizeM(Kind);
		return FVector(SizeM.X > 0 ? SizeM.X / N.X : 1.0, SizeM.Y > 0 ? SizeM.Y / N.Y : 1.0, SizeM.Z > 0 ? SizeM.Z / N.Z : 1.0);
	}

	TAutoConsoleVariable<int32> CVarPlantReferenceModels(TEXT("vrg.Plant.ReferenceModels"), 1, TEXT("Use authored plant exterior meshes; 0 uses the primitive fallback."));

	constexpr double ShapeCm = 100.0;   // engine basic shapes are 100 units, centred
	constexpr int32 ColorSteps = 63;

	uint32 Quantise(float C) { return uint32(FMath::RoundToInt(FMath::Clamp(C, 0.f, 1.f) * ColorSteps)); }
	uint32 ColorKey(const FLinearColor& C) { return (Quantise(C.R) << 12) | (Quantise(C.G) << 6) | Quantise(C.B); }
	FLinearColor KeyColor(uint32 Key)
	{
		return FLinearColor(float((Key >> 12) & 63) / ColorSteps, float((Key >> 6) & 63) / ColorSteps, float(Key & 63) / ColorSteps);
	}
}

FVector PlantVisual::NominalSizeM(EPlantVisualKind Kind)
{
	switch (Kind)
	{
	case EPlantVisualKind::DryCooler: return FVector(2.4, 1.2, 1.3);
	case EPlantVisualKind::Chiller: return FVector(2.4, 1.2, 1.8);
	case EPlantVisualKind::Pump: return FVector(0.6, 0.6, 0.6);
	case EPlantVisualKind::Valve2Way: return FVector(0.3, 0.2, 0.4);
	case EPlantVisualKind::CheckValve: return FVector(0.3, 0.2, 0.2);
	case EPlantVisualKind::LeakMarker: return FVector(0.35, 0.35, 0.05);
	default: return FVector(0.8, 0.5, 1.6);
	}
}

UStaticMesh* PlantVisual::ReferenceMesh(EPlantVisualKind Kind)
{
	if (CVarPlantReferenceModels.GetValueOnGameThread() == 0) return nullptr;
	const TCHAR* Name = nullptr;
	switch (Kind)
	{
	case EPlantVisualKind::DryCooler: Name = TEXT("SM_Plant_DryCooler"); break;
	case EPlantVisualKind::Chiller: Name = TEXT("SM_Plant_Chiller"); break;
	case EPlantVisualKind::Pump: Name = TEXT("SM_Plant_Pump"); break;
	case EPlantVisualKind::Economizer: Name = TEXT("SM_Plant_PlateHx"); break;
	case EPlantVisualKind::Valve2Way: Name = TEXT("SM_Plant_Valve2Way"); break;
	case EPlantVisualKind::CheckValve: Name = TEXT("SM_Plant_CheckValve"); break;
	case EPlantVisualKind::LeakMarker: Name = TEXT("SM_Plant_LeakMarker"); break;
	}
	return Name ? LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Connections/%s.%s"), Name, Name), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
}

void PlantVisual::AppendEquipment(EPlantVisualKind Kind, const FVector& SizeM, const FLinearColor& Accent,
	TArray<FPlantInstance>& Out, const FTransform& Unit)
{
	if (UStaticMesh* Mesh = ReferenceMesh(Kind))
	{
		Out.Add({ Mesh, Accent, FTransform(FQuat::Identity, FVector::ZeroVector, Scale(SizeM, Kind)) * Unit, true });
		return;
	}
	TArray<FPlantVisualPart> Parts;
	BuildParts(Kind, SizeM, Parts);
	for (const FPlantVisualPart& P : Parts)
		Out.Add({ P.bCylinder ? PlantInstances::Cylinder() : PlantInstances::Cube(), ToneColor(P.Tone, Accent), PartTransformCm(P) * Unit });
}

void PlantVisual::BuildParts(EPlantVisualKind Kind, const FVector& SizeM, TArray<FPlantVisualPart>& Out)
{
	Out.Reset();
	NominalParts(Kind, Out);
	const FVector S = Scale(SizeM, Kind);
	for (FPlantVisualPart& P : Out)
	{
		if (!P.bCylinder) { P.A *= S; P.BoxSizeM *= S; continue; }
		// A cylinder keeps a round section: its diameter takes the smaller scale across its axis.
		const FVector D = (P.B - P.A).GetAbs();
		const double Across = D.X >= D.Y && D.X >= D.Z ? FMath::Min(S.Y, S.Z) : D.Y >= D.Z ? FMath::Min(S.X, S.Z) : FMath::Min(S.X, S.Y);
		P.A *= S; P.B *= S; P.DiameterM *= Across;
	}
}

FVector PlantVisual::PortLocalM(EPlantVisualKind Kind, const FVector& SizeM, EPlantPort Port)
{
	FVector P = FVector::ZeroVector;
	const bool bIn = Port == EPlantPort::CoilIn || Port == EPlantPort::CondenserIn;
	const bool bCondenser = Port == EPlantPort::CondenserIn || Port == EPlantPort::CondenserOut;
	if (Kind == EPlantVisualKind::DryCooler) P = FVector(bIn ? -0.85 : 0.85, -0.6, 0.65);
	else if (Kind == EPlantVisualKind::Chiller) P = FVector(bIn ? -0.75 : 0.6, bCondenser ? 0.6 : -0.6, 0.55);
	else if (Kind == EPlantVisualKind::Pump) P = bIn ? FVector(0.3, 0, 0.3) : FVector(0.13, 0, 0.6);
	return P * Scale(SizeM, Kind);
}

FLinearColor PlantVisual::ToneColor(EPlantTone Tone, const FLinearColor& Accent)
{
	switch (Tone)
	{
	case EPlantTone::Body: return FLinearColor(.72f, .74f, .76f);
	case EPlantTone::Frame: return FLinearColor(.17f, .18f, .19f);
	case EPlantTone::Coil: return FLinearColor(.46f, .5f, .48f);
	case EPlantTone::Fan: return FLinearColor(.05f, .055f, .06f);
	case EPlantTone::Pipe: return FLinearColor(.58f, .6f, .62f);
	case EPlantTone::Panel: return FLinearColor(.84f, .85f, .86f);
	case EPlantTone::Screen: return FLinearColor(.05f, .25f, .33f);
	case EPlantTone::Shell: return FLinearColor(.36f, .48f, .6f);
	default: return Accent;
	}
}

FTransform PlantVisual::BoxTransformCm(const FVector& CenterCm, const FVector& SizeCm)
{
	return FTransform(FQuat::Identity, CenterCm, SizeCm / ShapeCm);
}

FTransform PlantVisual::TubeTransformCm(const FVector& StartCm, const FVector& EndCm, double DiameterCm)
{
	const FVector D = EndCm - StartCm;
	const double L = D.Length();
	if (L <= UE_DOUBLE_SMALL_NUMBER) return FTransform(FQuat::Identity, StartCm, FVector::ZeroVector);
	return FTransform(FRotationMatrix::MakeFromZ(D / L).ToQuat(), 0.5 * (StartCm + EndCm), FVector(DiameterCm / ShapeCm, DiameterCm / ShapeCm, L / ShapeCm));
}

FTransform PlantVisual::PartTransformCm(const FPlantVisualPart& P)
{
	return P.bCylinder ? TubeTransformCm(P.A * 100.0, P.B * 100.0, P.DiameterM * 100.0) : BoxTransformCm(P.A * 100.0, P.BoxSizeM * 100.0);
}

UStaticMesh* PlantInstances::Cube() { return SimMeshBuilder::LoadCubeMesh(); }
UStaticMesh* PlantInstances::Cylinder() { return SimMeshBuilder::LoadCylinderMesh(); }

int32 PlantInstances::Apply(AActor* Owner, USceneComponent* Parent, FSets& Sets, const TArray<FPlantInstance>& List, bool bCastShadow)
{
	if (Owner == nullptr || Parent == nullptr) return 0;
	TMap<uint64, TArray<FTransform>> Groups;
	TMap<uint64, UStaticMesh*> Meshes;
	int32 Drawn = 0;
	for (const FPlantInstance& I : List)
	{
		if (I.Mesh == nullptr || I.Transform.GetScale3D().IsNearlyZero()) continue;
		const uint64 Key = (uint64(I.Mesh->GetUniqueID()) << 19) | (uint64(I.bUseMeshMaterials) << 18) | ColorKey(I.Color); // unique mesh, material mode, quantised accent
		Groups.FindOrAdd(Key).Add(I.Transform);
		Meshes.Add(Key, I.Mesh);
				++Drawn;
	}
	for (auto& Pair : Sets)
		if (IsValid(Pair.Value) && !Groups.Contains(Pair.Key) && Pair.Value->GetInstanceCount() > 0) Pair.Value->ClearInstances();
	for (const auto& G : Groups)
	{
		TObjectPtr<UInstancedStaticMeshComponent>* Found = Sets.Find(G.Key);
		UInstancedStaticMeshComponent* C = Found != nullptr && IsValid(*Found) && (*Found)->GetOwner() == Owner ? Found->Get() : nullptr;
		if (C == nullptr)
		{
			C = NewObject<UInstancedStaticMeshComponent>(Owner, MakeUniqueObjectName(Owner, UInstancedStaticMeshComponent::StaticClass(), TEXT("PlantShapes")), RF_Transient);
			C->SetStaticMesh(Meshes[G.Key]);
			C->SetMobility(EComponentMobility::Movable);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(bCastShadow);
			C->SetupAttachment(Parent);
			C->RegisterComponent();
			if ((G.Key & (uint64(1) << 18)) != 0)
			{
				// Keep the equipment's metal / coil / screen materials; recolour the status accents only.
				const auto& Slots = Meshes[G.Key]->GetStaticMaterials();
				for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
					if (Slots[Slot].MaterialSlotName == TEXT("MC_Plant_Accent"))
						if (UMaterialInstanceDynamic* M = C->CreateDynamicMaterialInstance(Slot))
							M->SetVectorParameterValue(TEXT("Color"), KeyColor(uint32(G.Key & 0x3FFFF)));
			}
			else if (UMaterialInterface* Base = SimMeshBuilder::LoadColorableMaterial())
			{
				if (UMaterialInstanceDynamic* M = C->CreateDynamicMaterialInstance(0, Base))
				{
					const FLinearColor Color = KeyColor(uint32(G.Key & 0x3FFFF));
					M->SetVectorParameterValue(TEXT("Color"), Color);
					M->SetVectorParameterValue(TEXT("BaseColor"), Color);
				}
			}
			Sets.Add(G.Key, C);
		}
		C->ClearInstances();
		C->AddInstances(G.Value, false);
	}
	return Drawn;
}

uint32 PlantInstances::Hash(const TArray<FPlantInstance>& List)
{
	uint32 H = GetTypeHash(List.Num());
	for (const FPlantInstance& I : List)
	{
		const FVector L = I.Transform.GetLocation(), S = I.Transform.GetScale3D();
		const FQuat Q = I.Transform.GetRotation();
		const int32 Q8[11] = { FMath::RoundToInt(L.X * 10), FMath::RoundToInt(L.Y * 10), FMath::RoundToInt(L.Z * 10),
			FMath::RoundToInt(S.X * 1e4), FMath::RoundToInt(S.Y * 1e4), FMath::RoundToInt(S.Z * 1e4),
			FMath::RoundToInt(Q.X * 1e5), FMath::RoundToInt(Q.Y * 1e5), FMath::RoundToInt(Q.Z * 1e5), FMath::RoundToInt(Q.W * 1e5), int32(ColorKey(I.Color)) | (I.bUseMeshMaterials ? (1 << 18) : 0) };
		H = FCrc::MemCrc32(Q8, sizeof(Q8), HashCombine(H, GetTypeHash(I.Mesh)));
	}
	return H;
}
