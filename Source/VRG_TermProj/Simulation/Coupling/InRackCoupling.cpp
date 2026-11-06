#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Core/ElapsedTimer.h"
#include "HAL/IConsoleManager.h"
#include "Async/ParallelFor.h"

namespace
{
	// Measurement switch for the A/B of the exact tray-transport reuse (2026-11-06). Results are the same either way.
	TAutoConsoleVariable<int32> CVarTrayTransportReuse(TEXT("vrg.Perf.TrayTransportReuse"), 1,
		TEXT("랙 내부 수송 계수 재사용(입력이 비트 단위로 같을 때만). 1 = 켬(기본), 0 = 매 스텝 다시 만듦(비교용)"));
	// Rack-parallel tray transport, tray step limits and rack models (2026-11-06). Each rack writes only its own entries and
	// every sum is then taken in rack order, so results are bit-identical to the serial loop. 0 = serial, for comparison.
	TAutoConsoleVariable<int32> CVarRackParallel(TEXT("vrg.Perf.RackParallel"), 1,
		TEXT("랙 단위 계산(수송 계수·트레이 스텝 한계·랙 열모델)을 랙마다 병렬로. 결과는 순차와 같다. 1 = 켬(기본), 0 = 순차(비교용)"));
}

bool FCouplingBridge::IsRackParallel() const
{
	return bRackParallel && CVarRackParallel.GetValueOnAnyThread() != 0;
}

bool FCouplingBridge::PrepareTrayTransport()
{
	FElapsedTimer Time(CouplingStageMs[0]);
	bool bHasInternal = false;
	for (const auto& Link : Links) bHasInternal |= !Link.Internal.Slots.IsEmpty();
	if (!bHasInternal) return true;
	if (!HydraulicNetwork) return false;
	// W3.5 measured hotspot: per-node queries repeatedly scanned every pipe in the room.
	// Recompute once for this solved state/step, never cache across a hydraulic change.
	TArray<double> NodeImbalancesM3s;
	HydraulicNetwork->GetNodeImbalancesM3s(NodeImbalancesM3s);
	LastTrayTransportReuses = LastTrayTransportBuilds = 0;
	const FHydraulicNetwork& Net = *HydraulicNetwork;
	const bool bReuse = bReuseTrayTransport && CVarTrayTransportReuse.GetValueOnGameThread() != 0;
	// Per rack: 0 skipped, 1 reused, 2 built, -1 failed. Racks only read the solved network and write their own entries.
	TArray<int8> Outcome; Outcome.Init(0, Targets.Num());
	ParallelFor(Targets.Num(), [&](int32 R)
	{
		if (Links[R].Internal.Slots.IsEmpty()) return;
		if (!Targets[R].Model || !Targets[R].Input || !Targets[R].Model->UsesTrayCoolant()) { Outcome[R] = -1; return; }
		const FInRackHandles& H = Links[R].Internal;
		const double CpJKgK = Targets[R].Model->GetParams().CoolantCpJKgK;
		// Everything InRackTransport::Build reads, in a fixed order: when it is bit-identical (memcmp, so -0 and +0 differ)
		// the builder would return the same bits, so its last output is copied instead. Any hydraulic change misses.
		TArray<double> Key;
		bool bKeyValid = bReuse && TrayTransportKeys.IsValidIndex(R) && Links[R].ReturnPipeIndex >= 0 && Links[R].ReturnPipeIndex < Net.GetNumPipes();
		if (bKeyValid)
		{
			Key.Add(CoolantDensityKgM3); Key.Add(CpJKgK); Key.Add(Links[R].ReturnPipeIndex); Key.Add(bIndexedTransport ? 1.0 : 0.0);
			Key.Add(Net.GetPipeFlowM3s(Links[R].ReturnPipeIndex));
			for (int32 P : H.Pipes) { bKeyValid &= P >= 0 && P < Net.GetNumPipes(); if (!bKeyValid) break; Key.Add(P); Key.Add(Net.GetPipeFlowM3s(P)); Key.Add(Net.GetPipeFromNodeIndex(P)); Key.Add(Net.GetPipeToNodeIndex(P)); }
			for (const TArray<int32>* List : { &H.InletPipes, &H.OutletPipes }) for (int32 P : *List) { bKeyValid &= P >= 0 && P < Net.GetNumPipes(); if (!bKeyValid) break; Key.Add(P); Key.Add(Net.GetPipeFlowM3s(P)); }
			for (int32 L : H.Leaks) { Key.Add(L); Key.Add(Net.GetLeakFlowM3s(L)); }
			for (int32 Node : H.Nodes) { bKeyValid &= NodeImbalancesM3s.IsValidIndex(Node); if (!bKeyValid) break; Key.Add(Node); Key.Add(NodeImbalancesM3s[Node]); }
			for (int32 Node : { H.SupplyNode, H.ReturnNode }) { bKeyValid &= NodeImbalancesM3s.IsValidIndex(Node); if (bKeyValid) { Key.Add(Node); Key.Add(NodeImbalancesM3s[Node]); } }
		}
		const TArray<double>& Old = bKeyValid ? TrayTransportKeys[R] : Key;
		if (bKeyValid && Old.Num() == Key.Num() && Key.Num() > 0 && FMemory::Memcmp(Old.GetData(), Key.GetData(), Key.Num() * sizeof(double)) == 0)
		{
			TrayInputs[R] = TrayTransportBuilt[R];
			Outcome[R] = 1;
			return;
		}
		if (!InRackTransport::Build(Net, H, Links[R].ReturnPipeIndex, CoolantDensityKgM3, CpJKgK, TrayInputs[R], &NodeImbalancesM3s, bIndexedTransport))
		{
			if (TrayTransportKeys.IsValidIndex(R)) TrayTransportKeys[R].Reset();
			Outcome[R] = -1;
			return;
		}
		Outcome[R] = 2;
		if (bKeyValid) { TrayTransportKeys[R] = MoveTemp(Key); TrayTransportBuilt[R] = TrayInputs[R]; }
		else if (TrayTransportKeys.IsValidIndex(R)) TrayTransportKeys[R].Reset();
	}, IsRackParallel() ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
	for (const int8 O : Outcome)
	{
		if (O < 0) return false;
		LastTrayTransportReuses += O == 1; LastTrayTransportBuilds += O == 2;
	}
	return true;
}

void FCouplingBridge::Step(double DtS, bool bResolved, bool bConverged)
{
	for (double& Ms : CouplingStageMs) Ms=0; for (double& Ms : AirStageMs) Ms=0;
	LastCoupledSubsteps = 0; CpuPressureCosts={};
	if (!(DtS > 0.0) || !FMath::IsFinite(DtS)) return;
	if (!ensureMsgf(PrepareTrayTransport(), TEXT("Invalid solved rack transport; coupled step rejected"))) return;
	bool bHasInternal = false;
	for (const auto& L : Links) bHasInternal |= !L.Internal.Slots.IsEmpty();
	if (!bHasInternal) { StepOnce(DtS, bResolved, bConverged); LastCoupledSubsteps = 1; return; }

	// A common explicit step lets HX rejection, local leaks and tray exchanges all read
	// the same old temperatures. Letting individual racks subcycle under a frozen HX
	// would silently create a global energy residual. Air advances over this same interval.
	double Remaining = DtS, ItJ = 0.0, LossJ = 0.0, FanJ = 0.0, LeakJ = 0.0, PumpJ = 0.0, RejectedJ = 0.0, AirJ = 0.0;
	double AirMs = 0.0, RackMs = 0.0;
	TArray<double> DutyJ; DutyJ.Init(0.0, CduGroups.Num());
	TArray<double> InternalLeakJ; InternalLeakJ.Init(0.0, LastInternalLeakHeatW.Num());
	while (Remaining > 0.0)
	{
		double StepS = Remaining;
		{ FElapsedTimer Time(CouplingStageMs[1]);
		// Each rack's limit in parallel (read only), then the minimum in rack order as before.
		TArray<double> Limit; Limit.Init(StepS, Targets.Num());
		ParallelFor(Targets.Num(), [&](int32 R)
		{
			if (!Links[R].Internal.Slots.IsEmpty()) Limit[R] = Targets[R].Model->LimitTrayStepS(*Targets[R].Input, TrayInputs[R], Remaining);
		}, IsRackParallel() ? EParallelForFlags::None : EParallelForFlags::ForceSingleThread);
		for (int32 R = 0; R < Targets.Num(); ++R)
			if (!Links[R].Internal.Slots.IsEmpty()) StepS = FMath::Min(StepS, Limit[R]);
		}
		if (!ensureMsgf(StepS > 0.0 && Remaining - StepS != Remaining, TEXT("No stable coupled interval"))) return;
		StepOnce(StepS, bResolved && LastCoupledSubsteps == 0, bConverged);
		ItJ += LastItHeatW * StepS; LossJ += LastConversionLossW * StepS; FanJ += LastFanPowerW * StepS;
		LeakJ += LastLeakHeatW * StepS; PumpJ += LastCduPumpHeatW * StepS;
		RejectedJ += LastRadiatorRejectedW * StepS; AirJ += LastAirCoolingRemovedW * StepS;
		AirMs += LastAirSolverMs; RackMs += LastRackModelsMs;
		for (int32 G = 0; G < DutyJ.Num(); ++G) DutyJ[G] += LastCduDutyW[G] * StepS;
		for (int32 K = 0; K < InternalLeakJ.Num(); ++K) InternalLeakJ[K] += LastInternalLeakHeatW[K] * StepS;
		Remaining -= StepS; ++LastCoupledSubsteps;
	}
	LastItHeatW = ItJ / DtS; LastConversionLossW = LossJ / DtS; LastFanPowerW = FanJ / DtS;
	LastLeakHeatW = LeakJ / DtS; LastCduPumpHeatW = PumpJ / DtS;
	LastRadiatorRejectedW = RejectedJ / DtS; LastAirCoolingRemovedW = AirJ / DtS;
	LastAirSolverMs = AirMs; LastRackModelsMs = RackMs;
	for (int32 G = 0; G < DutyJ.Num(); ++G) LastCduDutyW[G] = DutyJ[G] / DtS;
	for (int32 K = 0; K < InternalLeakJ.Num(); ++K) LastInternalLeakHeatW[K] = InternalLeakJ[K] / DtS;
}
