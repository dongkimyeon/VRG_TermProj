// Fill out your copyright notice in the Description page of Project Settings.

// S2b step 1 (Tools/S2b_누수_계획.md, 2026-10-20): leak orifice Flow = Cd A sqrt(2 (P - P_amb) / rho) at a node,
// gauge pressures. Closed forms (with and without a height difference), sqrt(dp) scaling, zero-area identity,
// one-way discharge, series pipe split, and mass conservation / plausibility on an NVL72 row.
// The CDU fixed node is still an unlimited reservoir here (step 2 adds the inventory), so these are static solves.

#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgLeakTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	FHydraulicNode LeakNode(bool bFixed, double PressurePa, double ElevationM)
	{
		FHydraulicNode Node;
		Node.bFixedPressure = bFixed; Node.PressurePa = PressurePa; Node.ElevationM = ElevationM;
		return Node;
	}

	FHydraulicPipe LeakPipe(int32 From, int32 To, double R)
	{
		FHydraulicPipe Pipe;
		Pipe.FromNodeIndex = From; Pipe.ToNodeIndex = To; Pipe.ResistancePaS2M6 = R;
		return Pipe;
	}

	FHydraulicLeak MakeLeak(int32 Node, double DiameterM)
	{
		FHydraulicLeak Leak;
		Leak.NodeIndex = Node; Leak.HoleDiameterM = DiameterM;
		return Leak;
	}

	// Source (fixed, z = 0) -> pipe R1 -> node at height H with a leak. Returns the solved leak flow.
	double SolveSourceLeak(double Ps, double R1, double H, double DiameterM, FHydraulicSolveResult& OutResult, double* OutPipeFlow = nullptr)
	{
		FHydraulicNetwork Net;
		const int32 Src = Net.AddNode(LeakNode(true, Ps, 0.0));
		const int32 M = Net.AddNode(LeakNode(false, 0.0, H));
		Net.AddPipe(LeakPipe(Src, M, R1));
		Net.AddLeak(MakeLeak(M, DiameterM));
		OutResult = Net.Solve(FHydraulicSolveSettings());
		if (OutPipeFlow) *OutPipeFlow = Net.GetPipeFlowM3s(0);
		return Net.GetLeakFlowM3s(0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicLeakAnalyticTest, "VRG.Hydraulics.Leak.OrificeAnalytic", VrgLeakTestFlags)
bool FHydraulicLeakAnalyticTest::RunTest(const FString& Parameters)
{
	// Synthetic test numbers (not a facility): 2 bar(g) source, 3 mm hole, pipe of the branch resistance scale.
	const double Rho = 997.0, G = FHydraulicNetwork::GravityMS2, Ps = 2.0e5, R1 = 1.7e10, D = 3.0e-3, H = 2.2;
	FHydraulicLeak Ref = MakeLeak(0, D);
	const double RL = FHydraulicNetwork::LeakResistancePaS2M6(Ref, Rho);
	const double CdA = Ref.DischargeCoefficient * 0.25 * UE_DOUBLE_PI * D * D;
	TestTrue(TEXT("R_leak = rho / (2 (Cd A)^2)"), FMath::Abs(RL - Rho / (2.0 * CdA * CdA)) / RL < 1e-15);

	// (a) Same level: Q = sqrt(Ps / (R1 + R_leak)); the leak carries exactly the pipe flow.
	FHydraulicSolveResult Ra;
	double PipeFlow = 0.0;
	const double Qa = SolveSourceLeak(Ps, R1, 0.0, D, Ra, &PipeFlow);
	const double Ea = FMath::Abs(Qa - FMath::Sqrt(Ps / (R1 + RL))) / FMath::Sqrt(Ps / (R1 + RL));
	AddInfo(FString::Printf(TEXT("(a) 누수 %.9e m3/s (해석 %.9e), 오차 %.3e, 배관-누수 차 %.3e, 반복 %d"),
		Qa, FMath::Sqrt(Ps / (R1 + RL)), Ea, FMath::Abs(PipeFlow - Qa) / Qa, Ra.IterationsUsed));
	TestTrue(TEXT("(a) converged"), Ra.bConverged);
	TestTrue(TEXT("(a) leak = sqrt(Ps/(R1+R_leak)) (rel < 1e-9)"), Ea < 1e-9);
	TestTrue(TEXT("(a) pipe flow = leak flow (rel < 1e-9)"), FMath::Abs(PipeFlow - Qa) / Qa < 1e-9);

	// (b) sqrt(dp): 4 x source pressure -> exactly 2 x leak.
	FHydraulicSolveResult Rb;
	const double Qb = SolveSourceLeak(4.0 * Ps, R1, 0.0, D, Rb);
	const double Eb = FMath::Abs(Qb / Qa - 2.0) / 2.0;
	AddInfo(FString::Printf(TEXT("(b) 압력 4배 누수 비 %.12f, 오차 %.3e"), Qb / Qa, Eb));
	TestTrue(TEXT("(b) 4x pressure -> 2x leak (rel < 1e-9)"), Rb.bConverged && Eb < 1e-9);

	// (c) Leak at height H above the source: static gauge pressure at the hole is lower by rho g H.
	//     Q = sqrt((Ps - rho g H) / (R1 + R_leak)).
	FHydraulicSolveResult Rc;
	const double Qc = SolveSourceLeak(Ps, R1, H, D, Rc);
	const double Ac = FMath::Sqrt((Ps - Rho * G * H) / (R1 + RL));
	const double Ec = FMath::Abs(Qc - Ac) / Ac;
	AddInfo(FString::Printf(TEXT("(c) 높이 %.1f m 누수 %.9e (해석 %.9e), 오차 %.3e, 바닥 대비 비 %.6f"), H, Qc, Ac, Ec, Qc / Qa));
	TestTrue(TEXT("(c) leak at height = sqrt((Ps - rho g H)/(R1+R_leak)) (rel < 1e-9)"), Rc.bConverged && Ec < 1e-9);

	// (d) Zero diameter: bit-identical to the network without the leak.
	{
		FHydraulicNetwork With, Without;
		for (FHydraulicNetwork* Net : { &With, &Without })
		{
			const int32 Src = Net->AddNode(LeakNode(true, Ps, 0.0));
			const int32 M = Net->AddNode(LeakNode(false, 0.0, 0.0));
			const int32 Out = Net->AddNode(LeakNode(true, 0.0, 0.0));
			Net->AddPipe(LeakPipe(Src, M, R1));
			Net->AddPipe(LeakPipe(M, Out, 2.0 * R1));
		}
		With.AddLeak(MakeLeak(1, 0.0));
		const FHydraulicSolveResult Rw = With.Solve(FHydraulicSolveSettings());
		const FHydraulicSolveResult Ro = Without.Solve(FHydraulicSolveSettings());
		const bool bSame = With.GetNodePressurePa(1) == Without.GetNodePressurePa(1) && With.GetPipeFlowM3s(0) == Without.GetPipeFlowM3s(0)
			&& Rw.IterationsUsed == Ro.IterationsUsed && With.GetLeakFlowM3s(0) == 0.0;
		TestTrue(TEXT("(d) zero-diameter leak is bit-identical to no leak"), bSame);
	}

	// (e) One-way: a hole below ambient pressure leaks nothing (no air/water drawn in).
	{
		FHydraulicNetwork Net;
		const int32 Neg = Net.AddNode(LeakNode(true, -1.0e4, 0.0));
		Net.AddLeak(MakeLeak(Neg, D));
		Net.Solve(FHydraulicSolveSettings());
		TestEqual(TEXT("(e) fixed node at -10 kPa(g): leak exactly 0"), Net.GetLeakFlowM3s(0), 0.0);
		FHydraulicSolveResult Re;
		double Pipe = 1.0;
		const double Qe = SolveSourceLeak(1.0e4, R1, H, D, Re, &Pipe);   // 10 kPa(g) source, hole 2.2 m higher (rho g H = 21.5 kPa)
		AddInfo(FString::Printf(TEXT("(e) 대기압 이하 구멍: 누수 %.3e, 배관 %.3e m3/s, 수렴 %d"), Qe, Pipe, Re.bConverged ? 1 : 0));
		TestTrue(TEXT("(e) hole above the hydrostatic level: leak 0, no flow, converged"), Re.bConverged && Qe == 0.0 && FMath::Abs(Pipe) < 1e-15);
	}

	// (f) SplitPipe keeps every flow (series quadratic losses add).
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = 8;
		FHydraulicNetwork A, B;
		const FInRowLoopHandles HA = InRowLoopBuilder::AddLoop(A, Spec);
		InRowLoopBuilder::AddLoop(B, Spec);
		const int32 NumPipes = A.GetNumPipes();
		A.Solve(FHydraulicSolveSettings());
		const int32 Mid = B.SplitPipe(HA.RackBranchPipeIndices[5], 0.5, 0.0);
		B.Solve(FHydraulicSolveSettings());
		double MaxDiff = 0.0;
		for (int32 P = 0; P < NumPipes; ++P) MaxDiff = FMath::Max(MaxDiff, FMath::Abs(A.GetPipeFlowM3s(P) - B.GetPipeFlowM3s(P)));
		MaxDiff /= A.GetPumpFlowM3s(0);
		AddInfo(FString::Printf(TEXT("(f) 배관 나누기: 새 노드 %d, 원래 배관 유량 차 최대 %.3e (펌프 유량 기준)"), Mid, MaxDiff));
		TestTrue(TEXT("(f) split pipe keeps the flows (rel to pump flow < 1e-9)"), Mid != INDEX_NONE && MaxDiff < 1e-9);
		TestEqual(TEXT("(f) bad fraction rejected"), B.SplitPipe(0, 1.0, 0.0), INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHydraulicLeakRowTest, "VRG.Hydraulics.Leak.InRowRowMassAndPlausibility", VrgLeakTestFlags)
bool FHydraulicLeakRowTest::RunTest(const FString& Parameters)
{
	// NVL72 row of 8 racks (FInRowLoopSpec defaults) with heights like the room's plain path (synthetic: headers
	// 0.25 m, rack manifolds 1.1 m, CDU node 0.6 m). Leak on the supply branch of rack 3, at mid height of the riser.
	double Prev = 0.0;
	bool bMonotonic = true;
	for (const double D : { 0.0, 1.0e-3, 3.0e-3, 10.0e-3 })
	{
		FInRowLoopSpec Spec;
		Spec.NumRacks = 8;
		FHydraulicNetwork Net;
		const FInRowLoopHandles H = InRowLoopBuilder::AddLoop(Net, Spec);
		for (int32 N = 0; N < Net.GetNumNodes(); ++N) Net.SetNodeElevationM(N, 0.25);
		Net.SetNodeElevationM(H.CduNodeIndex, 0.6);
		for (const int32 Node : H.RackNodeIndices) if (Node != INDEX_NONE) Net.SetNodeElevationM(Node, 1.1);
		const int32 Hole = Net.SplitPipe(H.RackBranchPipeIndices[3], 0.5, 0.675);
		Net.AddLeak(MakeLeak(Hole, D));
		const FHydraulicSolveResult R = Net.Solve(FHydraulicSolveSettings());

		const double Pump = Net.GetPumpFlowM3s(0);
		const double Leak = Net.GetLeakFlowM3s(0);
		// The CDU node supplies the leak: pump out - return in = leak. Every free node balances (leak included).
		const double MassErr = FMath::Abs(Pump - Net.GetPipeFlowM3s(H.ReturnToCduPipeIndex) - Leak) / Pump;
		double NodeErr = 0.0;
		for (int32 N = 0; N < Net.GetNumNodes(); ++N) if (N != H.CduNodeIndex) NodeErr = FMath::Max(NodeErr, FMath::Abs(Net.GetNodeImbalanceM3s(N)) / Pump);
		AddInfo(FString::Printf(TEXT("구멍 %.0f mm: 누수 %.4f L/s = 펌프 %.4f L/s의 %.3f%%, 구멍 정압 %.1f kPa(g), 질량 %.3e, 노드 최대 %.3e, 반복 %d"),
			D * 1e3, Leak * 1e3, Pump * 1e3, 100.0 * Leak / Pump, Net.GetNodePressurePa(Hole) * 1e-3, MassErr, NodeErr, R.IterationsUsed));
		TestTrue(FString::Printf(TEXT("%.0f mm: converged"), D * 1e3), R.bConverged);
		TestTrue(FString::Printf(TEXT("%.0f mm: pump out - return in = leak (rel to pump flow < 1e-9)"), D * 1e3), MassErr < 1e-9);
		TestTrue(FString::Printf(TEXT("%.0f mm: every node balances (rel to pump flow < 1e-9)"), D * 1e3), NodeErr < 1e-9);
		// Plausibility net (not a physical law): bigger hole leaks more, and never more than the pump moves.
		bMonotonic &= D == 0.0 ? Leak == 0.0 : Leak > Prev;
		TestTrue(FString::Printf(TEXT("%.0f mm: leak below pump flow"), D * 1e3), Leak < Pump);
		Prev = Leak;
	}
	TestTrue(TEXT("leak grows with the hole (0, 1, 3, 10 mm)"), bMonotonic);
	return true;
}

#endif
