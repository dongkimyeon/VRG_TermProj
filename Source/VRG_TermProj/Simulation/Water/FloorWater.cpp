// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Water/FloorWater.h"
#include "Async/ParallelFor.h"
#include "VrgFloorWaterGpu.h"

namespace
{
	struct FFaceFlux { double H = 0.0, NLow = 0.0, NHigh = 0.0, T = 0.0; };

	// One face along its normal: low-side state (h, normal and transverse velocity, bed) and high-side state.
	// bWallLow / bWallHigh: that side is a wall (solid, the room edge or a pinned contact line). A wall side gets
	// nothing; the other side sees its mirror image (zero mass flux, the pressure and reflection of a wall).
	FFaceFlux FaceFlux(double HL, double UL, double TL, double BL, bool bWallLow, double HR, double UR, double TR, double BR, bool bWallHigh, double G)
	{
		FFaceFlux F;
		if (bWallLow && bWallHigh) return F;
		bool bMirror = false, bLowWet = true;
		if (bWallLow) { HL = HR; UL = -UR; TL = TR; BL = BR; bMirror = true; bLowWet = false; }
		else if (bWallHigh) { HR = HL; UR = -UL; TR = TL; BR = BL; bMirror = true; }
		// Hydrostatic reconstruction (Audusse et al. 2004).
		const double Bs = FMath::Max(BL, BR);
		const double HLs = FMath::Max(0.0, HL + BL - Bs), HRs = FMath::Max(0.0, HR + BR - Bs);
		double Fh = 0.0, Fn = 0.0, Ft = 0.0;
		if (HLs > 0.0 || HRs > 0.0)
		{
			const double CL = FMath::Sqrt(G * HLs), CR = FMath::Sqrt(G * HRs);
			double SL = FMath::Min(UL - CL, UR - CR), SR = FMath::Max(UL + CL, UR + CR);
			if (HLs <= 0.0) SL = UR - 2.0 * CR;
			if (HRs <= 0.0) SR = UL + 2.0 * CL;
			const double FLh = HLs * UL, FLn = HLs * UL * UL + .5 * G * HLs * HLs, FLt = HLs * UL * TL;
			const double FRh = HRs * UR, FRn = HRs * UR * UR + .5 * G * HRs * HRs, FRt = HRs * UR * TR;
			if (SL >= 0.0) { Fh = FLh; Fn = FLn; Ft = FLt; }
			else if (SR <= 0.0) { Fh = FRh; Fn = FRn; Ft = FRt; }
			else
			{
				const double Inv = 1.0 / (SR - SL);
				Fh = (SR * FLh - SL * FRh + SL * SR * (HRs - HLs)) * Inv;
				Fn = (SR * FLn - SL * FRn + SL * SR * (HRs * UR - HLs * UL)) * Inv;
				Ft = (SR * FLt - SL * FRt + SL * SR * (HRs * TR - HLs * TL)) * Inv;
			}
		}
		const double NL = Fn + .5 * G * (HL * HL - HLs * HLs);
		const double NH = Fn + .5 * G * (HR * HR - HRs * HRs);
		if (bMirror)
		{
			// Mass through a wall is zero (exactly zero for the symmetric states; set so).
			if (bLowWet) { F.NLow = NL; } else { F.NHigh = NH; }
			return F;
		}
		F.H = Fh; F.NLow = NL; F.NHigh = NH; F.T = Ft;
		return F;
	}
}

FFloorWater::FFloorWater() = default;
FFloorWater::~FFloorWater() = default;

bool FFloorWater::WantsGpu() const
{
	if (Settings.GpuMode == 0 || !FVrgFloorWaterGpu::IsAvailable()) return false;
	if (Settings.GpuMode == 1) return true;
	const int64 Box = WetI0 <= WetI1 ? int64(WetI1 - WetI0 + 3) * (WetJ1 - WetJ0 + 3) : 0;
	return Box >= Settings.GpuMinCells;
}

bool FFloorWater::StepGpu(double DtS)
{
	if (!Gpu.IsValid()) Gpu = MakeUnique<FVrgFloorWaterGpu>();
	if (!bGpuConfigured)
	{
		FVrgFloorGpuConfig C;
		C.Nx = Nx; C.Ny = Ny; C.CellM = Settings.CellM; C.GravityMs2 = Settings.GravityMs2; C.DryDepthM = Settings.DryDepthM;
		C.PinDepthM = Settings.bContactLinePinning ? EquilibriumDepthM(Settings) : 0.0; C.ManningN = Settings.ManningN; C.Cfl = Settings.Cfl;
		C.MaxSubsteps = Settings.MaxSubsteps; C.BedM = B;
		C.Solid.SetNumUninitialized(Solid.Num());
		for (int32 I = 0; I < Solid.Num(); ++I) C.Solid[I] = Solid[I];
		if (!Gpu->Configure(C)) return false;
		bGpuConfigured = true; bGpuStateCurrent = false;
	}
	if (!bGpuStateCurrent)
	{
		if (!Gpu->Upload(H, HU, HV)) return false;
		GpuPendingCells.Reset();
	}
	// The deposits since the GPU last stepped: their cells take the CPU copy's depth (the same additions, same order).
	TArray<double> Depths;
	for (const int32 C : GpuPendingCells) Depths.Add(H[C]);
	TArray<double> OutH, OutHu, OutHv;
	double CreatedDepth = 0.0;
	int32 Sub = 0;
	if (!Gpu->Step(DtS, GpuPendingCells, Depths, OutH, OutHu, OutHv, CreatedDepth, Sub)) { bGpuStateCurrent = false; return false; }
	H = MoveTemp(OutH); HU = MoveTemp(OutHu); HV = MoveTemp(OutHv);
	GpuPendingCells.Reset();
	bGpuStateCurrent = true;
	Totals.ClippedM3 += CreatedDepth * Settings.CellM * Settings.CellM;
	Totals.LastSubsteps = Sub;
	++Totals.GpuSteps;
	UpdateWetBox(0, Nx - 1, 0, Ny - 1);
	return true;
}

double FFloorWater::EquilibriumDepthM(const FFloorWaterSettings& S)
{
	const double Lc = FMath::Sqrt(S.SurfaceTensionNm / (S.WaterDensityKgM3 * S.GravityMs2));
	return 2.0 * Lc * FMath::Sin(.5 * S.ContactAngleRad);
}

void FFloorWater::Reset(const FVector2D& OriginM, int32 InNx, int32 InNy, double FloorZM, const FFloorWaterSettings& InSettings)
{
	Settings = InSettings;
	Origin = OriginM; FloorZ = FloorZM;
	Nx = FMath::Max(InNx, 1); Ny = FMath::Max(InNy, 1);
	const int32 N = Nx * Ny;
	for (TArray<double>* A : { &H, &HU, &HV, &NH, &NHU, &NHV }) A->Init(0.0, N);
	B.SetNumUninitialized(N);
	for (int32 J = 0; J < Ny; ++J)
		for (int32 I = 0; I < Nx; ++I)
			B[Index(I, J)] = Settings.BedSlope.X * (I + .5) * Settings.CellM + Settings.BedSlope.Y * (J + .5) * Settings.CellM;
	Solid.Init(0, N);
	for (TArray<double>* A : { &FxH, &FxNL, &FxNH, &FxT }) A->Init(0.0, (Nx + 1) * Ny);
	for (TArray<double>* A : { &FyH, &FyNL, &FyNH, &FyT }) A->Init(0.0, Nx * (Ny + 1));
	Floods.Reset();
	Totals = FFloorWaterTotals();
	WetI0 = 1; WetJ0 = 1; WetI1 = 0; WetJ1 = 0;
	bGpuConfigured = false; bGpuStateCurrent = false;
	GpuPendingCells.Reset();
}

bool FFloorWater::ResetForRoom(const FSprayGeometry& Geometry, const FFloorWaterSettings& InSettings)
{
	const FBox& R = Geometry.RoomM;
	if (!R.IsValid || InSettings.CellM <= 0.0) { Reset(FVector2D::ZeroVector, 1, 1, 0.0, InSettings); return false; }
	Reset(FVector2D(R.Min.X, R.Min.Y), FMath::CeilToInt32((R.Max.X - R.Min.X) / InSettings.CellM), FMath::CeilToInt32((R.Max.Y - R.Min.Y) / InSettings.CellM), R.Min.Z, InSettings);
	for (const FSprayObstacle& O : Geometry.Obstacles)
	{
		if (O.Kind == ESprayObstacleKind::Ceiling) continue;
		// Footprint cells: cell centre inside the box seen from above, box reaching down to the floor.
		double Down = 0.0;
		for (int32 K = 0; K < 3; ++K) Down += FMath::Abs(O.Rotation.RotateVector(K == 0 ? FVector(1, 0, 0) : K == 1 ? FVector(0, 1, 0) : FVector(0, 0, 1)).Z) * O.HalfM[K];
		if (O.CenterM.Z - Down > FloorZ + .05) continue;   // hangs above the floor
		// A wall at least 0.51 cell thick on the floor grid, as the air grid does (MarkBarrierSolidCells): a cell-centre
		// step moves at most one cell, so a slab that thick catches every face path (watertight). A 2 cm curtain on the
		// 2.5 cm grid otherwise marked no cell at all where no cell centre fell inside it (found by the 2026-10-27 screen check).
		FVector Half = O.HalfM;
		if (O.Kind == ESprayObstacleKind::Barrier) { Half.X = FMath::Max(Half.X, .51 * Settings.CellM); Half.Y = FMath::Max(Half.Y, .51 * Settings.CellM); }
		double Rx = 0.0, Ry = 0.0;
		for (int32 K = 0; K < 3; ++K)
		{
			const FVector A = O.Rotation.RotateVector(K == 0 ? FVector(1, 0, 0) : K == 1 ? FVector(0, 1, 0) : FVector(0, 0, 1)) * Half[K];
			Rx += FMath::Abs(A.X); Ry += FMath::Abs(A.Y);
		}
		const int32 I0 = FMath::Max(0, FMath::FloorToInt32((O.CenterM.X - Rx - Origin.X) / Settings.CellM)), I1 = FMath::Min(Nx - 1, FMath::FloorToInt32((O.CenterM.X + Rx - Origin.X) / Settings.CellM));
		const int32 J0 = FMath::Max(0, FMath::FloorToInt32((O.CenterM.Y - Ry - Origin.Y) / Settings.CellM)), J1 = FMath::Min(Ny - 1, FMath::FloorToInt32((O.CenterM.Y + Ry - Origin.Y) / Settings.CellM));
		TArray<int32> Cells;
		for (int32 J = J0; J <= J1; ++J)
			for (int32 I = I0; I <= I1; ++I)
			{
				const FVector C(Origin.X + (I + .5) * Settings.CellM, Origin.Y + (J + .5) * Settings.CellM, O.CenterM.Z);
				const FVector L = O.Rotation.UnrotateVector(C - O.CenterM);
				if (FMath::Abs(L.X) <= Half.X && FMath::Abs(L.Y) <= Half.Y) Cells.Add(Index(I, J));
			}
		if (O.Kind == ESprayObstacleKind::Barrier) { for (const int32 C : Cells) Solid[C] = 1; }
		else AddFlood(O.Name, Cells);
	}
	return true;
}

void FFloorWater::SetSolid(int32 I, int32 J, bool bSolid)
{
	if (I < 0 || J < 0 || I >= Nx || J >= Ny) return;
	const int32 C = Index(I, J);
	Solid[C] = bSolid ? 1 : 0;
	if (bSolid) { H[C] = HU[C] = HV[C] = 0.0; }
	bGpuConfigured = false;
}

void FFloorWater::SetBed(int32 I, int32 J, double BedM)
{
	if (I >= 0 && J >= 0 && I < Nx && J < Ny) B[Index(I, J)] = BedM;
	bGpuConfigured = false;
}

void FFloorWater::SetState(int32 I, int32 J, double DepthM, double Hu, double Hv)
{
	if (I < 0 || J < 0 || I >= Nx || J >= Ny || Solid[Index(I, J)]) return;
	const int32 C = Index(I, J);
	H[C] = FMath::Max(DepthM, 0.0); HU[C] = Hu; HV[C] = Hv;
	bGpuStateCurrent = false;
	if (H[C] > 0.0)
	{
		if (WetI0 > WetI1) { WetI0 = WetI1 = I; WetJ0 = WetJ1 = J; }
		WetI0 = FMath::Min(WetI0, I); WetI1 = FMath::Max(WetI1, I); WetJ0 = FMath::Min(WetJ0, J); WetJ1 = FMath::Max(WetJ1, J);
	}
	UpdateTotals(-1.0);
	Totals.ReceivedM3 = Totals.StoredM3;   // set-up water is the starting stock
}

void FFloorWater::AddFlood(const FString& Name, const TArray<int32>& Cells)
{
	FFloorFlood& F = Floods.AddDefaulted_GetRef();
	F.Name = Name; F.Cells = Cells;
}

bool FFloorWater::AddWater(const FVector& PointM, double VolumeM3)
{
	if (VolumeM3 <= 0.0) return true;
	int32 I = FMath::FloorToInt32((PointM.X - Origin.X) / Settings.CellM), J = FMath::FloorToInt32((PointM.Y - Origin.Y) / Settings.CellM);
	if (I < 0 || J < 0 || I >= Nx || J >= Ny) return false;
	if (Solid[Index(I, J)])
	{
		// Onto a wall cell (its foot): the nearest open cell takes it (search in growing rings, fixed order).
		bool bFound = false;
		for (int32 Rg = 1; Rg <= 20 && !bFound; ++Rg)
			for (int32 DJ = -Rg; DJ <= Rg && !bFound; ++DJ)
				for (int32 DI = -Rg; DI <= Rg && !bFound; ++DI)
				{
					const int32 A = I + DI, Bj = J + DJ;
					if ((FMath::Abs(DI) == Rg || FMath::Abs(DJ) == Rg) && A >= 0 && Bj >= 0 && A < Nx && Bj < Ny && !Solid[Index(A, Bj)]) { I = A; J = Bj; bFound = true; }
				}
		if (!bFound) return false;
	}
	H[Index(I, J)] += VolumeM3 / (Settings.CellM * Settings.CellM);
	if (bGpuStateCurrent) GpuPendingCells.AddUnique(Index(I, J));
	Totals.ReceivedM3 += VolumeM3;
	if (WetI0 > WetI1) { WetI0 = WetI1 = I; WetJ0 = WetJ1 = J; }
	WetI0 = FMath::Min(WetI0, I); WetI1 = FMath::Max(WetI1, I); WetJ0 = FMath::Min(WetJ0, J); WetJ1 = FMath::Max(WetJ1, J);
	return true;
}

double FFloorWater::MaxWaveSpeed(int32 I0, int32 I1, int32 J0, int32 J1) const
{
	double Smax = 0.0;
	for (int32 J = J0; J <= J1; ++J)
		for (int32 I = I0; I <= I1; ++I)
		{
			const int32 C = Index(I, J);
			if (H[C] <= Settings.DryDepthM) continue;
			const double Cw = FMath::Sqrt(Settings.GravityMs2 * H[C]);
			Smax = FMath::Max(Smax, FMath::Abs(HU[C] / H[C]) + Cw + FMath::Abs(HV[C] / H[C]) + Cw);
		}
	return Smax;
}

void FFloorWater::Substep(double DtS, int32 I0, int32 I1, int32 J0, int32 J1)
{
	const double G = Settings.GravityMs2, Dry = Settings.DryDepthM, Dx = Settings.CellM;
	const double HPin = Settings.bContactLinePinning ? EquilibriumDepthM(Settings) : 0.0;
	auto Vel = [Dry](double Hc, double Q) { return Hc > Dry ? Q / Hc : 0.0; };
	// A side is a wall for this face when it is solid / off the grid, or when the contact line is pinned: a wet cell
	// thinner than h_eq facing a dry open cell does not advance (the dry cell is the wall).
	auto Walls = [&](int32 CL, int32 CR, bool& bWL, bool& bWR)
	{
		bWL = CL < 0 || Solid[CL] != 0;
		bWR = CR < 0 || Solid[CR] != 0;
		if (bWL || bWR || HPin <= 0.0) return;
		const bool bWetL = H[CL] > Dry, bWetR = H[CR] > Dry;
		if (bWetL && !bWetR && H[CL] < HPin) bWR = true;
		else if (bWetR && !bWetL && H[CR] < HPin) bWL = true;
	};
	// x faces I0..I1+1 of rows J0..J1 (face i lies between cells i-1 and i).
	ParallelFor(J1 - J0 + 1, [&](int32 Row)
	{
		const int32 J = J0 + Row;
		for (int32 Fi = I0; Fi <= I1 + 1; ++Fi)
		{
			const int32 CL = Fi - 1 >= 0 ? Index(Fi - 1, J) : -1, CR = Fi < Nx ? Index(Fi, J) : -1;
			bool bWL, bWR; Walls(CL, CR, bWL, bWR);
			const FFaceFlux F = FaceFlux(CL >= 0 ? H[CL] : 0.0, CL >= 0 ? Vel(H[CL], HU[CL]) : 0.0, CL >= 0 ? Vel(H[CL], HV[CL]) : 0.0, CL >= 0 ? B[CL] : 0.0, bWL,
				CR >= 0 ? H[CR] : 0.0, CR >= 0 ? Vel(H[CR], HU[CR]) : 0.0, CR >= 0 ? Vel(H[CR], HV[CR]) : 0.0, CR >= 0 ? B[CR] : 0.0, bWR, G);
			const int32 K = J * (Nx + 1) + Fi;
			FxH[K] = F.H; FxNL[K] = F.NLow; FxNH[K] = F.NHigh; FxT[K] = F.T;
		}
	});
	// y faces J0..J1+1 of columns I0..I1 (normal = v, transverse = u).
	ParallelFor(J1 - J0 + 2, [&](int32 Row)
	{
		const int32 Fj = J0 + Row;
		for (int32 I = I0; I <= I1; ++I)
		{
			const int32 CL = Fj - 1 >= 0 ? Index(I, Fj - 1) : -1, CR = Fj < Ny ? Index(I, Fj) : -1;
			bool bWL, bWR; Walls(CL, CR, bWL, bWR);
			const FFaceFlux F = FaceFlux(CL >= 0 ? H[CL] : 0.0, CL >= 0 ? Vel(H[CL], HV[CL]) : 0.0, CL >= 0 ? Vel(H[CL], HU[CL]) : 0.0, CL >= 0 ? B[CL] : 0.0, bWL,
				CR >= 0 ? H[CR] : 0.0, CR >= 0 ? Vel(H[CR], HV[CR]) : 0.0, CR >= 0 ? Vel(H[CR], HU[CR]) : 0.0, CR >= 0 ? B[CR] : 0.0, bWR, G);
			const int32 K = Fj * Nx + I;
			FyH[K] = F.H; FyNL[K] = F.NLow; FyNH[K] = F.NHigh; FyT[K] = F.T;
		}
	});
	// Cells: conservative update into the second set, then implicit Manning friction.
	const double R = DtS / Dx;
	const double N2 = Settings.ManningN * Settings.ManningN;
	TArray<double> Created;
	Created.Init(0.0, J1 - J0 + 1);
	ParallelFor(J1 - J0 + 1, [&](int32 Row)
	{
		const int32 J = J0 + Row;
		for (int32 I = I0; I <= I1; ++I)
		{
			const int32 C = Index(I, J);
			if (Solid[C]) { NH[C] = NHU[C] = NHV[C] = 0.0; continue; }
			const int32 XL = J * (Nx + 1) + I, XR = XL + 1, YL = J * Nx + I, YR = (J + 1) * Nx + I;
			double Hn = H[C] - R * (FxH[XR] - FxH[XL]) - R * (FyH[YR] - FyH[YL]);
			double Hu = HU[C] - R * (FxNL[XR] - FxNH[XL]) - R * (FyT[YR] - FyT[YL]);
			double Hv = HV[C] - R * (FxT[XR] - FxT[XL]) - R * (FyNL[YR] - FyNH[YL]);
			if (Hn < 0.0) { Created[Row] -= Hn; Hn = 0.0; }
			if (Hn > Dry)
			{
				const double Speed = FMath::Sqrt(Hu * Hu + Hv * Hv) / Hn;
				const double Kf = DtS * G * N2 * Speed / FMath::Pow(Hn, 4.0 / 3.0);
				Hu /= (1.0 + Kf); Hv /= (1.0 + Kf);
			}
			else { Hu = 0.0; Hv = 0.0; }
			NH[C] = Hn; NHU[C] = Hu; NHV[C] = Hv;
		}
	});
	const double A = Dx * Dx;
	for (const double V : Created) Totals.ClippedM3 += V * A;
	// Only the region changed: copy it back (the rest of the read set is already the state).
	for (int32 J = J0; J <= J1; ++J)
	{
		const int32 C0 = Index(I0, J), Cn = I1 - I0 + 1;
		FMemory::Memcpy(&H[C0], &NH[C0], Cn * sizeof(double));
		FMemory::Memcpy(&HU[C0], &NHU[C0], Cn * sizeof(double));
		FMemory::Memcpy(&HV[C0], &NHV[C0], Cn * sizeof(double));
	}
}

void FFloorWater::UpdateWetBox(int32 I0, int32 I1, int32 J0, int32 J1)
{
	int32 A0 = Nx, A1 = -1, B0 = Ny, B1 = -1;
	for (int32 J = J0; J <= J1; ++J)
		for (int32 I = I0; I <= I1; ++I)
			if (H[Index(I, J)] > 0.0) { A0 = FMath::Min(A0, I); A1 = FMath::Max(A1, I); B0 = FMath::Min(B0, J); B1 = FMath::Max(B1, J); }
	if (A1 < 0) { WetI0 = 1; WetI1 = 0; WetJ0 = 1; WetJ1 = 0; }
	else { WetI0 = A0; WetI1 = A1; WetJ0 = B0; WetJ1 = B1; }
}

void FFloorWater::Step(double DtS, double NowS)
{
	Totals.LastSubsteps = 0;
	Totals.bLastStepGpu = false;
	if (DtS <= 0.0 || Nx * Ny <= 0) return;
	if (WetI0 <= WetI1 && WantsGpu() && StepGpu(DtS))
	{
		Totals.bLastStepGpu = true;
		Totals.Substeps += Totals.LastSubsteps;
		UpdateTotals(NowS + DtS);
		return;
	}
	bGpuStateCurrent = false;   // the CPU copy moves on; the GPU copy is stale
	double Left = DtS;
	while (Left > 0.0 && WetI0 <= WetI1)
	{
		// The region: the wet box grown by one cell (water moves at most one cell per substep under the CFL limit).
		const int32 I0 = FMath::Max(0, WetI0 - 1), I1 = FMath::Min(Nx - 1, WetI1 + 1), J0 = FMath::Max(0, WetJ0 - 1), J1 = FMath::Min(Ny - 1, WetJ1 + 1);
		const double S = MaxWaveSpeed(I0, I1, J0, J1);
		double Dt = S > 0.0 ? Settings.Cfl * Settings.CellM / S : Left;
		if (Totals.LastSubsteps + 1 >= Settings.MaxSubsteps) Dt = Left;   // limit reached: finish the interval (reported)
		Dt = FMath::Min(Dt, Left);
		// Avoid a sliver at the end: two equal halves instead of a full step and a tiny one.
		if (Left - Dt > 0.0 && Left - Dt < .1 * Dt) Dt = .5 * Left;
		Substep(Dt, I0, I1, J0, J1);
		UpdateWetBox(I0, I1, J0, J1);
		Left -= Dt;
		++Totals.LastSubsteps;
	}
	Totals.Substeps += Totals.LastSubsteps;
	UpdateTotals(NowS + DtS);
}

void FFloorWater::UpdateTotals(double NowS)
{
	double Stored = 0.0, MaxSpeed = 0.0, MaxDepth = 0.0;
	int32 Wet = 0;
	for (int32 J = WetJ0; J <= WetJ1; ++J)
		for (int32 I = WetI0; I <= WetI1; ++I)
		{
			const int32 C = Index(I, J);
			if (H[C] <= 0.0) continue;
			Stored += H[C];
			MaxDepth = FMath::Max(MaxDepth, H[C]);
			if (H[C] > Settings.DryDepthM) { ++Wet; MaxSpeed = FMath::Max(MaxSpeed, FMath::Sqrt(HU[C] * HU[C] + HV[C] * HV[C]) / H[C]); }
		}
	Totals.StoredM3 = Stored * Settings.CellM * Settings.CellM;
	Totals.WetCells = Wet; Totals.MaxSpeedMs = MaxSpeed; Totals.MaxDepthM = MaxDepth;
	Totals.Residual = Totals.ReceivedM3 > 0.0 ? FMath::Abs(Totals.ReceivedM3 - Totals.StoredM3 + Totals.ClippedM3) / Totals.ReceivedM3 : 0.0;
	if (NowS < 0.0) return;
	for (FFloorFlood& F : Floods)
	{
		double Sum = 0.0, Max = 0.0;
		for (const int32 C : F.Cells) { Sum += H[C]; Max = FMath::Max(Max, H[C]); }
		F.MeanDepthM = F.Cells.Num() ? Sum / F.Cells.Num() : 0.0;
		F.MaxDepthM = Max;
		F.PeakDepthM = FMath::Max(F.PeakDepthM, Max);
		if (F.FirstWetS < 0.0 && Max > Settings.DryDepthM) F.FirstWetS = NowS;
	}
}
