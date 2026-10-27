// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Water/LeakSpray.h"
#include "Math/RandomStream.h"

namespace
{
	// Smallest root of A t^2 + B t + C = 0 in [0, T] (-1 when none). Stable form of the quadratic formula.
	double SmallestRoot(double A, double B, double C, double T)
	{
		double Best = -1.0;
		auto Consider = [&](double Tr) { if (Tr >= 0.0 && Tr <= T && (Best < 0.0 || Tr < Best)) Best = Tr; };
		if (FMath::Abs(A) * T * T <= 1e-14 * (FMath::Abs(B) * T + FMath::Abs(C)))
		{
			if (B != 0.0) Consider(-C / B);
			return Best;
		}
		const double Disc = B * B - 4.0 * A * C;
		if (Disc < 0.0) return -1.0;
		const double Qr = -0.5 * (B + (B >= 0.0 ? FMath::Sqrt(Disc) : -FMath::Sqrt(Disc)));
		if (Qr != 0.0) { Consider(Qr / A); Consider(C / Qr); }
		else Consider(0.0);
		return Best;
	}

	FVector Axis(int32 K) { return K == 0 ? FVector(1, 0, 0) : K == 1 ? FVector(0, 1, 0) : FVector(0, 0, 1); }

	// Lowest world z of an oriented box.
	double BoxBottomZ(const FSprayObstacle& O)
	{
		double Down = 0.0;
		for (int32 K = 0; K < 3; ++K) Down += FMath::Abs(O.Rotation.RotateVector(Axis(K)).Z) * O.HalfM[K];
		return O.CenterM.Z - Down;
	}

	constexpr int32 RoomExteriorObstacle = -2;
}

double FLeakSpray::JetVelocityMs(double GaugePressurePa, const FSpraySettings& S, const FSprayFluid& F)
{
	return S.VelocityCoefficient * FMath::Sqrt(2.0 * FMath::Max(GaugePressurePa, 0.0) / F.WaterDensityKgM3);
}

double FLeakSpray::JetDiameterM(double FlowM3s, double VelocityMs)
{
	return VelocityMs > 0.0 && FlowM3s > 0.0 ? FMath::Sqrt(4.0 * FlowM3s / (PI * VelocityMs)) : 0.0;
}

double FLeakSpray::BreakupLengthM(double JetDiameterM, double VelocityMs, const FSprayFluid& F)
{
	const double We = F.WaterDensityKgM3 * VelocityMs * VelocityMs * JetDiameterM / F.SurfaceTensionNm;
	return JetDiameterM * 8.51 * FMath::Pow(FMath::Max(We, 0.0), 0.32);
}

double FLeakSpray::MaxStableDropM(double RelativeSpeedMs, const FSpraySettings& S, const FSprayFluid& F)
{
	const double V2 = RelativeSpeedMs * RelativeSpeedMs;
	return V2 > 0.0 ? S.CriticalWeber * F.SurfaceTensionNm / (F.AirDensityKgM3 * V2) : TNumericLimits<double>::Max();
}

double FLeakSpray::DropDragCoefficient(double Reynolds)
{
	if (Reynolds <= 0.0) return 0.0;
	return Reynolds < 1000.0 ? 24.0 / Reynolds * (1.0 + 0.15 * FMath::Pow(Reynolds, 0.687)) : 0.44;
}

bool FLeakSpray::Splashes(double NormalSpeedMs, double DiameterM, const FSpraySettings& S, const FSprayFluid& F)
{
	if (NormalSpeedMs <= 0.0 || DiameterM <= 0.0) return false;
	const double We = F.WaterDensityKgM3 * NormalSpeedMs * NormalSpeedMs * DiameterM / F.SurfaceTensionNm;
	const double Re = F.WaterDensityKgM3 * NormalSpeedMs * DiameterM / F.WaterViscosityPaS;
	return FMath::Sqrt(We) * FMath::Pow(Re, 0.25) > S.SplashK;
}

double FLeakSpray::NusseltThicknessM(double FlowPerWidthM2s, const FSprayFluid& F)
{
	return FMath::Pow(3.0 * F.WaterViscosityPaS * FMath::Max(FlowPerWidthM2s, 0.0) / (F.WaterDensityKgM3 * F.GravityMs2), 1.0 / 3.0);
}

FVector FLeakSpray::ConeDirection(uint32 InSeed, double HalfAngleRad)
{
	// Uniform over the spherical cap around -Z.
	FRandomStream R(static_cast<int32>(InSeed));
	const double CosT = 1.0 - double(R.FRand()) * (1.0 - FMath::Cos(HalfAngleRad));
	const double SinT = FMath::Sqrt(FMath::Max(0.0, 1.0 - CosT * CosT));
	const double Phi = 2.0 * PI * double(R.FRand());
	return FVector(SinT * FMath::Cos(Phi), SinT * FMath::Sin(Phi), -CosT);
}

void FLeakSpray::Reset(const FSprayGeometry& InGeometry, const FSpraySettings& InSettings, const FSprayFluid& InFluid)
{
	Geometry = InGeometry;
	Settings = InSettings;
	Fluid = InFluid;
	for (TArray<FVector>* A : { &Pos, &Vel, &NPos, &NVel, &TrailFrom, &TrailTo }) A->Reset();
	TrailJet.Reset();
	for (TArray<double>* A : { &Vol, &Dia, &JetDia, &Path, &Breakup, &NVol, &NDia, &NJetDia, &NPath, &NBreakup }) A->Reset();
	for (TArray<uint8>* A : { &Jet, &Splashed, &NJet, &NSplashed }) A->Reset();
	Src.Reset(); NSrc.Reset(); Seed.Reset(); NSeed.Reset();
	Films.Reset(); FilmIndex.Reset(); FloorBins.Reset(); GroundBins.Reset(); Waiting.Reset(); Emitters.Reset(); Emissions.Reset();
	Wetting.Init(FSprayWetting(), Geometry.Obstacles.Num());
	FirstImpacts.Reset(); FirstBreakups.Reset();
	Totals = FSprayTotals();
	StepFloorDeposits.Reset();
}

int32 FLeakSpray::FindObstacleContaining(const FVector& PointM) const
{
	for (int32 I = 0; I < Geometry.Obstacles.Num(); ++I)
	{
		const FSprayObstacle& O = Geometry.Obstacles[I];
		const FVector L = O.Rotation.UnrotateVector(PointM - O.CenterM);
		if (FMath::Abs(L.X) <= O.HalfM.X && FMath::Abs(L.Y) <= O.HalfM.Y && FMath::Abs(L.Z) <= O.HalfM.Z) return I;
	}
	return INDEX_NONE;
}

bool FLeakSpray::IsOutdoors(const FVector& PointM) const
{
	const FBox& R = Geometry.RoomM;
	return !R.IsValid || PointM.X < R.Min.X || PointM.X > R.Max.X || PointM.Y < R.Min.Y || PointM.Y > R.Max.Y;
}

FLeakSpray::FHit FLeakSpray::FindHit(const FVector& X0, const FVector& V0, const FVector& G, double DtS) const
{
	FHit Best;
	auto Offer = [&](double T, const FVector& N, ESpraySurface S, int32 Obstacle, int32 Face)
	{
		if (T >= 0.0 && T < Best.TimeS) { Best.TimeS = T; Best.NormalM = N; Best.Surface = S; Best.Obstacle = Obstacle; Best.Face = Face; }
	};
	// Entering a box from outside: x(t) = p0 + u t + a t^2 / 2 in the box frame (constant acceleration over the step:
	// exact for gravity alone).
	auto Box = [&](const FVector& C, const FQuat& Rot, const FVector& H, ESpraySurface S, int32 Obstacle)
	{
		const FVector P0 = Rot.UnrotateVector(X0 - C), U = Rot.UnrotateVector(V0), A = Rot.UnrotateVector(G);
		if (FMath::Abs(P0.X) <= H.X && FMath::Abs(P0.Y) <= H.Y && FMath::Abs(P0.Z) <= H.Z) return;   // started inside: not this box
		for (int32 K = 0; K < 3; ++K)
			for (const double Sgn : { -1.0, 1.0 })
			{
				if (Sgn * P0[K] <= H[K]) continue;   // not on the outer side of this face
				const double T = SmallestRoot(.5 * A[K], U[K], P0[K] - Sgn * H[K], DtS);
				if (T < 0.0 || T >= Best.TimeS) continue;
				const FVector P = P0 + U * T + .5 * A * T * T;
				bool bOn = true;
				for (int32 J = 0; J < 3; ++J) if (J != K && FMath::Abs(P[J]) > H[J] * (1.0 + 1e-12) + 1e-12) bOn = false;
				if (bOn) Offer(T, Rot.RotateVector(Axis(K) * Sgn), S, Obstacle, 2 * K + (Sgn > 0.0 ? 1 : 0));
			}
	};
	for (int32 I = 0; I < Geometry.Obstacles.Num(); ++I)
	{
		const FSprayObstacle& O = Geometry.Obstacles[I];
		Box(O.CenterM, O.Rotation, O.HalfM, O.Kind == ESprayObstacleKind::Barrier ? ESpraySurface::Barrier : ESpraySurface::Equipment, I);
	}
	const FBox& R = Geometry.RoomM;
	const bool bInside = R.IsValid && R.IsInsideOrOn(X0);
	if (bInside)
	{
		// Leaving the room box from inside: its faces are floor, walls and ceiling; normals point into the room.
		for (int32 K = 0; K < 3; ++K)
		{
			const double TMin = SmallestRoot(.5 * G[K], V0[K], X0[K] - R.Min[K], DtS);
			const double TMax = SmallestRoot(.5 * G[K], V0[K], X0[K] - R.Max[K], DtS);
			const ESpraySurface SMin = K == 2 ? ESpraySurface::Floor : ESpraySurface::Wall;
			const ESpraySurface SMax = K == 2 ? ESpraySurface::Ceiling : ESpraySurface::Wall;
			// A root at t = 0 of a face the parcel moves away from is its own starting face (a splash leaves it).
			if (TMin >= 0.0 && !(TMin == 0.0 && V0[K] >= 0.0)) Offer(TMin, Axis(K), SMin, INDEX_NONE, 2 * K);
			if (TMax >= 0.0 && !(TMax == 0.0 && V0[K] <= 0.0)) Offer(TMax, -Axis(K), SMax, INDEX_NONE, 2 * K + 1);
		}
	}
	else
	{
		if (R.IsValid) Box(R.GetCenter(), FQuat::Identity, R.GetExtent(), ESpraySurface::RoomExterior, RoomExteriorObstacle);
		if (X0.Z > Geometry.GroundZM) Offer(SmallestRoot(.5 * G.Z, V0.Z, X0.Z - Geometry.GroundZM, DtS), FVector(0, 0, 1), ESpraySurface::Ground, INDEX_NONE, 4);
	}
	return Best;
}

void FLeakSpray::PushParcel(const FVector& P, const FVector& V, double Volume, double D, double JetD, double PathM, double BreakupM, bool bJet, bool bSplashed, int32 Source, uint32 InSeed)
{
	NPos.Add(P); NVel.Add(V); NVol.Add(Volume); NDia.Add(D); NJetDia.Add(JetD); NPath.Add(PathM); NBreakup.Add(BreakupM);
	NJet.Add(bJet ? 1 : 0); NSplashed.Add(bSplashed ? 1 : 0); NSrc.Add(Source); NSeed.Add(InSeed);
}

void FLeakSpray::AddFloor(const FVector& PointM, double Volume, bool bOutdoors)
{
	if (Volume <= 0.0) return;
	const FIntPoint Bin(FMath::FloorToInt32(PointM.X / Settings.FloorBinM), FMath::FloorToInt32(PointM.Y / Settings.FloorBinM));
	(bOutdoors ? GroundBins : FloorBins).FindOrAdd(Bin) += Volume;
	if (!bOutdoors) StepFloorDeposits.Add({ PointM, Volume });   // W3: the floor model takes these each step
}

void FLeakSpray::Touch(int32 Obstacle, const FVector& PointM, double Volume, double NowS)
{
	if (!Wetting.IsValidIndex(Obstacle)) return;
	FSprayWetting& W = Wetting[Obstacle];
	if (W.FirstWetS < 0.0) W.FirstWetS = NowS;
	W.TopWetZM = FMath::Max(W.TopWetZM, PointM.Z);
	W.WaterM3 += Volume;
}

void FLeakSpray::Deposit(const FHit& Hit, const FVector& PointM, double Volume, int32 Source, double NowS)
{
	if (!FirstImpacts.Contains(Source)) FirstImpacts.Add(Source, { PointM, NowS, Hit.Surface });
	if (Hit.Surface == ESpraySurface::Floor) { AddFloor(PointM, Volume, false); return; }
	if (Hit.Surface == ESpraySurface::Ground) { AddFloor(PointM, Volume, true); return; }
	Touch(Hit.Obstacle, PointM, Volume, NowS);
	if (Hit.Surface == ESpraySurface::Ceiling || Hit.NormalM.Z < -0.7)
	{
		// A downward face: the water hangs and drips off as drops (no further splash, ESTIMATE).
		PushParcel(PointM + Hit.NormalM * 1e-4, FVector::ZeroVector, Volume, Settings.DripDiameterM, Settings.DripDiameterM, 0.0, 0.0, false, true, Source, 0u);
		return;
	}

	// A film on a vertical face, or on a box top draining over its edges.
	double WidthM = 0.0, BottomZ = 0.0;
	FVector FootM = PointM;
	if (Hit.Obstacle == INDEX_NONE)
	{
		// Room wall: its length along the floor; the foot is the wall base.
		const FBox& R = Geometry.RoomM;
		const int32 K = Hit.Face / 2;
		WidthM = K == 0 ? R.Max.Y - R.Min.Y : R.Max.X - R.Min.X;
		BottomZ = R.Min.Z;
		FootM = PointM + Hit.NormalM * 1e-3;
	}
	else
	{
		const bool bRoom = Hit.Obstacle == RoomExteriorObstacle;
		const FSprayObstacle O = bRoom
			? FSprayObstacle{ Geometry.RoomM.GetCenter(), FQuat::Identity, Geometry.RoomM.GetExtent(), ESprayObstacleKind::Barrier, TEXT("방 외벽") }
			: Geometry.Obstacles[Hit.Obstacle];
		BottomZ = BoxBottomZ(O);
		const int32 K = Hit.Face / 2;
		// The face's two in-plane axes; the more horizontal one is its width.
		const int32 A1 = (K + 1) % 3, A2 = (K + 2) % 3;
		if (Hit.NormalM.Z > 0.7)
		{
			// Top: it spills over the whole perimeter (ESTIMATE); its foot is the nearest edge, just outside.
			WidthM = 4.0 * (O.HalfM[A1] + O.HalfM[A2]);
			const FVector L = O.Rotation.UnrotateVector(PointM - O.CenterM);
			const int32 Edge = O.HalfM[A1] - FMath::Abs(L[A1]) < O.HalfM[A2] - FMath::Abs(L[A2]) ? A1 : A2;
			FVector LF = L;
			LF[Edge] = (L[Edge] >= 0.0 ? 1.0 : -1.0) * (O.HalfM[Edge] + 1e-3);
			FootM = O.CenterM + O.Rotation.RotateVector(LF);
		}
		else
		{
			const double H1 = FMath::Abs(O.Rotation.RotateVector(Axis(A1)).Z), H2 = FMath::Abs(O.Rotation.RotateVector(Axis(A2)).Z);
			WidthM = 2.0 * (H1 <= H2 ? O.HalfM[A1] : O.HalfM[A2]);
			FootM = PointM + Hit.NormalM * 1e-3;
		}
	}
	FootM.Z = BottomZ;
	const uint64 Key = (uint64(uint32(Hit.Obstacle + 3)) << 8) | uint64(Hit.Face);
	int32* Found = FilmIndex.Find(Key);
	if (Found == nullptr)
	{
		FSprayFilm& F = Films.AddDefaulted_GetRef();
		F.Obstacle = Hit.Obstacle; F.Face = Hit.Face; F.Surface = Hit.Surface; F.WidthM = FMath::Max(WidthM, 1e-3);
		F.FootM = FootM; F.bFootOutdoors = IsOutdoors(FootM) || Hit.Obstacle == RoomExteriorObstacle;
		Found = &FilmIndex.Add(Key, Films.Num() - 1);
	}
	FSprayFilm& F = Films[*Found];
	F.WetHeightM = FMath::Max(F.WetHeightM, PointM.Z - BottomZ);
	F.InflowM3 += Volume;
}

void FLeakSpray::StepFilms(double DtS)
{
	for (FSprayFilm& F : Films)
	{
		const double V0 = F.VolumeM3 + F.InflowM3;
		F.InflowM3 = 0.0;
		if (V0 <= 0.0) { F.LastOutflowM3s = 0.0; F.LastThicknessM = 0.0; continue; }
		// Implicit in the outflow: Dt k x^3 + x = V0, k = W rho g / (3 mu (W H)^3). Newton from x = V0 (above the root,
		// the function is convex and increasing): monotone convergence. Outflow = V0 - x, so the ledger is exact.
		const double WH = F.WidthM * FMath::Max(F.WetHeightM, 1e-3);
		const double K = F.WidthM * Fluid.WaterDensityKgM3 * Fluid.GravityMs2 / (3.0 * Fluid.WaterViscosityPaS * WH * WH * WH);
		double X = V0;
		for (int32 It = 0; It < 100; ++It)
		{
			const double Fx = DtS * K * X * X * X + X - V0;
			const double Dx = Fx / (3.0 * DtS * K * X * X + 1.0);
			X -= Dx;
			if (FMath::Abs(Dx) <= 1e-15 * V0) break;
		}
		X = FMath::Clamp(X, 0.0, V0);
		const double Out = V0 - X;
		F.VolumeM3 = X;
		F.LastOutflowM3s = Out / DtS;
		F.LastThicknessM = X / WH;
		AddFloor(F.FootM, Out, F.bFootOutdoors);
	}
}

void FLeakSpray::Step(const TArray<FSpraySource>& Sources, double DtS, double NowS)
{
	if (DtS <= 0.0) return;
	StepFloorDeposits.Reset();
	const FVector G(0, 0, -Fluid.GravityMs2);

	// 1. What left the holes this step. A hole inside a box (pipe inside a cabinet) wets that box from the hole down
	//    and its water reaches the floor under it at once (no inner flow path is modelled, ESTIMATE).
	for (const FSpraySource& S : Sources)
	{
		if (S.VolumeM3 <= 0.0) continue;
		Totals.EmittedM3 += S.VolumeM3;
		const int32 Inside = FindObstacleContaining(S.OriginM);
		if (Inside != INDEX_NONE && Geometry.Obstacles[Inside].Kind != ESprayObstacleKind::Ceiling)
		{
			Totals.ThroughEquipmentM3 += S.VolumeM3;
			Touch(Inside, S.OriginM, S.VolumeM3, NowS);
			const FVector Foot(S.OriginM.X, S.OriginM.Y, Geometry.RoomM.IsValid ? Geometry.RoomM.Min.Z : Geometry.GroundZM);
			AddFloor(Foot, S.VolumeM3, IsOutdoors(Foot));
			if (!FirstImpacts.Contains(S.Id)) FirstImpacts.Add(S.Id, { S.OriginM, NowS, ESpraySurface::Equipment });
			continue;
		}
		Waiting.FindOrAdd(S.Id) += S.VolumeM3;
		const double V0 = JetVelocityMs(S.GaugePressurePa, Settings, Fluid);
		FEmitter& E = Emitters.FindOrAdd(S.Id);
		E.Source = S;
		E.JetDiameterM = V0 > 1e-6 ? JetDiameterM(S.VolumeM3 / DtS, V0) : Settings.DripDiameterM;
	}
	// Waiting water leaves as one parcel per hole while there is room under the parcel limit, also after the hole
	// closed (with the hole's last jet: ESTIMATE for the little that waited). Holes in id order (deterministic).
	TArray<int32> Ids;
	Waiting.GetKeys(Ids);
	Ids.Sort();
	for (const int32 Id : Ids)
	{
		double& Wait = Waiting[Id];
		const FEmitter* E = Emitters.Find(Id);
		if (Wait <= 0.0 || E == nullptr || Pos.Num() >= Settings.MaxParcels) continue;   // waits at its hole, still counted
		const double Volume = Wait;
		Wait = 0.0;
		const FSpraySource& S = E->Source;
		const double V0 = JetVelocityMs(S.GaugePressurePa, Settings, Fluid);
		const uint32 ParcelSeed = HashCombine(GetTypeHash(Id), Emissions.FindOrAdd(Id)++);
		const bool bDrip = V0 <= 1e-6;
		const double D = E->JetDiameterM;
		Pos.Add(S.OriginM); Vel.Add(S.DirectionM.GetSafeNormal() * V0); Vol.Add(Volume); Dia.Add(D); JetDia.Add(D); Path.Add(0.0);
		Breakup.Add(bDrip ? 0.0 : BreakupLengthM(D, V0, Fluid)); Jet.Add(bDrip ? 0 : 1); Splashed.Add(bDrip ? 1 : 0); Src.Add(Id); Seed.Add(ParcelSeed);
	}

	// 2. Every parcel over the step, written into the second set.
	for (TArray<FVector>* A : { &NPos, &NVel, &TrailFrom, &TrailTo }) A->Reset();
	TrailJet.Reset();
	for (TArray<double>* A : { &NVol, &NDia, &NJetDia, &NPath, &NBreakup }) A->Reset();
	for (TArray<uint8>* A : { &NJet, &NSplashed }) A->Reset();
	NSrc.Reset(); NSeed.Reset();
	const double DensityRatio = Fluid.AirDensityKgM3 / Fluid.WaterDensityKgM3;
	for (int32 I = 0; I < Pos.Num(); ++I)
	{
		const FVector X0 = Pos[I];
		FVector V = Vel[I];
		double D = Dia[I];
		bool bJet = Jet[I] != 0;
		// Acceleration over the step, held constant so the path is a parabola whose crossings are solved exactly:
		// gravity alone for the jet; for a drop, gravity and sphere drag implicit in the speed,
		// v1 = (v0 + g Dt) / (1 + k Dt), k = 3/4 (rho_a / rho_w) Cd |v0| / D: stable for any step, and at rest in the
		// balance v = g / k (the terminal speed) exactly.
		FVector Acc = G;
		if (!bJet)
		{
			const double DMax = MaxStableDropM(V.Size(), Settings, Fluid);
			if (D > DMax) { D = DMax; ++Totals.Breakups; }
			const double Speed = V.Size();
			if (Settings.bAirDrag && Speed > 0.0)
			{
				const double Re = Fluid.AirDensityKgM3 * Speed * D / Fluid.AirViscosityPaS;
				const double KDrag = 0.75 * DensityRatio * DropDragCoefficient(Re) * Speed / D;
				Acc = ((V + G * DtS) / (1.0 + KDrag * DtS) - V) / DtS;
			}
		}
		const FHit Hit = FindHit(X0, V, Acc, DtS);
		if (Hit.TimeS <= DtS)
		{
			const double T = Hit.TimeS;
			const FVector XH = X0 + V * T + .5 * Acc * T * T;
			const FVector VH = V + Acc * T;
			const double Vn = FVector::DotProduct(VH, Hit.NormalM);   // < 0 approaching
			const double DImpact = bJet ? JetDia[I] : D;
			double Stay = Vol[I];
			if (Splashed[I] == 0 && Splashes(-Vn, DImpact, Settings, Fluid))
			{
				const double Secondary = Settings.SplashMassFraction * Vol[I];
				Stay -= Secondary;
				const FVector Vt = VH - Vn * Hit.NormalM;
				const FVector VOut = Vt * Settings.ReboundTangential - Vn * Settings.ReboundNormal * Hit.NormalM;
				PushParcel(XH + Hit.NormalM * 1e-4, VOut, Secondary, DImpact * Settings.SplashDropRatio, DImpact * Settings.SplashDropRatio,
					0.0, 0.0, false, true, Src[I], Seed[I] * 31u + 7u);
				++Totals.Splashes;
			}
			TrailFrom.Add(X0); TrailTo.Add(XH); TrailJet.Add(bJet ? 1 : 0);
			Deposit(Hit, XH, Stay, Src[I], NowS + T);
			continue;
		}
		const FVector X1 = X0 + V * DtS + .5 * Acc * DtS * DtS;
		FVector V1 = V + Acc * DtS;
		double PathM = Path[I];
		if (bJet)
		{
			const double Chord = FVector::Dist(X0, X1);
			PathM += Chord;
			if (PathM >= Breakup[I])
			{
				// Breakup: Rayleigh drops, no larger than the air lets survive, spread a little (fixed seed).
				const double SpeedMs = V1.Size();
				D = FMath::Min(Settings.RayleighDropRatio * JetDia[I], MaxStableDropM(SpeedMs, Settings, Fluid));
				FRandomStream R(static_cast<int32>(Seed[I]));
				V1 = R.VRandCone(V1.GetSafeNormal(), float(Settings.BreakupSpreadRad)) * V1.Size();
				bJet = false;
				++Totals.Breakups;
				if (!FirstBreakups.Contains(Src[I])) FirstBreakups.Add(Src[I], { PathM, Chord, Breakup[I], D, JetDia[I], SpeedMs });
			}
		}
		TrailFrom.Add(X0); TrailTo.Add(X1); TrailJet.Add(Jet[I]);
		PushParcel(X1, V1, Vol[I], D, JetDia[I], PathM, Breakup[I], bJet, Splashed[I] != 0, Src[I], Seed[I]);
	}
	Swap(Pos, NPos); Swap(Vel, NVel); Swap(Vol, NVol); Swap(Dia, NDia); Swap(JetDia, NJetDia); Swap(Path, NPath); Swap(Breakup, NBreakup);
	Swap(Jet, NJet); Swap(Splashed, NSplashed); Swap(Src, NSrc); Swap(Seed, NSeed);

	// 3. Films drain to their foot.
	StepFilms(DtS);
	UpdateTotals();
}

void FLeakSpray::UpdateTotals()
{
	Totals.AirborneM3 = 0.0; Totals.JetParcels = 0;
	for (int32 I = 0; I < Vol.Num(); ++I) { Totals.AirborneM3 += Vol[I]; Totals.JetParcels += Jet[I]; }
	Totals.Parcels = Vol.Num();
	Totals.WaitingM3 = 0.0; for (const auto& W : Waiting) Totals.WaitingM3 += W.Value;
	Totals.FilmM3 = 0.0; for (const FSprayFilm& F : Films) Totals.FilmM3 += F.VolumeM3;
	Totals.FloorM3 = 0.0; for (const auto& B : FloorBins) Totals.FloorM3 += B.Value;
	Totals.GroundM3 = 0.0; for (const auto& B : GroundBins) Totals.GroundM3 += B.Value;
	const double Held = Totals.AirborneM3 + Totals.WaitingM3 + Totals.FilmM3 + Totals.FloorM3 + Totals.GroundM3;
	Totals.Residual = Totals.EmittedM3 > 0.0 ? FMath::Abs(Totals.EmittedM3 - Held) / Totals.EmittedM3 : 0.0;
}
