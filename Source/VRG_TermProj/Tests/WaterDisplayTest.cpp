// Fill out your copyright notice in the Description page of Project Settings.

// W6 display snapshot (Tools/누수_실시간_계획.md L6, 2026-11-10): what is drawn is what is computed.
// The snapshot built by WaterDisplay::Build from the W3 floor water and the W2 spray is read back and compared with the
// models' own state: floor volume from the mesh, every parcel and last-step path, film and tray sheets, wet marks and
// wet skins, and bit-identical rebuilds. Plain C++ only (no world).

#include "Misc/AutomationTest.h"
#include "Simulation/Water/FloorWater.h"
#include "Simulation/Water/LeakSpray.h"
#include "Visualization/WaterDisplay.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VrgWaterDisplayTest
{
	constexpr EAutomationTestFlags DisplayTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	double WdRelErr(double Shown, double Computed)
	{
		return Computed != 0.0 ? FMath::Abs(Shown - Computed) / FMath::Abs(Computed) : FMath::Abs(Shown);
	}

	FVector WdAxis(int32 K) { FVector V = FVector::ZeroVector; V[K] = 1.0; return V; }

	// Gauge pressure that gives a discharge speed v (inverse of v = Cv sqrt(2 p / rho)).
	double PressureForSpeed(double SpeedMs, const FSpraySettings& S, const FSprayFluid& F)
	{
		return .5 * F.WaterDensityKgM3 * FMath::Square(SpeedMs / S.VelocityCoefficient);
	}

	FSpraySource SourceToward(int32 Id, const FVector& OriginM, const FVector& VelocityMs, double VolumeM3, const FSpraySettings& S, const FSprayFluid& F)
	{
		FSpraySource Src;
		Src.Id = Id; Src.OriginM = OriginM; Src.DirectionM = VelocityMs.GetSafeNormal();
		Src.GaugePressurePa = PressureForSpeed(VelocityMs.Size(), S, F); Src.VolumeM3 = VolumeM3;
		return Src;
	}

	FSprayObstacle& WdAddBox(FSprayGeometry& G, const FVector& CenterM, const FVector& HalfM, const TCHAR* Name)
	{
		FSprayObstacle& O = G.Obstacles.AddDefaulted_GetRef();
		O.CenterM = CenterM; O.HalfM = HalfM; O.Name = Name;
		return O;
	}

	// World half extents of a rotated box of full size SizeM.
	FVector WorldHalfExtent(const FQuat& R, const FVector& SizeM)
	{
		return (R.GetAxisX().GetAbs() * SizeM.X + R.GetAxisY().GetAbs() * SizeM.Y + R.GetAxisZ().GetAbs() * SizeM.Z) * .5;
	}

	// Walks the floor surface mesh quad by quad (Build writes a top quad, then that cell's skirts).
	struct FSurfaceWalk
	{
		int32 TopQuads = 0, Skirts = 0, ZeroAreaTriangles = 0;
		double WorstTopZ = 0.0;        // |top z - (floor + bed + depth + lift)|
		double WorstBelowBed = 0.0;    // max(floor + bed + lift - vertex z) over all vertices of a cell's quads
		double WorstSkirtTop = 0.0;    // |skirt top - its cell's top|
		bool bLayout = true;           // 4 vertices / 6 indices per quad, normals and colours per vertex
		bool bSkirtsVertical = true;
	};

	FSurfaceWalk WalkSurface(const FWaterSurfaceMesh& M, const FFloorWater& Floor, double LiftM)
	{
		FSurfaceWalk W;
		const int32 NumV = M.VerticesM.Num();
		W.bLayout = NumV % 4 == 0 && M.Triangles.Num() == NumV / 4 * 6 && M.NormalsM.Num() == NumV && M.Colors.Num() == NumV;
		if (!W.bLayout) return W;
		const double Cell = Floor.GetCellM();
		const FVector2D O = Floor.GetOriginM();
		double CellBedZ = 0.0, CellTopZ = 0.0;
		bool bHaveCell = false;
		for (int32 Quad = 0; Quad < NumV / 4; ++Quad)
		{
			const FVector& V0 = M.VerticesM[4 * Quad];
			const FVector& V1 = M.VerticesM[4 * Quad + 1];
			const FVector& V2 = M.VerticesM[4 * Quad + 2];
			const FVector& V3 = M.VerticesM[4 * Quad + 3];
			if (M.NormalsM[4 * Quad] == FVector::UpVector)
			{
				++W.TopQuads;
				const double Cx = .5 * (V0.X + V2.X), Cy = .5 * (V0.Y + V2.Y);
				const int32 I = FMath::FloorToInt32((Cx - O.X) / Cell), J = FMath::FloorToInt32((Cy - O.Y) / Cell);
				if (I < 0 || J < 0 || I >= Floor.GetNx() || J >= Floor.GetNy()) { W.bLayout = false; return W; }
				const int32 C = Floor.Index(I, J);
				CellBedZ = Floor.GetFloorZM() + Floor.GetBedM()[C] + LiftM;
				CellTopZ = Floor.GetFloorZM() + Floor.GetBedM()[C] + Floor.GetDepthM()[C] + LiftM;
				bHaveCell = true;
				for (const FVector* P : { &V0, &V1, &V2, &V3 }) W.WorstTopZ = FMath::Max(W.WorstTopZ, FMath::Abs(P->Z - CellTopZ));
			}
			else
			{
				++W.Skirts;
				if (!bHaveCell) { W.bLayout = false; return W; }
				// AddQuad order for a skirt: (A, Zb), (A, Zt), (B, Zt), (B, Zb).
				W.bSkirtsVertical &= V0.X == V1.X && V0.Y == V1.Y && V2.X == V3.X && V2.Y == V3.Y && V0.Z == V3.Z && V1.Z == V2.Z && V1.Z > V0.Z;
				W.bSkirtsVertical &= M.NormalsM[4 * Quad].Z == 0.0;
				W.WorstSkirtTop = FMath::Max(W.WorstSkirtTop, FMath::Abs(V1.Z - CellTopZ));
			}
			for (const FVector* P : { &V0, &V1, &V2, &V3 }) W.WorstBelowBed = FMath::Max(W.WorstBelowBed, CellBedZ - P->Z);
		}
		for (int32 T = 0; T + 2 < M.Triangles.Num(); T += 3)
		{
			const FVector& A = M.VerticesM[M.Triangles[T]];
			const FVector& B = M.VerticesM[M.Triangles[T + 1]];
			const FVector& C = M.VerticesM[M.Triangles[T + 2]];
			if ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X) == 0.0) ++W.ZeroAreaTriangles;
		}
		return W;
	}

	// Σ H cell² over the open cells, the part thinner than MinDepthM, and the number of drawn cells.
	void FloorExpectation(const FFloorWater& Floor, double MinDepthM, double& OutVolumeM3, double& OutHiddenM3, int32& OutDrawn)
	{
		OutVolumeM3 = 0.0; OutHiddenM3 = 0.0; OutDrawn = 0;
		const double A = Floor.GetCellM() * Floor.GetCellM();
		for (int32 J = 0; J < Floor.GetNy(); ++J)
			for (int32 I = 0; I < Floor.GetNx(); ++I)
			{
				const int32 C = Floor.Index(I, J);
				const double H = Floor.GetDepthM()[C];
				if (Floor.GetSolid()[C] || H <= 0.0) continue;
				OutVolumeM3 += H * A;
				if (H < MinDepthM) OutHiddenM3 += H * A; else ++OutDrawn;
			}
	}

	template <typename ElementType>
	bool WdSameBits(const TArray<ElementType>& A, const TArray<ElementType>& B)
	{
		return A.Num() == B.Num() && (A.Num() == 0 || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(ElementType)) == 0);
	}

	bool SameMesh(const FWaterSurfaceMesh& A, const FWaterSurfaceMesh& B)
	{
		return A.Cells == B.Cells && WdSameBits(A.VerticesM, B.VerticesM) && WdSameBits(A.Triangles, B.Triangles) && WdSameBits(A.NormalsM, B.NormalsM) && WdSameBits(A.Colors, B.Colors);
	}

	bool SameQuat(const FQuat& A, const FQuat& B) { return A.X == B.X && A.Y == B.Y && A.Z == B.Z && A.W == B.W; }

	bool SameSnapshot(const FWaterDisplaySnapshot& A, const FWaterDisplaySnapshot& B)
	{
		bool bSame = SameMesh(A.Surface, B.Surface) && SameMesh(A.WetFloor, B.WetFloor);
		bSame &= A.Sheets.Num() == B.Sheets.Num() && A.Drops.Num() == B.Drops.Num() && A.Streaks.Num() == B.Streaks.Num() && A.Skins.Num() == B.Skins.Num();
		if (!bSame) return false;
		for (int32 I = 0; I < A.Sheets.Num(); ++I)
		{
			const FWaterSheet &X = A.Sheets[I], &Y = B.Sheets[I];
			bSame &= X.CenterM == Y.CenterM && SameQuat(X.Rotation, Y.Rotation) && X.SizeM == Y.SizeM && X.VolumeM3 == Y.VolumeM3 && X.bTray == Y.bTray;
		}
		for (int32 I = 0; I < A.Drops.Num(); ++I)
		{
			const FWaterDropShape &X = A.Drops[I], &Y = B.Drops[I];
			bSame &= X.CenterM == Y.CenterM && X.DiameterM == Y.DiameterM && X.VolumeM3 == Y.VolumeM3 && X.bJet == Y.bJet;
		}
		for (int32 I = 0; I < A.Streaks.Num(); ++I)
		{
			const FWaterStreakShape &X = A.Streaks[I], &Y = B.Streaks[I];
			bSame &= X.FromM == Y.FromM && X.ToM == Y.ToM && X.DiameterM == Y.DiameterM && X.bJet == Y.bJet;
		}
		for (int32 I = 0; I < A.Skins.Num(); ++I)
		{
			const FWetSkin &X = A.Skins[I], &Y = B.Skins[I];
			bSame &= X.CenterM == Y.CenterM && SameQuat(X.Rotation, Y.Rotation) && X.SizeM == Y.SizeM && X.Obstacle == Y.Obstacle;
		}
		const FWaterDisplayTotals &T = A.Totals, &U = B.Totals;
		bSame &= T.FloorShownM3 == U.FloorShownM3 && T.FloorComputedM3 == U.FloorComputedM3 && T.FloorHiddenM3 == U.FloorHiddenM3
			&& T.TrayShownM3 == U.TrayShownM3 && T.TrayComputedM3 == U.TrayComputedM3 && T.FilmShownM3 == U.FilmShownM3 && T.FilmComputedM3 == U.FilmComputedM3
			&& T.AirShownM3 == U.AirShownM3 && T.AirComputedM3 == U.AirComputedM3 && T.ParcelsShown == U.ParcelsShown && T.ParcelsComputed == U.ParcelsComputed
			&& T.StreaksShown == U.StreaksShown && T.TrailsComputed == U.TrailsComputed && T.SurfaceCells == U.SurfaceCells && T.WetMarkCells == U.WetMarkCells
			&& T.WetSkins == U.WetSkins && T.Residual == U.Residual;
		return bSame;
	}

	// A room with a cabinet; a gentle sideways jet onto the cabinet's -X face and one onto the room wall x = 0 (films).
	FSprayGeometry FilmRoom()
	{
		FSprayGeometry G;
		G.RoomM = FBox(FVector::ZeroVector, FVector(6, 6, 3));
		WdAddBox(G, FVector(3, 3, 1), FVector(.5, .5, 1), TEXT("cabinet"));
		return G;
	}

	void RunFilmSpray(FLeakSpray& Spray, int32 Steps, double DtS)
	{
		const FSpraySettings& S = Spray.GetSettings();
		const FSprayFluid& F = Spray.GetFluid();
		const double FlowM3s = 1.0e-5;
		for (int32 I = 0; I < Steps; ++I)
			Spray.Step({ SourceToward(0, FVector(2.45, 3, 1.2), FVector(.5, 0, 0), FlowM3s * DtS, S, F),
				SourceToward(1, FVector(.05, 1.5, 1.0), FVector(-.5, 0, 0), FlowM3s * DtS, S, F) }, DtS, I * DtS);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayFloorVolumeTest, "VRG.Water.Display.FloorVolumeMatchesComputed", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayFloorVolumeTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	// A 40 x 30 floor (2.5 cm cells) on a ramp up to 4 mm, a short wall, depths 0.02-12 mm with momentum, and a few cells
	// thinner than the drawing limit: the volume read back from the mesh plus the hidden part equals the stored volume.
	const FFloorWaterSettings FS;
	const int32 Nx = 40, Ny = 30;
	const double FloorZ = .3;
	FFloorWater Floor;
	Floor.Reset(FVector2D(1.0, -2.0), Nx, Ny, FloorZ, FS);
	for (int32 J = 0; J < Ny; ++J)
		for (int32 I = 0; I < Nx; ++I) Floor.SetBed(I, J, 4.0e-3 * double(I) / double(Nx - 1));
	for (int32 J = 10; J <= 14; ++J) Floor.SetSolid(20, J, true);
	for (int32 J = 4; J <= 25; ++J)
		for (int32 I = 5; I <= 34; ++I)
		{
			const double Frac = double((I * 7 + J * 13) % 31) / 30.0;
			const double H = 2.0e-5 + (12.0e-3 - 2.0e-5) * Frac;
			Floor.SetState(I, J, H, .05 * H * FMath::Sin(.3 * I), -.04 * H * FMath::Cos(.2 * J));
		}
	// Thinner than MinDepthM (1e-6 m): hidden, inside and outside the wet region.
	Floor.SetState(1, 1, 3.0e-7); Floor.SetState(2, 1, 5.0e-7); Floor.SetState(38, 28, 9.0e-7); Floor.SetState(6, 4, 4.0e-7);

	FLeakSpray Spray;
	Spray.Reset(FSprayGeometry());
	const FWaterDisplaySettings S;
	FWaterDisplaySnapshot Snap;
	WaterDisplay::Build(Floor, Spray, nullptr, S, Snap);
	const FWaterDisplayTotals& T = Snap.Totals;

	double Expected = 0.0, ExpectedHidden = 0.0;
	int32 ExpectedDrawn = 0;
	FloorExpectation(Floor, S.MinDepthM, Expected, ExpectedHidden, ExpectedDrawn);
	const double MeshV = WaterDisplay::SurfaceVolumeM3(Snap.Surface, Floor, S.LiftM);
	const double ErrSelf = WdRelErr(MeshV + T.FloorHiddenM3, Expected);
	const double ErrModel = WdRelErr(MeshV + T.FloorHiddenM3, Floor.GetTotals().StoredM3);
	const double ErrHidden = WdRelErr(T.FloorHiddenM3, ExpectedHidden);
	const FSurfaceWalk W = WalkSurface(Snap.Surface, Floor, S.LiftM);

	// Translation invariance: another lift moves the whole surface, the volume read back stays.
	FWaterDisplaySettings S0 = S; S0.LiftM = 0.0;
	FWaterDisplaySettings S5 = S; S5.LiftM = .05;
	FWaterDisplaySnapshot Snap0, Snap5;
	WaterDisplay::Build(Floor, Spray, nullptr, S0, Snap0);
	WaterDisplay::Build(Floor, Spray, nullptr, S5, Snap5);
	const double V0 = WaterDisplay::SurfaceVolumeM3(Snap0.Surface, Floor, S0.LiftM), V5 = WaterDisplay::SurfaceVolumeM3(Snap5.Surface, Floor, S5.LiftM);
	const double ErrLift = FMath::Max(WdRelErr(V0, MeshV), WdRelErr(V5, MeshV));
	const FSurfaceWalk W5 = WalkSurface(Snap5.Surface, Floor, S5.LiftM);

	AddInfo(FString::Printf(TEXT("바닥 표시: 칸 %d(기대 %d), 윗면 %d, 옆면 %d(면적 0 삼각형 %d) | 계산 %.6e m3, 메시 %.6e + 숨김 %.3e m3(기대 숨김 %.3e) | 상대 오차: 직접 합 %.2e, 모델 StoredM3 %.2e, 숨김 %.2e, Residual %.2e | 띄움 0/1/50 mm 부피 차 %.2e | 윗면 높이 %.2e m, 바닥 아래 %.2e m, 옆면 위 %.2e m"),
		T.SurfaceCells, ExpectedDrawn, W.TopQuads, W.Skirts, W.ZeroAreaTriangles, Expected, MeshV, T.FloorHiddenM3, ExpectedHidden,
		ErrSelf, ErrModel, ErrHidden, T.Residual, ErrLift, W.WorstTopZ, W.WorstBelowBed, W.WorstSkirtTop));
	TestTrue(TEXT("mesh layout: 4 vertices / 2 triangles per quad, a normal and a colour per vertex"), W.bLayout && W5.bLayout);
	TestTrue(TEXT("one top quad per drawn cell (non-solid, depth >= MinDepthM)"), T.SurfaceCells == ExpectedDrawn && Snap.Surface.Cells == ExpectedDrawn && W.TopQuads == ExpectedDrawn);
	TestTrue(TEXT("mesh volume + hidden = computed floor volume (relative <= 1e-12)"), ErrSelf <= 1e-12 && ErrModel <= 1e-12);
	TestTrue(TEXT("hidden volume = cells thinner than MinDepthM"), ErrHidden <= 1e-12 && ExpectedHidden > 0.0);
	TestTrue(TEXT("snapshot residual <= 1e-12 and FloorShown = mesh volume"), T.Residual <= 1e-12 && T.FloorShownM3 == MeshV && T.FloorComputedM3 == Floor.GetTotals().StoredM3);
	TestTrue(TEXT("top = floor + bed + depth + lift; nothing below floor + bed + lift (1e-12 m)"), W.WorstTopZ <= 1e-12 && W.WorstBelowBed <= 1e-12 && W5.WorstTopZ <= 1e-12 && W5.WorstBelowBed <= 1e-12);
	TestTrue(TEXT("skirts exist at the wet edges, vertical (zero plan area), from the cell top down"), W.Skirts > 0 && W.bSkirtsVertical && W.ZeroAreaTriangles == 2 * W.Skirts && W.WorstSkirtTop <= 1e-12);
	TestTrue(TEXT("the display lift is a translation: same volume read back (relative <= 1e-12)"), ErrLift <= 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayFloorSpreadTest, "VRG.Water.Display.FloorAfterSpreading", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayFloorSpreadTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	// Water poured at a point for 0.5 s and left to spread for 2 s: the drawn floor still holds exactly the stored volume.
	const FFloorWaterSettings FS;
	FFloorWater Floor;
	Floor.Reset(FVector2D::ZeroVector, 60, 60, 0.0, FS);
	const double Dt = .02;
	for (int32 I = 0; I < 100; ++I)
	{
		if (I < 25) Floor.AddWater(FVector(.75, .75, 0.0), 2.0e-5);
		Floor.Step(Dt, I * Dt);
	}
	FLeakSpray Spray;
	Spray.Reset(FSprayGeometry());
	const FWaterDisplaySettings S;
	FWaterDisplaySnapshot Snap;
	WaterDisplay::Build(Floor, Spray, nullptr, S, Snap);
	const FWaterDisplayTotals& T = Snap.Totals;
	double Expected = 0.0, ExpectedHidden = 0.0;
	int32 ExpectedDrawn = 0;
	FloorExpectation(Floor, S.MinDepthM, Expected, ExpectedHidden, ExpectedDrawn);
	const double MeshV = WaterDisplay::SurfaceVolumeM3(Snap.Surface, Floor, S.LiftM);
	const double ErrModel = WdRelErr(MeshV + T.FloorHiddenM3, Floor.GetTotals().StoredM3);
	const double ErrSelf = WdRelErr(MeshV + T.FloorHiddenM3, Expected);
	const FSurfaceWalk W = WalkSurface(Snap.Surface, Floor, S.LiftM);
	AddInfo(FString::Printf(TEXT("퍼진 뒤(2 s, 받은 %.3f L): 바닥 모델 잔차 %.2e, 저장 %.6e m3, 칸 %d(기대 %d), 최대 수심 %.3f mm | 메시 %.6e + 숨김 %.3e m3 | 상대 오차 모델 %.2e, 직접 합 %.2e, Residual %.2e"),
		Floor.GetTotals().ReceivedM3 * 1e3, Floor.GetTotals().Residual, Floor.GetTotals().StoredM3, T.SurfaceCells, ExpectedDrawn, Floor.GetTotals().MaxDepthM * 1e3,
		MeshV, T.FloorHiddenM3, ErrModel, ErrSelf, T.Residual));
	TestTrue(TEXT("the water spread over many cells"), T.SurfaceCells > 25 && Floor.GetTotals().StoredM3 > 0.0);
	TestTrue(TEXT("drawn cells = cells at or above MinDepthM"), T.SurfaceCells == ExpectedDrawn && W.TopQuads == ExpectedDrawn && W.bLayout);
	TestTrue(TEXT("mesh volume + hidden = stored volume (relative <= 1e-12)"), ErrModel <= 1e-12 && ErrSelf <= 1e-12 && T.Residual <= 1e-12);
	TestTrue(TEXT("surface geometry: top at bed + depth + lift, vertical skirts"), W.WorstTopZ <= 1e-12 && W.WorstBelowBed <= 1e-12 && W.bSkirtsVertical && W.Skirts > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayParcelsTest, "VRG.Water.Display.ParcelsAndStreaks", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayParcelsTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	// A 150 kPa jet across a room with one cabinet, 10 steps: every parcel is one sphere and every last-step path one tube,
	// with the computed position, diameter and volume.
	FSprayGeometry G;
	G.RoomM = FBox(FVector::ZeroVector, FVector(4, 4, 3));
	WdAddBox(G, FVector(3, 2, .5), FVector(.3, .3, .5), TEXT("cabinet"));
	FLeakSpray Spray;
	Spray.Reset(G);
	const double Dt = .02;
	for (int32 I = 0; I < 10; ++I)
	{
		FSpraySource Src;
		Src.Id = 0; Src.OriginM = FVector(.5, 2, 1.5); Src.DirectionM = FVector(1, 0, -.2).GetSafeNormal();
		Src.GaugePressurePa = 1.5e5; Src.VolumeM3 = 2.0e-6;
		Spray.Step({ Src }, Dt, I * Dt);
	}
	FFloorWater Floor;
	Floor.ResetForRoom(G, FFloorWaterSettings());
	const FWaterDisplaySettings S;
	FWaterDisplaySnapshot Snap;
	WaterDisplay::Build(Floor, Spray, nullptr, S, Snap);
	const FWaterDisplayTotals& T = Snap.Totals;

	const TArray<FVector>& P = Spray.GetParcelPositionsM();
	const TArray<double>& Dia = Spray.GetParcelDiametersM();
	const TArray<double>& Vol = Spray.GetParcelVolumesM3();
	const TArray<uint8>& Jet = Spray.GetParcelIsJet();
	bool bDrops = Snap.Drops.Num() == P.Num() && T.ParcelsComputed == P.Num() && T.ParcelsShown == P.Num() && Spray.GetTotals().Parcels == P.Num();
	double SumVol = 0.0;
	int32 Jets = 0;
	for (int32 I = 0; bDrops && I < P.Num(); ++I)
	{
		const FWaterDropShape& D = Snap.Drops[I];
		bDrops &= D.CenterM == P[I] && D.DiameterM == Dia[I] && D.VolumeM3 == Vol[I] && D.bJet == (Jet[I] != 0) && D.DiameterM > 0.0;
		SumVol += Vol[I];
		Jets += Jet[I] != 0 ? 1 : 0;
	}
	const double ErrAirSum = WdRelErr(T.AirShownM3, SumVol), ErrAirModel = WdRelErr(T.AirShownM3, Spray.GetTotals().AirborneM3);

	const TArray<FVector>& From = Spray.GetLastStepTrailFromM();
	const TArray<FVector>& To = Spray.GetLastStepTrailToM();
	const TArray<uint8>& TJ = Spray.GetLastStepTrailIsJet();
	const TArray<double>& TD = Spray.GetLastStepTrailDiameterM();
	bool bStreaks = TD.Num() == From.Num() && To.Num() == From.Num() && TJ.Num() == From.Num() && T.TrailsComputed == From.Num();
	int32 Next = 0;
	for (int32 I = 0; bStreaks && I < From.Num(); ++I)
	{
		if (FVector::DistSquared(From[I], To[I]) <= 0.0) continue;
		if (!Snap.Streaks.IsValidIndex(Next)) { bStreaks = false; break; }
		const FWaterStreakShape& St = Snap.Streaks[Next++];
		bStreaks &= St.FromM == From[I] && St.ToM == To[I] && St.DiameterM == TD[I] && St.DiameterM > 0.0 && St.bJet == (TJ[I] != 0);
	}
	bStreaks &= Next == Snap.Streaks.Num() && T.StreaksShown == Snap.Streaks.Num() && T.StreaksShown <= T.TrailsComputed;

	AddInfo(FString::Printf(TEXT("공중: 입자 %d(물줄기 %d) = 그림 %d, 공중 물 %.6e m3(모델 %.6e) 상대 %.2e / %.2e | 경로 %d → 줄 %d | 분사 잔차 %.2e, 표시 Residual %.2e"),
		P.Num(), Jets, Snap.Drops.Num(), T.AirShownM3, Spray.GetTotals().AirborneM3, ErrAirSum, ErrAirModel, From.Num(), Snap.Streaks.Num(),
		Spray.GetTotals().Residual, T.Residual));
	TestTrue(TEXT("there is water in the air and paths to draw"), P.Num() > 0 && Snap.Streaks.Num() > 0);
	TestTrue(TEXT("one sphere per parcel: same centre, diameter, volume, jet flag (exact)"), bDrops);
	TestTrue(TEXT("air shown = sum of parcel volumes = spray airborne (relative <= 1e-12)"), ErrAirSum <= 1e-12 && ErrAirModel <= 1e-12 && T.AirComputedM3 == Spray.GetTotals().AirborneM3);
	TestTrue(TEXT("one tube per moving last-step path, with the trail's diameter (> 0)"), bStreaks);
	TestTrue(TEXT("snapshot residual <= 1e-12"), T.Residual <= 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayFilmsTraysTest, "VRG.Water.Display.FilmsAndTrays", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayFilmsTraysTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	// Gentle jets onto a cabinet face and a room wall make films; a 0.6 x 0.3 m drip tray holds 0.25 L.
	const FSprayGeometry G = FilmRoom();
	FLeakSpray Spray;
	Spray.Reset(G);
	RunFilmSpray(Spray, 100, .02);
	FFloorWater Floor;
	Floor.ResetForRoom(G, FFloorWaterSettings());
	const FVector2D TrayMin(4.5, .5), TrayMax(5.1, .8);
	Floor.AddTray(TEXT("tray"), TrayMin, TrayMax, 1.0e-3);
	Floor.AddWater(FVector(4.8, .6, 0.0), 2.5e-4);
	const FWaterDisplaySettings S;
	FWaterDisplaySnapshot Snap;
	WaterDisplay::Build(Floor, Spray, nullptr, S, Snap);
	const FWaterDisplayTotals& T = Snap.Totals;

	// Films: sheets in film order after the trays.
	const TArray<FSprayFilm>& Films = Spray.GetFilms();
	double FilmV = 0.0;
	int32 FilmsWithWater = 0;
	bool bCabinet = false, bRoomWall = false;
	for (const FSprayFilm& F : Films)
	{
		FilmV += F.VolumeM3;
		FilmsWithWater += F.VolumeM3 > 0.0 ? 1 : 0;
		bCabinet |= F.Obstacle == 0 && F.VolumeM3 > 0.0;
		bRoomWall |= F.Obstacle == INDEX_NONE && F.VolumeM3 > 0.0;
	}
	TArray<const FWaterSheet*> FilmSheets, TraySheets;
	for (const FWaterSheet& Sh : Snap.Sheets) (Sh.bTray ? TraySheets : FilmSheets).Add(&Sh);
	const double ErrFilm = WdRelErr(WaterDisplay::SheetVolumeM3(Snap.Sheets, false), FilmV);
	double WorstFoot = 0.0, WorstPlane = 0.0, WorstAxis = 0.0, WorstSize = 0.0, WorstOne = 0.0;
	bool bInside = true, bPositive = true;
	if (FilmSheets.Num() == FilmsWithWater)
	{
		int32 Next = 0;
		for (const FSprayFilm& F : Films)
		{
			if (F.VolumeM3 <= 0.0) continue;
			const FWaterSheet& Sh = *FilmSheets[Next++];
			// The wetted face as LeakSpray's hit test defines it: face 2K + (+side ? 1 : 0); a room face's normal points into the room.
			const int32 K = F.Face / 2;
			const double Side = (F.Face % 2) ? 1.0 : -1.0;
			FVector N, PlanePoint;
			if (F.Obstacle >= 0)
			{
				const FSprayObstacle& O = G.Obstacles[F.Obstacle];
				N = O.Rotation.RotateVector(WdAxis(K) * Side);
				PlanePoint = O.CenterM + N * O.HalfM[K];
			}
			else
			{
				N = -WdAxis(K) * Side;
				PlanePoint = G.RoomM.GetCenter() - N * G.RoomM.GetExtent()[K];
				bInside &= G.RoomM.IsInside(Sh.CenterM);
			}
			bPositive &= Sh.SizeM.X > 0.0 && Sh.SizeM.Y > 0.0 && Sh.SizeM.Z > 0.0 && !Sh.bTray;
			WorstFoot = FMath::Max(WorstFoot, FMath::Abs(Sh.CenterM.Z - .5 * Sh.SizeM.Z - F.FootM.Z));
			WorstPlane = FMath::Max(WorstPlane, FMath::Abs(FVector::DotProduct(Sh.CenterM - PlanePoint, N) - .5 * Sh.SizeM.X));
			WorstAxis = FMath::Max(WorstAxis, 1.0 - FVector::DotProduct(Sh.Rotation.GetAxisX(), N));
			WorstSize = FMath::Max(WorstSize, FMath::Max(FMath::Abs(Sh.SizeM.Y - F.WidthM), FMath::Abs(Sh.SizeM.Z - F.WetHeightM)));
			WorstOne = FMath::Max(WorstOne, WdRelErr(Sh.SizeM.X * Sh.SizeM.Y * Sh.SizeM.Z, F.VolumeM3));
		}
	}

	// Tray: stored / area deep, over the tray's footprint.
	const FFloorTray& Tray = Floor.GetTrays()[0];
	const double ErrTray = WdRelErr(WaterDisplay::SheetVolumeM3(Snap.Sheets, true), Tray.StoredM3);
	FVector TrayHalf = FVector::ZeroVector, TrayCenter = FVector::ZeroVector;
	double ExpectedDepth = 0.0;
	if (TraySheets.Num() == 1)
	{
		TrayHalf = WorldHalfExtent(TraySheets[0]->Rotation, TraySheets[0]->SizeM);
		TrayCenter = TraySheets[0]->CenterM;
		ExpectedDepth = Tray.StoredM3 / ((TrayMax.X - TrayMin.X) * (TrayMax.Y - TrayMin.Y));
	}
	const double TrayFootErr = FMath::Max(FMath::Abs(TrayHalf.X - .5 * (TrayMax.X - TrayMin.X)), FMath::Abs(TrayHalf.Y - .5 * (TrayMax.Y - TrayMin.Y)));
	const double TrayDepthErr = FMath::Abs(2.0 * TrayHalf.Z - ExpectedDepth);
	const double TrayCenterErr = FMath::Max(FMath::Abs(TrayCenter.X - .5 * (TrayMin.X + TrayMax.X)), FMath::Abs(TrayCenter.Y - .5 * (TrayMin.Y + TrayMax.Y)));
	const double TrayBottomErr = FMath::Abs(TrayCenter.Z - TrayHalf.Z - (Floor.GetFloorZM() + S.LiftM));

	AddInfo(FString::Printf(TEXT("물막 %d개(물 있음 %d, 판 %d) 부피 %.6e m3 상대 %.2e(판별 최대 %.2e) | 발치 %.2e m, 면 위치 %.2e m, 축 %.2e, 폭/높이 %.2e m | 받침 저장 %.6e m3 상대 %.2e, 평면 크기 오차 %.2e m(반폭 %.4f x %.4f m, 기대 %.4f x %.4f), 깊이 %.2e m, 중심 %.2e m, 바닥 %.2e m | Residual %.2e, 분사 잔차 %.2e"),
		Films.Num(), FilmsWithWater, FilmSheets.Num(), FilmV, ErrFilm, WorstOne, WorstFoot, WorstPlane, WorstAxis, WorstSize,
		Tray.StoredM3, ErrTray, TrayFootErr, TrayHalf.X, TrayHalf.Y, .5 * (TrayMax.X - TrayMin.X), .5 * (TrayMax.Y - TrayMin.Y), TrayDepthErr, TrayCenterErr, TrayBottomErr,
		T.Residual, Spray.GetTotals().Residual));
	TestTrue(TEXT("films on the cabinet face and on the room wall"), bCabinet && bRoomWall);
	TestTrue(TEXT("one sheet per film with water, positive sizes"), FilmSheets.Num() == FilmsWithWater && bPositive);
	TestTrue(TEXT("film sheets hold the films' volume (relative <= 1e-12, each and in sum)"), ErrFilm <= 1e-12 && WorstOne <= 1e-12 && WdRelErr(T.FilmShownM3, T.FilmComputedM3) <= 1e-12);
	TestTrue(TEXT("film sheet: as wide as the film, as high as its wet height, bottom at the film foot (1e-12 m)"), WorstSize <= 1e-12 && WorstFoot <= 1e-12);
	TestTrue(TEXT("film sheet lies on the wetted face, on the water side (inner plane on the face, 1e-12 m; axis = face normal)"), WorstPlane <= 1e-12 && WorstAxis <= 1e-9 && bInside);
	TestTrue(TEXT("tray sheet holds the tray's stored volume (relative <= 1e-12)"), TraySheets.Num() == 1 && Tray.StoredM3 > 0.0 && ErrTray <= 1e-12 && WdRelErr(T.TrayShownM3, T.TrayComputedM3) <= 1e-12);
	TestTrue(TEXT("tray sheet: depth = stored / area, centred on the tray, on the floor + lift (1e-9 m)"), TrayDepthErr <= 1e-9 && TrayCenterErr <= 1e-9 && TrayBottomErr <= 1e-9);
	TestTrue(TEXT("tray sheet covers the tray footprint in world X and Y (1e-9 m; a 0.6 x 0.3 m tray, not square)"), TrayFootErr <= 1e-9);
	TestTrue(TEXT("snapshot residual <= 1e-12"), T.Residual <= 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayWetMarksTest, "VRG.Water.Display.WetMarksAndSkins", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayWetMarksTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	const FWaterDisplaySettings S;
	FLeakSpray NoSpray;
	NoSpray.Reset(FSprayGeometry());

	// ---- wet marks: wet once, dry now ----
	FFloorWater Floor;
	Floor.Reset(FVector2D(-1.0, -1.0), 20, 20, .2, FFloorWaterSettings());
	for (int32 J = 0; J < 20; ++J)
		for (int32 I = 0; I < 20; ++I) Floor.SetBed(I, J, 1.0e-3 * double(J) / 19.0);
	for (int32 J = 5; J <= 9; ++J)
		for (int32 I = 5; I <= 9; ++I) Floor.SetState(I, J, 2.0e-3);
	Floor.SetState(15, 15, 3.0e-7);   // thinner than MinDepthM: never counted wet
	TArray<uint8> EverWet;
	WaterDisplay::UpdateEverWet(Floor, EverWet, S.MinDepthM);
	int32 EverCount = 0;
	for (const uint8 E : EverWet) EverCount += E ? 1 : 0;
	for (int32 J = 5; J <= 9; ++J)
		for (int32 I = 5; I <= 7; ++I) Floor.SetState(I, J, 0.0);   // 15 cells dry again
	Floor.SetSolid(9, 9, true);                                     // a wall put on a wet cell: no mark
	FWaterDisplaySnapshot Snap;
	WaterDisplay::Build(Floor, NoSpray, &EverWet, S, Snap);
	int32 ExpectedMarks = 0;
	for (int32 C = 0; C < EverWet.Num(); ++C)
		ExpectedMarks += EverWet[C] && !Floor.GetSolid()[C] && Floor.GetDepthM()[C] < S.MinDepthM ? 1 : 0;
	double WorstMarkZ = 0.0;
	const double Cell = Floor.GetCellM();
	for (int32 V = 0; V < Snap.WetFloor.VerticesM.Num(); ++V)
	{
		const FVector& P = Snap.WetFloor.VerticesM[V];
		// Each mark quad: the cell is the one its first corner is the min corner of.
		const FVector& Corner = Snap.WetFloor.VerticesM[V - V % 4];
		const int32 I = FMath::RoundToInt32((Corner.X - Floor.GetOriginM().X) / Cell), J = FMath::RoundToInt32((Corner.Y - Floor.GetOriginM().Y) / Cell);
		const double Z = Floor.GetFloorZM() + Floor.GetBedM()[Floor.Index(FMath::Clamp(I, 0, 19), FMath::Clamp(J, 0, 19))] + S.WetMarkLiftM;
		WorstMarkZ = FMath::Max(WorstMarkZ, FMath::Abs(P.Z - Z));
	}
	// A different grid clears the memory.
	FFloorWater Other;
	Other.Reset(FVector2D::ZeroVector, 10, 10, 0.0, FFloorWaterSettings());
	TArray<uint8> EverOther = EverWet;
	WaterDisplay::UpdateEverWet(Other, EverOther, S.MinDepthM);
	int32 OtherCount = 0;
	for (const uint8 E : EverOther) OtherCount += E ? 1 : 0;

	// ---- wet skins: a cabinet hit by a jet, a box standing in floor water, a dry box ----
	FSprayGeometry G = FilmRoom();
	WdAddBox(G, FVector(1.5, 4.5, .4), FVector(.3, .3, .4), TEXT("flooded"));
	WdAddBox(G, FVector(4.8, 4.8, .5), FVector(.2, .2, .5), TEXT("dry"));
	FLeakSpray Spray;
	Spray.Reset(G);
	RunFilmSpray(Spray, 50, .02);
	FFloorWater Room;
	Room.ResetForRoom(G, FFloorWaterSettings());
	const FFloorFlood* Flood = nullptr;
	for (const FFloorFlood& Fl : Room.GetFloods()) if (Fl.SourceObstacle == 1) Flood = &Fl;
	if (Flood)
		for (const int32 C : TArray<int32>(Flood->Cells)) Room.SetState(C % Room.GetNx(), C / Room.GetNx(), 4.0e-3);
	Room.Step(.02, 1.0);
	Flood = nullptr;
	for (const FFloorFlood& Fl : Room.GetFloods()) if (Fl.SourceObstacle == 1) Flood = &Fl;
	FWaterDisplaySnapshot SkinSnap;
	WaterDisplay::Build(Room, Spray, nullptr, S, SkinSnap);

	const FSprayWetting& Hit = Spray.GetWetting()[0];
	double FloodTop = -1.0;
	if (Flood && Flood->Cells.Num() > 0)
	{
		double Bed = 0.0;
		for (const int32 C : Flood->Cells) Bed += Room.GetBedM()[C];
		FloodTop = Room.GetFloorZM() + Bed / Flood->Cells.Num() + Flood->PeakDepthM;
	}
	bool bSkins = SkinSnap.Skins.Num() == 2 && SkinSnap.Totals.WetSkins == 2;
	double WorstBottom = 0.0, WorstTop = 0.0, WorstSide = 0.0, WorstAboveBox = -1.0;
	for (const FWetSkin& Sk : SkinSnap.Skins)
	{
		if (!G.Obstacles.IsValidIndex(Sk.Obstacle) || Sk.Obstacle == 2) { bSkins = false; continue; }
		const FSprayObstacle& O = G.Obstacles[Sk.Obstacle];
		const double Bottom = O.CenterM.Z - O.HalfM.Z, BoxTop = O.CenterM.Z + O.HalfM.Z;
		const double SkBottom = Sk.CenterM.Z - .5 * Sk.SizeM.Z, SkTop = Sk.CenterM.Z + .5 * Sk.SizeM.Z;
		const double Wet = Sk.Obstacle == 0 ? Hit.TopWetZM : FloodTop;
		WorstBottom = FMath::Max(WorstBottom, FMath::Abs(SkBottom - Bottom));
		WorstTop = FMath::Max(WorstTop, FMath::Abs(SkTop - FMath::Min(Wet, BoxTop)));
		WorstAboveBox = FMath::Max(WorstAboveBox, SkTop - (BoxTop + S.SkinOffsetM));
		WorstSide = FMath::Max(WorstSide, FMath::Max(FMath::Abs(Sk.SizeM.X - (2.0 * O.HalfM.X + 2.0 * S.SkinOffsetM)), FMath::Abs(Sk.SizeM.Y - (2.0 * O.HalfM.Y + 2.0 * S.SkinOffsetM))));
		bSkins &= Sk.CenterM.X == O.CenterM.X && Sk.CenterM.Y == O.CenterM.Y;
	}

	AddInfo(FString::Printf(TEXT("젖은 자국: 젖었던 칸 %d, 자국 %d(기대 %d), 높이 오차 %.2e m, 다른 격자로 바꾸면 %d칸/%d | 젖은 껍질 %d개: 분사 최고점 %.4f m, 바닥 물 상자 꼭대기 %.4f m(최대 수심 %.3f mm), 밑 %.2e m, 위 %.2e m, 옆 %.2e m, 상자 위+간격 초과 %.2e m"),
		EverCount, Snap.Totals.WetMarkCells, ExpectedMarks, WorstMarkZ, OtherCount, EverOther.Num(), SkinSnap.Skins.Num(), Hit.TopWetZM, FloodTop,
		Flood ? Flood->PeakDepthM * 1e3 : -1.0, WorstBottom, WorstTop, WorstSide, WorstAboveBox));
	TestTrue(TEXT("ever-wet memory: the 25 wet cells, not the one thinner than MinDepthM"), EverCount == 25 && EverWet[Floor.Index(15, 15)] == 0);
	TestTrue(TEXT("wet marks = cells wet once, dry now and not solid (15)"), Snap.Totals.WetMarkCells == ExpectedMarks && ExpectedMarks == 15 && Snap.WetFloor.Cells == 15 && Snap.WetFloor.VerticesM.Num() == 4 * 15);
	TestTrue(TEXT("wet marks lie at floor + bed + WetMarkLiftM (1e-12 m)"), WorstMarkZ <= 1e-12);
	TestTrue(TEXT("a different floor grid clears the memory"), EverOther.Num() == 100 && OtherCount == 0);
	TestTrue(TEXT("the jet and the floor water each wetted one box"), Hit.FirstWetS >= 0.0 && Flood && Flood->FirstWetS >= 0.0 && FloodTop > 0.0);
	TestTrue(TEXT("one skin per wetted box (the dry box has none)"), bSkins);
	TestTrue(TEXT("skin from the box bottom to the highest wet point, never above box top + SkinOffsetM (1e-12 m)"), WorstBottom <= 1e-12 && WorstTop <= 1e-12 && WorstAboveBox <= 1e-12 && WorstSide <= 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterDisplayDeterministicTest, "VRG.Water.Display.Deterministic", VrgWaterDisplayTest::DisplayTestFlags)
bool FWaterDisplayDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace VrgWaterDisplayTest;
	// Everything at once (floor water fed by the spray, films, parcels, a tray, wet marks, skins): building twice from the
	// same state, into a fresh snapshot and into a reused one, gives the same bits.
	const FSprayGeometry G = FilmRoom();
	FLeakSpray Spray;
	Spray.Reset(G);
	FFloorWater Floor;
	Floor.ResetForRoom(G, FFloorWaterSettings());
	Floor.AddTray(TEXT("tray"), FVector2D(4.5, .5), FVector2D(5.1, .8), 1.0e-3);
	Floor.AddWater(FVector(4.8, .6, 0.0), 1.0e-4);
	const FWaterDisplaySettings S;
	TArray<uint8> EverWet;
	const FSpraySettings& SS = Spray.GetSettings();
	const FSprayFluid& F = Spray.GetFluid();
	const double Dt = .02;
	for (int32 I = 0; I < 60; ++I)
	{
		TArray<FSpraySource> Src = { SourceToward(0, FVector(2.45, 3, 1.2), FVector(.5, 0, 0), 2.0e-7, SS, F) };
		if (I < 40)
		{
			FSpraySource& Jet = Src.AddDefaulted_GetRef();
			Jet.Id = 1; Jet.OriginM = FVector(1.0, 1.0, 1.5); Jet.DirectionM = FLeakSpray::ConeDirection(11u, SS.ConeHalfAngleRad);
			Jet.GaugePressurePa = 1.5e5; Jet.VolumeM3 = 2.0e-6;
		}
		Spray.Step(Src, Dt, I * Dt);
		for (const TPair<FVector, double>& Dep : Spray.GetLastStepFloorDepositsM3()) Floor.AddWater(Dep.Key, Dep.Value);
		Floor.Step(Dt, I * Dt);
		WaterDisplay::UpdateEverWet(Floor, EverWet, S.MinDepthM);
	}
	FWaterDisplaySnapshot A, B;
	WaterDisplay::Build(Floor, Spray, &EverWet, S, A);
	WaterDisplay::Build(Floor, Spray, &EverWet, S, B);
	const bool bFresh = SameSnapshot(A, B);
	WaterDisplay::Build(Floor, Spray, &EverWet, S, A);   // reused snapshot: no leftovers
	const bool bReused = SameSnapshot(A, B);
	const FWaterDisplayTotals& T = A.Totals;
	AddInfo(FString::Printf(TEXT("결정성: 바닥 칸 %d(정점 %d), 자국 %d, 판 %d, 입자 %d, 줄 %d, 껍질 %d | 새로/재사용 동일 %d/%d | Residual %.2e(바닥 %.6e, 받침 %.6e, 물막 %.6e, 공중 %.6e m3)"),
		T.SurfaceCells, A.Surface.VerticesM.Num(), T.WetMarkCells, A.Sheets.Num(), A.Drops.Num(), A.Streaks.Num(), A.Skins.Num(), bFresh ? 1 : 0, bReused ? 1 : 0,
		T.Residual, T.FloorComputedM3, T.TrayComputedM3, T.FilmComputedM3, T.AirComputedM3));
	TestTrue(TEXT("the scene has floor water, sheets and parcels"), T.SurfaceCells > 0 && A.Sheets.Num() > 0 && A.Drops.Num() > 0);
	TestTrue(TEXT("two builds from the same state are bit-identical"), bFresh);
	TestTrue(TEXT("building into a reused snapshot leaves nothing from before"), bReused);
	TestTrue(TEXT("snapshot residual <= 1e-12"), T.Residual <= 1e-12);
	return true;
}

#endif
