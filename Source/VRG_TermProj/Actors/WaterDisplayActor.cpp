// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/WaterDisplayActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/PlantVisual.h"
#include "Visualization/SimMeshBuilder.h"

namespace
{
	UStaticMesh* WaterSphereMesh() { static UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")); return M; }

	// A tinted engine material when the water materials are not installed (tests, a fresh checkout before the script).
	UMaterialInterface* Fallback(UObject* Outer, const FLinearColor& Color)
	{
		UMaterialInterface* Base = SimMeshBuilder::LoadColorableMaterial();
		if (Base == nullptr) return nullptr;
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, Outer);
		M->SetVectorParameterValue(TEXT("Color"), Color);
		M->SetVectorParameterValue(TEXT("BaseColor"), Color);
		return M;
	}

	// The engine cube and sphere are 100 units across, centred.
	FTransform BoxCm(const FVector& CenterM, const FQuat& Rotation, const FVector& SizeM)
	{
		return FTransform(Rotation, SimUnits::MetersToUnreal(CenterM), SimUnits::MetersToUnreal(SizeM) / 100.0);
	}

	void SetInstances(UInstancedStaticMeshComponent* C, const TArray<FTransform>& T)
	{
		if (C == nullptr) return;
		C->ClearInstances();
		if (!T.IsEmpty()) C->AddInstances(T, false, true);
	}

	void ToCm(const FWaterSurfaceMesh& M, TArray<FVector>& V, TArray<FVector>& N, TArray<FVector2D>& UV, TArray<FProcMeshTangent>& Tan)
	{
		V.SetNumUninitialized(M.VerticesM.Num()); N = M.NormalsM; UV.SetNumUninitialized(M.VerticesM.Num());
		Tan.Init(FProcMeshTangent(1.f, 0.f, 0.f), M.VerticesM.Num());
		for (int32 I = 0; I < M.VerticesM.Num(); ++I)
		{
			V[I] = SimUnits::MetersToUnreal(M.VerticesM[I]);
			UV[I] = FVector2D(M.VerticesM[I].X, M.VerticesM[I].Y);   // metres: the material may tile on it
		}
	}
}

AWaterDisplayActor::AWaterDisplayActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AWaterDisplayActor::EnsureComponents()
{
	if (Surface != nullptr) return;
	UMaterialInterface* W = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Water/Materials/M_VrgWater.M_VrgWater"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UMaterialInterface* Wet = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Water/Materials/M_VrgWetSurface.M_VrgWetSurface"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	bWaterMaterial = W != nullptr;
	WaterMaterial = W ? W : Fallback(this, FLinearColor(.15f, .45f, .9f));
	WetMaterial = Wet ? Wet : Fallback(this, FLinearColor(.05f, .06f, .08f));

	Surface = NewObject<UProceduralMeshComponent>(this, TEXT("WaterSurface"));
	Surface->SetupAttachment(RootComponent);
	Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Surface->SetCastShadow(false);
	Surface->bUseAsyncCooking = true;
	Surface->RegisterComponent();
	auto MakeSet = [&](const TCHAR* Name, UStaticMesh* Mesh, UMaterialInterface* Material)
	{
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this, Name);
		C->SetupAttachment(RootComponent);
		C->SetStaticMesh(Mesh);
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->RegisterComponent();
		if (Material) C->SetMaterial(0, Material);
		return C;
	};
	// Engine shapes carry white vertex colour, which the water material reads as deep water: drops, jets and films are
	// thin, so their instance material is tinted back to the shallow colour and made less opaque (display choice).
	UMaterialInterface* Thin = WaterMaterial;
	if (bWaterMaterial)
	{
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(WaterMaterial, this);
		M->SetVectorParameterValue(TEXT("Tint"), FLinearColor(8.f, 2.8f, 2.2f));   // deep (0.05, 0.25, 0.35) -> about the shallow tint
		M->SetScalarParameterValue(TEXT("OpacityScale"), .55f);
		Thin = M;
	}
	Drops = MakeSet(TEXT("WaterDrops"), WaterSphereMesh(), Thin);
	Streaks = MakeSet(TEXT("WaterStreaks"), PlantInstances::Cylinder(), Thin);
	Sheets = MakeSet(TEXT("WaterSheets"), PlantInstances::Cube(), Thin);
	Skins = MakeSet(TEXT("WetSkins"), PlantInstances::Cube(), WetMaterial);
}

void AWaterDisplayActor::Show(const FWaterDisplaySnapshot& S, bool bFloor)
{
	EnsureComponents();
	if (bFloor)
	{
		TArray<FVector> V, N; TArray<FVector2D> UV; TArray<FProcMeshTangent> Tan;
		if (S.Surface.Triangles.IsEmpty()) { if (bSurfaceShown) Surface->ClearMeshSection(0); bSurfaceShown = false; }
		else
		{
			ToCm(S.Surface, V, N, UV, Tan);
			Surface->CreateMeshSection_LinearColor(0, V, S.Surface.Triangles, N, UV, S.Surface.Colors, Tan, false);
			Surface->SetMaterial(0, WaterMaterial);
			bSurfaceShown = true;
		}
		if (S.WetFloor.Triangles.IsEmpty()) { if (bWetShown) Surface->ClearMeshSection(1); bWetShown = false; }
		else
		{
			ToCm(S.WetFloor, V, N, UV, Tan);
			Surface->CreateMeshSection_LinearColor(1, V, S.WetFloor.Triangles, N, UV, S.WetFloor.Colors, Tan, false);
			Surface->SetMaterial(1, WetMaterial);
			bWetShown = true;
		}
		if (S.YardSurface.Triangles.IsEmpty()) { if (bYardShown) Surface->ClearMeshSection(2); bYardShown = false; }
		else
		{
			ToCm(S.YardSurface, V, N, UV, Tan);
			Surface->CreateMeshSection_LinearColor(2, V, S.YardSurface.Triangles, N, UV, S.YardSurface.Colors, Tan, false);
			Surface->SetMaterial(2, WaterMaterial);
			bYardShown = true;
		}
		TArray<FTransform> T;
		for (const FWaterSheet& Sh : S.Sheets) T.Add(BoxCm(Sh.CenterM, Sh.Rotation, Sh.SizeM));
		SetInstances(Sheets, T);
		T.Reset();
		for (const FWetSkin& Sk : S.Skins) T.Add(BoxCm(Sk.CenterM, Sk.Rotation, Sk.SizeM));
		SetInstances(Skins, T);
	}
	TArray<FTransform> T;
	T.Reserve(S.Drops.Num());
	for (const FWaterDropShape& D : S.Drops) T.Add(BoxCm(D.CenterM, FQuat::Identity, FVector(D.DiameterM)));
	SetInstances(Drops, T);
	T.Reset();
	for (const FWaterStreakShape& St : S.Streaks)
		T.Add(PlantVisual::TubeTransformCm(SimUnits::MetersToUnreal(St.FromM), SimUnits::MetersToUnreal(St.ToM), SimUnits::MetersToUnreal(FVector(St.DiameterM, 0, 0)).X));
	SetInstances(Streaks, T);
	NumInstances = Drops->GetInstanceCount() + Streaks->GetInstanceCount() + Sheets->GetInstanceCount() + Skins->GetInstanceCount();
}

void AWaterDisplayActor::Clear()
{
	if (Surface == nullptr) return;
	Surface->ClearAllMeshSections();
	bSurfaceShown = bWetShown = bYardShown = false;
	for (UInstancedStaticMeshComponent* C : { Drops.Get(), Streaks.Get(), Sheets.Get(), Skins.Get() }) if (C) C->ClearInstances();
	NumInstances = 0;
}
