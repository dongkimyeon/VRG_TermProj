// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Simulation/Air/AirSolver.h"
#include "UI/VrgWebDashboard.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

// Web dashboard room air tab (2026-11-11): statistics are over fluid cells only, the hotspot is the hottest fluid cell's
// centre, the max speed is the face-mean cell-centre speed, slices hold the layer's temperatures with solid cells as
// NaN (JSON null), and the JSON round-trips exactly. Synthetic field: T = 290 + X + 10 Y + 100 Z K, u = X, v = 2Y, w = -3Z.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardRoomAirTest, "VRG.Web.RoomAirSlices",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardRoomAirTest::RunTest(const FString& Parameters)
{
	FAirGridParams Params;
	Params.NumCellsX = 8; Params.NumCellsY = 6; Params.NumCellsZ = 4; Params.CellSizeM = .25;
	FAirSolver Solver;
	Solver.Initialize(Params, 297.15);
	FUniformAirGrid& G = Solver.GetMutableGrid();
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 8; ++X)
		G.TemperatureK[G.CellIndex(X, Y, Z)] = 290.0 + X + 10.0 * Y + 100.0 * Z;
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X <= 8; ++X) G.VelocityXMs[G.FaceXIndex(X, Y, Z)] = X;
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y <= 6; ++Y) for (int32 X = 0; X < 8; ++X) G.VelocityYMs[G.FaceYIndex(X, Y, Z)] = 2.0 * Y;
	for (int32 Z = 0; Z <= 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 8; ++X) G.VelocityZMs[G.FaceZIndex(X, Y, Z)] = -3.0 * Z;
	// Solid: the hottest cell (7,5,3) and one cell on a slice (2,1,1); both must be left out.
	G.bSolidCell[G.CellIndex(7, 5, 3)] = true;
	G.bSolidCell[G.CellIndex(2, 1, 1)] = true;
	const FVector Origin(10.0, -2.0, 0.5);
	// World heights 0.80 (layer 1), 1.30 (layer 3), 1.35 (duplicate of 3), 9 and 0 (outside the grid: dropped).
	const FVrgDashboardRoomAir A = FVrgDashboardRoomAir::Build(Solver, Origin, { .80, 1.30, 1.35, 9.0, 0.0 });

	double MinK = 1e9, MaxK = -1e9, SumK = 0.0, MaxSpeed = 0.0;
	int32 Fluid = 0;
	FVector Hot = FVector::ZeroVector;
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 8; ++X)
	{
		if (G.bSolidCell[G.CellIndex(X, Y, Z)]) continue;
		const double K = 290.0 + X + 10.0 * Y + 100.0 * Z;
		++Fluid; SumK += K; MinK = FMath::Min(MinK, K);
		if (K > MaxK) { MaxK = K; Hot = Origin + FVector(X + .5, Y + .5, Z + .5) * .25; }
		MaxSpeed = FMath::Max(MaxSpeed, FVector(X + .5, 2.0 * Y + 1.0, -3.0 * Z - 1.5).Size());   // closed-form face mean
	}
	TestEqual(TEXT("fluid cells"), A.FluidCells, Fluid);
	TestTrue(TEXT("min / max over fluid cells (solid hottest cell excluded)"), A.MinK == MinK && A.MaxK == MaxK && MaxK == 290.0 + 6 + 50 + 300);
	TestTrue(TEXT("mean over fluid cells (rel < 1e-15)"), FMath::Abs(A.MeanK - SumK / Fluid) <= 1e-15 * A.MeanK);
	TestTrue(TEXT("hotspot = centre of the hottest fluid cell"), A.HotspotM.Equals(Hot, 1e-12));
	TestTrue(TEXT("max speed = face-mean cell-centre speed (rel < 1e-15)"), FMath::Abs(A.MaxSpeedMs - MaxSpeed) <= 1e-15 * MaxSpeed);
	TestTrue(TEXT("two slices at layers 1 and 3, cell-centre heights"), A.SliceLayers == TArray<int32>({ 1, 3 })
		&& A.SliceHeightsM.Num() == 2 && FMath::IsNearlyEqual(A.SliceHeightsM[0], .5 + .375, 1e-12) && FMath::IsNearlyEqual(A.SliceHeightsM[1], .5 + .875, 1e-12));
	bool bSlices = A.SliceK.Num() == 2 * 8 * 6;
	for (int32 S = 0; bSlices && S < 2; ++S)
		for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 8; ++X)
		{
			const int32 Z = A.SliceLayers[S];
			const double V = A.SliceK[(S * 6 + Y) * 8 + X];
			bSlices &= G.bSolidCell[G.CellIndex(X, Y, Z)] ? FMath::IsNaN(V) : V == 290.0 + X + 10.0 * Y + 100.0 * Z;
		}
	TestTrue(TEXT("slice values exact, X fastest, solid = NaN"), bSlices);

	FVrgDashboardRoomAir Back;
	TestTrue(TEXT("JSON round-trip parses"), FVrgDashboardRoomAir::FromJson(A.ToJson(), Back));
	bool bSame = Back.SliceK.Num() == A.SliceK.Num();
	for (int32 I = 0; bSame && I < A.SliceK.Num(); ++I)
		bSame &= FMath::IsNaN(A.SliceK[I]) ? FMath::IsNaN(Back.SliceK[I]) : Back.SliceK[I] == A.SliceK[I];
	TestTrue(TEXT("JSON round-trip exact (NaN kept as null)"), bSame && Back.MinK == A.MinK && Back.MaxK == A.MaxK && Back.MeanK == A.MeanK
		&& Back.HotspotM == A.HotspotM && Back.OriginM == A.OriginM && Back.MaxSpeedMs == A.MaxSpeedMs && Back.SliceLayers == A.SliceLayers
		&& Back.SliceHeightsM == A.SliceHeightsM && Back.NumX == 8 && Back.NumY == 6 && Back.NumZ == 4 && Back.FluidCells == Fluid);
	const FVrgDashboardRoomAir None = FVrgDashboardRoomAir::Build(Solver, Origin, { 7.0 });
	TestTrue(TEXT("no slice inside -> statistics only"), None.SliceK.Num() == 0 && None.FluidCells == Fluid);
	return true;
}

namespace
{
	TSharedPtr<FJsonObject> ParseJsonForTabsTest(const FString& Text)
	{
		TSharedPtr<FJsonObject> O;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
		return O;
	}
}

// Web dashboard cooling / validation tabs (2026-11-11): /api/status carries the report's own sections (same builder
// functions), so at one SimTime they equal a report written then; it is built only while the page asks (202 before).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardStatusTest, "VRG.Web.StatusMatchesReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardStatusTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->bShowSummary = false;
	for (const double Y : { 0.0, 66.0 })   // one row of two racks (synthetic placement), default in-row CRAH + CDU
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);

	Sim->EnterLayoutMode();
	const TSharedPtr<FJsonObject> L = ParseJsonForTabsTest(Sim->BuildDashboardStatusJson());
	TestTrue(TEXT("layout mode: parses, no sections, step costs present"), L.IsValid() && L->GetStringField(TEXT("mode")) == TEXT("layout")
		&& !L->HasField(TEXT("checks")) && L->HasField(TEXT("perf")));

	Room->RebuildRoomNow();
	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.1f);   // 1 s
	// vrg.Web.AutoOpen (default 1) must not start the server or open a browser inside automation tests.
	TestFalse(TEXT("no auto-open in automation: guard"), FVrgWebDashboard::ShouldAutoOpen());
	TestFalse(TEXT("no auto-open in automation: server stays off after entering simulation"), FVrgWebDashboard::Get().IsRunning());

	// Same SimTime: the status sections against a report written now.
	const TSharedPtr<FJsonObject> S = ParseJsonForTabsTest(Sim->BuildDashboardStatusJson());
	FString Summary, ReportText;
	TestTrue(TEXT("report written"), Sim->WriteEvaluationReport(TEXT("AutomationWebStatus"), Summary));
	FFileHelper::LoadFileToString(ReportText, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), TEXT("AutomationWebStatus.json")));
	const TSharedPtr<FJsonObject> R = ParseJsonForTabsTest(ReportText);
	TestTrue(TEXT("status and report parse"), S.IsValid() && R.IsValid());
	if (S.IsValid() && R.IsValid())
	{
		TestTrue(TEXT("same SimTime"), S->GetNumberField(TEXT("simTimeS")) == R->GetNumberField(TEXT("simTimeS")));
		const TSharedPtr<FJsonObject> SC = S->GetObjectField(TEXT("checks")), RC = R->GetObjectField(TEXT("checks"));
		TestTrue(TEXT("checks = report checks (residual, ledger, pressure, hydraulics)"),
			SC->GetNumberField(TEXT("globalNormalizedEnergyResidual")) == RC->GetNumberField(TEXT("globalNormalizedEnergyResidual"))
			&& SC->GetStringField(TEXT("ledger")) == RC->GetStringField(TEXT("ledger"))
			&& SC->GetNumberField(TEXT("pressureSolves")) == RC->GetNumberField(TEXT("pressureSolves"))
			&& SC->GetNumberField(TEXT("pressureFailures")) == RC->GetNumberField(TEXT("pressureFailures"))
			&& SC->GetBoolField(TEXT("hydraulicsConverged")) == RC->GetBoolField(TEXT("hydraulicsConverged")));
		auto Text = [](const TSharedPtr<FJsonObject>& O) { FString T; FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&T)); return T; };
		TestEqual(TEXT("dew point section = report"), Text(S->GetObjectField(TEXT("dewPoint"))), Text(R->GetObjectField(TEXT("dewPoint"))));
		TestEqual(TEXT("water display section = report"), Text(S->GetObjectField(TEXT("waterDisplay"))), Text(R->GetObjectField(TEXT("waterDisplay"))));
		TestEqual(TEXT("W4 water impact section = report"), Text(S->GetObjectField(TEXT("waterImpact"))), Text(R->GetObjectField(TEXT("waterImpact"))));
		TestEqual(TEXT("instruments section = report"), Text(S->GetObjectField(TEXT("instruments"))), Text(R->GetObjectField(TEXT("instruments"))));
		const TSharedPtr<FJsonObject> SP = S->GetObjectField(TEXT("power")), RP = R->GetObjectField(TEXT("power"));
		TestTrue(TEXT("window power = report (IT, CRAH fans, pumps, partial PUE)"), SP->GetNumberField(TEXT("itW")) == RP->GetNumberField(TEXT("itW"))
			&& SP->GetNumberField(TEXT("crahFanW")) == RP->GetNumberField(TEXT("crahFanW")) && SP->GetNumberField(TEXT("pumpW")) == RP->GetNumberField(TEXT("pumpW"))
			&& SP->GetNumberField(TEXT("partialPue")) == RP->GetNumberField(TEXT("partialPue")));
		TestTrue(TEXT("fixed-boundary run: no plant section, as in the report"), !S->GetBoolField(TEXT("plantEnabled"))
			&& S->HasTypedField<EJson::Null>(TEXT("plant")) && !R->HasField(TEXT("plant")) && !S->HasField(TEXT("plantLive")));
		TestTrue(TEXT("plausible: residual small, IT > 0, PUE >= 1"), FMath::Abs(SC->GetNumberField(TEXT("globalNormalizedEnergyResidual"))) < 1e-9
			&& SP->GetNumberField(TEXT("itW")) > 0.0 && SP->GetNumberField(TEXT("partialPue")) >= 1.0);
	}

	// Server path: nothing is built until the page asks; the first ask is pending (202), the next publish builds it.
	IConsoleVariable* Enable = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Enable"));
	IConsoleVariable* Port = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Port"));
	FVrgWebDashboard& Web = FVrgWebDashboard::Get();
	if (Enable != nullptr && Port != nullptr)
	{
		const int32 OldPort = Port->GetInt();
		Port->Set(18094, ECVF_SetByCode);
		Enable->Set(1, ECVF_SetByCode);
		TestTrue(TEXT("server on"), Web.IsRunning());
		TestTrue(TEXT("not asked yet"), !Web.WantsStatus() && !Web.WantsRoomAir());
		Sim->Tick(.02f);   // first publish after start: tabs not built (not asked)
		TestTrue(TEXT("first ask pending: nothing built without a request"), Web.RequestStatusJson().IsEmpty() && Web.RequestRoomAirJson().IsEmpty());
		TestTrue(TEXT("asked now"), Web.WantsStatus() && Web.WantsRoomAir());
		FPlatformProcess::Sleep(.55f);   // the publish period is real time (0.5 s)
		Sim->Tick(.02f);
		const TSharedPtr<FJsonObject> Served = ParseJsonForTabsTest(Web.RequestStatusJson());
		FVrgDashboardRoomAir ServedAir;
		TestTrue(TEXT("status served after the next publish"), Served.IsValid() && Served->GetStringField(TEXT("mode")) == TEXT("simulation"));
		TestTrue(TEXT("room air served after the next publish"), FVrgDashboardRoomAir::FromJson(Web.RequestRoomAirJson(), ServedAir));
		const FVrgDashboardRoomAir Direct = Sim->BuildDashboardRoomAir();
		TestTrue(TEXT("served room air = direct build at that SimTime"), ServedAir.SimTimeS == Direct.SimTimeS && ServedAir.MaxK == Direct.MaxK
			&& ServedAir.MeanK == Direct.MeanK && ServedAir.FluidCells == Direct.FluidCells && ServedAir.SliceLayers == Direct.SliceLayers);
		TestTrue(TEXT("room air plausible: fluid cells, 15..45 C mean, slices"), Direct.FluidCells > 0 && Direct.MeanK > 288.15 && Direct.MeanK < 318.15
			&& Direct.MinK <= Direct.MeanK && Direct.MeanK <= Direct.MaxK && Direct.SliceLayers.Num() > 0 && !Direct.bOctree);
		Enable->Set(0, ECVF_SetByCode);
		TestTrue(TEXT("off: copies cleared"), !Web.IsRunning() && Web.RequestStatusJson().IsEmpty() && Web.RequestRoomAirJson().IsEmpty());
		Web.Stop();   // the asks above re-marked the time; clear again
		Port->Set(OldPort, ECVF_SetByCode);
	}
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
