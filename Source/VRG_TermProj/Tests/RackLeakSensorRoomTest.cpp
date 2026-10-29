#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackLeakSensorRoom, "VRG.Sensors.RackInterior.RoomAndReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackLeakSensorRoom::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = false;
	auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(200, 150, 0), FRotator(0, 90, 0));
	Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false; Rack->LayoutId = TEXT("sensor_rack_a");
	Rack->SetActorScale3D(FVector(1.1, .9, 1.2));
	auto* Other = World->SpawnActor<ARackThermalActor>(FVector(200, 300, 0), FRotator(0, 90, 0));
	Other->ApplyGb200Nvl72Preset(); Other->bShowReadout = false; Other->LayoutId = TEXT("sensor_rack_b");
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100, 150, 0), FRotator::ZeroRotator);
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false; Sim->bFacilityPlant = false; Sim->MaxStepsPerFrame = 16;
	Room->RebuildRoomNow();
	TArray<double> ReferenceHeld, ReferenceTransit, ReferenceRear, ReferenceExposure, ReferenceDepth, ReferenceTemp, ReferenceFlow;
	TArray<FLeakAlarm> ReferenceAlarms;
	TArray<FString> ReferenceIds;
	double ReferenceInput = 0.0, MaxEnergy = 0.0, MaxWater = 0.0, MaxFloor = 0.0;
	for (int32 Path = 0; Path < 3; ++Path)
	{
		// Same physics with Tick, RunSteps, and contact instruments disabled; no model parameter changes.
		Sim->bRackLeakSensors = Path != 2;
		Sim->EnterSimulationMode();
		if (!TestTrue(TEXT("native room enters simulation"), Sim->GetSimMode() == ESimMode::Simulation)) break;
		TestTrue(TEXT("fresh run immediately clears sensors and water baselines"), Sim->GetInstruments().GetAlarmLog().IsEmpty()
			&& Sim->GetRackInteriorCoupling().GetWater().ReceivedM3 == 0.0);
		TestTrue(TEXT("internal manifold and plate sources open"), Room->SetInternalLeakHoleDiameterM(Rack, 20, 0, .003)
			&& Room->SetInternalLeakHoleDiameterM(Rack, 0, 2, InRackNetwork::HoseDiameterM));
		auto Run = [&](int32 Steps)
		{
			if (Path == 0) for (int32 Frame = 0; Frame < Steps / 5; ++Frame) Sim->Tick(.1f);
			else
			{
				const double EndS = Sim->GetSnapshot().SimTimeS + Steps * Sim->FixedStepS;
				Sim->QueueWarmupSteps(Steps);
				for (int32 Frame = 0; Frame < 100 && Sim->GetSnapshot().SimTimeS < EndS - 1e-9; ++Frame) Sim->Tick(0.0f);
				TestTrue(TEXT("RunSteps covers requested time"), FMath::Abs(Sim->GetSnapshot().SimTimeS - EndS) < 1e-9);
			}
		};
		Run(150);
		const int32 BeforeRepair = Sim->GetInstruments().GetAlarmLog().Num();
		Room->SetInternalLeakHoleDiameterM(Rack, 20, 0, 0.0); Room->SetInternalLeakHoleDiameterM(Rack, 0, 2, 0.0);
		Run(50);
		const auto& Bank = Sim->GetInstruments(); const auto& C = Sim->GetRackInteriorCoupling(); const auto& W = C.GetWater();
		TArray<FString> Ids;
		for (int32 I = 0; I < Bank.Num(); ++I)
		{
			const auto& P = Bank.GetPoint(I); if (!P.bWaterContact) continue;
			Ids.Add(P.Id);
			if (P.Site == EInstrumentSite::RackDripTray)
				TestEqual(TEXT("drip uses generated bottom arrival, independently of W3 acknowledgements"), Bank.GetContactWaterM3(I), W.FloorM3[P.Key] + W.RearFloorM3[P.Key]);
			if (P.RackId == Other->LayoutId) TestFalse(TEXT("no cross rack alarm"), Bank.IsLeakAlarm(I));
			if (P.Site == EInstrumentSite::RackManifoldRope)
			{
				double LocalMax = 0.0;
				for (int32 S = W.Start[P.Key]; S < W.Start[P.Key + 1]; ++S) LocalMax = FMath::Max(LocalMax, W.RearExposureM3[S]);
				TestEqual(TEXT("rope uses local exposure, not sum of repeated downstream contacts"), Bank.GetContactWaterM3(I), LocalMax);
			}
		}
		TestEqual(TEXT("two contact points per native rack, or none when disabled"), Ids.Num(), Path == 2 ? 0 : 4);
		TestEqual(TEXT("only actual wet rack alarms, no internal leaks misreported as room pipes"), Bank.GetAlarmLog().Num(), Path == 2 ? 0 : 2);
		TestEqual(TEXT("repair retains existing alarms without duplicates"), Bank.GetAlarmLog().Num(), BeforeRepair);
		for (const auto& A : Bank.GetAlarmLog())
		{
			const auto& P = Bank.GetPoint(A.Point);
			TestTrue(TEXT("contact alarm attributed to wet rack"), P.bWaterContact && P.RackId == Rack->LayoutId && A.FirstContactS > 0);
			TestTrue(TEXT("native polling and response bounds"), A.TimeS >= A.ThresholdS + P.Contact.ResponseDelayS - 1e-9
				&& A.TimeS <= A.ThresholdS + P.Contact.ResponseDelayS + P.Contact.SamplePeriodS + Sim->FixedStepS);
			AddInfo(FString::Printf(TEXT("path%d %s first %.12g threshold %.12g alarm %.12g s, contact %.12g m3, delay excess %.12g s"),
				Path, *P.Id, A.FirstContactS, A.ThresholdS, A.TimeS, A.WaterM3, A.TimeS - A.ThresholdS - P.Contact.ResponseDelayS));
		}
		TArray<double> Flows;
		for (int32 I = 0; I < Room->GetNetwork().GetNumPipes(); ++I) Flows.Add(Room->GetNetwork().GetPipeFlowM3s(I));
		const auto Snapshot = Sim->GetSnapshot();
		MaxEnergy = FMath::Max(MaxEnergy, FMath::Abs(Snapshot.NormalizedEnergyResidual));
		MaxWater = FMath::Max(MaxWater, FMath::Abs(W.BalanceErrorM3()));
		MaxFloor = FMath::Max(MaxFloor, FMath::Abs(Sim->GetFloorWater().GetTotals().Residual));
		TestTrue(TEXT("pressure and hydraulic convergence retained"), Snapshot.bAirPressureConverged && Snapshot.bHydraulicsConverged);
		for (double K : Snapshot.RackAirInletTempK) TestTrue(TEXT("existing inlet plausibility"), K >= 288.15 && K <= 308.15);
		TestEqual(TEXT("internal source never enters W2"), Sim->GetLeakSpray().GetTotals().EmittedM3, 0.0);
		if (Path == 0)
		{
			ReferenceHeld = W.HeldM3; ReferenceTransit = W.TransitM3; ReferenceRear = W.GetRearTransitM3(); ReferenceExposure = W.RearExposureM3;
			ReferenceDepth = Sim->GetFloorWater().GetDepthM(); ReferenceTemp = Rack->GetMutableModel().GetState().ComponentTempK;
			ReferenceFlow = Flows; ReferenceInput = W.ReceivedM3; ReferenceAlarms = Bank.GetAlarmLog(); ReferenceIds = Ids;
		}
		else
		{
			TestTrue(TEXT("instruments and dispatch path do not change physical water, heat or flow"), ReferenceHeld == W.HeldM3 && ReferenceTransit == W.TransitM3
				&& ReferenceRear == W.GetRearTransitM3() && ReferenceExposure == W.RearExposureM3 && ReferenceDepth == Sim->GetFloorWater().GetDepthM()
				&& ReferenceTemp == Rack->GetMutableModel().GetState().ComponentTempK && ReferenceFlow == Flows && ReferenceInput == W.ReceivedM3);
			if (Path == 1)
			{
				TestTrue(TEXT("rerun sensor IDs stable"), ReferenceIds == Ids);
				for (int32 I = 0; I < FMath::Min(ReferenceAlarms.Num(), Bank.GetAlarmLog().Num()); ++I)
				{
					const auto& A = Bank.GetAlarmLog()[I]; const auto& B = ReferenceAlarms[I];
					TestTrue(TEXT("Tick/RunSteps same detection times and values"), A.Point == B.Point && A.TimeS == B.TimeS
						&& A.FirstContactS == B.FirstContactS && A.ThresholdS == B.ThresholdS && A.WaterM3 == B.WaterM3);
				}
			}
		}
		if (Path == 1)
		{
			const FString Name = TEXT("W35RackSensors"); FString Summary, JsonText, Markdown;
			TestTrue(TEXT("existing report writer succeeds"), Sim->WriteEvaluationReport(Name, Summary));
			TestTrue(TEXT("JSON and Markdown available"), FFileHelper::LoadFileToString(JsonText, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), Name + TEXT(".json")))
				&& FFileHelper::LoadFileToString(Markdown, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), Name + TEXT(".md"))));
			TSharedPtr<FJsonObject> Root;
			if (TestTrue(TEXT("report JSON parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) && Root.IsValid()))
			{
				const auto J = Root->GetObjectField(TEXT("instruments"));
				TestTrue(TEXT("D9 and legacy count fields retained"), J->GetBoolField(TEXT("simulatedMeasurement")) && J->GetNumberField(TEXT("points")) == Bank.Num() && J->HasField(TEXT("leakCables")));
				TestEqual(TEXT("report describes all rack sensors"), J->GetArrayField(TEXT("rackWaterSensors")).Num(), 4);
				const auto& Events = J->GetArrayField(TEXT("leakAlarms"));
				TestEqual(TEXT("report alarm count"), Events.Num(), Bank.GetAlarmLog().Num());
				for (int32 I = 0; I < FMath::Min(Events.Num(), Bank.GetAlarmLog().Num()); ++I)
				{
					const auto E = Events[I]->AsObject(); const auto& A = Bank.GetAlarmLog()[I]; const auto& P = Bank.GetPoint(A.Point);
					TestTrue(TEXT("report identity, type and timing match measured alarm"), E->GetStringField(TEXT("sensorId")) == P.Id && E->GetStringField(TEXT("rackId")) == P.RackId
						&& E->GetStringField(TEXT("type")) == InstrumentSpecs::SiteCode(P.Site) && E->GetNumberField(TEXT("timeS")) == A.TimeS
						&& E->GetNumberField(TEXT("firstContactS")) == A.FirstContactS && E->GetNumberField(TEXT("thresholdReachedS")) == A.ThresholdS
						&& E->GetNumberField(TEXT("contactWaterM3")) == A.WaterM3 && !E->GetBoolField(TEXT("flowBased")));
					TestTrue(TEXT("existing alarm table identifies rack/type"), Markdown.Contains(P.Id) && Markdown.Contains(InstrumentSpecs::SiteCode(P.Site)) && Markdown.Contains(TEXT("D9")));
				}
			}
		}
		AddInfo(FString::Printf(TEXT("path%d input %.12g m3; interior residual %.12g m3; floor relative %.12g; global energy %.12g; alarms %d"),
			Path, W.ReceivedM3, W.BalanceErrorM3(), Sim->GetFloorWater().GetTotals().Residual, Snapshot.NormalizedEnergyResidual, Bank.GetAlarmLog().Num()));
		Sim->EnterLayoutMode();
	}
	TestTrue(TEXT("existing conservation criteria"), MaxWater < 1e-15 && MaxFloor < 1e-12 && MaxEnergy < 1e-9);
	Sim->EnterLayoutMode(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}
#endif
