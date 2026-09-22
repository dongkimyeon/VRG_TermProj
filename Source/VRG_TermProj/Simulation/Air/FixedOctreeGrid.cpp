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
	PlannedLeaves = 0;
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

FBox FFixedOctreeGrid::NodeBoundsM(int32 NodeIndex) const
{
	const FFixedOctreeNode& Node = Nodes[NodeIndex];
	const double FineSizeM = BaseParams.CellSizeM / double(1 << Settings.MaxDepth);
	const FVector MinM = FVector(Node.MinFine) * FineSizeM;
	const double SizeM = BaseParams.CellSizeM / double(1 << Node.Depth);
	return FBox(MinM, MinM + FVector(SizeM));
}

double FFixedOctreeGrid::LeafVolumeM3(int32 LeafIndex) const
{
	const double SizeM = BaseParams.CellSizeM / double(1 << Nodes[LeafNodeIndices[LeafIndex]].Depth);
	return SizeM * SizeM * SizeM;
}

bool FFixedOctreeGrid::RefineNode(int32 NodeIndex, const TArray<FBox>& BoxesM)
{
	// AddDefaulted가 배열을 재할당할 수 있으므로 참조를 잡아 두지 않는다.
	const FFixedOctreeNode Parent = Nodes[NodeIndex];
	bool bRefine = false;
	if (Parent.Depth < Settings.MaxDepth)
	{
		const FBox Bounds = NodeBoundsM(NodeIndex);
		for (const FBox& Box : BoxesM)
		{
			if (OctreePositiveOverlap(Bounds, Box))
			{
				bRefine = true;
				break;
			}
		}
	}
	if (!bRefine)
	{
		LeafNodeIndices.Add(NodeIndex);
		return true;
	}
	if (PlannedLeaves > Settings.MaxLeaves - 7)
	{
		return false;
	}
	PlannedLeaves += 7;
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
	}
	for (int32 Child = 0; Child < 8; ++Child)
	{
		if (!RefineNode(FirstChild + Child, BoxesM))
		{
			return false;
		}
	}
	return true;
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
	LeafNodeIndices.Sort([this](int32 A, int32 B) { return Nodes[A].MortonCode < Nodes[B].MortonCode; });
	for (int32 Leaf = 0; Leaf < LeafNodeIndices.Num(); ++Leaf)
	{
		Nodes[LeafNodeIndices[Leaf]].LeafIndex = Leaf;
	}
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
