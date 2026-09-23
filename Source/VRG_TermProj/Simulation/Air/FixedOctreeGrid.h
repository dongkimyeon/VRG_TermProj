// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Air/UniformAirGrid.h"

// S5-A: 비정방 균일 도메인의 셀을 루트로 삼는 고정 옥트리 forest.
// 영역을 정육면체로 패딩하지 않는다. 루트별 8분할이라 기존 영역을 정확히 덮는다.
// 랙 영역은 격자 원점 기준 AABB [m]. 액터 회전/UE 좌표 변환은 호출자 책임이다.
//
// S5-B: 여기에 2:1 밸런스와 면·이웃 인덱스 배열을 더한다.
// D7대로 Morton 코드는 구축용이고, 솔버는 아래 Faces/LeafFaceIndices만 읽는다.
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

// S5-B: 비균일 유한체적의 면 하나.
//
// 면은 축에 수직이고 LeafLow가 축 음의 방향, LeafHigh가 양의 방향 쪽이다.
// 둘 중 하나가 INDEX_NONE이면 도메인 바깥 벽이다 (Neumann이라 플럭스가 0).
//
// 규약 6: 면마다 플럭스를 한 번 계산해 양쪽에 부호만 바꿔 적용한다.
// 그래서 면은 전체 목록에 정확히 한 번만 들어가고, 양쪽 리프의 인덱스 목록이
// 같은 면 번호를 가리킨다. 양쪽에서 따로 계산하면 보존이 깨진다.
//
// 비정합 면(깊이가 다른 두 리프 사이)의 AreaM2는 **잘은 쪽** 면적이고
// DistanceM은 두 셀 중심의 축방향 거리다. 굵은 셀 중심이 면 중심에서 벗어나 있어
// 이 두점 플럭스 근사(TPFA)는 비정합 면에서 1차 정확도다. 균일 구간은 2차다.
struct FFixedOctreeFace
{
	int32 LeafLow = INDEX_NONE;
	int32 LeafHigh = INDEX_NONE;
	int32 Axis = 0;
	double AreaM2 = 0.0;
	double DistanceM = 0.0;
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
	// 박스 세분 뒤 2:1 밸런스를 강제하므로 리프 수가 요청 영역보다 늘어날 수 있다.
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
	FFixedOctreeFields& GetMutableFields() { return Fields; }
	const FAirGridParams& GetBaseParams() const { return BaseParams; }
	int32 GetNumLeaves() const { return LeafNodeIndices.Num(); }
	int32 GetMaxDepth() const { return Settings.MaxDepth; }
	bool HasFields() const { return GetNumLeaves() > 0 && Fields.TemperatureK.Num() == GetNumLeaves(); }
	FBox NodeBoundsM(int32 NodeIndex) const;
	double LeafVolumeM3(int32 LeafIndex) const;

	// ---- S5-B: 면·이웃 인덱스 (D7) ----
	const TArray<FFixedOctreeFace>& GetFaces() const { return Faces; }

	// CSR. 리프 L의 면은 LeafFaceIndices[LeafFaceStart[L] .. LeafFaceStart[L+1])다.
	const TArray<int32>& GetLeafFaceStart() const { return LeafFaceStart; }
	const TArray<int32>& GetLeafFaceIndices() const { return LeafFaceIndices; }

	// 2:1 밸런스 때문에 추가로 쪼갠 노드 수. 요청 영역 밖에서 늘어난 양이다.
	int32 GetNumBalanceSplits() const { return NumBalanceSplits; }

	// 깊이가 다른 두 리프를 잇는 면의 수. 0이면 사실상 균일 격자다.
	int32 GetNumHangingFaces() const { return NumHangingFaces; }

	double LeafSizeM(int32 LeafIndex) const;
	FVector LeafCenterM(int32 LeafIndex) const;
	int32 LeafDepth(int32 LeafIndex) const { return Nodes[LeafNodeIndices[LeafIndex]].Depth; }

	// 고체 표시가 없으면(온도 전송 전) 전부 공기로 본다.
	FORCEINLINE bool IsLeafFluid(int32 LeafIndex) const
	{
		return !Fields.bSolidCell.IsValidIndex(LeafIndex) || !Fields.bSolidCell[LeafIndex];
	}

private:
	FAirGridParams BaseParams;
	FFixedOctreeSettings Settings;
	TArray<FFixedOctreeNode> Nodes;
	TArray<int32> LeafNodeIndices;
	FFixedOctreeFields Fields;
	TArray<FFixedOctreeFace> Faces;
	TArray<int32> LeafFaceStart;
	TArray<int32> LeafFaceIndices;
	int32 PlannedLeaves = 0;
	int32 NumBalanceSplits = 0;
	int32 NumHangingFaces = 0;

	// 구축 중에만 쓰는 (깊이, Morton) -> 노드 조회표. Build 끝에 비운다.
	// 남겨 두면 솔버가 이것으로 이웃을 찾고 싶어지는데, 그건 D7 위반이다.
	TMap<uint64, int32> NodeLookup;

	bool RefineNode(int32 NodeIndex, const TArray<FBox>& BoxesM);
	bool SplitNode(int32 NodeIndex);
	bool BalanceTree();
	bool HasFinerFaceNeighbor(int32 NodeIndex, int32 Axis, int32 Sign) const;
	void CollectLeaves();
	void BuildFaces();
	FIntVector GetFineCount() const;
	static uint64 EncodeMorton(const FIntVector& Coord);
	static uint64 NodeKey(int32 Depth, uint64 MortonCode)
	{
		// MinFine은 256셀 x 깊이 6 = 16384 미만이라 Morton이 42비트를 넘지 않는다.
		return (uint64(Depth) << 48) | MortonCode;
	}
};
