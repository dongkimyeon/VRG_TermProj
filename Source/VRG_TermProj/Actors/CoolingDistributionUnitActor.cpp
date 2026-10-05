// Fill out your copyright notice in the Description page of Project Settings.

#include "Actors/CoolingDistributionUnitActor.h"

#include "Simulation/Core/SimUnits.h"
#include "Visualization/SimMeshBuilder.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

namespace
{
	const FLinearColor CduBodyColor(0.12f, 0.13f, 0.15f);
	const FLinearColor CduTrimColor(0.85f, 0.65f, 0.15f);

	// 포트 색은 배관 색과 맞춘다. 공급은 파랑, 회수는 빨강이다.
	const FLinearColor SupplyPortColor(0.20f, 0.55f, 0.95f);
	const FLinearColor ReturnPortColor(0.90f, 0.30f, 0.25f);
}

ACoolingDistributionUnitActor::ACoolingDistributionUnitActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

#if WITH_EDITOR
void ACoolingDistributionUnitActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bMeshesDirty = true;
}
#endif

void ACoolingDistributionUnitActor::SetPumpSpeedRatio(double InSpeedRatio)
{
	PumpSpeedRatio = FMath::Clamp(InSpeedRatio, 0.0, 1.5);
}

FVector ACoolingDistributionUnitActor::GetSupplyPortWorldM() const
{
	// 포트는 앞면(+X)에 붙는다. 액터를 돌리면 포트도 같이 돈다.
	const FVector LocalM(
		SizeM.X * 0.5,
		-ConnectionSpacingM * 0.5,
		ConnectionHeightM);

	return SimUnits::UnrealToMeters(GetActorLocation())
		+ GetActorRotation().RotateVector(LocalM);
}

FVector ACoolingDistributionUnitActor::GetReturnPortWorldM() const
{
	const FVector LocalM(
		SizeM.X * 0.5,
		ConnectionSpacingM * 0.5,
		ConnectionHeightM);

	return SimUnits::UnrealToMeters(GetActorLocation())
		+ GetActorRotation().RotateVector(LocalM);
}

void ACoolingDistributionUnitActor::RebuildMeshes()
{
	for (UStaticMeshComponent* MeshComponent : BodyMeshes)
	{
		if (MeshComponent != nullptr)
		{
			MeshComponent->DestroyComponent();
		}
	}
	BodyMeshes.Reset();

	UStaticMesh* CubeMesh = SimMeshBuilder::LoadCubeMesh();
	UStaticMesh* CylinderMesh = SimMeshBuilder::LoadCylinderMesh();
	if (CubeMesh == nullptr || CylinderMesh == nullptr)
	{
		return;
	}
	UMaterialInterface* Material = SimMeshBuilder::LoadColorableMaterial();

	// 길이 단위 변환은 여기서만 한다 (규약 1).
	const FVector SizeCm = SimUnits::MetersToUnreal(SizeM);

	auto AddBox = [&](const FVector& CenterCm, const FVector& BoxSizeCm, const FLinearColor& Color)
	{
		UStaticMeshComponent* Box = SimMeshBuilder::CreateMeshComponent(this, CubeMesh, Material, TEXT("Cdu"));
		SimMeshBuilder::SetBox(Box, CenterCm, BoxSizeCm);
		SimMeshBuilder::SetMeshColor(Box, Color);
		BodyMeshes.Add(Box);
	};

	// The authored mesh uses +X front, a floor-centre pivot and baked centimetres.
	// Match the existing actor envelope; hydraulic ports and inputs remain independent.
	UStaticMesh* ReferenceMesh = bUseReferenceModel
		? LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Connections/SM_CDU_Reference.SM_CDU_Reference"))
		: nullptr;
	if (ReferenceMesh)
	{
		UStaticMeshComponent* Body = SimMeshBuilder::CreateMeshComponent(this, ReferenceMesh, nullptr, TEXT("CduReference"));
		Body->SetRelativeScale3D(FVector(SizeM.X / 0.9, SizeM.Y / 0.8, SizeM.Z / 1.9));
		BodyMeshes.Add(Body);
	}
	else
	{
		AddBox(FVector(0.0, 0.0, SizeCm.Z * 0.5), SizeCm, CduBodyColor);
		AddBox(FVector(SizeCm.X * 0.5, 0.0, SizeCm.Z * 0.82),
			FVector(2.0, SizeCm.Y * 0.8, SizeCm.Z * 0.06), CduTrimColor);
	}

	// 포트 두 개. 배관이 붙는 자리를 눈으로 알 수 있게 짧은 관으로 그린다.
	const double PortDiameterCm = 12.0;
	const double PortLengthCm = 14.0;
	const double PortXCm = SizeCm.X * 0.5;
	const double PortZCm = SimUnits::MetersToCentimeters(ConnectionHeightM);
	const double PortYCm = SimUnits::MetersToCentimeters(ConnectionSpacingM) * 0.5;

	auto AddPort = [&](double YCm, const FLinearColor& Color)
	{
		UStaticMeshComponent* Port =
			SimMeshBuilder::CreateMeshComponent(this, CylinderMesh, Material, TEXT("CduPort"));
		SimMeshBuilder::SetTube(Port,
			FVector(PortXCm - PortLengthCm, YCm, PortZCm),
			FVector(PortXCm, YCm, PortZCm),
			PortDiameterCm);
		SimMeshBuilder::SetMeshColor(Port, Color);
		BodyMeshes.Add(Port);
	};

	AddPort(-PortYCm, SupplyPortColor);
	AddPort(PortYCm, ReturnPortColor);
}

void ACoolingDistributionUnitActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bMeshesDirty)
	{
		RebuildMeshes();
		bMeshesDirty = false;
	}
}

FCduLayoutSettings ACoolingDistributionUnitActor::CaptureLayoutSettings() const
{
	FCduLayoutSettings S;
	S.SizeM = SizeM;
	S.ConnectionHeightM = ConnectionHeightM;
	S.ConnectionSpacingM = ConnectionSpacingM;
	S.bUseReferenceModel = bUseReferenceModel;
	S.ReferencePressurePa = ReferencePressurePa;
	S.PumpShutoffHeadPa = PumpShutoffHeadPa;
	S.PumpCurveCoeffPaS2M6 = PumpCurveCoeffPaS2M6;
	S.PumpEfficiency = PumpEfficiency;
	S.PumpSpeedRatio = PumpSpeedRatio;
	S.FacilitySupplyTempK = FacilitySupplyTempK;
	S.FacilityMassFlowKgs = FacilityMassFlowKgs;
	S.HxUAWK = HxUAWK;
	S.FacilityDesignRiseK = FacilityDesignRiseK;
	S.HxDesignApproachK = HxDesignApproachK;
	return S;
}

void ACoolingDistributionUnitActor::ApplyLayoutSettings(const FCduLayoutSettings& S)
{
	SizeM = S.SizeM;
	ConnectionHeightM = S.ConnectionHeightM;
	ConnectionSpacingM = S.ConnectionSpacingM;
	bUseReferenceModel = S.bUseReferenceModel;
	ReferencePressurePa = S.ReferencePressurePa;
	PumpShutoffHeadPa = S.PumpShutoffHeadPa;
	PumpCurveCoeffPaS2M6 = S.PumpCurveCoeffPaS2M6;
	PumpEfficiency = S.PumpEfficiency;
	PumpSpeedRatio = S.PumpSpeedRatio;
	FacilitySupplyTempK = S.FacilitySupplyTempK;
	FacilityMassFlowKgs = S.FacilityMassFlowKgs;
	HxUAWK = S.HxUAWK;
	FacilityDesignRiseK = S.FacilityDesignRiseK;
	HxDesignApproachK = S.HxDesignApproachK;
	bMeshesDirty = true;
}

bool FCduLayoutSettings::operator==(const FCduLayoutSettings& Other) const
{
	return SizeM == Other.SizeM && ConnectionHeightM == Other.ConnectionHeightM && ConnectionSpacingM == Other.ConnectionSpacingM && bUseReferenceModel == Other.bUseReferenceModel && ReferencePressurePa == Other.ReferencePressurePa && PumpShutoffHeadPa == Other.PumpShutoffHeadPa && PumpCurveCoeffPaS2M6 == Other.PumpCurveCoeffPaS2M6 && PumpEfficiency == Other.PumpEfficiency && PumpSpeedRatio == Other.PumpSpeedRatio && FacilitySupplyTempK == Other.FacilitySupplyTempK && FacilityMassFlowKgs == Other.FacilityMassFlowKgs && HxUAWK == Other.HxUAWK && FacilityDesignRiseK == Other.FacilityDesignRiseK && HxDesignApproachK == Other.HxDesignApproachK;
}

bool FCduLayoutSettings::IsValid() const
{
	// Admissibility and existing actor authoring limits; no new hardware rating or fitted physics.
	return !SizeM.ContainsNaN() && SizeM.X > 0 && SizeM.Y > 0 && SizeM.Z > 0
		&& FMath::IsFinite(ConnectionHeightM)
		&& FMath::IsFinite(ConnectionSpacingM)
		&& FMath::IsFinite(ReferencePressurePa)
		&& FMath::IsFinite(PumpShutoffHeadPa)
		&& FMath::IsFinite(PumpCurveCoeffPaS2M6)
		&& FMath::IsFinite(PumpEfficiency)
		&& FMath::IsFinite(PumpSpeedRatio)
		&& FMath::IsFinite(FacilitySupplyTempK)
		&& FMath::IsFinite(FacilityMassFlowKgs)
		&& FMath::IsFinite(HxUAWK)
		&& FMath::IsFinite(FacilityDesignRiseK)
		&& FMath::IsFinite(HxDesignApproachK)
		&& ConnectionHeightM >= 0 && ConnectionSpacingM >= 0 && ReferencePressurePa >= 0
		&& PumpShutoffHeadPa >= 0 && PumpCurveCoeffPaS2M6 >= 0 && PumpEfficiency >= .01 && PumpEfficiency <= 1
		&& PumpSpeedRatio >= 0 && PumpSpeedRatio <= 1.5 && FacilitySupplyTempK > 0
		&& FacilityMassFlowKgs >= 0 && HxUAWK >= 0 && FacilityDesignRiseK >= 1 && HxDesignApproachK >= .5;
}
