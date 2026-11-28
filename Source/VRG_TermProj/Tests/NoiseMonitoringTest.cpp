#include "Misc/AutomationTest.h"
#include "Simulation/Acoustics/NoiseMonitoring.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interaction/VrgLayoutFile.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Simulation/Acoustics/NoiseReporting.h"
#include "UI/VrgWebDashboard.h"
#include "Visualization/GjRackLayout.h"

#include <cmath>

namespace
{
	constexpr EAutomationTestFlags NoiseMonitoringFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	VrgNoise::FLevel MakeTestNoisePressure(double Db)
	{
		VrgNoise::FLevel Level;
		Level.Quantity = VrgNoise::EQuantity::SoundPressure;
		Level.State = VrgNoise::ELevelState::Known;
		Level.Db = Db;
		Level.KnownSources = 1;
		return Level;
	}

	FString NoiseTestJsonText(const TSharedRef<FJsonObject>& Json)
	{
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	bool ReadNoiseTestJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTimeAverageTest, "VRG.Noise.Monitoring.TimeAverage", NoiseMonitoringFlags)
bool FNoiseTimeAverageTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	FTimeAverage Average;
	Average.Add(MakeTestNoisePressure(60), 1.0);
	Average.Add(MakeTestNoisePressure(70), 2.0);
	const double Expected = 10.0 * std::log10((1e6 + 2.0 * 1e7) / 3.0);
	const double WeightedErrorDb = FMath::Abs(Average.GetDb().Get(-1.0) - Expected);
	FLevel Silent;
	Silent.State = ELevelState::Silent;
	Silent.SilentSources = 1;
	Average.Add(Silent, 1.0);
	const double SilenceErrorDb = FMath::Abs(Average.GetDb().Get(-1.0) - 10.0 * std::log10(21e6 / 4.0));
	FLevel Missing;
	Missing.State = ELevelState::MissingData;
	Missing.MissingSources = 1;
	Average.Add(Missing, 0.5);
	FLevel Partial = MakeTestNoisePressure(50);
	Partial.MissingSources = 2;
	Average.Add(Partial, 0.5);
	const double PartialErrorDb = FMath::Abs(Average.GetDb().Get(-1.0) - 10.0 * std::log10((21e6 + .5e5) / 5.0));
	TestTrue(TEXT("partial coverage records gaps and includes known energy as a lower bound"),
		FString(Average.GetState()) == TEXT("partial") && Average.IncompleteS == 1.0 && Average.DurationS == 5.0);

	FTimeAverage Quiet;
	Quiet.Add(Silent, 2.0);
	TestTrue(TEXT("zero energy serializes as silent with no finite dBA"), !Quiet.GetDb().IsSet() && FString(Quiet.GetState()) == TEXT("silent"));
	FTimeAverage Zero;
	Zero.Add(MakeTestNoisePressure(0), .5);
	Zero.Add(MakeTestNoisePressure(0), 1.5);
	TestTrue(TEXT("0 dBA is a real level and remains 0"), Zero.GetDb() == TOptional<double>(0.0));
	FTimeAverage Big;
	Big.Add(MakeTestNoisePressure(5000.0), 1.0);
	TestTrue(TEXT("averaging avoids intermediate power overflow"), Big.GetDb() == TOptional<double>(5000.0));

	FNoiseReceiverGroup Group;
	const FNoiseReading One{FVector(1, 2, 3), MakeTestNoisePressure(63), 0};
	for (int32 I = 0; I < 300; ++I)
		Group.Observe(MakeArrayView(&One, 1), I * .25, (I + 1) * .25, (I + 1) * .25, {100, 20, 290.0});
	Group.Observe(MakeArrayView(&One, 1), 74.75, 75.0, 75.0, {100, 20, 290.0});
	const auto& Track = Group.Tracks[0];
	TestTrue(TEXT("trace ring wraps and duplicate timestamp adds no time"), Track.Trace.Num() == NoiseTraceLimit
		&& Track.Trace[Track.NextTrace].TimeS == 11.25 && Track.Average.DurationS == 75.0 && Group.EndS == 75.0
		&& Group.MeanItW == 100 && Group.MeanCoolingW == 20 && Group.MeanMaxInletK == TOptional<double>(290.0));
	const double ConstantErrorDb = FMath::Abs(Track.Average.GetDb().Get(-1.0) - 63.0);
	AddInfo(FString::Printf(TEXT("N5 LAeq absolute errors weighted %.6e dB, silent interval %.6e dB, partial %.6e dB, constant %.6e dB"),
		WeightedErrorDb, SilenceErrorDb, PartialErrorDb, ConstantErrorDb));
	TestTrue(TEXT("weighted, silent, partial and constant means agree with independent energy sums"),
		WeightedErrorDb < 1e-12 && SilenceErrorDb < 1e-12 && PartialErrorDb < 1e-12 && ConstantErrorDb < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoisePersistenceAndSamplingTest, "VRG.Noise.Monitoring.LayoutReportAndWarmup", NoiseMonitoringFlags)
bool FNoisePersistenceAndSamplingTest::RunTest(const FString& Parameters)
{
	using namespace VrgNoise;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	const double Speed = ADataCenterSimulationActor::GetRequestedSimSpeed();
	ADataCenterSimulationActor::SetRequestedSimSpeed(1.0);
	const FString Name = TEXT("NoiseN5_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString LayoutPath = VrgLayoutFile::PathFor(Name);
	const FString ReportBase = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), Name);
	ON_SCOPE_EXIT
	{
		for (const FString& Path : {LayoutPath, ReportBase + TEXT(".json"), ReportBase + TEXT(".md")})
			IFileManager::Get().Delete(*Path, false, false, true);
		ADataCenterSimulationActor::SetRequestedSimSpeed(Speed);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = Sim->bShowPlant = Sim->bFloorDrains = false;
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	for (const double Y : {0.0, 66.0})
	{
		auto* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset();
		Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Rack->Input.FanSpeedOverride = .5;
	}
	Room->RebuildRoomNow();
	Sim->EnterLayoutMode();
	FRoomAirDomainSettings Domain;
	Domain.bManual = true;
	Domain.MinXYM = FVector2D(-4, -2); Domain.MaxXYM = FVector2D(5, 4); Domain.CeilingHeightM = 3.5;
	FString Summary;
	if (!TestTrue(TEXT("manual room accepted"), Sim->TrySetRoomAirDomain(Domain, Summary))) { AddError(Summary); return false; }

	FNoiseLayout Input;
	Input.Probe = FOutdoorReceiver{FVector(45, 25, 2), EOutdoorRadiation::Hemisphere};
	Input.Boundary = FBoundaryRequest{{{45, -10}, {45, 10}}, 10.0, 2.0};
	Input.IndoorPositionM = FVector2D(-2, 1);
	Input.IndoorHeightM = 1.5;
	Input.CduLwA = 76.0;
	Input.Environment.GroundFactor = .5;
	Input.Environment.Screens.Add({{35, -20}, {35, 20}, 6, .4});
	Input.Absorption.Shell[4] = .15;
	Sim->ApplyNoiseLayout(Input);
	const FString ExpectedSettings = NoiseTestJsonText(WriteNoiseLayout(Input));
	if (!TestTrue(TEXT("acoustic settings saved through normal v2 path"), VrgLayoutFile::Save(World, Name, Summary))) return false;
	TSharedPtr<FJsonObject> Layout;
	if (!TestTrue(TEXT("read saved layout"), ReadNoiseTestJson(LayoutPath, Layout))) return false;
	Sim->ApplyNoiseLayout(FNoiseLayout());
	for (int32 I = 0; I < 2; ++I)
	{
		TestTrue(TEXT("normal and repeated load succeed"), VrgLayoutFile::Load(World, Name, Summary));
		TestEqual(TEXT("authored values restore exactly"), NoiseTestJsonText(WriteNoiseLayout(Sim->CaptureNoiseLayout())), ExpectedSettings);
		TestTrue(TEXT("layout restoration does not restore runtime history"), Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor).Tracks.IsEmpty());
	}

	Sim->EnterSimulationMode();
	for (int32 I = 0; I < 3; ++I) Sim->Tick(.021f);
	const auto Tick = Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor);
	const auto& Quiet = Sim->GetNoiseReceiverGroup(EReceiverSet::Probe);
	if (!TestTrue(TEXT("indoor and silent plant acquired at the shared endpoint"), Tick.Tracks.Num() == 1
		&& Tick.Tracks[0].Latest.Pressure.Db.IsSet() && Quiet.Tracks.Num() == 1 && FString(Quiet.Tracks[0].Average.GetState()) == TEXT("silent")
		&& Tick.EndS == Sim->GetSimClockTimeS())) return false;
	Sim->PauseSim();
	Sim->Tick(.3f);
	TestEqual(TEXT("pause adds no duration"), Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor).EndS, Tick.EndS);

	const auto Bad = Layout->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("noise"))->GetObjectField(TEXT("environment"));
	const double PriorPressure = Bad->GetNumberField(TEXT("pressurePa"));
	Bad->SetNumberField(TEXT("pressurePa"), 0);
	FFileHelper::SaveStringToFile(NoiseTestJsonText(Layout.ToSharedRef()), *LayoutPath);
	TestFalse(TEXT("invalid noise settings reject load before live state changes"), VrgLayoutFile::Load(World, Name, Summary));
	TestTrue(TEXT("failed load preserves mode, SimTime and acoustic history"), Sim->GetSimMode() == ESimMode::Simulation
		&& Sim->GetSimClockTimeS() == Tick.EndS && Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor).EndS == Tick.EndS);
	Bad->SetNumberField(TEXT("pressurePa"), PriorPressure);
	Layout->GetObjectField(TEXT("settings"))->RemoveField(TEXT("noise"));
	FFileHelper::SaveStringToFile(NoiseTestJsonText(Layout.ToSharedRef()), *LayoutPath);
	TestTrue(TEXT("old v2 layout without the optional field still loads"), VrgLayoutFile::Load(World, Name, Summary));
	TestEqual(TEXT("old file retains configured receivers"), NoiseTestJsonText(WriteNoiseLayout(Sim->CaptureNoiseLayout())), ExpectedSettings);

	Sim->EnterSimulationMode();
	TestTrue(TEXT("clock reset clears aggregate and traces"), Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor).Tracks.IsEmpty());
	Sim->QueueWarmupSteps(3);
	for (int32 I = 0; I < 15 && Sim->GetSimClockTimeS() < Tick.EndS; ++I) Sim->Tick(0.f);
	const auto& Warmup = Sim->GetNoiseReceiverGroup(EReceiverSet::Indoor);
	if (!TestTrue(TEXT("warmup produced comparable indoor time-average data"), Warmup.Tracks.Num() == 1
		&& Warmup.Tracks[0].Average.GetDb().IsSet() && Tick.Tracks[0].Average.GetDb().IsSet())) return false;
	const double ClockError = FMath::Abs(Warmup.EndS - Tick.EndS);
	const double DbError = FMath::Abs(*Warmup.Tracks[0].Average.GetDb() - *Tick.Tracks[0].Average.GetDb());
	const FString Web = Sim->BuildNoiseDashboardJson(TEXT("indoor:0"));
	TSharedPtr<FJsonObject> WebObj;
	if (!TestTrue(TEXT("web payload parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Web), WebObj) && WebObj.IsValid())) return false;
	const auto WebFirst = WebObj->GetArrayField(TEXT("receivers"))[0]->AsObject();
	const double DisplayError = FMath::Abs(WebFirst->GetNumberField(TEXT("lpA")) - *Warmup.Tracks[0].Latest.Pressure.Db);
	TestTrue(TEXT("web carries a requested trace only and matches display inputs"),
		WebFirst->GetArrayField(TEXT("trace")).Num() == 1 && !WebObj->GetArrayField(TEXT("receivers"))[1]->AsObject()->HasField(TEXT("trace"))
		&& WebFirst->GetStringField(TEXT("color")) == TEXT("#") + NoiseLevelColor(Warmup.Tracks[0].Latest.Pressure).ToHex().Left(6));
	FVrgWebDashboard& Dashboard = FVrgWebDashboard::Get();
	Dashboard.RequestNoiseJson(TEXT("indoor:0"));
	FString Trace;
	TestTrue(TEXT("web requests use shared publish path"), Dashboard.GetNoiseRequest(Trace) && Trace == TEXT("indoor:0"));
	Dashboard.PublishNoise(Trace, FString(Web));
	TestEqual(TEXT("web hands back same payload"), Dashboard.RequestNoiseJson(Trace), Web);

	if (!TestTrue(TEXT("evaluation report saved"), Sim->WriteEvaluationReport(Name, Summary))) return false;
	TSharedPtr<FJsonObject> Report;
	if (!TestTrue(TEXT("evaluation JSON includes shared noise fields"), ReadNoiseTestJson(ReportBase + TEXT(".json"), Report)
		&& Report->HasField(TEXT("noise")))) return false;
	const auto ReportFirst = Report->GetObjectField(TEXT("noise"))->GetArrayField(TEXT("receivers"))[0]->AsObject();
	TestEqual(TEXT("evaluation trace and fields match web"), NoiseTestJsonText(ReportFirst.ToSharedRef()), NoiseTestJsonText(WebFirst.ToSharedRef()));
	FString Ab;
	AppendNoiseComparison(*Report, *Report, Ab);
	TestTrue(TEXT("comparison matches position, interval and cooling context"), Ab.Contains(TEXT("indoor:0"))
		&& Ab.Contains(TEXT("+0.00")) && Ab.Contains(TEXT("평균 냉각 kW A/B")));
	const FString OriginalKey = ReportFirst->GetStringField(TEXT("comparisonKey"));
	ReportFirst->SetStringField(TEXT("comparisonKey"), OriginalKey + TEXT("|moved"));
	FString Moved;
	TSharedPtr<FJsonObject> OriginalReport;
	if (!TestTrue(TEXT("original report remains available for comparison"), ReadNoiseTestJson(ReportBase + TEXT(".json"), OriginalReport))) return false;
	AppendNoiseComparison(*OriginalReport, *Report, Moved);
	TestFalse(TEXT("no noise comparison for a mismatched receiver"), Moved.Contains(TEXT("| indoor:0 |")));
	ReportFirst->SetStringField(TEXT("comparisonKey"), OriginalKey);
	ReportFirst->SetStringField(TEXT("laeqState"), TEXT("partial"));
	FString Partial;
	AppendNoiseComparison(*OriginalReport, *Report, Partial);
	TestTrue(TEXT("partial A/B row exposes level and suppresses delta"), Partial.Contains(TEXT(" (부분) | — |")));
	Dashboard.RequestNoiseJson(FString());
	TestTrue(TEXT("same SimTime gives the same LAeq through Tick and RunSteps"), ClockError == 0.0 && DbError < 1e-12 && DisplayError < 1e-12);
	AddInfo(FString::Printf(TEXT("N5 integration: Tick/RunSteps %.6e s and %.6e dBA, display/report %.6e dBA"), ClockError, DbError, DisplayError));
	Sim->EnterLayoutMode();
	return true;
}

#endif
