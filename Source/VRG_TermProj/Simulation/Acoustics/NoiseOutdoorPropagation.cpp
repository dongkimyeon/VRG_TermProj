#include "Simulation/Acoustics/NoiseOutdoorPropagation.h"

#include <cmath>

namespace VrgNoise
{
namespace
{
	constexpr double ReferencePressurePa = 101325.0;
	constexpr double ReferenceTemperatureK = 293.15;

	double DistanceM(const FVector& A, const FVector& B)
	{
		return std::hypot(A.X - B.X, A.Y - B.Y, A.Z - B.Z);
	}

	double DivergenceDb(double Distance)
	{
		return 10.0 * std::log10(4.0 * UE_DOUBLE_PI) + 20.0 * std::log10(Distance);
	}

	// ISO 9613-2:1996 Table 3. This model uses the same G for all three ground regions.
	double GroundDb(int32 Band, double ProjectedM, double SourceHeightM, double ReceiverHeightM, double G)
	{
		const double MiddleFraction = ProjectedM > 0.0 ? FMath::Max(0.0, 1.0 - 30.0 * (SourceHeightM + ReceiverHeightM) / ProjectedM) : 0.0;
		if (Band == 0) return -3.0 * (1.0 + MiddleFraction);
		double Ground = -3.0 * (1.0 - G) * (1.0 + MiddleFraction);
		if (G == 0.0 || Band >= 5) return Ground;

		const double DistanceTerm = -std::expm1(-ProjectedM / 50.0);
		for (const double HeightM : {SourceHeightM, ReceiverHeightM})
		{
			const double HeightSquared = FMath::Square(HeightM);
			switch (Band)
			{
			case 1:
				Ground += G * (3.0 * std::exp(-0.12 * FMath::Square(HeightM - 5.0)) * DistanceTerm
					- 5.7 * std::exp(-0.09 * HeightSquared) * std::expm1(-2.8e-6 * FMath::Square(ProjectedM)));
				break;
			case 2: Ground += G * 8.6 * std::exp(-0.09 * HeightSquared) * DistanceTerm; break;
			case 3: Ground += G * 14.0 * std::exp(-0.46 * HeightSquared) * DistanceTerm; break;
			case 4: Ground += G * 5.0 * std::exp(-0.9 * HeightSquared) * DistanceTerm; break;
			}
		}
		return Ground;
	}

	FVector2D XY(const FVector& Point) { return FVector2D(Point.X, Point.Y); }

	bool HitScreen(const FVector& From, const FVector& To, const FOutdoorScreen& Screen, double GroundZM, FVector& Hit)
	{
		const FVector2D Ray = XY(To) - XY(From);
		const FVector2D Edge = Screen.EndM - Screen.StartM;
		const double Cross = FVector2D::CrossProduct(Ray, Edge);
		if (Cross == 0.0) return false;
		const FVector2D Offset = Screen.StartM - XY(From);
		const double AlongRay = FVector2D::CrossProduct(Offset, Edge) / Cross;
		const double AlongEdge = FVector2D::CrossProduct(Offset, Ray) / Cross;
		if (AlongRay <= 0.0 || AlongRay >= 1.0 || AlongEdge < 0.0 || AlongEdge > 1.0) return false;
		Hit = FMath::Lerp(From, To, AlongRay);
		return Hit.Z >= GroundZM && Hit.Z <= GroundZM + Screen.HeightM;
	}

	double EdgePathM(const FVector& A, const FVector& B, const FVector& From, const FVector& To)
	{
		const FVector Edge = B - A;
		const double Length = Edge.Size();
		const FVector Axis = Edge / Length;
		const double AlongFrom = FVector::DotProduct(From - A, Axis);
		const double AlongTo = FVector::DotProduct(To - A, Axis);
		const double PerpFrom = DistanceM(From, A + Axis * AlongFrom);
		const double PerpTo = DistanceM(To, A + Axis * AlongTo);
		const double OffAxis = PerpFrom + PerpTo;
		const double Along = OffAxis > 0.0 ? FMath::Lerp(AlongFrom, AlongTo, PerpFrom / OffAxis) : AlongFrom;
		const FVector Bend = A + Axis * FMath::Clamp(Along, 0.0, Length);
		return DistanceM(From, Bend) + DistanceM(Bend, To);
	}

	double DiffractionDb(double ExcessM, double WavelengthM)
	{
		// ISO 9613-2:1996 7.4, single edge, Kmet = 1 as an explicit approximation.
		return FMath::Min(20.0, 10.0 * std::log10(3.0 + 20.0 * ExcessM / WavelengthM));
	}
}

bool FOutdoorScreen::IsValid() const
{
	const double Length = (EndM - StartM).Size();
	return std::isfinite(Length) && Length > UE_DOUBLE_SMALL_NUMBER && std::isfinite(HeightM)
		&& HeightM > UE_DOUBLE_SMALL_NUMBER && ReflectedEnergy >= 0.0 && ReflectedEnergy <= 1.0;
}

bool FOutdoorEnvironment::IsValid() const
{
	if (!std::isfinite(GroundZM) || !(GroundFactor >= 0.0 && GroundFactor <= 1.0)
		|| !(RelativeHumidity >= 0.0 && RelativeHumidity <= 1.0)
		|| !std::isfinite(PressurePa) || PressurePa <= 0.0 || Screens.Num() > MaxOutdoorScreens)
		return false;
	for (const auto& Screen : Screens)
		if (!Screen.IsValid() || !std::isfinite(GroundZM + Screen.HeightM) || GroundZM + Screen.HeightM <= GroundZM) return false;
	return true;
}

TOptional<FOutdoorAir> OutdoorAir(double TemperatureK, double RelativeHumidity, double PressurePa)
{
	if (!(TemperatureK >= 253.15 && TemperatureK <= 323.15) || !(RelativeHumidity >= 0.0 && RelativeHumidity <= 1.0)
		|| !std::isfinite(PressurePa) || PressurePa <= 0.0) return {};

	// ISO 9613-1:1993 pure-tone absorption evaluated at exact base-10 octave centres.
	const double TemperatureRatio = TemperatureK / ReferenceTemperatureK;
	const double PressureRatio = PressurePa / ReferencePressurePa;
	const double SaturationRatio = std::pow(10.0, -6.8346 * std::pow(273.16 / TemperatureK, 1.261) + 4.6151);
	const double WaterMolarPercent = 100.0 * RelativeHumidity * SaturationRatio / PressureRatio;
	const double OxygenHz = PressureRatio * (24.0 + 4.04e4 * WaterMolarPercent * (0.02 + WaterMolarPercent) / (0.391 + WaterMolarPercent));
	const double NitrogenHz = PressureRatio / std::sqrt(TemperatureRatio) * (9.0 + 280.0 * WaterMolarPercent
		* std::exp(-4.17 * (std::pow(TemperatureRatio, -1.0 / 3.0) - 1.0)));
	FOutdoorAir Air;
	Air.TemperatureK = TemperatureK;
	for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
	{
		const double FrequencySquared = FMath::Square(1000.0 * std::pow(10.0, 0.3 * (Band - 4)));
		const double Oxygen = 0.01275 * std::exp(-2239.1 / TemperatureK) / (OxygenHz + FrequencySquared / OxygenHz);
		const double Nitrogen = 0.1068 * std::exp(-3352.0 / TemperatureK) / (NitrogenHz + FrequencySquared / NitrogenHz);
		const double Coefficient = 8.686 * FrequencySquared * (1.84e-11 * std::sqrt(TemperatureRatio) / PressureRatio
			+ std::pow(TemperatureRatio, -2.5) * (Oxygen + Nitrogen));
		if (!std::isfinite(Coefficient) || Coefficient < 0.0) return {};
		Air.AbsorptionDbPerM[Band] = Coefficient;
	}
	return Air;
}

TOptional<FOutdoorPathLoss> OutdoorPath(const FVector& SourceM, const FVector& ReceiverM,
	const FOutdoorEnvironment& Environment, const FOutdoorAir& Air)
{
	const double SourceHeight = SourceM.Z - Environment.GroundZM;
	const double ReceiverHeight = ReceiverM.Z - Environment.GroundZM;
	FOutdoorPathLoss Path;
	Path.DistanceM = DistanceM(SourceM, ReceiverM);
	if (!Environment.IsValid() || !std::isfinite(Path.DistanceM) || Path.DistanceM <= 0.0
		|| !std::isfinite(SourceHeight) || !std::isfinite(ReceiverHeight) || SourceHeight < 0.0 || ReceiverHeight < 0.0
		|| !(Air.TemperatureK >= 253.15 && Air.TemperatureK <= 323.15)) return {};

	const double ProjectedM = (XY(SourceM) - XY(ReceiverM)).Size();
	const double SpeedOfSoundMps = 331.3 * std::sqrt(Air.TemperatureK / 273.15);
	Path.DivergenceDb = DivergenceDb(Path.DistanceM);
	for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
	{
		if (!std::isfinite(Air.AbsorptionDbPerM[Band]) || Air.AbsorptionDbPerM[Band] < 0.0) return {};
		Path.Bands[Band].AtmosphereDb = Air.AbsorptionDbPerM[Band] * Path.DistanceM;
		Path.Bands[Band].GroundDb = GroundDb(Band, ProjectedM, SourceHeight, ReceiverHeight, Environment.GroundFactor);
	}

	const double GroundZM = Environment.GroundZM;
	for (const auto& Screen : Environment.Screens)
	{
		FVector Hit;
		if (!HitScreen(SourceM, ReceiverM, Screen, GroundZM, Hit)) continue;
		++Path.BlockingScreens;
		const FVector BaseA(Screen.StartM, GroundZM), BaseB(Screen.EndM, GroundZM);
		const FVector TopA(Screen.StartM, GroundZM + Screen.HeightM), TopB(Screen.EndM, GroundZM + Screen.HeightM);
		const double TopExcess = FMath::Max(0.0, EdgePathM(TopA, TopB, SourceM, ReceiverM) - Path.DistanceM);
		const double LeftExcess = FMath::Max(0.0, EdgePathM(BaseA, TopA, SourceM, ReceiverM) - Path.DistanceM);
		const double RightExcess = FMath::Max(0.0, EdgePathM(BaseB, TopB, SourceM, ReceiverM) - Path.DistanceM);
		const FVector2D Axis = (XY(ReceiverM) - XY(SourceM)).GetSafeNormal();
		const double CrossWidth = FMath::Abs(FVector2D::CrossProduct(Screen.EndM - Screen.StartM, Axis));
		for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
		{
			const double Wavelength = SpeedOfSoundMps / OutdoorNominalHz[Band];
			if (CrossWidth <= Wavelength) continue;
			auto& Loss = Path.Bands[Band];
			const double Top = FMath::Max(0.0, DiffractionDb(TopExcess, Wavelength) - Loss.GroundDb);
			const double Left = DiffractionDb(LeftExcess, Wavelength);
			const double Right = DiffractionDb(RightExcess, Wavelength);
			const double Energy = std::pow(10.0, -Top / 10.0) + std::pow(10.0, -Left / 10.0) + std::pow(10.0, -Right / 10.0);
			// For intersecting screens use the dominant individual insertion loss. A multi-edge route needs a different model.
			Loss.BarrierDb = FMath::Max(Loss.BarrierDb, -10.0 * std::log10(Energy));
		}
	}

	double ReflectedRelativeEnergy[OutdoorOctaves] = {};
	for (const auto& Screen : Environment.Screens)
	{
		if (Screen.ReflectedEnergy <= 0.2) continue;
		const FVector2D Edge = Screen.EndM - Screen.StartM;
		const double EdgeM = Edge.Size();
		const FVector2D Normal(-Edge.Y / EdgeM, Edge.X / EdgeM);
		const double SourceSide = FVector2D::DotProduct(XY(SourceM) - Screen.StartM, Normal);
		const double ReceiverSide = FVector2D::DotProduct(XY(ReceiverM) - Screen.StartM, Normal);
		if (SourceSide == 0.0 || ReceiverSide == 0.0 || (SourceSide > 0.0) != (ReceiverSide > 0.0)) continue;
		const FVector Image(XY(SourceM) - 2.0 * SourceSide * Normal, SourceM.Z);
		FVector Hit;
		if (!HitScreen(Image, ReceiverM, Screen, GroundZM, Hit)) continue;
		bool bBlocked = false;
		for (const auto& Other : Environment.Screens)
		{
			if (&Other == &Screen) continue;
			FVector OtherHit;
			if (HitScreen(SourceM, Hit, Other, GroundZM, OtherHit) || HitScreen(Hit, ReceiverM, Other, GroundZM, OtherHit))
			{
				bBlocked = true;
				break;
			}
		}
		if (bBlocked) continue;

		const double FirstM = DistanceM(SourceM, Hit), SecondM = DistanceM(Hit, ReceiverM);
		const double Length = FirstM + SecondM;
		const double ReflectProjectedM = (XY(Image) - XY(ReceiverM)).Size();
		const double VisibleSizeM = FMath::Min(EdgeM, Screen.HeightM) * FMath::Abs(SourceSide) / FirstM;
		const double MinFrequencyPerSoundSpeed = 2.0 * FirstM * SecondM / (Length * FMath::Square(VisibleSizeM));
		bool bAnyBand = false;
		for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
		{
			// ISO 9613-2:1996 7.5 finite reflector screen and single image source.
			if (OutdoorNominalHz[Band] / SpeedOfSoundMps <= MinFrequencyPerSoundSpeed) continue;
			const auto& Direct = Path.Bands[Band];
			const double ReflectLoss = DivergenceDb(Length) + Air.AbsorptionDbPerM[Band] * Length
				+ GroundDb(Band, ReflectProjectedM, SourceHeight, ReceiverHeight, Environment.GroundFactor);
			const double DirectLoss = Path.DivergenceDb + Direct.AtmosphereDb + Direct.GroundDb + Direct.BarrierDb;
			ReflectedRelativeEnergy[Band] += Screen.ReflectedEnergy * std::pow(10.0, (DirectLoss - ReflectLoss) / 10.0);
			bAnyBand = true;
		}
		if (bAnyBand) ++Path.ReflectedPaths;
	}
	for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
	{
		auto& Loss = Path.Bands[Band];
		Loss.ReflectionGainDb = (10.0 / std::log(10.0)) * std::log1p(ReflectedRelativeEnergy[Band]);
		if (!std::isfinite(Loss.AtmosphereDb) || !std::isfinite(Loss.GroundDb)
			|| !std::isfinite(Loss.BarrierDb) || !std::isfinite(Loss.ReflectionGainDb)) return {};
	}
	return Path;
}

FSourceProfile Liebert940EstimatedOctaves()
{
	FSourceProfile Profile = Liebert940Standard60Hz();
	const double RatedDbA = Profile.LevelsDb[0];
	Profile.Id += TEXT("-airedale-dr95-shape");
	Profile.SourceUrl = TEXT("https://www.airedale.com/wp-content/uploads/2020/01/TM_DRYCOOLER_D10-D95_9308503_V2_06_2020.pdf");
	Profile.SourceLocator = TEXT("9308503 V2_06_2020 p.22, Horizontal DR95 Power, standard fan full speed");
	Profile.RatingCondition += TEXT("; Airedale DR95 unweighted shape normalized to the Liebert 940 total");
	Profile.Weighting = EWeighting::Unweighted;
	Profile.bEstimatedRating = true;
	Profile.FrequenciesHz.Append(OutdoorNominalHz, OutdoorOctaves);
	Profile.LevelsDb = {88, 85, 87, 84, 83, 79, 72, 64};
	const double Offset = RatedDbA - Evaluate(&Profile, true, 1.0).Db.GetValue();
	for (double& Level : Profile.LevelsDb) Level += Offset;
	return Profile;
}

TOptional<double> OutdoorOctaveTransferDb(const FOutdoorPathLoss& Path, const FSourceProfile& Profile)
{
	if (!Profile.IsValid() || Profile.FrequenciesHz.Num() != OutdoorOctaves) return {};
	double AtSource[OutdoorOctaves], AtReceiver[OutdoorOctaves];
	double MaxSource = -TNumericLimits<double>::Max(), MaxReceiver = -TNumericLimits<double>::Max();
	for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
	{
		if (Profile.FrequenciesHz[Band] != OutdoorNominalHz[Band]) return {};
		double Correction = 0.0;
		if (Profile.Weighting == EWeighting::Unweighted) TryOctaveACorrection(OutdoorNominalHz[Band], Correction);
		const auto& Loss = Path.Bands[Band];
		AtSource[Band] = Profile.LevelsDb[Band] + Correction;
		AtReceiver[Band] = AtSource[Band] - Path.DivergenceDb - Loss.AtmosphereDb - Loss.GroundDb - Loss.BarrierDb + Loss.ReflectionGainDb;
		if (!std::isfinite(AtReceiver[Band])) return {};
		MaxSource = FMath::Max(MaxSource, AtSource[Band]);
		MaxReceiver = FMath::Max(MaxReceiver, AtReceiver[Band]);
	}
	double SourceSum = 0.0, ReceiverSum = 0.0;
	for (int32 Band = 0; Band < OutdoorOctaves; ++Band)
	{
		SourceSum += std::pow(10.0, (AtSource[Band] - MaxSource) / 10.0);
		ReceiverSum += std::pow(10.0, (AtReceiver[Band] - MaxReceiver) / 10.0);
	}
	const double Transfer = (MaxReceiver - MaxSource) + 10.0 * std::log10(ReceiverSum / SourceSum);
	return std::isfinite(Transfer) ? TOptional<double>(Transfer) : TOptional<double>();
}

bool SampleOutdoorBoundary(const TArray<FVector2D>& VerticesM, double SpacingM, TArray<FVector2D>& Out)
{
	if (VerticesM.Num() < 2 || VerticesM.Num() > MaxBoundaryReceivers + 1 || !std::isfinite(SpacingM) || SpacingM <= 0.0) return false;
	TArray<FVector2D> Points = {VerticesM[0]};
	for (int32 Index = 1; Index < VerticesM.Num(); ++Index)
	{
		const FVector2D& A = VerticesM[Index - 1];
		const FVector2D& B = VerticesM[Index];
		const double Length = std::hypot(B.X - A.X, B.Y - A.Y);
		const double Intervals = std::ceil(Length / SpacingM);
		const bool bClosing = Index == VerticesM.Num() - 1 && B == VerticesM[0];
		if (Length <= 0.0 || !std::isfinite(Intervals) || Intervals < 1.0
			|| Intervals - int32(bClosing) > MaxBoundaryReceivers - Points.Num()) return false;
		const int32 NumIntervals = int32(Intervals);
		for (int32 Step = 1; Step <= NumIntervals - int32(bClosing); ++Step)
			Points.Add(Step == NumIntervals ? B : FMath::Lerp(A, B, double(Step) / NumIntervals));
	}
	if (Points.Num() < 2) return false;
	Out = MoveTemp(Points);
	return true;
}
}
