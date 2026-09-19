// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/SimMeshBuilder.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// 엔진 기본 도형은 원점 중심 100 단위다. 여기서만 쓰는 상수라 밖으로 내지 않는다.
	constexpr double BasicShapeSizeCm = 100.0;

	// BasicShapeMaterial이 노출하는 벡터 파라미터 이름.
	// 다른 머티리얼을 끼워도 깨지지 않게 둘 다 시도한다 (없는 이름은 무시된다).
	const FName ColorParameterNames[] = { FName(TEXT("Color")), FName(TEXT("BaseColor")) };
}

UStaticMesh* SimMeshBuilder::LoadCubeMesh()
{
	static UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	return Cube;
}

UStaticMesh* SimMeshBuilder::LoadCylinderMesh()
{
	static UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	return Cylinder;
}

UMaterialInterface* SimMeshBuilder::LoadColorableMaterial()
{
	static UMaterialInterface* Material =
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	return Material;
}

UStaticMeshComponent* SimMeshBuilder::CreateMeshComponent(AActor* Owner, UStaticMesh* Mesh,
	UMaterialInterface* Material, FName BaseName)
{
	if (Owner == nullptr || Mesh == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* MeshComponent = NewObject<UStaticMeshComponent>(Owner,
		MakeUniqueObjectName(Owner, UStaticMeshComponent::StaticClass(), BaseName));
	if (MeshComponent == nullptr)
	{
		return nullptr;
	}

	MeshComponent->SetupAttachment(Owner->GetRootComponent());
	MeshComponent->SetStaticMesh(Mesh);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetCastShadow(false);
	MeshComponent->RegisterComponent();

	if (Material != nullptr)
	{
		// 컴포넌트마다 다른 색을 넣어야 하므로 동적 인스턴스를 하나씩 만든다.
		MeshComponent->CreateDynamicMaterialInstance(0, Material);
	}

	return MeshComponent;
}

void SimMeshBuilder::SetMeshColor(UStaticMeshComponent* MeshComponent, const FLinearColor& Color)
{
	if (MeshComponent == nullptr)
	{
		return;
	}

	UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(MeshComponent->GetMaterial(0));
	if (DynamicMaterial == nullptr)
	{
		return;
	}

	for (const FName& ParameterName : ColorParameterNames)
	{
		DynamicMaterial->SetVectorParameterValue(ParameterName, Color);
	}
}

void SimMeshBuilder::SetBox(UStaticMeshComponent* MeshComponent, const FVector& CenterCm, const FVector& SizeCm)
{
	if (MeshComponent == nullptr)
	{
		return;
	}

	MeshComponent->SetRelativeLocation(CenterCm);
	MeshComponent->SetRelativeRotation(FRotator::ZeroRotator);
	MeshComponent->SetRelativeScale3D(SizeCm / BasicShapeSizeCm);
}

void SimMeshBuilder::SetTube(UStaticMeshComponent* MeshComponent, const FVector& StartCm,
	const FVector& EndCm, double DiameterCm)
{
	if (MeshComponent == nullptr)
	{
		return;
	}

	const FVector DeltaCm = EndCm - StartCm;
	const double LengthCm = DeltaCm.Length();

	// 길이가 0이면 방향을 정할 수 없다. 숨겨서 퇴화한 관이 보이지 않게 한다.
	if (LengthCm <= UE_DOUBLE_SMALL_NUMBER)
	{
		MeshComponent->SetVisibility(false);
		return;
	}
	MeshComponent->SetVisibility(true);

	// 기본 원통은 Z축을 따라 서 있다. Z를 관 방향에 맞춘다.
	MeshComponent->SetRelativeRotation(FRotationMatrix::MakeFromZ(DeltaCm / LengthCm).Rotator());
	MeshComponent->SetRelativeLocation((StartCm + EndCm) * 0.5);
	MeshComponent->SetRelativeScale3D(FVector(
		DiameterCm / BasicShapeSizeCm,
		DiameterCm / BasicShapeSizeCm,
		LengthCm / BasicShapeSizeCm));
}

FLinearColor SimMeshBuilder::TemperatureToColor(double TempK, double MinTempK, double MaxTempK)
{
	// 빨강은 단조 증가, 파랑은 단조 감소하도록 고른 정지점이다.
	// 중간에서 빨강이 한 번이라도 줄면 두 온도를 눈으로 비교할 때 순서를 뒤집어 읽게 된다.
	static const FLinearColor Stops[] = {
		FLinearColor(0.10f, 0.35f, 0.90f),  // 찬 쪽
		FLinearColor(0.15f, 0.80f, 0.45f),
		FLinearColor(0.95f, 0.80f, 0.20f),
		FLinearColor(1.00f, 0.15f, 0.10f)   // 뜨거운 쪽
	};
	static constexpr int32 NumStops = UE_ARRAY_COUNT(Stops);

	const double SpanK = MaxTempK - MinTempK;
	const double Normalized = SpanK > UE_DOUBLE_SMALL_NUMBER ? (TempK - MinTempK) / SpanK : 0.0;

	const float T = FMath::Clamp(static_cast<float>(Normalized), 0.0f, 1.0f);
	const float Scaled = T * (NumStops - 1);
	const int32 Index0 = FMath::Min(FMath::FloorToInt(Scaled), NumStops - 1);
	const int32 Index1 = FMath::Min(Index0 + 1, NumStops - 1);
	return FMath::Lerp(Stops[Index0], Stops[Index1], Scaled - Index0);
}
