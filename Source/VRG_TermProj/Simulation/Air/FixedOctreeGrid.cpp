// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/FixedOctreeGrid.h"

namespace
{
	bool OctreePositiveOverlap(const FBox& A, const FBox& B)
	{
		// 면만 닿은 셀은 세분하지 않는다. 영역 밖 랙이 경계 셀을 잘게 만들지 않게 한다.
		return A.Min.X < B.Max.X && B.Min.X < A.Max.X
			&& A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y
			&& A.Min.Z < B.Max.Z && B.Min.Z < A.Max.Z;
	}
}

void FFixedOctreeGrid::Reset()
{
	Nodes.Reset();
	LeafNodeIndices.Reset();
	Fields.TemperatureK.Reset();
	Fields.bSolidCell.Reset();
	Faces.Reset();
	LeafFaceStart.Reset();
	LeafFaceIndices.Reset();
	NodeLookup.Reset();
	PlannedLeaves = 0;
	NumBalanceSplits = 0;
	NumHangingFaces = 0;
}

uint64 FFixedOctreeGrid::EncodeMorton(const FIntVector& Coord)
{
	uint64 Code = 0;
	for (int32 Bit = 0; Bit < 21; ++Bit)
	{
		Code |= uint64((Coord.X >> Bit) & 1) << (3 * Bit);
		Code |= uint64((Coord.Y >> Bit) & 1) << (3 * Bit + 1);
		Code |= uint64((Coord.Z >> Bit) & 1) << (3 * Bit + 2);
	}
	return Code;
}

FIntVector FFixedOctreeGrid::GetFineCount() const
{
	const int32 RootWidth = 1 << Settings.MaxDepth;
	return FIntVector(BaseParams.NumCellsX * RootWidth,
		BaseParams.NumCellsY * RootWidth, BaseParams.NumCellsZ * RootWidth);
}

FBox FFixedOctreeGrid::NodeBoundsM(int32 NodeIndex) const
{
	const FFixedOctreeNode& Node = Nodes[NodeIndex];
	const double FineSizeM = BaseParams.CellSizeM / double(1 << Settings.MaxDepth);
	const FVector MinM = FVector(Node.MinFine) * FineSizeM;
	const double SizeM = BaseParams.CellSizeM / double(1 << Node.Depth);
	return FBox(MinM, MinM + FVector(SizeM));
}

double FFixedOctreeGrid::LeafSizeM(int32 LeafIndex) const
{
	return BaseParams.CellSizeM / double(1 << Nodes[LeafNodeIndices[LeafIndex]].Depth);
}

FVector FFixedOctreeGrid::LeafCenterM(int32 LeafIndex) const
{
	return NodeBoundsM(LeafNodeIndices[LeafIndex]).GetCenter();
}

double FFixedOctreeGrid::LeafVolumeM3(int32 LeafIndex) const
{
	const double SizeM = LeafSizeM(LeafIndex);
	return SizeM * SizeM * SizeM;
}

bool FFixedOctreeGrid::SplitNode(int32 NodeIndex)
{
	// 리프 하나가 8개로 바뀌므로 리프 수는 7 늘어난다.
	if (PlannedLeaves > Settings.MaxLeaves - 7)
	{
		return false;
	}
	PlannedLeaves += 7;

	// AddDefaulted가 배열을 재할당할 수 있으므로 부모 참조를 잡아 두지 않는다.
	const FFixedOctreeNode Parent = Nodes[NodeIndex];
	const int32 FirstChild = Nodes.AddDefaulted(8);
	Nodes[NodeIndex].FirstChildIndex = FirstChild;
	const int32 ChildWidth = 1 << (Settings.MaxDepth - Parent.Depth - 1);
	for (int32 Child = 0; Child < 8; ++Child)
	{
		FFixedOctreeNode& Node = Nodes[FirstChild + Child];
		Node.MinFine = Parent.MinFine + FIntVector(Child & 1, (Child >> 1) & 1, (Child >> 2) & 1) * ChildWidth;
		Node.MortonCode = EncodeMorton(Node.MinFine);
		Node.Depth = Parent.Depth + 1;
		Node.ParentIndex = NodeIndex;
		Node.RootCellIndex = Parent.RootCellIndex;
		NodeLookup.Add(NodeKey(Node.Depth, Node.MortonCode), FirstChild + Child);
	}
	return true;
}

bool FFixedOctreeGrid::RefineNode(int32 NodeIndex, const TArray<FBox>& BoxesM)
{
	if (Nodes[NodeIndex].Depth >= Settings.MaxDepth)
	{
		return true;
	}
	const FBox Bounds = NodeBoundsM(NodeIndex);
	bool bRefine = false;
	for (const FBox& Box : BoxesM)
	{
		if (OctreePositiveOverlap(Bounds, Box))
		{
			bRefine = true;
			break;
		}
	}
	if (!bRefine)
	{
		return true;
	}
	if (!SplitNode(NodeIndex))
	{
		return false;
	}
	const int32 FirstChild = Nodes[NodeIndex].FirstChildIndex;
	for (int32 Child = 0; Child < 8; ++Child)
	{
		if (!RefineNode(FirstChild + Child, BoxesM))
		{
			return false;
		}
	}
	return true;
}

bool FFixedOctreeGrid::HasFinerFaceNeighbor(int32 NodeIndex, int32 Axis, int32 Sign) const
{
	const FFixedOctreeNode& Node = Nodes[NodeIndex];
	const int32 Width = 1 << (Settings.MaxDepth - Node.Depth);
	FIntVector Coord = Node.MinFine;
	Coord[Axis] += Sign * Width;
	const FIntVector FineCount = GetFineCount();
	if (Coord[Axis] < 0 || Coord[Axis] >= FineCount[Axis])
	{
		return false;
	}
	const int32* Found = NodeLookup.Find(NodeKey(Node.Depth, EncodeMorton(Coord)));
	if (Found == nullptr || Nodes[*Found].FirstChildIndex == INDEX_NONE)
	{
		// 같은 깊이에 노드가 없으면 이웃이 더 굵다. 리프면 깊이가 같다. 둘 다 위반이 아니다.
		return false;
	}
	// 이웃이 한 단계 잘다. 맞닿은 네 자식 중 하나라도 또 쪼개져 있으면 깊이 차가 2다.
	const int32 TouchBit = Sign > 0 ? 0 : 1;
	const int32 FirstChild = Nodes[*Found].FirstChildIndex;
	for (int32 Child = 0; Child < 8; ++Child)
	{
		if (((Child >> Axis) & 1) != TouchBit)
		{
			continue;
		}
		if (Nodes[FirstChild + Child].FirstChildIndex != INDEX_NONE)
		{
			return true;
		}
	}
	return false;
}

bool FFixedOctreeGrid::BalanceTree()
{
	// 2:1 밸런스. 면을 맞댄 두 리프의 깊이 차를 1 이하로 만든다.
	// 이게 없으면 굵은 셀 하나가 4개, 16개, 64개짜리 잘은 면과 맞닿아
	// 면 목록과 비균일 압력 연산자가 깊이 차마다 경우를 나눠야 한다.
	//
	// 한 번 쪼개면 그 이웃이 새로 위반이 될 수 있어 변화가 없을 때까지 돈다.
	// 깊이 상한이 MaxDepth라 반복은 반드시 끝난다.
	TArray<int32> ToSplit;
	for (;;)
	{
		ToSplit.Reset();
		for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
		{
			const FFixedOctreeNode& Node = Nodes[NodeIndex];
			if (Node.FirstChildIndex != INDEX_NONE || Node.Depth >= Settings.MaxDepth)
			{
				continue;
			}
			bool bViolates = false;
			for (int32 Axis = 0; Axis < 3 && !bViolates; ++Axis)
			{
				bViolates = HasFinerFaceNeighbor(NodeIndex, Axis, -1)
					|| HasFinerFaceNeighbor(NodeIndex, Axis, 1);
			}
			if (bViolates)
			{
				ToSplit.Add(NodeIndex);
			}
		}
		if (ToSplit.Num() == 0)
		{
			return true;
		}
		for (const int32 NodeIndex : ToSplit)
		{
			if (!SplitNode(NodeIndex))
			{
				return false;
			}
			++NumBalanceSplits;
		}
	}
}

void FFixedOctreeGrid::CollectLeaves()
{
	LeafNodeIndices.Reset();
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		if (Nodes[NodeIndex].FirstChildIndex == INDEX_NONE)
		{
			LeafNodeIndices.Add(NodeIndex);
		}
	}
	LeafNodeIndices.Sort([this](int32 A, int32 B) { return Nodes[A].MortonCode < Nodes[B].MortonCode; });
	for (int32 Leaf = 0; Leaf < LeafNodeIndices.Num(); ++Leaf)
	{
		Nodes[LeafNodeIndices[Leaf]].LeafIndex = Leaf;
	}
}

void FFixedOctreeGrid::BuildFaces()
{
	Faces.Reset();
	LeafFaceStart.Reset();
	LeafFaceIndices.Reset();
	NumHangingFaces = 0;

	const int32 NumLeaves = GetNumLeaves();
	const FIntVector FineCount = GetFineCount();

	// 면은 정확히 한 번만 만든다 (규약 6). 중복을 피하는 규칙은 둘이다.
	//   - 깊이가 같으면 축의 + 쪽 리프만 만든다.
	//   - 깊이가 다르면 **잘은 쪽**만 만든다. 굵은 쪽은 같은 깊이 이웃을 못 찾고
	//     지나가므로, 맞닿은 잘은 리프 네 개가 각자 자기 면을 만든다.
	for (int32 Leaf = 0; Leaf < NumLeaves; ++Leaf)
	{
		const FFixedOctreeNode& Node = Nodes[LeafNodeIndices[Leaf]];
		const int32 Width = 1 << (Settings.MaxDepth - Node.Depth);
		const double SizeM = BaseParams.CellSizeM / double(1 << Node.Depth);
		const double AreaM2 = SizeM * SizeM;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const int32 Sign = Side == 0 ? -1 : 1;
				FIntVector Coord = Node.MinFine;
				Coord[Axis] += Sign * Width;

				if (Coord[Axis] < 0 || Coord[Axis] >= FineCount[Axis])
				{
					// 도메인 벽. 한쪽만 있는 면이라 중복될 수 없다.
					FFixedOctreeFace& Face = Faces.AddDefaulted_GetRef();
					Face.LeafLow = Sign > 0 ? Leaf : INDEX_NONE;
					Face.LeafHigh = Sign > 0 ? INDEX_NONE : Leaf;
					Face.Axis = Axis;
					Face.AreaM2 = AreaM2;
					Face.DistanceM = 0.0;
					continue;
				}

				if (const int32* Found = NodeLookup.Find(NodeKey(Node.Depth, EncodeMorton(Coord))))
				{
					if (Nodes[*Found].FirstChildIndex != INDEX_NONE || Sign < 0)
					{
						// 이웃이 더 잘거나(잘은 쪽이 만든다), 같은 깊이의 - 쪽이다.
						continue;
					}
					FFixedOctreeFace& Face = Faces.AddDefaulted_GetRef();
					Face.LeafLow = Leaf;
					Face.LeafHigh = Nodes[*Found].LeafIndex;
					Face.Axis = Axis;
					Face.AreaM2 = AreaM2;
					Face.DistanceM = SizeM;
					continue;
				}

				// 같은 깊이에 노드가 없다 = 이웃이 더 굵다. 가장 깊은 조상이 그 리프다.
				int32 CoarseNode = INDEX_NONE;
				int32 CoarseDepth = 0;
				for (int32 Depth = Node.Depth - 1; Depth >= 0 && CoarseNode == INDEX_NONE; --Depth)
				{
					const int32 CoarseWidth = 1 << (Settings.MaxDepth - Depth);
					const FIntVector Snapped(
						(Coord.X / CoarseWidth) * CoarseWidth,
						(Coord.Y / CoarseWidth) * CoarseWidth,
						(Coord.Z / CoarseWidth) * CoarseWidth);
					if (const int32* Ancestor = NodeLookup.Find(NodeKey(Depth, EncodeMorton(Snapped))))
					{
						CoarseNode = *Ancestor;
						CoarseDepth = Depth;
					}
				}
				if (CoarseNode == INDEX_NONE)
				{
					// 도메인을 빈틈없이 덮으므로 여기 오면 토폴로지가 깨진 것이다.
					// 조용히 지나가도 리프별 면적 합 검사가 잡는다.
					continue;
				}
				const double CoarseSizeM = BaseParams.CellSizeM / double(1 << CoarseDepth);
				FFixedOctreeFace& Face = Faces.AddDefaulted_GetRef();
				Face.LeafLow = Sign > 0 ? Leaf : Nodes[CoarseNode].LeafIndex;
				Face.LeafHigh = Sign > 0 ? Nodes[CoarseNode].LeafIndex : Leaf;
				Face.Axis = Axis;
				Face.AreaM2 = AreaM2;
				// 두 셀 중심의 축방향 거리. 2:1이면 (h + 2h)/2 = 1.5h다.
				Face.DistanceM = 0.5 * (SizeM + CoarseSizeM);
				++NumHangingFaces;
			}
		}
	}

	// CSR로 리프별 면 목록을 만든다. 솔버는 이 배열만 읽는다 (D7).
	LeafFaceStart.Init(0, NumLeaves + 1);
	for (const FFixedOctreeFace& Face : Faces)
	{
		if (Face.LeafLow != INDEX_NONE) { ++LeafFaceStart[Face.LeafLow + 1]; }
		if (Face.LeafHigh != INDEX_NONE) { ++LeafFaceStart[Face.LeafHigh + 1]; }
	}
	for (int32 Leaf = 0; Leaf < NumLeaves; ++Leaf)
	{
		LeafFaceStart[Leaf + 1] += LeafFaceStart[Leaf];
	}
	TArray<int32> Cursor = LeafFaceStart;
	LeafFaceIndices.SetNumUninitialized(LeafFaceStart[NumLeaves]);
	for (int32 FaceIndex = 0; FaceIndex < Faces.Num(); ++FaceIndex)
	{
		const FFixedOctreeFace& Face = Faces[FaceIndex];
		if (Face.LeafLow != INDEX_NONE) { LeafFaceIndices[Cursor[Face.LeafLow]++] = FaceIndex; }
		if (Face.LeafHigh != INDEX_NONE) { LeafFaceIndices[Cursor[Face.LeafHigh]++] = FaceIndex; }
	}
}

bool FFixedOctreeGrid::Build(const FAirGridParams& InBaseParams,
	const FFixedOctreeSettings& InSettings, const TArray<FBox>& RefinementBoxesM, FString& OutError)
{
	Reset();
	OutError.Reset();
	// Morton 좌표 폭과 메모리 예산을 제한한다. 입력을 조용히 clamp하지 않는다.
	if (InBaseParams.NumCellsX < 1 || InBaseParams.NumCellsX > 256
		|| InBaseParams.NumCellsY < 1 || InBaseParams.NumCellsY > 256
		|| InBaseParams.NumCellsZ < 1 || InBaseParams.NumCellsZ > 256
		|| !FMath::IsFinite(InBaseParams.CellSizeM) || InBaseParams.CellSizeM < 0.001
		|| !FMath::IsFinite(InBaseParams.DensityKgM3) || InBaseParams.DensityKgM3 <= 0.0
		|| !FMath::IsFinite(InBaseParams.SpecificHeatJKgK) || InBaseParams.SpecificHeatJKgK <= 0.0
		|| InSettings.MaxDepth < 0 || InSettings.MaxDepth > 6
		|| InSettings.MaxLeaves < 1 || InSettings.MaxLeaves > 1000000)
	{
		OutError = TEXT("Invalid octree settings (dimensions 1..256, depth 0..6, leaves 1..1000000).");
		return false;
	}
	for (const FBox& Box : RefinementBoxesM)
	{
		if (!Box.IsValid || Box.Min.ContainsNaN() || Box.Max.ContainsNaN()
			|| Box.Min.X >= Box.Max.X || Box.Min.Y >= Box.Max.Y || Box.Min.Z >= Box.Max.Z)
		{
			OutError = TEXT("Refinement bounds must be finite, positive-volume boxes in metres.");
			return false;
		}
	}
	const int32 RootCount = InBaseParams.NumCellsX * InBaseParams.NumCellsY * InBaseParams.NumCellsZ;
	if (RootCount > InSettings.MaxLeaves)
	{
		OutError = TEXT("Base grid already exceeds MaxLeaves.");
		return false;
	}
	BaseParams = InBaseParams;
	Settings = InSettings;
	PlannedLeaves = RootCount;
	Nodes.AddDefaulted(RootCount);
	NodeLookup.Reserve(RootCount);
	const int32 RootWidth = 1 << Settings.MaxDepth;
	for (int32 Z = 0; Z < BaseParams.NumCellsZ; ++Z)
	{
		for (int32 Y = 0; Y < BaseParams.NumCellsY; ++Y)
		{
			for (int32 X = 0; X < BaseParams.NumCellsX; ++X)
			{
				const int32 Index = X + BaseParams.NumCellsX * (Y + BaseParams.NumCellsY * Z);
				Nodes[Index].RootCellIndex = Index;
				Nodes[Index].MinFine = FIntVector(X, Y, Z) * RootWidth;
				Nodes[Index].MortonCode = EncodeMorton(Nodes[Index].MinFine);
				NodeLookup.Add(NodeKey(0, Nodes[Index].MortonCode), Index);
			}
		}
	}
	for (int32 Root = 0; Root < RootCount; ++Root)
	{
		if (!RefineNode(Root, RefinementBoxesM))
		{
			Reset();
			OutError = TEXT("Refinement exceeds MaxLeaves; no partial topology is published.");
			return false;
		}
	}
	if (!BalanceTree())
	{
		Reset();
		OutError = TEXT("2:1 balancing exceeds MaxLeaves; no partial topology is published.");
		return false;
	}
	CollectLeaves();
	BuildFaces();
	// D7: 이후 해석은 면·이웃 인덱스 배열만 읽는다. 구축용 조회표는 여기서 버린다.
	NodeLookup.Empty();
	return true;
}

bool FFixedOctreeGrid::ImportUniform(const FUniformAirGrid& Source, FString& OutError)
{
	Fields.TemperatureK.Reset();
	Fields.bSolidCell.Reset();
	OutError.Reset();
	const FAirGridParams& P = Source.GetParams();
	if (GetNumLeaves() == 0 || P.NumCellsX != BaseParams.NumCellsX
		|| P.NumCellsY != BaseParams.NumCellsY || P.NumCellsZ != BaseParams.NumCellsZ
		|| P.CellSizeM != BaseParams.CellSizeM || P.DensityKgM3 != BaseParams.DensityKgM3
		|| P.SpecificHeatJKgK != BaseParams.SpecificHeatJKgK
		|| Source.TemperatureK.Num() != Source.GetNumCells() || Source.bSolidCell.Num() != Source.GetNumCells())
	{
		OutError = TEXT("Uniform source must match the built domain, material and field lengths.");
		return false;
	}
	for (double TempK : Source.TemperatureK)
	{
		if (!FMath::IsFinite(TempK) || TempK <= 0.0)
		{
			OutError = TEXT("Source temperatures must be finite and positive Kelvin.");
			return false;
		}
	}
	Fields.TemperatureK.SetNumUninitialized(GetNumLeaves());
	Fields.bSolidCell.SetNumUninitialized(GetNumLeaves());
	for (int32 Leaf = 0; Leaf < GetNumLeaves(); ++Leaf)
	{
		const int32 Root = Nodes[LeafNodeIndices[Leaf]].RootCellIndex;
		Fields.TemperatureK[Leaf] = Source.TemperatureK[Root];
		Fields.bSolidCell[Leaf] = Source.bSolidCell[Root];
	}
	return true;
}

bool FFixedOctreeGrid::RestrictTemperatureToUniform(TArray<double>& OutTemperatureK) const
{
	OutTemperatureK.Reset();
	if (!HasFields()) { return false; }
	OutTemperatureK.Init(0.0, BaseParams.NumCellsX * BaseParams.NumCellsY * BaseParams.NumCellsZ);
	for (int32 Leaf = 0; Leaf < GetNumLeaves(); ++Leaf)
	{
		const FFixedOctreeNode& Node = Nodes[LeafNodeIndices[Leaf]];
		// 루트 체적으로 나눈 자식 체적 = 1/8^depth. 동일 재질의 체적 가중 평균.
		OutTemperatureK[Node.RootCellIndex] += Fields.TemperatureK[Leaf] / double(uint64(1) << (3 * Node.Depth));
	}
	return true;
}

double FFixedOctreeGrid::TotalThermalEnergyJ(double DatumTempK) const
{
	double EnergyJ = 0.0;
	if (!HasFields()) { return EnergyJ; }
	const double CapacityPerM3 = BaseParams.DensityKgM3 * BaseParams.SpecificHeatJKgK;
	for (int32 Leaf = 0; Leaf < GetNumLeaves(); ++Leaf)
	{
		if (!Fields.bSolidCell[Leaf])
		{
			EnergyJ += CapacityPerM3 * LeafVolumeM3(Leaf) * (Fields.TemperatureK[Leaf] - DatumTempK);
		}
	}
	return EnergyJ;
}
