// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// S6 (2026-09-27): a rerun with the same seed, initial conditions and events gives the same evaluation (compared as
// numbers, not as a race test); a utilization event lowers the IT power from its time on; the report files are written.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEvaluationTest, "VRG.Eval.RerunEventsAndReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FEvaluationTest::RunTest(const FString& Parameters)
{
	struct FResult { TArray<double> MeanK, SensorK; double ItW = 0.0, EndItW = 0.0; bool bReport = false; FString Summary; };
	auto Run = [&](bool bEvent, const FString& ReportName)
	{
		FResult Out;
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
		Sim->EvalSettings.Seed = 11;
		Sim->EvalSettings.WindowS = 2.0;
		Sim->EvalSettings.Sensor.SamplePeriodS = .5;
		if (bEvent)
		{
			ADataCenterSimulationActor::FEvalEvent E; E.TimeS = 1.0; E.Kind = TEXT("util"); E.Value = .3;
			Sim->EvalEvents.Add(E);
		}
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 20; ++F) Sim->Tick(.32f);  // 16 steps x 0.02 s per tick: 6.4 s
		const FVrgEvaluation& Eval = Sim->GetEvaluation();
		for (int32 I = 0; I < Eval.GetNumInlets(); ++I)
		{
			Out.MeanK.Add(Eval.GetInlet(I).WindowMeanK);
			Out.SensorK.Add(Eval.GetInlet(I).SensorLastK);
		}
		Out.ItW = Eval.GetWindowPower().ItW;
		Out.EndItW = Sim->GetSnapshot().TotalItHeatW;
		if (!ReportName.IsEmpty()) Out.bReport = Sim->WriteEvaluationReport(ReportName, Out.Summary);
		World->DestroyWorld(false);
		return Out;
	};

	const FResult A = Run(true, TEXT("AutomationEvalA")), B = Run(true, FString()), NoEvent = Run(false, FString());
	bool bSame = A.MeanK.Num() == B.MeanK.Num() && A.MeanK.Num() > 0 && A.ItW == B.ItW;
	for (int32 I = 0; bSame && I < A.MeanK.Num(); ++I) bSame = A.MeanK[I] == B.MeanK[I] && A.SensorK[I] == B.SensorK[I];
	AddInfo(FString::Printf(TEXT("흡기 %d개 | 같은 입력 두 번: 동일 %d | 창 평균 IT %.1f kW (이벤트 없음 %.1f kW), 끝 IT %.1f / %.1f kW | %s"),
		A.MeanK.Num(), bSame, A.ItW / 1e3, NoEvent.ItW / 1e3, A.EndItW / 1e3, NoEvent.EndItW / 1e3, *A.Summary));
	TestTrue(TEXT("Rerun gives the same evaluation"), bSame);
	TestTrue(TEXT("The utilization event lowers the IT power"), A.EndItW < .6 * NoEvent.EndItW && A.ItW < NoEvent.ItW);
	TestTrue(TEXT("Report written"), A.bReport);
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"));
	TestTrue(TEXT("JSON and Markdown exist"), IFileManager::Get().FileExists(*FPaths::Combine(Dir, TEXT("AutomationEvalA.json")))
		&& IFileManager::Get().FileExists(*FPaths::Combine(Dir, TEXT("AutomationEvalA.md"))));
	IFileManager::Get().Delete(*FPaths::Combine(Dir, TEXT("AutomationEvalA.json")));
	IFileManager::Get().Delete(*FPaths::Combine(Dir, TEXT("AutomationEvalA.md")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
