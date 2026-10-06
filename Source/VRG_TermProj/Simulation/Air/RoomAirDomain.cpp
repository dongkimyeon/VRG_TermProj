#include "Simulation/Air/RoomAirDomain.h"

bool VrgRoomAirDomain::BuildGrid(const FRoomAirDomainSettings& S, const FAirGridParams& Source,
	FAirGridParams& OutGrid, FBox& OutActualBoundsM, FString& Reason)
{
	Reason.Reset();
	if (!S.bManual) { Reason = TEXT("자동 맞춤에는 수동 격자 계획이 필요하지 않습니다."); return false; }
	const FVector MinM(S.MinXYM.X, S.MinXYM.Y, 0.0), MaxM(S.MaxXYM.X, S.MaxXYM.Y, S.CeilingHeightM);
	const FVector SpanM = MaxM - MinM;
	if (MinM.ContainsNaN() || MaxM.ContainsNaN() || SpanM.GetMin() <= 0.0)
	{ Reason = TEXT("방 XY 최소/최대는 유한하며 순서가 맞아야 하고 천장 높이는 0m보다 커야 합니다."); return false; }
	const double H = Source.CellSizeM;
	if (!FMath::IsFinite(H) || H < MinFitCellSizeM)
	{ Reason = TEXT("셀 크기가 기존 방 맞춤 최소 0.01m보다 작거나 유한하지 않습니다."); return false; }
	int32 Counts[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// No inward epsilon or clipping: actual outer faces contain the entire requested box.
		const double Count = FMath::CeilToDouble(SpanM[Axis] / H);
		if (!FMath::IsFinite(Count) || Count < 1 || Count > MaxFitCellsPerAxis)
		{ Reason = FString::Printf(TEXT("방 %c축에 %.0f셀이 필요합니다. 기존 축당 %d셀 한도 초과입니다. 범위나 셀 크기를 조정하세요."), TCHAR('X' + Axis), Count, MaxFitCellsPerAxis); return false; }
		Counts[Axis] = int32(Count);
	}
	FAirGridParams Grid = Source;
	Grid.NumCellsX = Counts[0]; Grid.NumCellsY = Counts[1]; Grid.NumCellsZ = Counts[2];
	OutGrid = Grid;
	OutActualBoundsM = FBox(MinM, MinM + FVector(Counts[0], Counts[1], Counts[2]) * H);
	return true;
}

FBox VrgRoomAirDomain::TransformedBoxM(const FVector& LocalMinM, const FVector& LocalMaxM, const FTransform& TransformM)
{
	FBox Result(ForceInit);
	for (int32 Corner = 0; Corner < 8; ++Corner)
		Result += TransformM.TransformPosition(FVector(Corner & 1 ? LocalMaxM.X : LocalMinM.X,
			Corner & 2 ? LocalMaxM.Y : LocalMinM.Y, Corner & 4 ? LocalMaxM.Z : LocalMinM.Z));
	return Result;
}

bool VrgRoomAirDomain::BuildCeilingPlans(const FRoomAirDomainSettings& Room, const TArray<FRoomCeilingPlate>& Plates,
	const FAirGridParams& Grid, const FBox& ActualRoomM, TArray<FRoomCeilingPlan>& OutPlans, FString& Reason)
{
	Reason.Reset();
	TArray<FRoomCeilingPlan> Plans;
	if (!Plates.IsEmpty() && !Room.bManual)
	{ Reason = TEXT("천장 판은 수동 방 범위에서만 설정할 수 있습니다."); return false; }
	for (int32 I = 0; I < Plates.Num(); ++I)
	{
		const auto& P = Plates[I];
		const double H = Grid.CellSizeM;
		if (P.MinXYM.ContainsNaN() || P.MaxXYM.ContainsNaN() || !FMath::IsFinite(P.HeightM)
			|| P.MinXYM.X >= P.MaxXYM.X || P.MinXYM.Y >= P.MaxXYM.Y || P.HeightM <= 0 || P.HeightM >= Room.CeilingHeightM
			|| P.MinXYM.X < Room.MinXYM.X || P.MinXYM.Y < Room.MinXYM.Y || P.MaxXYM.X > Room.MaxXYM.X || P.MaxXYM.Y > Room.MaxXYM.Y
			|| !ActualRoomM.IsValid || !FMath::IsFinite(H) || H <= 0)
		{ Reason = FString::Printf(TEXT("천장 판 %d: 유한한 양의 XY 범위와 0 < 높이 < 방 천장이 필요하며 요청 방 안에 있어야 합니다."), I); return false; }
		const FVector Lo(P.MinXYM.X, P.MinXYM.Y, P.HeightM), Hi(P.MaxXYM.X, P.MaxXYM.Y, P.HeightM);
		FRoomCeilingPlan Plan;
		Plan.MinCell = FIntVector(FMath::FloorToInt32((Lo.X - ActualRoomM.Min.X) / H),
			FMath::FloorToInt32((Lo.Y - ActualRoomM.Min.Y) / H), FMath::FloorToInt32((Lo.Z - ActualRoomM.Min.Z) / H));
		Plan.MaxCellExclusive = FIntVector(FMath::CeilToInt32((Hi.X - ActualRoomM.Min.X) / H),
			FMath::CeilToInt32((Hi.Y - ActualRoomM.Min.Y) / H), Plan.MinCell.Z + 1);
		if (Plan.MinCell.GetMin() < 0 || Plan.MaxCellExclusive.X > Grid.NumCellsX || Plan.MaxCellExclusive.Y > Grid.NumCellsY
			|| Plan.MaxCellExclusive.Z > Grid.NumCellsZ)
		{ Reason = FString::Printf(TEXT("천장 판 %d: 계산 셀 범위가 공기 격자 밖입니다. 판을 잘라 적용하지 않습니다."), I); return false; }
		Plan.ActualBoundsM = FBox(ActualRoomM.Min + FVector(Plan.MinCell) * H, ActualRoomM.Min + FVector(Plan.MaxCellExclusive) * H);
		Plans.Add(Plan);
	}
	OutPlans = MoveTemp(Plans);
	return true;
}

TArray<int32> VrgRoomAirDomain::MarkCeilingCells(FUniformAirGrid& Grid, const TArray<FRoomCeilingPlan>& Plans)
{
	TArray<int32> Added;
	for (const auto& P : Plans)
		for (int32 Z = P.MinCell.Z; Z < P.MaxCellExclusive.Z; ++Z)
			for (int32 Y = P.MinCell.Y; Y < P.MaxCellExclusive.Y; ++Y)
				for (int32 X = P.MinCell.X; X < P.MaxCellExclusive.X; ++X)
				{
					const int32 Index = Grid.CellIndex(X,Y,Z);
					if (Grid.bSolidCell[Index]) continue;
					Grid.bSolidCell[Index] = true;
					Added.Add(Index);
				}
	return Added;
}
