#pragma once
#include "Simulation/Hydraulics/InRackNetwork.h"
#include "Simulation/Thermal/TrayCoolantModel.h"

namespace InRackTransport
{
	// Optional local diagnosis only; null in production. Stages include observer overhead.
	struct FBuildCosts { double SetupMs=0, LookupMs=0, MixingMs=0, AssemblyMs=0; int32 EdgeVisits=0; };
	// Only solved signed flows are consumed. ReturnPipe is the edge leaving return storage,
	// before any external return leak; outer supply/return losses stay outside this control volume.
	bool Build(const FHydraulicNetwork& Net, const FInRackHandles& H, int32 ReturnPipe,
		double DensityKgM3, double CpJKgK, FTrayCoolantInput& Out, const TArray<double>* NodeImbalancesM3s = nullptr,
		bool bIndexedTraversal = false, FBuildCosts* Costs = nullptr);
}
