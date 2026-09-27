#pragma once
#include "Simulation/Air/FixedOctreeGrid.h"
#include "Simulation/Air/MultigridPoissonSolver.h"

struct FAirSolverInput;

// S5-C: closed-wall, constant-density finite volume air solver. SI / Kelvin throughout.
// Face normal velocities, cell scalars, prebuilt CSR neighbours; no UObject or independent clock.
// First-order upwind temperature/momentum; TPFA at hanging faces is only first order.
// Adapt() is transactional: failed topology/projection leaves the old solution and ledger intact.
class VRG_TERMPROJ_API FOctreeAirSolver
{
public:
	bool Initialize(const FUniformAirGrid& Source, const FFixedOctreeSettings& Settings,
		const TArray<FBox>& BoxesM, FString& Error);
	bool Adapt(const TArray<FBox>& BoxesM, double ProjectionDtS, FString& Error);
	bool Step(const FAirSolverInput& RootInput, double DtS);
	FPoissonSolveResult Project(double DtS);
	void ExportUniform(FUniformAirGrid& Out) const;
	const FFixedOctreeGrid& GetGrid() const { return Tree; }
	FFixedOctreeGrid& GetMutableGrid() { return Tree; }
	TArray<double>& GetMutableFaceVelocityMs() { return FaceVelocityMs; }
	const TArray<double>& GetFaceVelocityMs() const { return FaceVelocityMs; }
	FPoissonSolveSettings PressureSettings;
	FPoissonSolveResult PressureResult;
	double EnergyInJ = 0, EnergyOutJ = 0, InitialEnergyJ = 0;
	double LastCoolingRemovedW = 0, LastCourant = 0, LastPressureMs = 0;
	int64 PressureSolves = 0, PressureFailures = 0;
	double EnergyStoredJ() const { return Tree.TotalThermalEnergyJ(Tree.GetBaseParams().ReferenceTempK) - InitialEnergyJ; }
	// S5 finish (2026-09-27): fan ducts. The root duct faces of FAirSolverInput (validated by FAirSolver) are mapped onto
	// the octree faces that lie on them (solid leaf | fluid leaf). Last step's duct coil / fan rates and fan energy.
	double LastDuctCoilRemovedW = 0, LastDuctFanHeatW = 0, LastDuctFanHeatJ = 0;
	int32 LastNumMappedDuctFaces = 0;
	FString LastDuctError;
	double MaxDivergencePerS() const;
	double NormalizedDivergence() const;
private:
	FFixedOctreeGrid Tree;
	FFixedOctreeSettings TreeSettings;
	TArray<double> FaceVelocityMs, PressurePa;
	TArray<int32> CompactOf, LeafOf;
	struct FDuctFace { int32 Face = INDEX_NONE; int32 FluidLeaf = INDEX_NONE; bool bFluidLow = true; int32 Duct = 0; bool bExhaust = false; int32 InputFace = 0; };
	TArray<FDuctFace> DuctFaces;
	TArray<int32> DuctOfFace;          // octree face -> index into DuctFaces, or INDEX_NONE
	uint32 DuctSignature = 0;
	bool bDuctMapValid = false;
	const FAirSolverInput* Ducts = nullptr;
	double DuctCoilJ = 0, DuctFanJ = 0;
	bool MapDucts(const FAirSolverInput& Input, FString& Error);
	void ApplyDuctVelocities();
	void AddDuctOutflowRates(TArray<double>& Rate) const;
	FMultigridPoisson Pressure;
	void BuildPressure();
	bool IsOpen(const FFixedOctreeFace& F) const;
	void CellVelocities(TArray<FVector>& Out) const;
	void FacesFromCells(const TArray<FVector>& Cells);
	void AdvectMomentum(double DtS);
	void AdvectTemperature(double DtS);
};
