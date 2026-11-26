#include "Simulation/Acoustics/NoiseIndoor.h"
#include "Simulation/Air/AirConnectivity.h"
#include "Simulation/Air/UniformAirGrid.h"

#include <cmath>
#include <limits>

namespace VrgNoise
{
namespace
{
	bool CellAt(const FUniformAirGrid& Grid, const FVector& PositionInCells, FIntVector& Cell)
	{
		if (PositionInCells.ContainsNaN() || PositionInCells.GetMin() < 0.0
			|| PositionInCells.X >= Grid.GetNumCellsX() || PositionInCells.Y >= Grid.GetNumCellsY() || PositionInCells.Z >= Grid.GetNumCellsZ())
			return false;
		Cell = FIntVector(int32(PositionInCells.X), int32(PositionInCells.Y), int32(PositionInCells.Z));
		return true;
	}
}

bool FIndoorAbsorption::IsValid() const
{
	for (int32 Band = 0; Band < IndoorOctaves; ++Band)
		if (!(Shell[Band] >= 0.0 && Shell[Band] <= 1.0 && Equipment[Band] >= 0.0 && Equipment[Band] <= 1.0)) return false;
	return true;
}

void FIndoorGeometry::Build(const FUniformAirGrid& Grid, const FVector& InOriginM, TConstArrayView<int32> Owners, int32 EquipmentOwners)
{
	OriginM = InOriginM;
	Zones.Init(FIndoorZone(), VrgAirConnectivity::LabelAirRegions(Grid, CellZone));
	const double FaceM2 = FMath::Square(Grid.GetCellSizeM());
	const FIntVector Neighbors[] = {{-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}};
	const bool bHasOwners = Owners.Num() == Grid.GetNumCells();
	for (int32 Z = 0; Z < Grid.GetNumCellsZ(); ++Z)
		for (int32 Y = 0; Y < Grid.GetNumCellsY(); ++Y)
			for (int32 X = 0; X < Grid.GetNumCellsX(); ++X)
			{
				const int32 Zone = CellZone[Grid.CellIndex(X, Y, Z)];
				if (Zone == INDEX_NONE) continue;
				FIndoorZone& Room = Zones[Zone];
				for (const FIntVector& Offset : Neighbors)
				{
					const FIntVector N = FIntVector(X, Y, Z) + Offset;
					if (Grid.IsFluidCell(N.X, N.Y, N.Z)) continue;
					Room.SurfaceM2 += FaceM2;
					if (bHasOwners && Grid.IsValidCell(N.X, N.Y, N.Z))
					{
						const int32 Owner = Owners[Grid.CellIndex(N.X, N.Y, N.Z)];
						if (Owner >= 0 && Owner < EquipmentOwners) Room.EquipmentSurfaceM2 += FaceM2;
					}
				}
			}
}

int32 FIndoorGeometry::ZoneAt(const FUniformAirGrid& Grid, const FVector& PositionM) const
{
	FIntVector Cell;
	if (!CellAt(Grid, (PositionM - OriginM) / Grid.GetCellSizeM(), Cell)) return INDEX_NONE;
	const int32 Index = Grid.CellIndex(Cell.X, Cell.Y, Cell.Z);
	return CellZone.IsValidIndex(Index) ? CellZone[Index] : INDEX_NONE;
}

bool FIndoorGeometry::HasDirectPath(const FUniformAirGrid& Grid, const FVector& FromM, const FVector& ToM) const
{
	const FVector Start = (FromM - OriginM) / Grid.GetCellSizeM(), End = (ToM - OriginM) / Grid.GetCellSizeM();
	FIntVector Cell, Target;
	if (!CellAt(Grid, Start, Cell) || !CellAt(Grid, End, Target)) return false;
	const FVector Delta = End - Start;
	FIntVector Step(0);
	FVector Crossing, Interval;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		// Ignore axes whose endpoints occupy the same cell. This also avoids
		// advancing beyond an endpoint exactly on a face.
		Crossing[Axis] = Interval[Axis] = std::numeric_limits<double>::infinity();
		if (Cell[Axis] == Target[Axis]) continue;
		Step[Axis] = Target[Axis] > Cell[Axis] ? 1 : -1;
		Crossing[Axis] = (Cell[Axis] + (Step[Axis] > 0 ? 1.0 : 0.0) - Start[Axis]) / Delta[Axis];
		Interval[Axis] = 1.0 / FMath::Abs(Delta[Axis]);
	}
	while (Grid.IsFluidCell(Cell.X, Cell.Y, Cell.Z))
	{
		if (Cell == Target) return true;
		const double Next = Crossing.GetMin();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Crossing[Axis] != Next) continue;
			Cell[Axis] += Step[Axis];
			Crossing[Axis] = Cell[Axis] == Target[Axis] ? std::numeric_limits<double>::infinity() : Crossing[Axis] + Interval[Axis];
		}
	}
	return false;
}

TOptional<double> IndoorDiffuseGain(const FIndoorZone& Zone, const FIndoorAbsorption& Absorption, const FSourceProfile* Spectrum)
{
	if (!FMath::IsFinite(Zone.SurfaceM2) || !(Zone.SurfaceM2 > 0.0)
		|| !(Zone.EquipmentSurfaceM2 >= 0.0 && Zone.EquipmentSurfaceM2 <= Zone.SurfaceM2) || !Absorption.IsValid())
		return {};
	const double EquipmentFraction = Zone.EquipmentSurfaceM2 / Zone.SurfaceM2;
	const auto GainAt = [&](int32 Band) -> TOptional<double>
	{
		const double Alpha = FMath::Lerp(Absorption.Shell[Band], Absorption.Equipment[Band], EquipmentFraction);
		if (Alpha <= 0.0) return {}; // no steady diffuse field in a lossless enclosure
		const double Gain = 4.0 * (1.0 - Alpha) / (Zone.SurfaceM2 * Alpha);
		return FMath::IsFinite(Gain) ? TOptional<double>(Gain) : TOptional<double>();
	};
	if (!Spectrum) return GainAt(4); // effective broadband 1 kHz
	constexpr double Frequencies[IndoorOctaves] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
	if (!Spectrum->IsValid() || Spectrum->FrequenciesHz.Num() != IndoorOctaves) return {};
	double LevelDb[IndoorOctaves], MaxDb = -TNumericLimits<double>::Max();
	for (int32 Band = 0; Band < IndoorOctaves; ++Band)
	{
		if (Spectrum->FrequenciesHz[Band] != Frequencies[Band]) return {};
		double A = 0.0;
		if (Spectrum->Weighting == EWeighting::Unweighted) TryOctaveACorrection(Frequencies[Band], A);
		LevelDb[Band] = Spectrum->LevelsDb[Band] + A;
		MaxDb = FMath::Max(MaxDb, LevelDb[Band]);
	}
	double Sum = 0.0, Weight = 0.0;
	for (int32 Band = 0; Band < IndoorOctaves; ++Band)
	{
		const auto Gain = GainAt(Band);
		if (!Gain.IsSet()) return {};
		const double BandWeight = std::pow(10.0, (LevelDb[Band] - MaxDb) / 10.0);
		Sum += BandWeight * *Gain;
		Weight += BandWeight;
	}
	return FMath::IsFinite(Sum / Weight) ? TOptional<double>(Sum / Weight) : TOptional<double>();
}

double IndoorDirectGain(double DistanceM)
{
	return 1.0 / (4.0 * UE_DOUBLE_PI * FMath::Square(DistanceM));
}

bool SampleIndoorMap(const FBox& RoomM, double SpacingM, double HeightM, TArray<FVector>& Out)
{
	if (!RoomM.IsValid || RoomM.Min.ContainsNaN() || RoomM.Max.ContainsNaN() || !FMath::IsFinite(SpacingM)
		|| SpacingM <= 0.0 || !(HeightM > 0.0 && HeightM < RoomM.GetSize().Z)) return false;
	const FVector Extent = RoomM.GetSize();
	const double CountX = std::ceil(Extent.X / SpacingM), CountY = std::ceil(Extent.Y / SpacingM);
	if (!(CountX >= 1.0 && CountY >= 1.0 && CountX * CountY <= MaxIndoorReceivers)) return false;
	TArray<FVector> Points;
	for (int32 Y = 0; Y < int32(CountY); ++Y)
		for (int32 X = 0; X < int32(CountX); ++X)
			Points.Add(RoomM.Min + FVector((X + .5) * Extent.X / CountX, (Y + .5) * Extent.Y / CountY, HeightM));
	Out = MoveTemp(Points);
	return true;
}

FSourceProfile SanAceIndoorFan(bool bCounterRotating)
{
	FSourceProfile Profile;
	Profile.Id = bCounterRotating ? TEXT("sanace-9crj-1m-estimate") : TEXT("sanace-9hva-1m-estimate");
	Profile.Model = bCounterRotating ? TEXT("Sanyo Denki 9CRJ0412P5J001") : TEXT("Sanyo Denki 9HVA0412P3J001");
	Profile.SourceUrl = bCounterRotating
		? TEXT("https://products.sanyodenki.com/en/sanace/dc/counter-rotating-fan/9CRJ0412P5J001/")
		: TEXT("https://products.sanyodenki.com/en/sanace/dc/dc-fan/9HVA0412P3J001/");
	Profile.SourceLocator = TEXT("Specifications, SPL dB(A); Sanyo Denki About Fan, Noise characteristics");
	Profile.RatingCondition = TEXT("One catalog fan; 1 m anechoic intake measurement interpreted as spherical equivalent power");
	Profile.LevelsDb = { (bCounterRotating ? 72.0 : 71.0) + 10.0 * std::log10(4.0 * UE_DOUBLE_PI) };
	Profile.bEstimatedRating = true;
	Profile.bFanSpeedEstimate = true;
	return Profile;
}

FSourceProfile Pcw035IndoorCrah()
{
	FSourceProfile Profile;
	Profile.Id = TEXT("liebert-pcw-pw035-downflow-proxy");
	Profile.Model = TEXT("Vertiv Liebert PCW PW035 whole-unit CRAH proxy");
	Profile.SourceUrl = TEXT("https://www.vertiv.com/491e2b/globalassets/shared/liebert-pcw---user-manual---english---10012648man_eng.pdf");
	Profile.SourceLocator = TEXT("10012648MAN_ENG 07.01.2025 Table 24 p.46, PW035 PWL, 63 Hz to 8 kHz");
	Profile.RatingCondition = TEXT("High-efficiency EC, Downflow Down, max airflow; fixed operating reference while simulation fan runs");
	Profile.Weighting = EWeighting::Unweighted;
	Profile.FrequenciesHz = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
	Profile.LevelsDb = {76.0, 80.7, 85.3, 89.0, 90.2, 86.0, 91.5, 76.1};
	Profile.bEstimatedRating = true;
	return Profile;
}
}
