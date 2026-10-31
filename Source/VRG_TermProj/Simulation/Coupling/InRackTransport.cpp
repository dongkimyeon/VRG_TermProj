#include "Simulation/Coupling/InRackTransport.h"

bool InRackTransport::Build(const FHydraulicNetwork& Net, const FInRackHandles& H, int32 ReturnPipe,
	double DensityKgM3, double CpJKgK, FTrayCoolantInput& Out, const TArray<double>* NodeImbalancesM3s, bool bIndexedTraversal, FBuildCosts* Costs)
{
	double Mark = Costs ? FPlatformTime::Seconds() : 0.0;
	if (Costs) *Costs = {};
	const int32 N = H.Slots.Num(), Columns = N + 3;
	if (N == 0 || H.Nodes.Num() != 5 * N || H.Leaks.Num() != 5 * N || H.Pipes.Num() != 6 * N
		|| ReturnPipe < 0 || ReturnPipe >= Net.GetNumPipes() || !(DensityKgM3 > 0.0) || !(CpJKgK > 0.0)
		|| (NodeImbalancesM3s && NodeImbalancesM3s->Num() != Net.GetNumNodes())) return false;
	FTrayCoolantInput Candidate;
	FTrayNetworkTransport& T = Candidate.Network;
	T.Reservoirs = N + 1;
	T.NetWK.Init(0.0, (N + 1) * Columns); T.BoundaryWK.Init(0.0, Columns); T.LeakWK = T.BoundaryWK;
	T.ClassLeakWK.Init(0.0, 3 * Columns); T.ClassLeakKgs.Init(0.0, 3);
	TArray<int32> Nodes = H.Nodes; Nodes.Add(H.SupplyNode); Nodes.Add(H.ReturnNode);
	TArray<int32> Reservoir, Order, Pending;
	Reservoir.Init(INDEX_NONE, Nodes.Num()); Pending.Init(0, Nodes.Num());
	TArray<double> Coeff; Coeff.Init(0.0, Nodes.Num() * Columns);
	TArray<double> Outflow; Outflow.Init(0.0, Nodes.Num());
	struct FEdge { int32 A, B; double Kgs; };
	TArray<FEdge> Edges;
	for (int32 R = 0; R < N; ++R) Reservoir[R * 5 + 2] = R;
	Reservoir[5 * N] = N + 1; Reservoir[5 * N + 1] = N;
	// Rebuilt from this call's handles and signed flows: no cross-step cache/invalidation.
	TArray<int32> LocalNode, FirstEdge, LastEdge, NextEdge;
	if (Costs) { const double Now=FPlatformTime::Seconds(); Costs->SetupMs=(Now-Mark)*1000; Mark=Now; }
	if (bIndexedTraversal)
	{
		LocalNode.Init(INDEX_NONE, Net.GetNumNodes());
		FirstEdge.Init(INDEX_NONE, Nodes.Num()); LastEdge=FirstEdge;
		for (int32 A=0; A<Nodes.Num(); ++A) {
			if (!LocalNode.IsValidIndex(Nodes[A])) return false;
			if (LocalNode[Nodes[A]]==INDEX_NONE) LocalNode[Nodes[A]]=A; // same first match as IndexOfByKey
		}
	}
	for (int32 P : H.Pipes)
	{
		double Flow = Net.GetPipeFlowM3s(P) * DensityKgM3;
		if (!FMath::IsFinite(Flow)) return false;
		const int32 From=Net.GetPipeFromNodeIndex(P), To=Net.GetPipeToNodeIndex(P);
		int32 A = bIndexedTraversal ? (LocalNode.IsValidIndex(From) ? LocalNode[From] : INDEX_NONE) : Nodes.IndexOfByKey(From);
		int32 B = bIndexedTraversal ? (LocalNode.IsValidIndex(To) ? LocalNode[To] : INDEX_NONE) : Nodes.IndexOfByKey(To);
		if (Flow < 0.0) { Swap(A, B); Flow = -Flow; }
		if (A == INDEX_NONE || B == INDEX_NONE) return false;
		if (Flow == 0.0) continue;
		const int32 E=Edges.Add({A, B, Flow}); Outflow[A] += Flow;
		if (bIndexedTraversal) {
			NextEdge.Add(INDEX_NONE);
			if (FirstEdge[A]==INDEX_NONE) FirstEdge[A]=E; else NextEdge[LastEdge[A]]=E;
			LastEdge[A]=E; // preserve original pipe/edge order exactly
		}
		if (Reservoir[B] == INDEX_NONE) ++Pending[B];
	}
	for (int32 L = 0; L < H.Leaks.Num(); ++L) Outflow[L] += Net.GetLeakFlowM3s(H.Leaks[L]) * DensityKgM3;
	if (Costs) { const double Now=FPlatformTime::Seconds(); Costs->LookupMs=(Now-Mark)*1000; Mark=Now; }
	// Thermal reservoirs are roots even when hydraulically downstream. The remaining passive
	// junction graph is acyclic (strictly decreasing piezometric pressure along every nonzero edge).
	for (int32 A = 0; A < Nodes.Num(); ++A)
	{
		if (Reservoir[A] != INDEX_NONE) Coeff[A * Columns + Reservoir[A]] = 1.0;
		if (Reservoir[A] != INDEX_NONE || Pending[A] == 0) Order.Add(A);
		const double ResidualM3s = NodeImbalancesM3s ? (*NodeImbalancesM3s)[Nodes[A]] : Net.GetNodeImbalanceM3s(Nodes[A]);
		T.MassResidualKgs = FMath::Max(T.MassResidualKgs, FMath::Abs(ResidualM3s) * DensityKgM3);
	}
	for (int32 Cursor = 0; Cursor < Order.Num(); ++Cursor)
	{
		const int32 A = Order[Cursor];
		for (int32 EIndex=bIndexedTraversal ? FirstEdge[A] : 0;
			EIndex!=INDEX_NONE && EIndex<Edges.Num(); EIndex=bIndexedTraversal ? NextEdge[EIndex] : EIndex+1)
		{
			if (Costs) ++Costs->EdgeVisits;
			const FEdge& E=Edges[EIndex];
			if (E.A!=A || Reservoir[E.B]!=INDEX_NONE) continue;
			// Outgoing mass (including the local hole) enforces the junction enthalpy equation
			// exactly. Its difference from incoming mass is the separately reported hydraulic residual.
			if (!(Outflow[E.B] > 0.0)) return false;
			for (int32 C = 0; C < Columns; ++C) Coeff[E.B * Columns + C] += E.Kgs * Coeff[A * Columns + C] / Outflow[E.B];
			if (--Pending[E.B] == 0) Order.Add(E.B);
		}
	}
	if (Order.Num() != Nodes.Num()) return false;
	if (Costs) { const double Now=FPlatformTime::Seconds(); Costs->MixingMs=(Now-Mark)*1000; Mark=Now; }
	for (const FEdge& E : Edges)
		for (int32 C = 0; C < Columns; ++C)
		{
			const double W = E.Kgs * CpJKgK * Coeff[E.A * Columns + C];
			if (Reservoir[E.A] >= 0 && Reservoir[E.A] <= N) T.NetWK[Reservoir[E.A] * Columns + C] -= W;
			if (Reservoir[E.B] >= 0 && Reservoir[E.B] <= N) T.NetWK[Reservoir[E.B] * Columns + C] += W;
			if (E.A == 5 * N) T.BoundaryWK[C] -= W;
			if (E.B == 5 * N) T.BoundaryWK[C] += W;
		}
	for (int32 L = 0; L < H.Leaks.Num(); ++L)
	{
		const double Kgs = Net.GetLeakFlowM3s(H.Leaks[L]) * DensityKgM3;
		const int32 Kind = L % 5 < 2 ? 0 : L % 5 == 2 ? 1 : 2;
		T.ClassLeakKgs[Kind] += Kgs; T.LeakKgs += Kgs;
		for (int32 C = 0; C < Columns; ++C)
		{
			const double W = Kgs * CpJKgK * Coeff[L * Columns + C];
			T.LeakWK[C] += W; T.ClassLeakWK[Kind * Columns + C] += W;
			if (Reservoir[L] >= 0) T.NetWK[Reservoir[L] * Columns + C] -= W;
		}
	}
	const double ReturnKgs = Net.GetPipeFlowM3s(ReturnPipe) * DensityKgM3;
	T.NetWK[N * Columns + (ReturnKgs >= 0.0 ? N : N + 2)] -= ReturnKgs * CpJKgK;
	T.BoundaryWK[ReturnKgs >= 0.0 ? N : N + 2] += ReturnKgs * CpJKgK;
	Candidate.ReturnMassFlowKgs = ReturnKgs;
	for (int32 R = 0; R < N; ++R)
	{
		Candidate.InletMassFlowKgs.Add(Net.GetPipeFlowM3s(H.InletPipes[R]) * DensityKgM3);
		Candidate.OutletMassFlowKgs.Add(Net.GetPipeFlowM3s(H.OutletPipes[R]) * DensityKgM3);
		Candidate.LeakMassFlowKgs.Add((Net.GetLeakFlowM3s(H.Leaks[5 * R + 1]) + Net.GetLeakFlowM3s(H.Leaks[5 * R + 2])
			+ Net.GetLeakFlowM3s(H.Leaks[5 * R + 3])) * DensityKgM3);
	}
	if (!T.IsValid(N)) return false;
	if (Costs) Costs->AssemblyMs=(FPlatformTime::Seconds()-Mark)*1000;
	Out = MoveTemp(Candidate);
	return true;
}
