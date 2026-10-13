// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UI/VrgWebDashboard.h"

#if WITH_DEV_AUTOMATION_TESTS

// Web dashboard stage 1 (2026-10-13): the summary card JSON round-trips, its window values are exactly the report's
// (same FVrgEvaluation calls), its "now" values are the coupling snapshot, and the server publishes only while enabled.
// The HTTP transport itself is checked by hand in -game (Tools/웹대시보드_계획.md).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardSummaryTest, "VRG.Web.DashboardSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardSummaryTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	for (const double X : { -150.0, 150.0 })
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(X, 0, 0), FRotator(0, X < 0 ? 0 : 180, 0));
		Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
		Rack->Input = FRackThermalInput();
		Rack->RackSizeM = FVector(1.068, .6, 2.236);
		Rack->bShowReadout = false;
	}
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->bShowSummary = false;
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->EvalSettings.WindowS = 2.0;

	Sim->EnterLayoutMode();
	const FVrgDashboardSummary Layout = Sim->BuildDashboardSummary();
	TestFalse(TEXT("layout mode is reported as layout"), Layout.bSimulating);

	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.32f);   // 3.2 s
	const FVrgDashboardSummary S = Sim->BuildDashboardSummary();
	TestTrue(TEXT("simulation mode with evaluation samples"), S.bSimulating && S.bEvalReady && S.InletCount > 0);

	FVrgDashboardSummary Back;
	TestTrue(TEXT("JSON round-trip parses"), FVrgDashboardSummary::FromJson(S.ToJson(), Back));
	TestTrue(TEXT("JSON round-trip is exact"), Back.SimTimeS == S.SimTimeS && Back.ItWindowW == S.ItWindowW && Back.ItNowW == S.ItNowW
		&& Back.CoolantRemovedNowW == S.CoolantRemovedNowW && Back.AirRemovedNowW == S.AirRemovedNowW
		&& Back.bPartialPueValid == S.bPartialPueValid && Back.PartialPueWindow == S.PartialPueWindow
		&& Back.InletWindowMeanMaxK == S.InletWindowMeanMaxK && Back.InletWindowMeanAvgK == S.InletWindowMeanAvgK
		&& Back.InletCount == S.InletCount && Back.WindowS == S.WindowS && Back.bSimulating == S.bSimulating);

	// Same SimTime: the dashboard's window values must be the report's numbers (K stays K; the report writes C = K - 273.15).
	FString Summary;
	TestTrue(TEXT("report written"), Sim->WriteEvaluationReport(TEXT("AutomationWebDashboard"), Summary));
	FString Json;
	TSharedPtr<FJsonObject> Report;
	TestTrue(TEXT("report parsed"), FFileHelper::LoadFileToString(Json, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), TEXT("AutomationWebDashboard.json")))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report) && Report.IsValid());
	if (Report.IsValid())
	{
		const TSharedPtr<FJsonObject> Power = Report->GetObjectField(TEXT("power")), Sum = Report->GetObjectField(TEXT("summary"));
		TestEqual(TEXT("IT window = report"), S.ItWindowW, Power->GetNumberField(TEXT("itW")));
		TestEqual(TEXT("partial PUE = report"), S.PartialPueWindow, Power->GetNumberField(TEXT("partialPue")));
		TestEqual(TEXT("inlet max = report"), S.InletWindowMeanMaxK - 273.15, Sum->GetNumberField(TEXT("inletWindowMeanMaxC")));
		TestEqual(TEXT("inlet mean = report"), S.InletWindowMeanAvgK - 273.15, Sum->GetNumberField(TEXT("inletWindowMeanAvgC")));
		TestEqual(TEXT("window = report"), S.WindowS, Report->GetNumberField(TEXT("windowS")));
	}
	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	TestEqual(TEXT("IT now = snapshot"), S.ItNowW, Snap.TotalItHeatW);
	TestEqual(TEXT("coolant removed = snapshot"), S.CoolantRemovedNowW, Snap.TotalRadiatorRejectedW);
	TestEqual(TEXT("air removed = snapshot"), S.AirRemovedNowW, Snap.TotalAirCoolingRemovedW);
	TestTrue(TEXT("plausible: positive IT, removal and PUE >= 1"), S.ItWindowW > 0.0 && S.CoolantRemovedNowW + S.AirRemovedNowW > 0.0
		&& S.bPartialPueValid && S.PartialPueWindow >= 1.0);

	// Server: nothing published while off; enabled -> the actor's Tick publishes a copy equal to its summary; off again -> cleared.
	IConsoleVariable* Enable = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Enable"));
	IConsoleVariable* Port = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Port"));
	FVrgWebDashboard& Web = FVrgWebDashboard::Get();
	TestTrue(TEXT("console variables exist"), Enable != nullptr && Port != nullptr);
	if (Enable != nullptr && Port != nullptr)
	{
		const int32 OldPort = Port->GetInt();
		Port->Set(18093, ECVF_SetByCode);
		Enable->Set(0, ECVF_SetByCode);
		Sim->Tick(.02f);
		TestTrue(TEXT("off: no server, nothing published"), !Web.IsRunning() && Web.GetLatestJson().IsEmpty());
		Enable->Set(1, ECVF_SetByCode);
		TestTrue(TEXT("on: listening on the set port"), Web.IsRunning() && Web.GetPort() == 18093);
		Sim->Tick(.02f);
		FVrgDashboardSummary Published;
		TestTrue(TEXT("published copy parses"), FVrgDashboardSummary::FromJson(Web.GetLatestJson(), Published));
		const FVrgDashboardSummary Now = Sim->BuildDashboardSummary();
		TestTrue(TEXT("published copy = summary at that SimTime"), Published.SimTimeS == Now.SimTimeS && Published.ItWindowW == Now.ItWindowW
			&& Published.InletWindowMeanMaxK == Now.InletWindowMeanMaxK);
		Enable->Set(0, ECVF_SetByCode);
		TestTrue(TEXT("off again: stopped and cleared"), !Web.IsRunning() && Web.GetLatestJson().IsEmpty());
		Port->Set(OldPort, ECVF_SetByCode);
	}
	World->DestroyWorld(false);
	return true;
}

#endif
