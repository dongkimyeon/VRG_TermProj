// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Hydraulics/FacilityWaterLoop.h"

bool FFacilityWaterLoop::Build(const FFacilityLoopSpec& InSpec)
{
	bBuilt = false;
	Network.Reset();
	Handles = FFacilityLoopHandles();
	Openings.Reset(); Running.Reset(); ValveOpenResistance.Reset();
	Spec = InSpec;
	double TotalM3s = 0.0;
	for (const double Q : Spec.CduDesignFlowM3s)
	{
		if (!(Q > 0.0)) return false;   // every branch needs a design flow to be sized
		TotalM3s += Q;
	}
	// F1 builds the separate FWS only; the shared and series topologies join this builder in F6.
	if (TotalM3s <= 0.0 || Spec.Topology != EPlantTopology::Separate || Spec.DutyPumps < 1) return false;
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

	// CDU branches: every element's resistance from its design drop at the branch design flow (dp = R Q^2).
	const double Authority = FMath::Clamp(Spec.ValveAuthority, 0.05, 0.95);
	const double RestDropPa = Spec.BranchRunDesignDropPa + Spec.CduHxDesignDropPa;
	const double ValveOpenDropPa = Authority / (1.0 - Authority) * RestDropPa;
	const double BranchDropPa = RestDropPa + ValveOpenDropPa;
	for (const double Q : Spec.CduDesignFlowM3s)
	{
		const double Q2 = Q * Q;
		const int32 A = AddNode(false, Spec.SuctionPressurePa), B = AddNode(false, Spec.SuctionPressurePa), C = AddNode(false, Spec.SuctionPressurePa);
		Handles.BranchSupplyRunPipes.Add(AddPipe(Handles.SupplyHeaderNode, A, 0.5 * Spec.BranchRunDesignDropPa / Q2));
		ValveOpenResistance.Add(ValveOpenDropPa / Q2);
		Handles.ValvePipes.Add(AddPipe(A, B, ValveOpenDropPa / Q2));
		Handles.HxPipes.Add(AddPipe(B, C, Spec.CduHxDesignDropPa / Q2));
		Handles.BranchReturnRunPipes.Add(AddPipe(C, Handles.ReturnHeaderNode, 0.5 * Spec.BranchRunDesignDropPa / Q2));
		Openings.Add(1.0);
	}

	// Mains between the plant and the headers.
	const double Total2 = TotalM3s * TotalM3s;
	Handles.SupplyMainPipe = AddPipe(Handles.PlantSupplyNode, Handles.SupplyHeaderNode, 0.5 * Spec.MainDesignDropPa / Total2);
	Handles.ReturnMainPipe = AddPipe(Handles.ReturnHeaderNode, Handles.PlantReturnNode, 0.5 * Spec.MainDesignDropPa / Total2);

	// Dry cooler coils in parallel back to the pump suction.
	const double RatedQ = FMath::Max(Spec.DryCoolerRatedFlowM3s, UE_DOUBLE_SMALL_NUMBER);
	const int32 NumDry = Spec.DryCoolers > 0 ? Spec.DryCoolers : FMath::Max(1, FMath::CeilToInt32(TotalM3s / RatedQ - 1e-9));
	const double DryR = Spec.DryCoolerRatedDropPa / (RatedQ * RatedQ);
	for (int32 K = 0; K < NumDry; ++K) Handles.DryCoolerPipes.Add(AddPipe(Handles.PlantReturnNode, Handles.SuctionNode, DryR));
	const double PerDryQ = TotalM3s / NumDry;
	const double DryDropPa = DryR * PerDryQ * PerDryQ;

	// Pumps sized so the design point is exactly speed 1 at the design flows:
	//   H_design = check + mains + branch (valve fully open) + dry cooler,   H0 = k * H_design,
	//   C = (H0 - H_design) / Q_pump^2 with Q_pump = total / duty pumps.
	const double PumpQ = TotalM3s / Spec.DutyPumps;
	const double CheckR = Spec.CheckValveDesignDropPa / (PumpQ * PumpQ);
	DesignPumpHeadPa = Spec.CheckValveDesignDropPa + Spec.MainDesignDropPa + BranchDropPa + DryDropPa;
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
		if (I >= Spec.DutyPumps) Network.SetPipeClosed(Handles.PumpCheckPipes.Last(), true);
	}
	// The differential-pressure setpoint is the branch drop at design (every branch has the same design drop).
	DpSetpointPa = BranchDropPa;
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
	if (!Running.IsValidIndex(Pump)) return;
	Running[Pump] = bRunning;
	Network.SetPipeClosed(Handles.PumpCheckPipes[Pump], !bRunning);
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
