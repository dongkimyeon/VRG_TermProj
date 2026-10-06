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
