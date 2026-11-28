#include "Simulation/Acoustics/NoiseOutdoor.h"

#include <cmath>

namespace VrgNoise
{
bool FOutdoorReceiver::IsValid() const
{
	return !PositionM.ContainsNaN() && (Radiation == EOutdoorRadiation::Sphere || Radiation == EOutdoorRadiation::Hemisphere);
}

TOptional<double> OutdoorTransferDb(const FOutdoorReceiver& Receiver, const FVector& SourceM, double MinDistanceM)
{
	if (!Receiver.IsValid() || SourceM.ContainsNaN() || !std::isfinite(MinDistanceM) || MinDistanceM <= 0.0) return {};
	// hypot avoids squaring very large or small coordinates before taking the root.
	const double DistanceM = std::hypot(Receiver.PositionM.X - SourceM.X, Receiver.PositionM.Y - SourceM.Y, Receiver.PositionM.Z - SourceM.Z);
	if (!std::isfinite(DistanceM) || DistanceM < MinDistanceM) return {};

	// One ideal radiation model per probe: 4pi or 2pi solid angle, 1 m^2 reference.
	const double SolidAngle = (Receiver.Radiation == EOutdoorRadiation::Sphere ? 4.0 : 2.0) * UE_DOUBLE_PI;
	return -10.0 * std::log10(SolidAngle) - 20.0 * std::log10(DistanceM);
}

FLevel OutdoorContribution(const FLevel& SourcePower, TOptional<double> TransferDb)
{
	FLevel Result = SourcePower;
	Result.Quantity = EQuantity::SoundPressure;
	if (SourcePower.Quantity != EQuantity::SoundPower || SourcePower.Weighting != EWeighting::A
		|| (SourcePower.State == ELevelState::Known && (!SourcePower.Db.IsSet() || !std::isfinite(*SourcePower.Db)
			|| SourcePower.KnownSources <= 0 || SourcePower.MissingSources < 0
			|| SourcePower.MissingSources > MAX_int32 - SourcePower.KnownSources
			|| (TransferDb.IsSet() && !std::isfinite(*SourcePower.Db + *TransferDb)))))
	{
		Result.State = ELevelState::InvalidInput;
		Result.Db.Reset();
		return Result;
	}
	if (SourcePower.State != ELevelState::Known) return Result;
	if (!TransferDb.IsSet())
	{
		Result.MissingSources += Result.KnownSources;
		Result.KnownSources = 0;
		Result.State = ELevelState::MissingData;
		Result.Db.Reset();
		return Result;
	}
	Result.Db = *Result.Db + *TransferDb;
	return Result;
}
}
