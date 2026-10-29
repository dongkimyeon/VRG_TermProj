#pragma once
#include "Simulation/Hydraulics/InRackNetwork.h"
#include "Simulation/Thermal/TrayCoolantModel.h"

namespace InRackTransport
{
	// Only solved signed flows are consumed. ReturnPipe is the edge leaving return storage,
	// before any external return leak; outer supply/return losses stay outside this control volume.
	bool Build(const FHydraulicNetwork& Net, const FInRackHandles& H, int32 ReturnPipe,
		double DensityKgM3, double CpJKgK, FTrayCoolantInput& Out);
}
