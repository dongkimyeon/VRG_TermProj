// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// W2 (Tools/누수_실시간_계획.md L2, 2026-10-26): where the water that leaves a leak hole goes before it reaches the floor.
//
//  - Source: the leaked volume of a step comes from the hydraulic solve (the tank ledger for room leaks, the loop's leak
//    flow for plant leaks): computed once there, only moved here (rule 6). Discharge speed v0 = Cv sqrt(2 dp / rho) with
//    dp the hole node's gauge pressure; the jet diameter follows from continuity Q = v0 pi d^2 / 4.
//  - Direction: a scenario input. Default: a fixed-seed random direction in a downward cone (2026-10-26 user decision).
//    A real crack's direction is unknown, so it is an ESTIMATE.
//  - Intact jet: gravity only (the coherent core is treated as drag-free, ESTIMATE) until its path length reaches the
//    breakup length L_b. Then it becomes drops of the Rayleigh size 1.89 d, capped by the largest drop the air lets
//    survive (critical gas Weber number). Drops feel sphere drag (Schiller-Naumann) and gravity, and break again
//    whenever their gas Weber number exceeds the critical one.
//  - Impact on a box (equipment, wall, ceiling plate, room enclosure, ground): above the Mundo splash threshold a part
//    of the volume leaves as secondary drops (fraction and size ESTIMATES), the rest stays: the floor and the ground
//    collect it (the source of the floor model of W3), a vertical face carries it down as a gravity film (Nusselt) to
//    the floor at its foot, a downward face lets it drip off again.
//  - A hole inside a box (a pipe inside a cabinet) wets that box from the hole down; its water reaches the floor under
//    it at once (no inner flow path, ESTIMATE). Counted on the floor; "through equipment" is an extra counter.
//  - Ledger, exact by construction: emitted = airborne + waiting at the hole + films + floor + ground.
//
// Parcels are the computed water and also the drawn water (what is shown is what is computed). Plain C++: no actor
// access (rule 5), SI units (rule 1). Parcels are SoA arrays written into a second set each step (ping-pong, rule 7).
// Still air: the room air velocity is not felt by the drops (limitation of W2).

struct VRG_TERMPROJ_API FSprayFluid
{
	double WaterDensityKgM3 = 997.0;      // water at 25 °C (handbook)
	double WaterViscosityPaS = 8.9e-4;    // water at 25 °C (handbook)
	double SurfaceTensionNm = 0.072;      // water-air at 25 °C (handbook)
	double AirDensityKgM3 = 1.18;         // air at 25 °C, 1 atm (handbook)
	double AirViscosityPaS = 1.85e-5;     // air at 25 °C (handbook)
	double GravityMs2 = 9.81;
};

struct VRG_TERMPROJ_API FSpraySettings
{
	// Velocity coefficient of a sharp-edged orifice (textbook 0.97-0.99). A crack is not that shape: ESTIMATE.
	double VelocityCoefficient = 0.97;
	// Half angle of the default downward direction cone [rad]. ESTIMATE (no data on where cracks point).
	double ConeHalfAngleRad = PI / 3.0;
	// Critical gas Weber number of drop breakup, rho_air v^2 D / sigma = 12: Pilch & Erdman (1987), Int. J. Multiphase
	// Flow 13(6), 741.
	double CriticalWeber = 12.0;
	// Rayleigh capillary breakup: drop diameter = 1.89 x jet diameter (Rayleigh 1878, the fastest growing wave).
	double RayleighDropRatio = 1.89;
	// Splash when K = We^0.5 Re^0.25 > 57.7 (Mundo, Sommerfeld & Tropea 1995, Int. J. Multiphase Flow 21(2), 151).
	double SplashK = 57.7;
	// Share of an impacting volume that leaves as secondary drops, and their size / the impacting size. ESTIMATES (the
	// published splash mass ratios scatter widely with surface and angle).
	double SplashMassFraction = 0.2;
	double SplashDropRatio = 0.3;
	// Velocity kept by secondary drops: normal and tangential to the surface. ESTIMATES.
	double ReboundNormal = 0.2;
	double ReboundTangential = 0.8;
	// Diameter of a drop dripping off a downward face. ESTIMATE of the order of a pendant drop (capillary length 2.7 mm).
	double DripDiameterM = 5.0e-3;
	// Random spread of the drops' direction at breakup [rad]. ESTIMATE.
	double BreakupSpreadRad = FMath::DegreesToRadians(3.0);
	// Parcel limit (performance). Volume beyond it waits at its hole (still counted) until parcels land.
	int32 MaxParcels = 4000;
	bool bAirDrag = true;
	// Floor collection grid [m] (the cell size W3 will start from).
	double FloorBinM = 0.1;
};

enum class ESprayObstacleKind : uint8 { Equipment, Barrier, Ceiling };

// An oriented box in metres (centre, rotation, half extents).
struct VRG_TERMPROJ_API FSprayObstacle
{
	FVector CenterM = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector HalfM = FVector::ZeroVector;
	ESprayObstacleKind Kind = ESprayObstacleKind::Equipment;
	FString Name;
};

struct VRG_TERMPROJ_API FSprayGeometry
{
	TArray<FSprayObstacle> Obstacles;
	// Room enclosure: water inside stays in it (floor, walls, ceiling); water outside hits its outer walls. Invalid = no room.
	FBox RoomM = FBox(ForceInit);
	// Outdoor ground outside the room [m].
	double GroundZM = 0.0;
};

// One open hole this step.
struct VRG_TERMPROJ_API FSpraySource
{
	int32 Id = INDEX_NONE;            // stable per site (waiting volume and seeds are kept by it)
	FVector OriginM = FVector::ZeroVector;
	FVector DirectionM = FVector(0, 0, -1);
	double GaugePressurePa = 0.0;     // pressure at the hole minus the pressure it discharges into
	double VolumeM3 = 0.0;            // water that left the network through this hole during the step
};

enum class ESpraySurface : uint8 { Floor, Ground, Wall, Ceiling, Equipment, Barrier, RoomExterior };

// A wetted vertical face (or the top of a box draining over its edges): Nusselt gravity film.
//   Outflow at the foot = W rho g delta^3 / (3 mu), delta = V / (W H)  ->  steady delta = (3 mu Gamma / (rho g))^(1/3)
struct VRG_TERMPROJ_API FSprayFilm
{
	int32 Obstacle = INDEX_NONE;      // INDEX_NONE: a room wall; -2: the room's outer wall
	int32 Face = 0;                   // 0..5 = local -X,+X,-Y,+Y,-Z,+Z
	ESpraySurface Surface = ESpraySurface::Wall;
	double WidthM = 0.0;              // wetted width (whole face, ESTIMATE: real films can run as narrower rivulets)
	double WetHeightM = 0.0;          // from the highest impact down to the foot
	FVector FootM = FVector::ZeroVector;
	bool bFootOutdoors = false;
	double VolumeM3 = 0.0;
	double LastOutflowM3s = 0.0;
	double LastThicknessM = 0.0;
	double InflowM3 = 0.0;            // arrived this step (consumed by the film step)
};

// What a box saw of the water (input of W4).
struct VRG_TERMPROJ_API FSprayWetting
{
	double FirstWetS = -1.0;
	double TopWetZM = -1.0e30;         // highest point water touched (it runs down to the floor from there)
	double WaterM3 = 0.0;              // cumulative water that touched it (impacts + water leaking inside it)
};

struct VRG_TERMPROJ_API FSprayTotals
{
	double EmittedM3 = 0.0, AirborneM3 = 0.0, WaitingM3 = 0.0, FilmM3 = 0.0, FloorM3 = 0.0, GroundM3 = 0.0, ThroughEquipmentM3 = 0.0;
	int32 Parcels = 0, JetParcels = 0, Splashes = 0, Breakups = 0;
	// |emitted - (airborne + waiting + films + floor + ground)| / emitted (through equipment is already on the floor)
	double Residual = 0.0;
};

class VRG_TERMPROJ_API FLeakSpray
{
public:
	void Reset(const FSprayGeometry& InGeometry, const FSpraySettings& InSettings = FSpraySettings(), const FSprayFluid& InFluid = FSprayFluid());
	// One integrated step [s] ending at NowS + DtS (the shared clock, rule 4).
	void Step(const TArray<FSpraySource>& Sources, double DtS, double NowS);

	// ---- the relations (static, for the tests) ----
	static double JetVelocityMs(double GaugePressurePa, const FSpraySettings& S, const FSprayFluid& F);
	static double JetDiameterM(double FlowM3s, double VelocityMs);
	// Turbulent jet breakup length L_b / d = 8.51 We^0.32, We = rho_w v^2 d / sigma: Grant & Middleman (1966),
	// AIChE J. 12(4), 669, as quoted in the atomization literature (the original paper was not checked: treat the
	// constants as unverified). Their data are low-pressure water jets; far above that the value is an extrapolation.
	static double BreakupLengthM(double JetDiameterM, double VelocityMs, const FSprayFluid& F);
	// Largest drop the air lets survive at this relative speed (critical Weber).
	static double MaxStableDropM(double RelativeSpeedMs, const FSpraySettings& S, const FSprayFluid& F);
	// Sphere drag coefficient, Schiller & Naumann (1933): 24/Re (1 + 0.15 Re^0.687) below Re 1000, 0.44 above.
	static double DropDragCoefficient(double Reynolds);
	static bool Splashes(double NormalSpeedMs, double DiameterM, const FSpraySettings& S, const FSprayFluid& F);
	static double NusseltThicknessM(double FlowPerWidthM2s, const FSprayFluid& F);
	// A unit vector in the downward cone, fixed by the seed.
	static FVector ConeDirection(uint32 Seed, double HalfAngleRad);

	// ---- state ----
	const FSprayTotals& GetTotals() const { return Totals; }
	const TArray<FVector>& GetParcelPositionsM() const { return Pos; }
	const TArray<FVector>& GetParcelVelocitiesMs() const { return Vel; }
	const TArray<double>& GetParcelVolumesM3() const { return Vol; }
	const TArray<double>& GetParcelDiametersM() const { return Dia; }
	const TArray<uint8>& GetParcelIsJet() const { return Jet; }
	// The path every parcel travelled in the last step (start -> end or impact), for drawing: a fast jet crosses the
	// whole fall within one step, so its points alone would never be seen in the air.
	const TArray<FVector>& GetLastStepTrailFromM() const { return TrailFrom; }
	const TArray<FVector>& GetLastStepTrailToM() const { return TrailTo; }
	const TArray<uint8>& GetLastStepTrailIsJet() const { return TrailJet; }
	const TArray<FSprayFilm>& GetFilms() const { return Films; }
	const TMap<FIntPoint, double>& GetFloorBinsM3() const { return FloorBins; }
	// W3: the water that reached the room floor during the last step (point, volume), in order. The floor model takes
	// exactly these (the bins above keep the cumulative record).
	const TArray<TPair<FVector, double>>& GetLastStepFloorDepositsM3() const { return StepFloorDeposits; }
	const TArray<FSprayWetting>& GetWetting() const { return Wetting; }
	const FSprayGeometry& GetGeometry() const { return Geometry; }
	const FSpraySettings& GetSettings() const { return Settings; }
	const FSprayFluid& GetFluid() const { return Fluid; }
	// The obstacle a point lies inside (INDEX_NONE: none).
	int32 FindObstacleContaining(const FVector& PointM) const;
	bool HasWater() const { return Totals.EmittedM3 > 0.0; }

	// First landing of the parcels of a source (test hooks): impact point and time of the first deposit.
	struct FFirstImpact { FVector PointM = FVector::ZeroVector; double TimeS = -1.0; ESpraySurface Surface = ESpraySurface::Floor; };
	const TMap<int32, FFirstImpact>& GetFirstImpacts() const { return FirstImpacts; }
	// Path length at which the first parcel of a source broke up [m] (test hook), and the drop diameter it got.
	struct FFirstBreakup { double PathM = -1.0; double StepTravelM = 0.0; double BreakupLengthM = 0.0; double DropDiameterM = 0.0; double JetDiameterM = 0.0; double SpeedMs = 0.0; };
	const TMap<int32, FFirstBreakup>& GetFirstBreakups() const { return FirstBreakups; }

private:
	FSprayGeometry Geometry;
	FSpraySettings Settings;
	FSprayFluid Fluid;

	// Parcel SoA (read set) and the set the step writes.
	TArray<FVector> Pos, Vel;
	TArray<double> Vol, Dia, JetDia, Path, Breakup;
	TArray<uint8> Jet, Splashed;
	TArray<int32> Src;
	TArray<uint32> Seed;
	TArray<FVector> NPos, NVel;
	TArray<double> NVol, NDia, NJetDia, NPath, NBreakup;
	TArray<uint8> NJet, NSplashed;
	TArray<int32> NSrc;
	TArray<uint32> NSeed;

	TArray<FVector> TrailFrom, TrailTo;
	TArray<uint8> TrailJet;
	TArray<FSprayFilm> Films;
	TMap<uint64, int32> FilmIndex;
	TMap<FIntPoint, double> FloorBins, GroundBins;
	TArray<TPair<FVector, double>> StepFloorDeposits;
	TMap<int32, double> Waiting;
	struct FEmitter { FSpraySource Source; double JetDiameterM = 0.0; };   // each hole's last jet
	TMap<int32, FEmitter> Emitters;
	TMap<int32, uint32> Emissions;
	TArray<FSprayWetting> Wetting;
	TMap<int32, FFirstImpact> FirstImpacts;
	TMap<int32, FFirstBreakup> FirstBreakups;
	FSprayTotals Totals;

	struct FHit { double TimeS = TNumericLimits<double>::Max(); FVector NormalM = FVector::ZeroVector; ESpraySurface Surface = ESpraySurface::Floor; int32 Obstacle = INDEX_NONE; int32 Face = 0; };
	FHit FindHit(const FVector& X0, const FVector& V0, const FVector& Acc, double DtS) const;
	void PushParcel(const FVector& P, const FVector& V, double Volume, double D, double JetD, double PathM, double BreakupM, bool bJet, bool bSplashed, int32 Source, uint32 InSeed);
	void Deposit(const FHit& Hit, const FVector& PointM, double Volume, int32 Source, double NowS);
	void AddFloor(const FVector& PointM, double Volume, bool bOutdoors);
	void Touch(int32 Obstacle, const FVector& PointM, double Volume, double NowS);
	void StepFilms(double DtS);
	bool IsOutdoors(const FVector& PointM) const;
	void UpdateTotals();
};
