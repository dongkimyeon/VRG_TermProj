// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/FacilityWaterLoop.h"

bool FFacilityWaterLoop::Build(const FFacilityLoopSpec& InSpec)
{
	bBuilt = false;
	Network.Reset();
	Handles = FFacilityLoopHandles();
	Openings.Reset(); Running.Reset(); Tripped.Reset(); ValveOpenResistance.Reset(); LoopLeaks.Reset();
	Spec = InSpec;
	double FirstM3s = 0.0, SecondM3s = 0.0;
	for (const double Q : Spec.BranchDesignFlowM3s)
	{
		if (!(Q > 0.0)) return false;   // every branch needs a design flow to be sized
		FirstM3s += Q;
	}
	for (const double Q : Spec.SecondStageDesignFlowM3s)
	{
		if (!(Q > 0.0)) return false;
		SecondM3s += Q;
	}
	for (const double Q : Spec.StandbyBranchDesignFlowM3s) if (!(Q > 0.0)) return false;
	const bool bSeries = Spec.SecondStageDesignFlowM3s.Num() > 0;
	// Every topology uses this one builder (F6): one stage of branches (separate / shared) or two in series.
	const double TotalM3s = bSeries ? FMath::Max(FirstM3s, SecondM3s) : FirstM3s;
	if (TotalM3s <= 0.0 || Spec.DutyPumps < 1) return false;
	DesignTotalFlowM3s = TotalM3s;

	auto AddNode = [this](bool bFixed, double PressurePa)
	{
		FHydraulicNode N;
		N.bFixedPressure = bFixed;
		N.PressurePa = PressurePa;
		return Network.AddNode(N);
	};
	auto AddPipe = [this](int32 From, int32 To, double R)
	{
		FHydraulicPipe P;
		P.FromNodeIndex = From; P.ToNodeIndex = To; P.ResistancePaS2M6 = R;
		return Network.AddPipe(P);
	};

	Handles.SuctionNode = AddNode(true, Spec.SuctionPressurePa);
	Handles.PlantSupplyNode = AddNode(false, Spec.SuctionPressurePa);
	Handles.SupplyHeaderNode = AddNode(false, Spec.SuctionPressurePa);
	Handles.ReturnHeaderNode = AddNode(false, Spec.SuctionPressurePa);
	Handles.PlantReturnNode = AddNode(false, Spec.SuctionPressurePa);
	if (bSeries) Handles.MiddleNode = AddNode(false, Spec.SuctionPressurePa);

	// Branches: every element's resistance from its design drop at the branch design flow (dp = R Q^2).
	const double Authority = FMath::Clamp(Spec.ValveAuthority, 0.05, 0.95);
	const double RestDropPa = Spec.BranchRunDesignDropPa + Spec.BranchHxDesignDropPa;
	const double ValveOpenDropPa = Authority / (1.0 - Authority) * RestDropPa;
	const double BranchDropPa = RestDropPa + ValveOpenDropPa;
	auto AddBranch = [&](double Q, int32 From, int32 To)
	{
		const double Q2 = Q * Q;
		const int32 A = AddNode(false, Spec.SuctionPressurePa), B = AddNode(false, Spec.SuctionPressurePa), C = AddNode(false, Spec.SuctionPressurePa);
		Handles.BranchSupplyRunPipes.Add(AddPipe(From, A, 0.5 * Spec.BranchRunDesignDropPa / Q2));
		ValveOpenResistance.Add(ValveOpenDropPa / Q2);
		Handles.ValvePipes.Add(AddPipe(A, B, ValveOpenDropPa / Q2));
		Handles.HxPipes.Add(AddPipe(B, C, Spec.BranchHxDesignDropPa / Q2));
		Handles.BranchReturnRunPipes.Add(AddPipe(C, To, 0.5 * Spec.BranchRunDesignDropPa / Q2));
		Openings.Add(1.0);
	};
	const int32 FirstTo = bSeries ? Handles.MiddleNode : Handles.ReturnHeaderNode;
	for (const double Q : Spec.BranchDesignFlowM3s) AddBranch(Q, Handles.SupplyHeaderNode, FirstTo);
	Handles.NumFirstStage = Spec.BranchDesignFlowM3s.Num();
	double SeriesSecondDropPa = 0.0;
	if (bSeries)
	{
		// The second stage carries the whole loop flow; at design each of its branches runs at its share of it.
		for (const double Q : Spec.SecondStageDesignFlowM3s) AddBranch(Q, Handles.MiddleNode, Handles.ReturnHeaderNode);
		const double Scale = TotalM3s / SecondM3s;
		SeriesSecondDropPa = BranchDropPa * Scale * Scale;
		// Bypass around the first stage for what the second stage needs beyond it (fixed, sized open at design).
		if (SecondM3s > FirstM3s)
		{
			const double Qb = SecondM3s - FirstM3s;
			Handles.BypassPipe = AddPipe(Handles.SupplyHeaderNode, Handles.MiddleNode, BranchDropPa / (Qb * Qb));
		}
	}
	// Standby branches: sized like a duty branch, valve shut (isolated), after every other branch so duty indices keep.
	for (const double Q : Spec.StandbyBranchDesignFlowM3s)
	{
		AddBranch(Q, Handles.SupplyHeaderNode, Handles.ReturnHeaderNode);
		SetValveOpening(Openings.Num() - 1, 0.0);
	}
	Handles.NumStandbyBranches = Spec.StandbyBranchDesignFlowM3s.Num();

	// Mains between the plant and the headers.
	const double Total2 = TotalM3s * TotalM3s;
	const double SeriesHxDropPa = FMath::Max(Spec.SeriesHxDesignDropPa, 0.0);
	if (SeriesHxDropPa > 0.0)
	{
		// The economizer side splits one main: its node sits between the main and the exchanger.
		Handles.SeriesHxNode = AddNode(false, Spec.SuctionPressurePa);
		if (Spec.bSeriesHxOnSupply)
		{
			Handles.SeriesHxPipe = AddPipe(Handles.PlantSupplyNode, Handles.SeriesHxNode, SeriesHxDropPa / Total2);
			Handles.SupplyMainPipe = AddPipe(Handles.SeriesHxNode, Handles.SupplyHeaderNode, 0.5 * Spec.MainDesignDropPa / Total2);
			Handles.ReturnMainPipe = AddPipe(Handles.ReturnHeaderNode, Handles.PlantReturnNode, 0.5 * Spec.MainDesignDropPa / Total2);
		}
		else
		{
			Handles.SupplyMainPipe = AddPipe(Handles.PlantSupplyNode, Handles.SupplyHeaderNode, 0.5 * Spec.MainDesignDropPa / Total2);
			Handles.ReturnMainPipe = AddPipe(Handles.ReturnHeaderNode, Handles.SeriesHxNode, 0.5 * Spec.MainDesignDropPa / Total2);
			Handles.SeriesHxPipe = AddPipe(Handles.SeriesHxNode, Handles.PlantReturnNode, SeriesHxDropPa / Total2);
		}
	}
	else
	{
		Handles.SupplyMainPipe = AddPipe(Handles.PlantSupplyNode, Handles.SupplyHeaderNode, 0.5 * Spec.MainDesignDropPa / Total2);
		Handles.ReturnMainPipe = AddPipe(Handles.ReturnHeaderNode, Handles.PlantReturnNode, 0.5 * Spec.MainDesignDropPa / Total2);
	}

	// Dry cooler coils in parallel back to the pump suction.
	const double RatedQ = FMath::Max(Spec.ReturnCoilRatedFlowM3s, UE_DOUBLE_SMALL_NUMBER);
	const int32 NumDry = Spec.ReturnCoils > 0 ? Spec.ReturnCoils : FMath::Max(1, FMath::CeilToInt32(TotalM3s / RatedQ - 1e-9));
	const double DryR = Spec.ReturnCoilRatedDropPa / (RatedQ * RatedQ);
	for (int32 K = 0; K < NumDry; ++K) Handles.ReturnCoilPipes.Add(AddPipe(Handles.PlantReturnNode, Handles.SuctionNode, DryR));
	for (int32 K = 0; K < Spec.StandbyReturnCoils; ++K)
	{
		Handles.ReturnCoilPipes.Add(AddPipe(Handles.PlantReturnNode, Handles.SuctionNode, DryR));
		Network.SetPipeClosed(Handles.ReturnCoilPipes.Last(), true);
	}
	Handles.NumStandbyCoils = FMath::Max(Spec.StandbyReturnCoils, 0);
	const double PerDryQ = TotalM3s / NumDry;
	const double DryDropPa = DryR * PerDryQ * PerDryQ;

	// Pumps sized so the design point is exactly speed 1 at the design flows:
	//   H_design = check + mains + branch (valve fully open) + dry cooler,   H0 = k * H_design,
	//   C = (H0 - H_design) / Q_pump^2 with Q_pump = total / duty pumps.
	const double PumpQ = TotalM3s / Spec.DutyPumps;
	const double CheckR = Spec.CheckValveDesignDropPa / (PumpQ * PumpQ);
	DesignPumpHeadPa = Spec.CheckValveDesignDropPa + Spec.MainDesignDropPa + SeriesHxDropPa + BranchDropPa + SeriesSecondDropPa + DryDropPa;
	PumpShutoffPa = Spec.PumpShutoffToDesignHead * DesignPumpHeadPa;
	PumpCurveCoeff = (PumpShutoffPa - DesignPumpHeadPa) / (PumpQ * PumpQ);
	for (int32 I = 0; I < Spec.DutyPumps + Spec.StandbyPumps; ++I)
	{
		const int32 Discharge = AddNode(false, Spec.SuctionPressurePa);
		FHydraulicPump P;
		P.FromNodeIndex = Handles.SuctionNode; P.ToNodeIndex = Discharge;
		P.ShutoffHeadPa = PumpShutoffPa; P.CurveCoeffPaS2M6 = PumpCurveCoeff;
		P.Efficiency = Spec.PumpEfficiency; P.SpeedRatio = 1.0;
		Handles.Pumps.Add(Network.AddPump(P));
		Handles.PumpCheckPipes.Add(AddPipe(Discharge, Handles.PlantSupplyNode, CheckR));
		Running.Add(I < Spec.DutyPumps);
		Tripped.Add(false);
		if (I >= Spec.DutyPumps) Network.SetPipeClosed(Handles.PumpCheckPipes.Last(), true);
	}
	// The differential-pressure setpoint is the header-to-header drop at design (every branch of a stage has the same
	// design drop; the series topology adds its second stage).
	DpSetpointPa = BranchDropPa + SeriesSecondDropPa;
	bBuilt = true;
	return true;
}

double FFacilityWaterLoop::ValveResistancePaS2M6(int32 Branch, double Opening) const
{
	if (!ValveOpenResistance.IsValidIndex(Branch)) return 0.0;
	if (Opening <= 0.0) return TNumericLimits<double>::Max();
	const double X = FMath::Min(Opening, 1.0);
	return ValveOpenResistance[Branch] * FMath::Pow(FMath::Max(Spec.ValveRangeability, 1.0), 2.0 * (1.0 - X));
}

double FFacilityWaterLoop::BranchResistancePaS2M6(int32 Branch, double Opening) const
{
	if (!Handles.ValvePipes.IsValidIndex(Branch) || Opening <= 0.0) return TNumericLimits<double>::Max();
	return Network.GetPipeResistancePaS2M6(Handles.BranchSupplyRunPipes[Branch]) + ValveResistancePaS2M6(Branch, Opening)
		+ Network.GetPipeResistancePaS2M6(Handles.HxPipes[Branch]) + Network.GetPipeResistancePaS2M6(Handles.BranchReturnRunPipes[Branch]);
}

void FFacilityWaterLoop::ApplyValve(int32 Branch)
{
	const int32 Pipe = Handles.ValvePipes[Branch];
	const bool bShut = Openings[Branch] <= 0.0;
	Network.SetPipeClosed(Pipe, bShut);
	if (!bShut) Network.SetPipeResistancePaS2M6(Pipe, ValveResistancePaS2M6(Branch, Openings[Branch]));
}

void FFacilityWaterLoop::SetValveOpening(int32 Branch, double Opening)
{
	if (!Openings.IsValidIndex(Branch)) return;
	Openings[Branch] = FMath::Clamp(Opening, 0.0, 1.0);
	ApplyValve(Branch);
}

void FFacilityWaterLoop::SetPumpRunning(int32 Pump, bool bRunning)
{
	if (!Running.IsValidIndex(Pump) || (bRunning && Tripped[Pump])) return;   // a tripped pump does not restart
	Running[Pump] = bRunning;
	Network.SetPipeClosed(Handles.PumpCheckPipes[Pump], !bRunning);
}

int32 FFacilityWaterLoop::TripPump(int32 Pump)
{
	if (!Running.IsValidIndex(Pump) || Tripped[Pump]) return INDEX_NONE;
	const bool bWasRunning = Running[Pump];
	Tripped[Pump] = true;
	SetPumpRunning(Pump, false);
	if (!bWasRunning) return INDEX_NONE;   // an idle standby failed: nothing to replace
	for (int32 I = 0; I < Running.Num(); ++I)
	{
		if (!Running[I] && !Tripped[I]) { SetPumpRunning(I, true); return I; }
	}
	return INDEX_NONE;
}

int32 FFacilityWaterLoop::GetNumRunningPumps() const
{
	int32 N = 0;
	for (const bool B : Running) N += B ? 1 : 0;
	return N;
}

int32 FFacilityWaterLoop::AddLeak(EPlantLeakSite Site, int32 Branch, double HoleDiameterM, double DischargeCoefficient)
{
	if (!bBuilt) return INDEX_NONE;
	int32 Node = INDEX_NONE;
	switch (Site)
	{
	case EPlantLeakSite::SupplyHeader: Node = Handles.SupplyHeaderNode; break;
	case EPlantLeakSite::ReturnHeader: Node = Handles.ReturnHeaderNode; break;
	case EPlantLeakSite::BranchSupply:
		if (Handles.BranchSupplyRunPipes.IsValidIndex(Branch)) Node = Network.GetPipeToNodeIndex(Handles.BranchSupplyRunPipes[Branch]);
		break;
	case EPlantLeakSite::BranchReturn:
		if (Handles.BranchReturnRunPipes.IsValidIndex(Branch)) Node = Network.GetPipeFromNodeIndex(Handles.BranchReturnRunPipes[Branch]);
		break;
	}
	if (Node == INDEX_NONE) return INDEX_NONE;
	FHydraulicLeak L;
	L.NodeIndex = Node;
	L.HoleDiameterM = FMath::Max(HoleDiameterM, 0.0);
	L.DischargeCoefficient = FMath::Clamp(DischargeCoefficient, 0.01, 1.0);
	FPlantLeak P;
	P.Site = Site;
	P.Branch = (Site == EPlantLeakSite::BranchSupply || Site == EPlantLeakSite::BranchReturn) ? Branch : INDEX_NONE;
	P.NetworkLeak = Network.AddLeak(L);
	P.bSupplySide = Site == EPlantLeakSite::SupplyHeader || Site == EPlantLeakSite::BranchSupply;
	return LoopLeaks.Add(P);
}

void FFacilityWaterLoop::SetLeakDiameterM(int32 Leak, double HoleDiameterM)
{
	if (LoopLeaks.IsValidIndex(Leak)) Network.SetLeakHoleDiameterM(LoopLeaks[Leak].NetworkLeak, HoleDiameterM);
}

void FFacilityWaterLoop::ClearLeaks()
{
	for (const FPlantLeak& L : LoopLeaks) Network.SetLeakHoleDiameterM(L.NetworkLeak, 0.0);
}

double FFacilityWaterLoop::GetLeakFlowM3s(int32 Leak) const
{
	return LoopLeaks.IsValidIndex(Leak) ? Network.GetLeakFlowM3s(LoopLeaks[Leak].NetworkLeak) : 0.0;
}

double FFacilityWaterLoop::GetSupplySideLeakM3s() const
{
	double Sum = 0.0;
	for (const FPlantLeak& L : LoopLeaks) if (L.bSupplySide) Sum += Network.GetLeakFlowM3s(L.NetworkLeak);
	return Sum;
}

double FFacilityWaterLoop::GetReturnSideLeakM3s() const
{
	double Sum = 0.0;
	for (const FPlantLeak& L : LoopLeaks) if (!L.bSupplySide) Sum += Network.GetLeakFlowM3s(L.NetworkLeak);
	return Sum;
}

bool FFacilityWaterLoop::HasOpenLeak() const
{
	for (const FPlantLeak& L : LoopLeaks) if (Network.GetLeak(L.NetworkLeak).HoleDiameterM > 0.0) return true;
	return false;
}

double FFacilityWaterLoop::HeaderDeltaPa() const
{
	return Network.GetNodePiezometricPa(Handles.SupplyHeaderNode) - Network.GetNodePiezometricPa(Handles.ReturnHeaderNode);
}

FHydraulicSolveResult FFacilityWaterLoop::SolveAtSpeed(const FHydraulicSolveSettings& Settings, double SpeedRatio)
{
	for (int32 I = 0; I < Handles.Pumps.Num(); ++I) Network.SetPumpSpeedRatio(Handles.Pumps[I], Running[I] ? SpeedRatio : 0.0);
	return Network.Solve(Settings);
}

FFacilityLoopResult FFacilityWaterLoop::Solve(const FHydraulicSolveSettings& Settings, double ControlTolerance, int32 MaxControlIterations)
{
	FFacilityLoopResult R;
	R.SetpointPa = DpSetpointPa;
	if (!bBuilt) return R;
	bool bAny = false;
	for (const bool B : Running) bAny |= B;
	if (!bAny)
	{
		R.bNoPumpRunning = true;
		R.Hydraulics = SolveAtSpeed(Settings, 0.0);
		R.HeaderDeltaPPa = HeaderDeltaPa();
		R.ControlResidual = DpSetpointPa > 0.0 ? FMath::Abs(R.HeaderDeltaPPa - DpSetpointPa) / DpSetpointPa : 0.0;
		return R;
	}
	// f(r) = dp(r) - setpoint rises with the speed (pump head ~ r^2 against fixed resistances). Bracket, then
	// false position with the Illinois correction (keeps the bracket, converges superlinearly).
	auto Eval = [&](double Speed, FHydraulicSolveResult& Out) { Out = SolveAtSpeed(Settings, Speed); return HeaderDeltaPa() - DpSetpointPa; };
	const double Scale = FMath::Max(DpSetpointPa, 1.0);
	double Lo = FMath::Max(Spec.MinPumpSpeed, 0.0), Hi = FMath::Max(Spec.MaxPumpSpeed, Lo);
	FHydraulicSolveResult HLo, HHi;
	const double FHi = Eval(Hi, HHi);
	R.ControlIterations = 1;
	if (FHi < 0.0)
	{
		R.bSpeedSaturatedHigh = true;
		R.Hydraulics = HHi; R.PumpSpeedRatio = Hi; R.HeaderDeltaPPa = FHi + DpSetpointPa; R.ControlResidual = -FHi / Scale;
		return R;
	}
	const double FLo = Eval(Lo, HLo);
	R.ControlIterations = 2;
	if (FLo > 0.0)
	{
		R.bSpeedSaturatedLow = true;
		R.Hydraulics = HLo; R.PumpSpeedRatio = Lo; R.HeaderDeltaPPa = FLo + DpSetpointPa; R.ControlResidual = FLo / Scale;
		return R;
	}
	double A = Lo, FA = FLo, B = Hi, FB = FHi;
	int32 Side = 0;
	double X = B, FX = FB;
	FHydraulicSolveResult HX = HHi;
	while (R.ControlIterations < MaxControlIterations)
	{
		X = (FB - FA) != 0.0 ? B - FB * (B - A) / (FB - FA) : 0.5 * (A + B);
		if (!(X > A && X < B)) X = 0.5 * (A + B);
		FX = Eval(X, HX);
		++R.ControlIterations;
		if (FMath::Abs(FX) / Scale <= ControlTolerance || !HX.bConverged) break;
		if (FX > 0.0)
		{
			B = X; FB = FX;
			if (Side == 1) FA *= 0.5;
			Side = 1;
		}
		else
		{
			A = X; FA = FX;
			if (Side == -1) FB *= 0.5;
			Side = -1;
		}
	}
	R.Hydraulics = HX;
	R.PumpSpeedRatio = X;
	R.HeaderDeltaPPa = FX + DpSetpointPa;
	R.ControlResidual = FMath::Abs(FX) / Scale;
	R.bControlConverged = HX.bConverged && R.ControlResidual <= ControlTolerance;
	return R;
}

double FFacilityWaterLoop::GetBranchFlowM3s(int32 Branch) const
{
	return Handles.ValvePipes.IsValidIndex(Branch) ? Network.GetPipeFlowM3s(Handles.ValvePipes[Branch]) : 0.0;
}

double FFacilityWaterLoop::GetBranchDeltaPPa(int32 Branch) const
{
	if (!Handles.BranchSupplyRunPipes.IsValidIndex(Branch)) return 0.0;
	return Network.GetNodePiezometricPa(Network.GetPipeFromNodeIndex(Handles.BranchSupplyRunPipes[Branch]))
		- Network.GetNodePiezometricPa(Network.GetPipeToNodeIndex(Handles.BranchReturnRunPipes[Branch]));
}

double FFacilityWaterLoop::GetTotalPumpFlowM3s() const
{
	double Sum = 0.0;
	for (int32 I = 0; I < Handles.Pumps.Num(); ++I) if (Running[I]) Sum += Network.GetPumpFlowM3s(Handles.Pumps[I]);
	return Sum;
}

double FFacilityWaterLoop::GetTotalPumpElectricW() const
{
	double Sum = 0.0;
	for (int32 I = 0; I < Handles.Pumps.Num(); ++I) if (Running[I]) Sum += Network.GetPumpElectricalInputPowerW(Handles.Pumps[I]);
	return Sum;
}

double FFacilityWaterLoop::GetTotalPumpHydraulicW() const
{
	double Sum = 0.0;
	for (int32 I = 0; I < Handles.Pumps.Num(); ++I) if (Running[I]) Sum += Network.GetPumpHydraulicPowerW(Handles.Pumps[I]);
	return Sum;
}
