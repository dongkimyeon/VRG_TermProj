#include "Simulation/Air/OctreeAirSolver.h"
#include "Simulation/Air/AirSolver.h"

bool FOctreeAirSolver::IsOpen(const FFixedOctreeFace& F) const
{
	return F.LeafLow != INDEX_NONE && F.LeafHigh != INDEX_NONE && Tree.IsLeafFluid(F.LeafLow) && Tree.IsLeafFluid(F.LeafHigh);
}

bool FOctreeAirSolver::Initialize(const FUniformAirGrid& Source, const FFixedOctreeSettings& Settings,
	const TArray<FBox>& BoxesM, FString& Error)
{
	FFixedOctreeGrid NewTree;
	if (!NewTree.Build(Source.GetParams(), Settings, BoxesM, Error) || !NewTree.ImportUniform(Source, Error)) return false;
	Tree = MoveTemp(NewTree); TreeSettings = Settings;
	FaceVelocityMs.Init(0.0, Tree.GetFaces().Num());
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I];
		if (!IsOpen(F)) continue;
		const int32 Fine = Tree.LeafSizeM(F.LeafLow) <= Tree.LeafSizeM(F.LeafHigh) ? F.LeafLow : F.LeafHigh;
		FVector P = Tree.LeafCenterM(Fine);
		P[F.Axis] += (Fine == F.LeafLow ? .5 : -.5) * Tree.LeafSizeM(Fine);
		FaceVelocityMs[I] = Source.SampleVelocityMs(P)[F.Axis];
	}
	EnergyInJ = EnergyOutJ = LastCoolingRemovedW = LastCourant = 0.0;
	InitialEnergyJ = Tree.TotalThermalEnergyJ(Tree.GetBaseParams().ReferenceTempK);
	PressureSolves = PressureFailures = 0;
	BuildPressure();
	return true;
}

void FOctreeAirSolver::BuildPressure()
{
	CompactOf.Init(INDEX_NONE, Tree.GetNumLeaves()); LeafOf.Reset();
	FMultigridPoisson::FLevelInput L;
	for (int32 I = 0; I < Tree.GetNumLeaves(); ++I) if (Tree.IsLeafFluid(I))
	{
		CompactOf[I] = LeafOf.Add(I);
		L.Volume.Add(Tree.LeafVolumeM3(I)); L.Centre.Add(Tree.LeafCenterM(I));
		const auto& N = Tree.GetNodes()[Tree.GetLeafNodeIndices()[I]];
		L.BoxMin.Add(N.MinFine); L.BoxSize.Add(1 << (Tree.GetMaxDepth() - N.Depth));
	}
	for (int32 I : LeafOf)
	{
		L.RowStart.Add(L.Col.Num());
		for (int32 K = Tree.GetLeafFaceStart()[I]; K < Tree.GetLeafFaceStart()[I + 1]; ++K)
		{
			const auto& F = Tree.GetFaces()[Tree.GetLeafFaceIndices()[K]];
			if (!IsOpen(F)) continue;
			L.Col.Add(CompactOf[F.LeafLow == I ? F.LeafHigh : F.LeafLow]);
			L.Trans.Add(F.AreaM2 / F.DistanceM);
		}
	}
	L.RowStart.Add(L.Col.Num()); Pressure.Build(MoveTemp(L));
	PressurePa.Init(0.0, LeafOf.Num());
}

FPoissonSolveResult FOctreeAirSolver::Project(double DtS)
{
	if (!(DtS > 0) || !FMath::IsFinite(DtS)) { PressureResult = FPoissonSolveResult(); return PressureResult; }
	TArray<double> B; B.Init(0.0, LeafOf.Num());
	const double Rho = Tree.GetBaseParams().DensityKgM3;
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I];
		if (!IsOpen(F)) { FaceVelocityMs[I] = 0.0; continue; }
		const double Flux = F.AreaM2 * FaceVelocityMs[I] * Rho / DtS;
		B[CompactOf[F.LeafLow]] += Flux / Tree.LeafVolumeM3(F.LeafLow);
		B[CompactOf[F.LeafHigh]] -= Flux / Tree.LeafVolumeM3(F.LeafHigh);
	}
	const double Start = FPlatformTime::Seconds();
	PressureResult = Pressure.Solve(B, PressurePa, PressureSettings);
	LastPressureMs = 1000.0 * (FPlatformTime::Seconds() - Start);
	++PressureSolves;
	if (!PressureResult.bConverged) ++PressureFailures;
	TArray<double> Next = FaceVelocityMs;
	for (int32 I = 0; I < Next.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I];
		if (IsOpen(F)) Next[I] -= DtS / Rho * (PressurePa[CompactOf[F.LeafHigh]] - PressurePa[CompactOf[F.LeafLow]]) / F.DistanceM;
	}
	FaceVelocityMs = MoveTemp(Next);
	return PressureResult;
}

void FOctreeAirSolver::CellVelocities(TArray<FVector>& Out) const
{
	Out.Init(FVector::ZeroVector, Tree.GetNumLeaves());
	TArray<FVector> Area; Area.Init(FVector::ZeroVector, Tree.GetNumLeaves());
	for (int32 K = 0; K < Tree.GetFaces().Num(); ++K)
	{
		const auto& F = Tree.GetFaces()[K];
		for (int32 I : {F.LeafLow, F.LeafHigh}) if (I != INDEX_NONE)
		{
			Out[I][F.Axis] += FaceVelocityMs[K] * F.AreaM2;
			Area[I][F.Axis] += F.AreaM2;
		}
	}
	for (int32 I = 0; I < Out.Num(); ++I) for (int32 A = 0; A < 3; ++A)
		if (Area[I][A] > 0.0) Out[I][A] /= Area[I][A];
}

void FOctreeAirSolver::FacesFromCells(const TArray<FVector>& Cells)
{
	FaceVelocityMs.Init(0.0, Tree.GetFaces().Num());
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		const double LowH = Tree.LeafSizeM(F.LeafLow), HighH = Tree.LeafSizeM(F.LeafHigh);
		FaceVelocityMs[I] = (HighH * Cells[F.LeafLow][F.Axis] + LowH * Cells[F.LeafHigh][F.Axis]) / (LowH + HighH);
	}
}

void FOctreeAirSolver::AdvectMomentum(double DtS)
{
	TArray<FVector> Old; CellVelocities(Old);
	TArray<FVector> Delta; Delta.Init(FVector::ZeroVector, Old.Num());
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		const double FluxM3 = FaceVelocityMs[I] * F.AreaM2 * DtS;
		const FVector Transfer = FluxM3 * Old[FluxM3 >= 0.0 ? F.LeafLow : F.LeafHigh];
		Delta[F.LeafLow] -= Transfer / Tree.LeafVolumeM3(F.LeafLow);
		Delta[F.LeafHigh] += Transfer / Tree.LeafVolumeM3(F.LeafHigh);
	}
	// Add interpolated momentum increment to old face field, rather than smoothing the entire
	// velocity at every step (which would introduce dt-independent artificial viscosity).
	const TArray<double> Before = FaceVelocityMs;
	FacesFromCells(Delta);
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I) FaceVelocityMs[I] += Before[I];
}

void FOctreeAirSolver::AdvectTemperature(double DtS)
{
	const auto& Old = Tree.GetFields().TemperatureK;
	TArray<double> Next = Old;
	// Advect departure from a common reference. This avoids multiplying residual divergence by
	// absolute 300 K while preserving the same closed-domain energy exactly.
	const double Ref = Tree.GetBaseParams().ReferenceTempK;
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		const double Flux = FaceVelocityMs[I] * F.AreaM2 * DtS;
		const double HeatVolumeK = Flux * (Old[Flux >= 0.0 ? F.LeafLow : F.LeafHigh] - Ref);
		Next[F.LeafLow] -= HeatVolumeK / Tree.LeafVolumeM3(F.LeafLow);
		Next[F.LeafHigh] += HeatVolumeK / Tree.LeafVolumeM3(F.LeafHigh);
	}
	Tree.GetMutableFields().TemperatureK = MoveTemp(Next);
}

bool FOctreeAirSolver::Step(const FAirSolverInput& Input, double DtS)
{
	if (!(DtS > 0) || !FMath::IsFinite(DtS) || !Tree.HasFields()) return false;
	const auto& P = Tree.GetBaseParams();
	const int32 Roots = P.NumCellsX * P.NumCellsY * P.NumCellsZ;
	const double RootV = FMath::Pow(P.CellSizeM, 3), Capacity = P.DensityKgM3 * P.SpecificHeatJKgK;
	double Remaining = DtS, CoolingJ = 0.0, PressureMs = 0.0; LastCourant = 0.0;
	bool bAllConverged = true;
	while (Remaining > DtS * 1e-12)
	{
		// Numerical CFL ceiling, not a measured hardware parameter. Conservative explicit upwind
		// needs dt * sum(outgoing u*A)/V <= 1; use 0.5 plus a buoyancy acceleration bound.
		TArray<double> Rate; Rate.Init(0.0, Tree.GetNumLeaves());
		for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
		{
			const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
			const int32 Up = FaceVelocityMs[I] >= 0 ? F.LeafLow : F.LeafHigh;
			Rate[Up] += FMath::Abs(FaceVelocityMs[I]) * F.AreaM2 / Tree.LeafVolumeM3(Up);
		}
		double Dt = Remaining;
		for (int32 I : LeafOf)
		{
			if (Rate[I] > 0) Dt = FMath::Min(Dt, .5 / Rate[I]);
			if (Input.bEnableBuoyancy)
			{
				const double Accel = FMath::Abs(Input.GravityZMs2 * (Tree.GetFields().TemperatureK[I] - P.ReferenceTempK) / P.ReferenceTempK);
				if (Accel > 0) Dt = FMath::Min(Dt, FMath::Sqrt(.25 * Tree.LeafSizeM(I) / Accel));
			}
		}
		AdvectMomentum(Dt);
		TArray<double> NextVelocity = FaceVelocityMs;
		if (Input.bEnableBuoyancy) for (int32 I = 0; I < NextVelocity.Num(); ++I)
		{
			const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F) || F.Axis != 2) continue;
			const double T = .5 * (Tree.GetFields().TemperatureK[F.LeafLow] + Tree.GetFields().TemperatureK[F.LeafHigh]);
			NextVelocity[I] += -Input.GravityZMs2 * (T - P.ReferenceTempK) / P.ReferenceTempK * Dt;
		}
		FaceVelocityMs = MoveTemp(NextVelocity);
		Project(Dt);
		PressureMs += LastPressureMs; bAllConverged &= PressureResult.bConverged;
		// Projection may redistribute local outflow: subcycle scalar transport using the actual field.
		Rate.Init(0.0, Tree.GetNumLeaves());
		for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
		{
			const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
			const int32 Up = FaceVelocityMs[I] >= 0 ? F.LeafLow : F.LeafHigh;
			Rate[Up] += FMath::Abs(FaceVelocityMs[I]) * F.AreaM2 / Tree.LeafVolumeM3(Up);
		}
		double MaxRate = 0; for (double R : Rate) MaxRate = FMath::Max(MaxRate, R);
		const int32 Sub = FMath::Max(1, FMath::CeilToInt32(Dt * MaxRate / .5));
		LastCourant = FMath::Max(LastCourant, Dt * MaxRate / Sub);
		for (int32 I = 0; I < Sub; ++I) AdvectTemperature(Dt / Sub);
		TArray<double> Next = Tree.GetFields().TemperatureK;
		for (int32 I : LeafOf)
		{
			const int32 Root = Tree.GetNodes()[Tree.GetLeafNodeIndices()[I]].RootCellIndex;
			const double V = Tree.LeafVolumeM3(I), Fraction = V / RootV;
			const double HeatJ = Input.CellHeatW.Num() == Roots ? Input.CellHeatW[Root] * Fraction * Dt : 0;
			Next[I] += HeatJ / (Capacity * V); EnergyInJ += HeatJ;
			if (Input.CellCoolingUAWK.Num() == Roots && Input.CellCoolingUAWK[Root] > 0)
			{
				const double DifferenceK = Next[I] - Input.CoolingSupplyTempK;
				const double RemovedJ = DifferenceK * FMath::Min(Input.CellCoolingUAWK[Root] * Fraction * Dt, Capacity * V);
				Next[I] -= RemovedJ / (Capacity * V); EnergyOutJ += RemovedJ; CoolingJ += RemovedJ;
			}
		}
		Tree.GetMutableFields().TemperatureK = MoveTemp(Next);
		Remaining -= Dt;
	}
	LastCoolingRemovedW = CoolingJ / DtS;
	LastPressureMs = PressureMs; PressureResult.bConverged = bAllConverged;
	return bAllConverged;
}

double FOctreeAirSolver::MaxDivergencePerS() const
{
	TArray<double> Flux; Flux.Init(0.0, Tree.GetNumLeaves());
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		const double V = FaceVelocityMs[I] * F.AreaM2;
		Flux[F.LeafLow] += V; Flux[F.LeafHigh] -= V;
	}
	double Max = 0; for (int32 I : LeafOf) Max = FMath::Max(Max, FMath::Abs(Flux[I]) / Tree.LeafVolumeM3(I));
	return Max;
}

double FOctreeAirSolver::NormalizedDivergence() const
{
	double Ref = 0;
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		Ref = FMath::Max(Ref, FMath::Abs(FaceVelocityMs[I]) / FMath::Min(Tree.LeafSizeM(F.LeafLow), Tree.LeafSizeM(F.LeafHigh)));
	}
	return Ref > 1e-12 ? MaxDivergencePerS() / Ref : MaxDivergencePerS();
}

bool FOctreeAirSolver::Adapt(const TArray<FBox>& BoxesM, double ProjectionDtS, FString& Error)
{
	if (!(ProjectionDtS > 0) || !Tree.HasFields()) { Error = TEXT("Invalid adaptation state/dt"); return false; }
	FOctreeAirSolver Candidate;
	Candidate.TreeSettings = TreeSettings; Candidate.PressureSettings = PressureSettings;
	if (!Candidate.Tree.Build(Tree.GetBaseParams(), TreeSettings, BoxesM, Error)) return false;
	// Group old leaves by their uniform root. Dyadic partitions never cross root boundaries,
	// so overlap transfer needs no all-pairs leaf search and no solver-time Morton lookup (D7).
	TArray<TArray<int32>> ByRoot;
	const auto& P = Tree.GetBaseParams(); ByRoot.SetNum(P.NumCellsX * P.NumCellsY * P.NumCellsZ);
	for (int32 I = 0; I < Tree.GetNumLeaves(); ++I) ByRoot[Tree.GetNodes()[Tree.GetLeafNodeIndices()[I]].RootCellIndex].Add(I);
	TArray<FVector> OldVelocity; CellVelocities(OldVelocity);
	TArray<FVector> NewVelocity; NewVelocity.Init(FVector::ZeroVector, Candidate.Tree.GetNumLeaves());
	auto& Fields = Candidate.Tree.GetMutableFields();
	Fields.TemperatureK.Init(0.0, Candidate.Tree.GetNumLeaves()); Fields.bSolidCell.Init(false, Candidate.Tree.GetNumLeaves());
	for (int32 I = 0; I < Candidate.Tree.GetNumLeaves(); ++I)
	{
		const auto& N = Candidate.Tree.GetNodes()[Candidate.Tree.GetLeafNodeIndices()[I]];
		const FBox Box = Candidate.Tree.NodeBoundsM(Candidate.Tree.GetLeafNodeIndices()[I]);
		double SolidVolume = 0.0;
		for (int32 J : ByRoot[N.RootCellIndex])
		{
			const FBox OldBox = Tree.NodeBoundsM(Tree.GetLeafNodeIndices()[J]);
			const FVector Lo = Box.Min.ComponentMax(OldBox.Min), Hi = Box.Max.ComponentMin(OldBox.Max);
			const FVector S = (Hi - Lo).ComponentMax(FVector::ZeroVector);
			const double V = S.X * S.Y * S.Z, Weight = V / Candidate.Tree.LeafVolumeM3(I);
			Fields.TemperatureK[I] += Tree.GetFields().TemperatureK[J] * Weight;
			NewVelocity[I] += OldVelocity[J] * Weight;
			if (!Tree.IsLeafFluid(J)) SolidVolume += V;
		}
		// Solid geometry is inherited from the base grid. Reject mixed solid/fluid merges rather
		// than silently destroy fluid volume/energy if a caller supplied a finer obstacle mask.
		const double V = Candidate.Tree.LeafVolumeM3(I);
		if (SolidVolume > 1e-12 * V && SolidVolume < (1 - 1e-12) * V) { Error = TEXT("Cannot merge mixed solid/fluid leaves"); return false; }
		Fields.bSolidCell[I] = SolidVolume > .5 * V;
	}
	Candidate.FacesFromCells(NewVelocity); Candidate.BuildPressure();
	if (!Candidate.Project(ProjectionDtS).bConverged) { Error = TEXT("Adaptation reprojection failed; old state retained"); return false; }
	Candidate.EnergyInJ = EnergyInJ; Candidate.EnergyOutJ = EnergyOutJ; Candidate.InitialEnergyJ = InitialEnergyJ;
	Candidate.LastCoolingRemovedW = LastCoolingRemovedW;
	Candidate.PressureSolves += PressureSolves; Candidate.PressureFailures += PressureFailures;
	*this = MoveTemp(Candidate); Error.Reset(); return true;
}

void FOctreeAirSolver::ExportUniform(FUniformAirGrid& Out) const
{
	Tree.RestrictTemperatureToUniform(Out.TemperatureK);
	// Export normal fluxes at root faces by area averaging (not cell-centre interpolation).
	Out.VelocityXMs.Init(0.0, Out.GetNumFacesX()); Out.VelocityYMs.Init(0.0, Out.GetNumFacesY()); Out.VelocityZMs.Init(0.0, Out.GetNumFacesZ());
	const double H = Out.GetCellSizeM();
	for (int32 I = 0; I < FaceVelocityMs.Num(); ++I)
	{
		const auto& F = Tree.GetFaces()[I]; if (!IsOpen(F)) continue;
		const int32 Fine = Tree.LeafSizeM(F.LeafLow) <= Tree.LeafSizeM(F.LeafHigh) ? F.LeafLow : F.LeafHigh;
		FVector Pos = Tree.LeafCenterM(Fine); Pos[F.Axis] += (Fine == F.LeafLow ? .5 : -.5) * Tree.LeafSizeM(Fine);
		const int32 Plane = FMath::RoundToInt32(Pos[F.Axis] / H);
		if (FMath::Abs(Pos[F.Axis] / H - Plane) > 1e-9) continue;
		FIntVector C(FMath::FloorToInt32(Pos.X / H), FMath::FloorToInt32(Pos.Y / H), FMath::FloorToInt32(Pos.Z / H)); C[F.Axis] = Plane;
		const double Value = FaceVelocityMs[I] * F.AreaM2 / (H * H);
		if (F.Axis == 0) Out.VelocityXMs[Out.FaceXIndex(C.X, C.Y, C.Z)] += Value;
		if (F.Axis == 1) Out.VelocityYMs[Out.FaceYIndex(C.X, C.Y, C.Z)] += Value;
		if (F.Axis == 2) Out.VelocityZMs[Out.FaceZIndex(C.X, C.Y, C.Z)] += Value;
	}
}
