// Web dashboard check tab (2026-11-24): the status JSON carries the W5 detector faults and protection delays and the W4
// targets / recent state changes. Everything is a copy of what the models hold, from the same functions as the report.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TSharedPtr<FJsonObject> ParseWaterTablesJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> O;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
		return O;
	}
	FString WaterTablesText(const TSharedPtr<FJsonObject>& O)
	{
		FString T;
		FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&T));
		return T;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardWaterTablesTest, "VRG.Web.StatusWaterAndDetectorTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardWaterTablesTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false; Sim->MaxStepsPerFrame = 16;
	Sim->LeakProtectionPolicy.bEnabled = true;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	Room->bUseGjConnections = true; Room->bShowSummary = false;
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	for (const double Y : { 0.0, 66.0 })
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
	Room->RebuildRoomNow();
	GEngine->Exec(World, TEXT("vrg.Water.Impact 1 0.3 100"));   // short trip: a test value, so the tables fill quickly
	Sim->EnterSimulationMode();
	auto Run = [&](double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); };
	Run(0.5);

	// A detector fault on rack 1, a cold-plate leak on rack 0 (W4 trips its wet slots, W5 acts on the detectors).
	const auto& Sites = Sim->GetLiveLeakSites();
	int32 Site = INDEX_NONE;
	for (int32 I = 0; I < Sites.Num(); ++I) if (Sites[I].RackIndex == 0 && Sites[I].Tray == 0 && Sites[I].InternalSite == 2) Site = I;
	const int32 Faulty = Sim->ResolveDetector(TEXT("rack:1:slot:4:cold"));
	TestTrue(TEXT("leak site and detector found"), Site != INDEX_NONE && Faulty != INDEX_NONE);
	TestTrue(TEXT("detector fault set"), Sim->SetSensorHealth(Faulty, ESensorHealth::OpenCircuit));
	TestTrue(TEXT("leak opened"), Sim->SetLiveLeak(Site, 1.0e-3, TEXT("test")));
	Run(8.0);

	const TSharedPtr<FJsonObject> S = ParseWaterTablesJson(Sim->BuildDashboardStatusJson());
	if (!TestTrue(TEXT("status parses with both sections"), S.IsValid() && S->HasField(TEXT("waterImpact")) && S->HasField(TEXT("instruments"))))
	{
		GEngine->DestroyWorldContext(World); World->DestroyWorld(false); return false;
	}
	const TSharedPtr<FJsonObject> W = S->GetObjectField(TEXT("waterImpact")), I = S->GetObjectField(TEXT("instruments"));

	// W4: the not-normal targets are exactly the model's, the events are the fault monitor's last ones.
	const TArray<TSharedPtr<FJsonValue>>& Targets = W->GetArrayField(TEXT("targets"));
	int32 NotNormal = 0;
	for (EWaterImpactPhase P : Sim->GetWaterImpactObservation().GetState().Phase) NotNormal += P != EWaterImpactPhase::Normal ? 1 : 0;
	int32 Tripped = 0, CutShown = 0;
	for (const TSharedPtr<FJsonValue>& V : Targets)
	{
		const TSharedPtr<FJsonObject> T = V->AsObject();
		Tripped += T->GetStringField(TEXT("phase")) != TEXT("wet") ? 1 : 0;
		CutShown += T->GetBoolField(TEXT("powerCut")) ? 1 : 0;
	}
	TestEqual(TEXT("one row per not-normal target"), Targets.Num(), NotNormal);
	TestTrue(TEXT("tripped/damaged targets present, their cuts marked"), Tripped > 0 && CutShown > 0 && CutShown <= Tripped);
	TestEqual(TEXT("cut rows = W4 rack slot cuts (only rack slots here)"), CutShown, Sim->GetWaterRackPowerCuts().Num());
	const int32 EventCount = Sim->GetFaultMonitor().GetWaterImpacts().Num();
	TestEqual(TEXT("event count"), int32(W->GetNumberField(TEXT("eventCount"))), EventCount);
	TestEqual(TEXT("recent events = last min(50, all)"), W->GetArrayField(TEXT("recentEvents")).Num(), FMath::Min(50, EventCount));
	if (EventCount > 0)
	{
		const TSharedPtr<FJsonObject> Last = W->GetArrayField(TEXT("recentEvents")).Last()->AsObject();
		const FVrgWaterImpactRecord& R = Sim->GetFaultMonitor().GetWaterImpacts().Last();
		TestTrue(TEXT("last event copied as recorded"), Last->GetNumberField(TEXT("timeS")) == R.TimeS && Last->GetStringField(TEXT("to")) == R.To
			&& Last->GetStringField(TEXT("cause")) == R.Cause && Last->GetStringField(TEXT("device")) == R.Device);
	}

	// W5: the open-circuit fault and the protection delays.
	const TArray<TSharedPtr<FJsonValue>>& Faults = I->GetArrayField(TEXT("sensorFaults"));
	TestTrue(TEXT("one detector fault, open circuit"), Faults.Num() == 1 && Faults[0]->AsObject()->GetStringField(TEXT("health")) == TEXT("open")
		&& int32(Faults[0]->AsObject()->GetNumberField(TEXT("point"))) == Faulty);
	const TSharedPtr<FJsonObject> D = I->GetObjectField(TEXT("leakProtectionDelays"));
	TestTrue(TEXT("protection delays = the policy"), D->GetNumberField(TEXT("bmsCycleS")) == Sim->LeakProtectionPolicy.BmsPollS
		&& D->GetNumberField(TEXT("valveStrokeS")) == Sim->LeakProtectionPolicy.ValveStrokeS && D->GetBoolField(TEXT("estimated")));
	TestTrue(TEXT("protection acted and leak alarms exist"), I->GetArrayField(TEXT("leakProtection")).Num() > 0 && I->GetArrayField(TEXT("leakAlarms")).Num() > 0);

	// Same functions as the report: the two sections serialize identically at the same SimTime.
	FString Summary, ReportText;
	TestTrue(TEXT("report written"), Sim->WriteEvaluationReport(TEXT("AutomationWebWaterTables"), Summary));
	FFileHelper::LoadFileToString(ReportText, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), TEXT("AutomationWebWaterTables.json")));
	const TSharedPtr<FJsonObject> Rep = ParseWaterTablesJson(ReportText);
	if (TestTrue(TEXT("report parses"), Rep.IsValid()))
	{
		TestEqual(TEXT("W4 section = report"), WaterTablesText(W), WaterTablesText(Rep->GetObjectField(TEXT("waterImpact"))));
		TestEqual(TEXT("W5 section = report"), WaterTablesText(I), WaterTablesText(Rep->GetObjectField(TEXT("instruments"))));
	}
	TestTrue(TEXT("global heat ledger closed (< 1e-9)"), FMath::Abs(Sim->GetSnapshot().NormalizedEnergyResidual) < 1e-9);
	AddInfo(FString::Printf(TEXT("웹 표: 정상 아닌 대상 %d(트립·손상 %d, 차단 %d), 침수 사건 %d, 감지기 고장 %d, 보호 동작 %d, 누수 경보 %d"),
		Targets.Num(), Tripped, CutShown, EventCount, Faults.Num(), I->GetArrayField(TEXT("leakProtection")).Num(), I->GetArrayField(TEXT("leakAlarms")).Num()));
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}

#endif
