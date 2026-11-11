// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/WaterDisplay.h"

#include "Simulation/Water/FloorWater.h"
#include "Simulation/Water/LeakSpray.h"

namespace
{
	constexpr int32 RoomInteriorObstacle = INDEX_NONE;   // LeakSpray: a room wall
	constexpr int32 RoomExteriorObstacle = -2;           // LeakSpray: the room's outer wall

	FVector AxisVector(int32 K) { FVector V = FVector::ZeroVector; V[K] = 1.0; return V; }

	// Top of the water in a cell [world m, without the display lift]; bed when dry or outside.
	double SurfaceZ(const FFloorWater& F, int32 I, int32 J, double MinDepthM, bool& bWater)
	{
		bWater = false;
		if (I < 0 || J < 0 || I >= F.GetNx() || J >= F.GetNy()) return -TNumericLimits<double>::Max();
		const int32 C = F.Index(I, J);
		const double Bed = F.GetFloorZM() + F.GetBedM()[C];
		if (F.GetSolid()[C]) return -TNumericLimits<double>::Max();
		const double H = F.GetDepthM()[C];
		bWater = H >= MinDepthM;
		return Bed + (bWater ? H : 0.0);
	}

	void AddQuad(FWaterSurfaceMesh& M, const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& N, const FLinearColor& Col)
	{
		const int32 Base = M.VerticesM.Num();
		for (const FVector& P : { A, B, C, D }) { M.VerticesM.Add(P); M.NormalsM.Add(N); M.Colors.Add(Col); }
		M.Triangles.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
	}

	void AddFilmSheet(const FLeakSpray& Spray, const FSprayFilm& F, TArray<FWaterSheet>& Out)
	{
		if (F.VolumeM3 <= 0.0) return;
		const FSprayGeometry& G = Spray.GetGeometry();
		const int32 K = F.Face / 2;
		const double Sign = (F.Face % 2) ? 1.0 : -1.0;
		FVector Normal, FaceCenter;
		FQuat Rot = FQuat::Identity;
		FVector Half = FVector::ZeroVector;
		if (F.Obstacle >= 0)
		{
			if (!G.Obstacles.IsValidIndex(F.Obstacle)) return;
			const FSprayObstacle& O = G.Obstacles[F.Obstacle];
			Rot = O.Rotation; Half = O.HalfM;
			Normal = O.Rotation.RotateVector(AxisVector(K) * Sign);
			FaceCenter = O.CenterM + Normal * O.HalfM[K];
		}
		else
		{
			if (!G.RoomM.IsValid) return;
			Half = G.RoomM.GetExtent();
			const FVector Outward = AxisVector(K) * Sign;   // face K of the room box
			Normal = F.Obstacle == RoomExteriorObstacle ? Outward : -Outward;
			FaceCenter = G.RoomM.GetCenter() + Outward * Half[K];
		}
		if (Normal.Z > 0.7 && (F.Obstacle >= 0 || F.Obstacle == RoomExteriorObstacle))
		{
			// The top of a box draining over its edges: the water lies on the whole top face.
			const int32 A1 = (K + 1) % 3, A2 = (K + 2) % 3;
			const double Area = 4.0 * Half[A1] * Half[A2];
			if (Area <= 0.0) return;
			FWaterSheet& S = Out.AddDefaulted_GetRef();
			const double T = F.VolumeM3 / Area;
			S.Rotation = FRotationMatrix::MakeFromXY(Normal, Rot.RotateVector(AxisVector(A1))).ToQuat();
			S.SizeM = FVector(T, 2.0 * Half[A1], 2.0 * Half[A2]);
			S.CenterM = FaceCenter + Normal * (.5 * T);
			S.VolumeM3 = S.SizeM.X * S.SizeM.Y * S.SizeM.Z;
			return;
		}
		const double W = F.WidthM, H = F.WetHeightM;
		if (W <= 0.0 || H <= 0.0) return;
		const FVector N2 = FVector(Normal.X, Normal.Y, 0.0).GetSafeNormal();
		if (N2.IsNearlyZero()) return;
		const double T = F.VolumeM3 / (W * H);
		// On the face plane, centred on the face across, from the foot up the wet height.
		const FVector OnFace = FaceCenter + N2 * (.5 * T);
		FWaterSheet& S = Out.AddDefaulted_GetRef();
		S.Rotation = FRotationMatrix::MakeFromXZ(N2, FVector::UpVector).ToQuat();
		S.SizeM = FVector(T, W, H);
		S.CenterM = FVector(OnFace.X, OnFace.Y, F.FootM.Z + .5 * H);
		S.VolumeM3 = S.SizeM.X * S.SizeM.Y * S.SizeM.Z;
	}
}

void WaterDisplay::UpdateEverWet(const FFloorWater& Floor, TArray<uint8>& EverWet, double MinDepthM)
{
	const int32 N = Floor.GetNx() * Floor.GetNy();
	if (EverWet.Num() != N) EverWet.Init(0, N);
	const FIntRect Box = Floor.GetWetBox();
	for (int32 J = Box.Min.Y; J <= Box.Max.Y; ++J)
		for (int32 I = Box.Min.X; I <= Box.Max.X; ++I)
		{
			const int32 C = Floor.Index(I, J);
			if (Floor.GetDepthM()[C] >= MinDepthM) EverWet[C] = 1;
		}
}

void WaterDisplay::Build(const FFloorWater& Floor, const FLeakSpray& Spray, const TArray<uint8>* EverWet, const FWaterDisplaySettings& S, FWaterDisplaySnapshot& Out)
{
	Out.Surface.Reset(); Out.WetFloor.Reset(); Out.YardSurface.Reset();
	Out.Sheets.Reset(); Out.Drops.Reset(); Out.Streaks.Reset(); Out.Skins.Reset();
	FWaterDisplayTotals& T = Out.Totals;
	T = FWaterDisplayTotals();

	// ---- floor: one prism per wet cell ----
	T.FloorComputedM3 = Floor.GetTotals().StoredM3;
	T.SurfaceCells = BuildSurface(Floor, S, Out.Surface, T.FloorHiddenM3);
	T.FloorShownM3 = SurfaceVolumeM3(Out.Surface, Floor, S.LiftM);
	BuildRest(Floor, Spray, EverWet, S, Out);
}

int32 WaterDisplay::BuildSurface(const FFloorWater& Floor, const FWaterDisplaySettings& S, FWaterSurfaceMesh& Mesh, double& HiddenM3)
{
	Mesh.Reset();
	HiddenM3 = 0.0;
	int32 Cells = 0;
	const int32 Nx = Floor.GetNx(), Ny = Floor.GetNy();
	const double Cell = Floor.GetCellM();
	const FVector2D O = Floor.GetOriginM();
	const FIntRect Box = Floor.GetWetBox();
	if (Nx > 0 && Ny > 0)
		for (int32 J = Box.Min.Y; J <= Box.Max.Y; ++J)
			for (int32 I = Box.Min.X; I <= Box.Max.X; ++I)
			{
				const int32 C = Floor.Index(I, J);
				const double H = Floor.GetDepthM()[C];
				if (H <= 0.0) continue;
				if (Floor.GetSolid()[C] || H < S.MinDepthM || Cells >= S.MaxSurfaceCells) { HiddenM3 += H * Cell * Cell; continue; }
				++Cells;
				const double Bed = Floor.GetFloorZM() + Floor.GetBedM()[C];
				const double Top = Bed + H;
				const double X0 = O.X + I * Cell, X1 = X0 + Cell, Y0 = O.Y + J * Cell, Y1 = Y0 + Cell;
				const double U = Floor.GetHu()[C] / H, V = Floor.GetHv()[C] / H;
				const FLinearColor Col(float(FMath::Clamp(H / S.DepthColorRangeM, 0.0, 1.0)),
					float(FMath::Clamp(.5 + .5 * U / S.SpeedColorRangeMs, 0.0, 1.0)), float(FMath::Clamp(.5 + .5 * V / S.SpeedColorRangeMs, 0.0, 1.0)), 1.f);
				const double Zt = Top + S.LiftM;
				AddQuad(Mesh, FVector(X0, Y0, Zt), FVector(X0, Y1, Zt), FVector(X1, Y1, Zt), FVector(X1, Y0, Zt), FVector::UpVector, Col);
				// Skirts down to whatever stands next to it (its water top, or the own bed when it is dry / a wall).
				const int32 DI[4] = { -1, 1, 0, 0 }, DJ[4] = { 0, 0, -1, 1 };
				for (int32 Side = 0; Side < 4; ++Side)
				{
					bool bN = false;
					const double NTop = FMath::Max(SurfaceZ(Floor, I + DI[Side], J + DJ[Side], S.MinDepthM, bN), Bed);
					if (bN && NTop >= Top) continue;
					const double Zb = (bN ? NTop : Bed) + S.LiftM;
					if (Zt - Zb <= 0.0) continue;
					FVector A, B;
					switch (Side)
					{
					case 0: A = FVector(X0, Y1, 0); B = FVector(X0, Y0, 0); break;
					case 1: A = FVector(X1, Y0, 0); B = FVector(X1, Y1, 0); break;
					case 2: A = FVector(X0, Y0, 0); B = FVector(X1, Y0, 0); break;
					default: A = FVector(X1, Y1, 0); B = FVector(X0, Y1, 0); break;
					}
					const FVector N(DI[Side], DJ[Side], 0);
					AddQuad(Mesh, FVector(A.X, A.Y, Zb), FVector(A.X, A.Y, Zt), FVector(B.X, B.Y, Zt), FVector(B.X, B.Y, Zb), N, Col);
				}
			}
	Mesh.Cells = Cells;
	return Cells;
}

void WaterDisplay::BuildRest(const FFloorWater& Floor, const FLeakSpray& Spray, const TArray<uint8>* EverWet, const FWaterDisplaySettings& S, FWaterDisplaySnapshot& Out)
{
	FWaterDisplayTotals& T = Out.Totals;
	const int32 Nx = Floor.GetNx(), Ny = Floor.GetNy();
	const double Cell = Floor.GetCellM();
	const FVector2D O = Floor.GetOriginM();

	// Wet marks: wet once, dry now.
	if (EverWet != nullptr && EverWet->Num() == Nx * Ny)
		for (int32 J = 0; J < Ny; ++J)
			for (int32 I = 0; I < Nx; ++I)
			{
				const int32 C = Floor.Index(I, J);
				if (!(*EverWet)[C] || Floor.GetSolid()[C] || Floor.GetDepthM()[C] >= S.MinDepthM) continue;
				const double Z = Floor.GetFloorZM() + Floor.GetBedM()[C] + S.WetMarkLiftM;
				const double X0 = O.X + I * Cell, Y0 = O.Y + J * Cell;
				AddQuad(Out.WetFloor, FVector(X0, Y0, Z), FVector(X0, Y0 + Cell, Z), FVector(X0 + Cell, Y0 + Cell, Z), FVector(X0 + Cell, Y0, Z), FVector::UpVector, FLinearColor::White);
				++T.WetMarkCells;
			}
	Out.WetFloor.Cells = T.WetMarkCells;

	// ---- drip trays ----
	for (const FFloorTray& Tr : Floor.GetTrays())
	{
		T.TrayComputedM3 += Tr.StoredM3;
		const FVector2D Size = Tr.MaxM - Tr.MinM;
		const double Area = Size.X * Size.Y;
		if (Tr.StoredM3 <= 0.0 || Area <= 0.0) continue;
		FWaterSheet& Sh = Out.Sheets.AddDefaulted_GetRef();
		Sh.bTray = true;
		const double D = Tr.StoredM3 / Area;
		// X through the sheet = up.
		Sh.Rotation = FRotationMatrix::MakeFromXY(FVector::UpVector, FVector::ForwardVector).ToQuat();
		Sh.SizeM = FVector(D, Size.X, Size.Y);   // local Y = world X, local Z = world Y under this rotation
		const FVector2D C2 = .5 * (Tr.MinM + Tr.MaxM);
		Sh.CenterM = FVector(C2.X, C2.Y, Floor.GetFloorZM() + .5 * D + S.LiftM);
		Sh.VolumeM3 = Sh.SizeM.X * Sh.SizeM.Y * Sh.SizeM.Z;
	}
	T.TrayShownM3 = SheetVolumeM3(Out.Sheets, true);

	// ---- films ----
	for (const FSprayFilm& F : Spray.GetFilms())
	{
		T.FilmComputedM3 += F.VolumeM3;
		AddFilmSheet(Spray, F, Out.Sheets);
	}
	T.FilmShownM3 = SheetVolumeM3(Out.Sheets, false);

	// ---- air: every parcel, every last-step path ----
	const TArray<FVector>& P = Spray.GetParcelPositionsM();
	const TArray<double>& Dia = Spray.GetParcelDiametersM();
	const TArray<double>& Vol = Spray.GetParcelVolumesM3();
	const TArray<uint8>& Jet = Spray.GetParcelIsJet();
	T.ParcelsComputed = P.Num();
	T.AirComputedM3 = Spray.GetTotals().AirborneM3;
	Out.Drops.Reserve(P.Num());
	for (int32 I = 0; I < P.Num(); ++I)
	{
		FWaterDropShape& D = Out.Drops.AddDefaulted_GetRef();
		D.CenterM = P[I]; D.DiameterM = Dia[I]; D.VolumeM3 = Vol[I]; D.bJet = Jet[I] != 0;
		T.AirShownM3 += Vol[I];
	}
	T.ParcelsShown = Out.Drops.Num();
	const TArray<FVector>& From = Spray.GetLastStepTrailFromM();
	const TArray<FVector>& To = Spray.GetLastStepTrailToM();
	const TArray<uint8>& TJ = Spray.GetLastStepTrailIsJet();
	const TArray<double>& TD = Spray.GetLastStepTrailDiameterM();
	T.TrailsComputed = From.Num();
	for (int32 I = 0; I < From.Num(); ++I)
	{
		if (!TD.IsValidIndex(I) || FVector::DistSquared(From[I], To[I]) <= 0.0) continue;
		FWaterStreakShape& St = Out.Streaks.AddDefaulted_GetRef();
		St.FromM = From[I]; St.ToM = To[I]; St.DiameterM = TD[I]; St.bJet = TJ[I] != 0;
	}
	T.StreaksShown = Out.Streaks.Num();

	// ---- wet skins: every box the water touched, bottom to the highest wet point ----
	const FSprayGeometry& G = Spray.GetGeometry();
	const TArray<FSprayWetting>& Wet = Spray.GetWetting();
	TArray<double> TopWet;
	TopWet.Init(-TNumericLimits<double>::Max(), G.Obstacles.Num());
	for (int32 I = 0; I < FMath::Min(Wet.Num(), G.Obstacles.Num()); ++I)
		if (Wet[I].FirstWetS >= 0.0) TopWet[I] = Wet[I].TopWetZM;
	for (const FFloorFlood& Fl : Floor.GetFloods())
	{
		if (!G.Obstacles.IsValidIndex(Fl.SourceObstacle) || Fl.FirstWetS < 0.0 || Fl.Cells.IsEmpty()) continue;
		double Bed = 0.0;
		for (int32 C : Fl.Cells) Bed += Floor.GetBedM()[C];
		TopWet[Fl.SourceObstacle] = FMath::Max(TopWet[Fl.SourceObstacle], Floor.GetFloorZM() + Bed / Fl.Cells.Num() + Fl.PeakDepthM);
	}
	for (int32 I = 0; I < G.Obstacles.Num(); ++I)
	{
		const FSprayObstacle& Ob = G.Obstacles[I];
		if (TopWet[I] <= -1.0e29 || Ob.Kind == ESprayObstacleKind::Ceiling) continue;
		// Upright boxes only (all equipment stands upright; a tilted box would need a cut skin).
		if (FMath::Abs(Ob.Rotation.GetAxisZ().Z) < .999) continue;
		const double Bottom = Ob.CenterM.Z - Ob.HalfM.Z, TopBox = Ob.CenterM.Z + Ob.HalfM.Z;
		const double Top = FMath::Min(TopWet[I], TopBox);
		if (Top <= Bottom) continue;
		FWetSkin& Sk = Out.Skins.AddDefaulted_GetRef();
		Sk.Obstacle = I;
		Sk.Rotation = Ob.Rotation;
		Sk.SizeM = FVector(2.0 * Ob.HalfM.X + 2.0 * S.SkinOffsetM, 2.0 * Ob.HalfM.Y + 2.0 * S.SkinOffsetM, Top - Bottom + (Top >= TopBox ? S.SkinOffsetM : 0.0));
		Sk.CenterM = FVector(Ob.CenterM.X, Ob.CenterM.Y, Bottom + .5 * Sk.SizeM.Z);
	}
	T.WetSkins = Out.Skins.Num();

	auto Rel = [](double Shown, double Computed) { return Computed > 0.0 ? FMath::Abs(Shown - Computed) / Computed : (Shown != 0.0 ? 1.0 : 0.0); };
	T.Residual = FMath::Max(FMath::Max(Rel(T.FloorShownM3 + T.FloorHiddenM3, T.FloorComputedM3), Rel(T.TrayShownM3, T.TrayComputedM3)),
		FMath::Max(Rel(T.FilmShownM3, T.FilmComputedM3), Rel(T.AirShownM3, T.AirComputedM3)));
}

double WaterDisplay::SurfaceVolumeM3(const FWaterSurfaceMesh& M, const FFloorWater& Floor, double LiftM)
{
	const int32 Nx = Floor.GetNx(), Ny = Floor.GetNy();
	const double Cell = Floor.GetCellM();
	const FVector2D O = Floor.GetOriginM();
	double Volume = 0.0;
	for (int32 T = 0; T + 2 < M.Triangles.Num(); T += 3)
	{
		const FVector& A = M.VerticesM[M.Triangles[T]];
		const FVector& B = M.VerticesM[M.Triangles[T + 1]];
		const FVector& C = M.VerticesM[M.Triangles[T + 2]];
		const double PlanArea = .5 * FMath::Abs((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X));
		if (PlanArea <= 0.0) continue;   // a vertical skirt
		const FVector Centroid = (A + B + C) / 3.0;
		const int32 I = FMath::Clamp(FMath::FloorToInt32((Centroid.X - O.X) / Cell), 0, Nx - 1);
		const int32 J = FMath::Clamp(FMath::FloorToInt32((Centroid.Y - O.Y) / Cell), 0, Ny - 1);
		const double Bed = Floor.GetFloorZM() + Floor.GetBedM()[Floor.Index(I, J)];
		Volume += PlanArea * (Centroid.Z - LiftM - Bed);
	}
	return Volume;
}

double WaterDisplay::SheetVolumeM3(const TArray<FWaterSheet>& Sheets, bool bTrays)
{
	double V = 0.0;
	for (const FWaterSheet& S : Sheets) if (S.bTray == bTrays) V += S.SizeM.X * S.SizeM.Y * S.SizeM.Z;
	return V;
}
