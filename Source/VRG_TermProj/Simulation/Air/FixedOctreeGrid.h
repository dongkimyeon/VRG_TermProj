// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"

// S5-A: 비정방 균일 도메인의 셀을 루트로 삼는 고정 옥트리 forest.
// 영역을 정육면체로 패딩하지 않는다. 루트별 8분할이라 기존 영역을 정확히 덮는다.
// 랙 영역은 격자 원점 기준 AABB [m]. 액터 회전/UE 좌표 변환은 호출자 책임이다.
struct FFixedOctreeSettings
{
	int32 MaxDepth = 1;
	int32 MaxLeaves = 60000;
};

// 토폴로지만 저장한다. 필드는 별도 SoA 배열이다 (규약 7).
// MortonCode는 최정밀 정수 좌표의 위치 키이며 Depth와 함께 노드를 식별한다.
// D7: 구축용 키다. 이후 해석에서 이웃을 비트 연산으로 찾지 않는다.
struct FFixedOctreeNode
{
	uint64 MortonCode = 0;
	FIntVector MinFine = FIntVector::ZeroValue;
	int32 Depth = 0;
	int32 ParentIndex = INDEX_NONE;
	int32 FirstChildIndex = INDEX_NONE;
	int32 RootCellIndex = INDEX_NONE;
	int32 LeafIndex = INDEX_NONE;
};

struct FFixedOctreeFields
{
	TArray<double> TemperatureK;
	TArray<bool> bSolidCell;
};

class VRG_TERMPROJ_API FFixedOctreeGrid
{
public:
	// 실패 시 기존 결과도 비운다. 예산을 넘긴 부분 격자를 성공으로 반환하지 않는다.
	bool Build(const FAirGridParams& InBaseParams, const FFixedOctreeSettings& InSettings,
		const TArray<FBox>& RefinementBoxesM, FString& OutError);
	void Reset();

	// 동일 균일 격자의 셀 평균값을 자식에 복사한다. 고체도 부모 분류를 상속한다.
	// 온도 복사만 하므로 원본 솔버/시계/속도장은 건드리지 않는다.
	// 실패 시 이전 필드를 비워 오래된 결과가 표시되지 않게 한다.
	bool ImportUniform(const FUniformAirGrid& Source, FString& OutError);
	bool RestrictTemperatureToUniform(TArray<double>& OutTemperatureK) const;
	double TotalThermalEnergyJ(double DatumTempK) const;

	const TArray<FFixedOctreeNode>& GetNodes() const { return Nodes; }
	const TArray<int32>& GetLeafNodeIndices() const { return LeafNodeIndices; }
	const FFixedOctreeFields& GetFields() const { return Fields; }
	const FAirGridParams& GetBaseParams() const { return BaseParams; }
	int32 GetNumLeaves() const { return LeafNodeIndices.Num(); }
	int32 GetMaxDepth() const { return Settings.MaxDepth; }
	bool HasFields() const { return GetNumLeaves() > 0 && Fields.TemperatureK.Num() == GetNumLeaves(); }
	FBox NodeBoundsM(int32 NodeIndex) const;
	double LeafVolumeM3(int32 LeafIndex) const;

private:
	FAirGridParams BaseParams;
	FFixedOctreeSettings Settings;
	TArray<FFixedOctreeNode> Nodes;
	TArray<int32> LeafNodeIndices;
	FFixedOctreeFields Fields;
	int32 PlannedLeaves = 0;

	bool RefineNode(int32 NodeIndex, const TArray<FBox>& BoxesM);
	static uint64 EncodeMorton(const FIntVector& Coord);
};
