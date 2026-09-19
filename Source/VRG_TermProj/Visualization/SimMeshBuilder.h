// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

// 계산 결과를 스태틱 메쉬로 보여줄 때 쓰는 공용 도구.
// 랙 액터와 배관 액터가 같은 일(박스/원통 만들기, 색 입히기)을 하므로 여기에 모았다.
//
// 여기 있는 함수는 전부 게임 스레드 전용이다. 액터와 UObject를 직접 만진다.
// 워커 스레드에서 부르면 안 된다 (규약 5).
//
// 길이는 전부 UE 월드 단위(cm)로 받는다. m -> cm 변환은 부르는 쪽이 SimUnits로 한다 (규약 1).
namespace SimMeshBuilder
{
	// 엔진 기본 도형. 둘 다 원점 중심, 한 변/높이 100 단위다.
	VRG_TERMPROJ_API UStaticMesh* LoadCubeMesh();
	VRG_TERMPROJ_API UStaticMesh* LoadCylinderMesh();

	// 색 파라미터를 가진 엔진 기본 머티리얼. 없으면 nullptr.
	VRG_TERMPROJ_API UMaterialInterface* LoadColorableMaterial();

	// Owner에 붙은 스태틱 메쉬 컴포넌트를 새로 만들어 등록한다.
	// 충돌과 그림자를 꺼서 시각화 비용을 낮춘다.
	VRG_TERMPROJ_API UStaticMeshComponent* CreateMeshComponent(AActor* Owner, UStaticMesh* Mesh,
		UMaterialInterface* Material, FName BaseName);

	// 동적 머티리얼 인스턴스에 색을 넣는다.
	// 파라미터 이름을 모르는 머티리얼이면 아무 일도 일어나지 않는다 (회색으로 보인다).
	VRG_TERMPROJ_API void SetMeshColor(UStaticMeshComponent* MeshComponent, const FLinearColor& Color);

	// 원점 중심 100 단위 큐브를 주어진 크기의 상자로 만든다.
	VRG_TERMPROJ_API void SetBox(UStaticMeshComponent* MeshComponent, const FVector& CenterCm, const FVector& SizeCm);

	// 원점 중심 100 단위 원통을 두 점을 잇는 관으로 만든다.
	VRG_TERMPROJ_API void SetTube(UStaticMeshComponent* MeshComponent, const FVector& StartCm,
		const FVector& EndCm, double DiameterCm);

	// 온도 색상표 [K]. 청 -> 녹 -> 황 -> 적.
	//
	// 부품처럼 떨어진 대상의 온도를 읽는 용도다.
	// 공기 격자의 연속 온도장은 Simulation/Air 쪽에서 따로 색을 정한다
	// (연속장과 이산 대상은 눈으로 구분돼야 한다).
	VRG_TERMPROJ_API FLinearColor TemperatureToColor(double TempK, double MinTempK, double MaxTempK);
}
