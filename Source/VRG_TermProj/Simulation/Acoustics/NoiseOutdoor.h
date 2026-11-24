#pragma once

#include "Simulation/Acoustics/NoiseSource.h"

namespace VrgNoise
{
enum class EOutdoorRadiation : uint8 { Sphere, Hemisphere };

struct FOutdoorReceiver
{
	FVector PositionM = FVector::ZeroVector;
	EOutdoorRadiation Radiation = EOutdoorRadiation::Sphere;

	bool IsValid() const;
};

// Null result means invalid input or a receiver inside the point-source exclusion radius.
TOptional<double> OutdoorTransferDb(const FOutdoorReceiver& Receiver, const FVector& SourceM, double MinDistanceM);

// Converts one A-weighted source to a receiver contribution. No transfer for an active source
// means out of range and contributes to the missing count, rather than to silence.
FLevel OutdoorContribution(const FLevel& SourcePower, TOptional<double> TransferDb);

struct FOutdoorSample
{
	double SimTimeS = 0.0;
	FLevel Pressure;
	int32 NearSources = 0;
	bool bPlantRunning = false;
	double FwsSupplyK = 0.0;
	double CwSupplyK = 0.0;
	double DryCoolerFanW = 0.0;
	double DryCoolerRejectedW = 0.0;
};
}
