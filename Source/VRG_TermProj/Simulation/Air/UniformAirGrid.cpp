// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Air/UniformAirGrid.h"

void FUniformAirGrid::Initialize(const FAirGridParams& InParams)
{
	Params = InParams;
	Params.NumCellsX = FMath::Max(1, Params.NumCellsX);
	Params.NumCellsY = FMath::Max(1, Params.NumCellsY);
	Params.NumCellsZ = FMath::Max(1, Params.NumCellsZ);
	Params.CellSizeM = FMath::Max(UE_DOUBLE_SMALL_NUMBER, Params.CellSizeM);

	VelocityXMs.Init(0.0, GetNumFacesX());
	VelocityYMs.Init(0.0, GetNumFacesY());
	VelocityZMs.Init(0.0, GetNumFacesZ());
	TemperatureK.Init(Params.ReferenceTempK, GetNumCells());
	PressurePa.Init(0.0, GetNumCells());
	bSolidCell.Init(false, GetNumCells());

	NextVelocityXMs = VelocityXMs;
	NextVelocityYMs = VelocityYMs;
	NextVelocityZMs = VelocityZMs;
	NextTemperatureK = TemperatureK;
}

void FUniformAirGrid::SwapVelocityBuffers()
{
	Swap(VelocityXMs, NextVelocityXMs);
	Swap(VelocityYMs, NextVelocityYMs);
	Swap(VelocityZMs, NextVelocityZMs);
}

void FUniformAirGrid::SwapTemperatureBuffers()
{
	Swap(TemperatureK, NextTemperatureK);
}

void FUniformAirGrid::ResetToUniform(double InTemperatureK)
{
	for (double& Value : VelocityXMs) { Value = 0.0; }
	for (double& Value : VelocityYMs) { Value = 0.0; }
	for (double& Value : VelocityZMs) { Value = 0.0; }
	for (double& Value : PressurePa) { Value = 0.0; }
	for (double& Value : TemperatureK) { Value = InTemperatureK; }

	NextVelocityXMs = VelocityXMs;
	NextVelocityYMs = VelocityYMs;
	NextVelocityZMs = VelocityZMs;
	NextTemperatureK = TemperatureK;
}

double FUniformAirGrid::SampleFaceComponent(const TArray<double>& FaceField, const FVector& PositionM,
	int32 Axis) const
{
	// 면 격자는 해당 축으로 한 칸 더 많고, 그 축에서만 셀 경계에 놓인다.
	// 나머지 두 축은 셀 중심에 있으므로 반 칸을 빼서 격자 좌표로 옮긴다.
	const double InvCellSize = 1.0 / Params.CellSizeM;

	const int32 CountX = Params.NumCellsX + (Axis == 0 ? 1 : 0);
	const int32 CountY = Params.NumCellsY + (Axis == 1 ? 1 : 0);
	const int32 CountZ = Params.NumCellsZ + (Axis == 2 ? 1 : 0);

	const double GridX = PositionM.X * InvCellSize - (Axis == 0 ? 0.0 : 0.5);
	const double GridY = PositionM.Y * InvCellSize - (Axis == 1 ? 0.0 : 0.5);
	const double GridZ = PositionM.Z * InvCellSize - (Axis == 2 ? 0.0 : 0.5);

	// 격자 밖은 경계값으로 자른다. 벽 너머를 외삽하면 발산이 새어 든다.
	const double ClampedX = FMath::Clamp(GridX, 0.0, static_cast<double>(CountX - 1));
	const double ClampedY = FMath::Clamp(GridY, 0.0, static_cast<double>(CountY - 1));
	const double ClampedZ = FMath::Clamp(GridZ, 0.0, static_cast<double>(CountZ - 1));

	const int32 X0 = FMath::Clamp(FMath::FloorToInt32(ClampedX), 0, CountX - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(ClampedY), 0, CountY - 1);
	const int32 Z0 = FMath::Clamp(FMath::FloorToInt32(ClampedZ), 0, CountZ - 1);
	const int32 X1 = FMath::Min(X0 + 1, CountX - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, CountY - 1);
	const int32 Z1 = FMath::Min(Z0 + 1, CountZ - 1);

	const double TX = ClampedX - X0;
	const double TY = ClampedY - Y0;
	const double TZ = ClampedZ - Z0;

	auto At = [&](int32 X, int32 Y, int32 Z) -> double
	{
		const int32 Index = X + CountX * (Y + CountY * Z);
		return FaceField.IsValidIndex(Index) ? FaceField[Index] : 0.0;
	};

	const double C00 = FMath::Lerp(At(X0, Y0, Z0), At(X1, Y0, Z0), TX);
	const double C10 = FMath::Lerp(At(X0, Y1, Z0), At(X1, Y1, Z0), TX);
	const double C01 = FMath::Lerp(At(X0, Y0, Z1), At(X1, Y0, Z1), TX);
	const double C11 = FMath::Lerp(At(X0, Y1, Z1), At(X1, Y1, Z1), TX);

	return FMath::Lerp(FMath::Lerp(C00, C10, TY), FMath::Lerp(C01, C11, TY), TZ);
}

FVector FUniformAirGrid::SampleVelocityMs(const FVector& PositionM) const
{
	return FVector(
		SampleFaceComponent(VelocityXMs, PositionM, 0),
		SampleFaceComponent(VelocityYMs, PositionM, 1),
		SampleFaceComponent(VelocityZMs, PositionM, 2));
}

double FUniformAirGrid::CellDivergencePerS(int32 X, int32 Y, int32 Z) const
{
	// MAC 격자라 발산이 마주 보는 두 면의 차이로 바로 떨어진다.
	const double InvCellSize = 1.0 / Params.CellSizeM;

	const double DuDx = (VelocityXMs[FaceXIndex(X + 1, Y, Z)] - VelocityXMs[FaceXIndex(X, Y, Z)]) * InvCellSize;
	const double DvDy = (VelocityYMs[FaceYIndex(X, Y + 1, Z)] - VelocityYMs[FaceYIndex(X, Y, Z)]) * InvCellSize;
	const double DwDz = (VelocityZMs[FaceZIndex(X, Y, Z + 1)] - VelocityZMs[FaceZIndex(X, Y, Z)]) * InvCellSize;

	return DuDx + DvDy + DwDz;
}

double FUniformAirGrid::MaxAbsDivergencePerS() const
{
	double MaxAbs = 0.0;
	for (int32 Z = 0; Z < Params.NumCellsZ; ++Z)
	{
		for (int32 Y = 0; Y < Params.NumCellsY; ++Y)
		{
			for (int32 X = 0; X < Params.NumCellsX; ++X)
			{
				MaxAbs = FMath::Max(MaxAbs, FMath::Abs(CellDivergencePerS(X, Y, Z)));
			}
		}
	}
	return MaxAbs;
}

double FUniformAirGrid::MaxAbsVelocityMs() const
{
	double MaxAbs = 0.0;
	for (const double Value : VelocityXMs) { MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Value)); }
	for (const double Value : VelocityYMs) { MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Value)); }
	for (const double Value : VelocityZMs) { MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Value)); }
	return MaxAbs;
}

void FUniformAirGrid::ClearSolidCells()
{
	bSolidCell.Init(false, GetNumCells());
}

int32 FUniformAirGrid::GetNumFluidCells() const
{
	int32 Count = 0;
	for (int32 Index = 0; Index < GetNumCells(); ++Index)
	{
		Count += (bSolidCell.IsValidIndex(Index) && bSolidCell[Index]) ? 0 : 1;
	}
	return Count;
}

double FUniformAirGrid::TotalThermalEnergyJ(double DatumTempK) const
{
	// 밀도는 Boussinesq 기준값 하나를 쓴다. 부력에만 온도 의존을 남긴다.
	const double CellMassKg = Params.DensityKgM3 * GetCellVolumeM3();

	double EnergyJ = 0.0;
	for (int32 Index = 0; Index < TemperatureK.Num(); ++Index)
	{
		// 고체 셀에는 공기가 없다. 세면 장부가 어긋난다.
		if (bSolidCell.IsValidIndex(Index) && bSolidCell[Index])
		{
			continue;
		}
		EnergyJ += CellMassKg * Params.SpecificHeatJKgK * (TemperatureK[Index] - DatumTempK);
	}
	return EnergyJ;
}
