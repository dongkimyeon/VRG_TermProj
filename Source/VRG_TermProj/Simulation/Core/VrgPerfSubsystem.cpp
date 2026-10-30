// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Core/VrgPerfSubsystem.h"
#include "Simulation/Core/VrgHud.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "RenderCore.h"
#include "Actors/DataCenterSimulationActor.h"
#include "EngineUtils.h"

namespace
{
	// 백분위. 평균만 보면 가끔 튀는 프레임이 묻힌다.
	double PercentileMs(TArray<double>& Values, double Fraction)
	{
		if (Values.Num() == 0) { return 0.0; }
		Values.Sort();
		const int32 Index = FMath::Clamp(
			FMath::RoundToInt32(Fraction * (Values.Num() - 1)), 0, Values.Num() - 1);
		return Values[Index];
	}

	UVrgPerfSubsystem* FindSubsystem()
	{
		if (GEngine == nullptr) { return nullptr; }
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.OwningGameInstance != nullptr)
			{
				if (UVrgPerfSubsystem* Found = Context.OwningGameInstance->GetSubsystem<UVrgPerfSubsystem>())
				{
					return Found;
				}
			}
		}
		return nullptr;
	}
}

static FAutoConsoleCommand GVrgPerfOverlayCommand(
	TEXT("vrg.Perf.Overlay"),
	TEXT("화면 성능 오버레이를 켜고 끈다. vrg.Perf.Overlay 0|1"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UVrgPerfSubsystem* Perf = FindSubsystem())
		{
			const bool bEnable = Args.Num() == 0 ? !Perf->IsOverlayEnabled() : (FCString::Atoi(*Args[0]) != 0);
			Perf->SetOverlayEnabled(bEnable);
			UE_LOG(LogTemp, Display, TEXT("[VrgPerf] 오버레이 %s"), bEnable ? TEXT("켬") : TEXT("끔"));
		}
	}));

static FAutoConsoleCommand GVrgPerfCaptureCommand(
	TEXT("vrg.Perf.Capture"),
	TEXT("N초 동안 프레임 비용을 모아 Saved/Profiling에 CSV로 쓴다. vrg.Perf.Capture <초> [라벨] [quit]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UVrgPerfSubsystem* Perf = FindSubsystem())
		{
			const double DurationS = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 10.0;
			const FString Label = Args.Num() > 1 ? Args[1] : TEXT("baseline");
			const bool bQuit = Args.Num() > 2 && Args[2].Equals(TEXT("quit"), ESearchCase::IgnoreCase);
			Perf->BeginCapture(DurationS, Label, bQuit);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[VrgPerf] GameInstance가 없어 수집을 시작할 수 없다."));
		}
	}));

void UVrgPerfSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UVrgPerfSubsystem::Tick));
	BeginFrameHandle = FCoreDelegates::OnBeginFrame.AddUObject(this, &UVrgPerfSubsystem::OnBeginFrame);
	EndFrameHandle = FCoreDelegates::OnEndFrame.AddUObject(this, &UVrgPerfSubsystem::OnEndFrame);
}

void UVrgPerfSubsystem::Deinitialize()
{
	if (bCapturing)
	{
		// 수집 중에 PIE가 끝나도 모은 것까지는 남긴다.
		FinishCapture();
	}
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FCoreDelegates::OnBeginFrame.Remove(BeginFrameHandle);
	FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
	Super::Deinitialize();
}

void UVrgPerfSubsystem::OnBeginFrame()
{
	FrameStartCycles = FPlatformTime::Cycles64();
}

void UVrgPerfSubsystem::OnEndFrame()
{
	if (FrameStartCycles != 0)
	{
		LastGameThreadMs = FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - FrameStartCycles);
	}
	// 프레임이 끝났으니 누산기를 확정값으로 넘긴다. Tick은 이 확정값을 읽는다.
	VrgPerf::EndFrame();
}

void UVrgPerfSubsystem::BeginCapture(double DurationS, const FString& Label, bool bQuitWhenDone)
{
	if (bCapturing)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgPerf] 이미 수집 중이다."));
		return;
	}
	bCapturing = true;
	bQuitAfterCapture = bQuitWhenDone;
	CaptureRemainingS = FMath::Max(0.1, DurationS);
	CaptureLabel = Label;
	Samples.Reset();
	CaptureSim.Reset();
	if (GetWorld()) for (TActorIterator<ADataCenterSimulationActor> It(GetWorld()); It; ++It) { CaptureSim = *It; break; }
	UE_LOG(LogTemp, Display, TEXT("[VrgPerf] '%s' %.1f초 수집 시작"), *CaptureLabel, CaptureRemainingS);
}

bool UVrgPerfSubsystem::Tick(float DeltaTime)
{
	ElapsedS += DeltaTime;

	FVrgPerfSample Sample;
	Sample.TimeS = ElapsedS;
	if (const auto* Sim = CaptureSim.Get()) Sample.SimTimeS = Sim->GetEvaluation().GetSimTimeS();
	Sample.FrameMs = DeltaTime * 1000.0;
	// 게임 스레드는 우리가 직접 잰 값을 쓴다 (위 주석 참고).
	Sample.GameMs = LastGameThreadMs;

	// 렌더 스레드는 엔진 전역을 읽되, 에디터 PIE에서는 갱신되지 않아 0이나
	// 말도 안 되는 값이 나온다. 그럴 때는 0으로 두고 오버레이에 "-"로 적는다.
	const double RawDrawMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	Sample.DrawMs = (RawDrawMs > 0.0 && RawDrawMs < Sample.FrameMs * 5.0 + 50.0) ? RawDrawMs : 0.0;
	uint64 GpuCycles = 0;
	if (GpuHistoryState.PopFrameCycles(GpuCycles) != FRHIGPUFrameTimeHistory::EResult::Empty)
	{
		LastGpuMs = FPlatformTime::ToMilliseconds64(GpuCycles);
	}
	Sample.GpuMs = LastGpuMs;
	Sample.Costs = VrgPerf::LastFrame();

	if (bOverlayEnabled)
	{
		DrawOverlay(Sample);
	}

	// 예열이 남아 있으면 수집을 시작하지 않는다. 과도 상태 표본만 모으면
	// 정상 상태 수치와 섞여 비교가 성립하지 않는다.
	if (bCapturing && VrgPerf::PendingWarmupSteps() > 0)
	{
		return true;
	}

	if (bCapturing)
	{
		Samples.Add(Sample);
		// 한 프레임이 통째로 수집 창을 먹지 않게 한다. vrg.Air.RunSteps 같은
		// 동기 작업이 끼면 그 프레임의 델타가 수십 초가 되어 표본이 한 개만 남는다.
		CaptureRemainingS -= FMath::Min(double(DeltaTime), 1.0);
		if (CaptureRemainingS <= 0.0)
		{
			FinishCapture();
		}
	}

	if (PendingQuitFrames > 0 && --PendingQuitFrames == 0)
	{
		FPlatformMisc::RequestExit(false);
	}
	return true;
}

void UVrgPerfSubsystem::DrawOverlay(const FVrgPerfSample& Sample) const
{
	if (GEngine == nullptr)
	{
		return;
	}
	// D10: 이건 측정값이다. 목표치나 보장값이 아니다.
	// 게임 스레드 대비가 아니라 프레임 대비로 적는다. 에디터 PIE에서는 게임 스레드
	// 값이 신뢰할 수 없어 "게임의 3%" 같은 오해를 부르는 수치가 나왔다.
	const double SolverShare = Sample.FrameMs > 0.0 ? 100.0 * Sample.Costs.SolverMs / Sample.FrameMs : 0.0;
	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(0x56524750, 0.0f, FColor::Yellow,
		FString::Printf(
			TEXT("[VrgPerf] Frame %.2f ms (%.1f fps) | Game %.2f | Draw %s | GPU %s"),
			Sample.FrameMs, Sample.FrameMs > 0.0 ? 1000.0 / Sample.FrameMs : 0.0,
			Sample.GameMs,
			Sample.DrawMs > 0.0 ? *FString::Printf(TEXT("%.2f"), Sample.DrawMs) : TEXT("-"),
			Sample.GpuMs > 0.0 ? *FString::Printf(TEXT("%.2f"), Sample.GpuMs) : TEXT("-")));
	if (VrgHud::ShowDebugText()) GEngine->AddOnScreenDebugMessage(0x56524751, 0.0f, FColor::Yellow,
		FString::Printf(
			TEXT("[VrgPerf] 솔버 %.2f ms (프레임의 %.0f%%, 스텝 %d, 압력 %.2f ms / %d회) | 디버그 드로우 %.2f ms / %d개"),
			Sample.Costs.SolverMs, SolverShare, Sample.Costs.SolverSteps,
			Sample.Costs.PressureMs, Sample.Costs.PressureIterations,
			Sample.Costs.DebugDrawMs, Sample.Costs.DrawnPrimitives));
}

void UVrgPerfSubsystem::FinishCapture()
{
	bCapturing = false;
	if (Samples.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgPerf] 모은 프레임이 없다."));
		return;
	}

	// 첫 몇 프레임은 레벨 로드와 셰이더 컴파일이 섞여 대표값이 아니다.
	const int32 WarmupFrames = FMath::Min(Samples.Num() / 4, 30);

	FString Csv = TEXT("TimeS,FrameMs,GameMs,DrawMs,GpuMs,SolverMs,PressureMs,CouplingMs,DebugDrawMs,SolverSteps,PressureIterations,DrawnPrimitives,DashboardMs,LayoutEditMs,PlantDisplayMs,RoomTickMs,InventoryHydraulicsMs,RackModelsMs,WaterMs,SimTimeS,HydSetupMs,HydResidualMs,HydJacobianMs,SparseAssemblyMs,SparseAnalyzeMs,SparseFactorMs,SparseBacksolveMs,HydDenseMs,InventoryOtherMs,TransportMs,TrayLimitMs,PullInputsMs,CoolantHxMs,PushAirMs,AirStage0Ms,AirStage1Ms,AirStage2Ms,AirStage3Ms,AirStage4Ms,AirStage5Ms,AirStage6Ms,HydNewtonIterations,HydResidualCalls,SparseAnalyses,SparseReuseHits\n");
	TArray<double> FrameMs;
	TArray<double> GameMs;
	TArray<double> DrawMs;
	TArray<double> GpuMs;
	TArray<double> SolverMs;
	TArray<double> PressureMs;
	TArray<double> DebugDrawMs;
	double DrawnSum = 0.0;
	double IterationSum = 0.0;
	double DashboardSum = 0.0;
	// Layout-mode costs: the median hides a 0.25 s throttled cost, so the summary gives the 95% and the max.
	TArray<double> LayoutEditMs;
	TArray<double> PlantDisplayMs;
	TArray<double> RoomTickMs;
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		const FVrgPerfSample& S = Samples[Index];
		Csv += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f"),
			S.TimeS, S.FrameMs, S.GameMs, S.DrawMs, S.GpuMs,
			S.Costs.SolverMs, S.Costs.PressureMs, S.Costs.CouplingMs, S.Costs.DebugDrawMs,
			S.Costs.SolverSteps, S.Costs.PressureIterations, S.Costs.DrawnPrimitives, S.Costs.DashboardMs,
			S.Costs.LayoutEditMs, S.Costs.PlantDisplayMs, S.Costs.RoomTickMs,
			S.Costs.InventoryHydraulicsMs, S.Costs.RackModelsMs, S.Costs.WaterMs, S.SimTimeS);
 for(double Ms:S.Costs.DetailMs) Csv+=FString::Printf(TEXT(",%.6f"),Ms);
 Csv+=FString::Printf(TEXT(",%.0f,%.0f,%.0f,%.0f\n"),S.Costs.HydNewtonIterations,S.Costs.HydResidualCalls,S.Costs.SparseAnalyses,S.Costs.SparseReuseHits);
		if (Index < WarmupFrames)
		{
			continue;
		}
		FrameMs.Add(S.FrameMs);
		GameMs.Add(S.GameMs);
		DrawMs.Add(S.DrawMs);
		GpuMs.Add(S.GpuMs);
		SolverMs.Add(S.Costs.SolverMs);
		PressureMs.Add(S.Costs.PressureMs);
		DebugDrawMs.Add(S.Costs.DebugDrawMs);
		DrawnSum += S.Costs.DrawnPrimitives;
		IterationSum += S.Costs.PressureIterations;
		DashboardSum += S.Costs.DashboardMs;
		LayoutEditMs.Add(S.Costs.LayoutEditMs);
		PlantDisplayMs.Add(S.Costs.PlantDisplayMs);
		RoomTickMs.Add(S.Costs.RoomTickMs);
	}
	auto MaxMs = [](const TArray<double>& V) { double M = 0.0; for (double X : V) M = FMath::Max(M, X); return M; };

	const int32 Counted = FMath::Max(1, FrameMs.Num());
	const FString Summary = FString::Printf(
		TEXT("[VrgPerf] '%s' 프레임 %d개 (워밍업 %d개 제외 %d개)\n")
		TEXT("  중앙값 ms  Frame %.2f | Game %.2f | Draw %.2f | GPU %.2f\n")
		TEXT("  95%% ms     Frame %.2f | Game %.2f | Draw %.2f | GPU %.2f\n")
		TEXT("  중앙값 ms  솔버 %.2f | 압력 %.2f | 디버그 드로우 %.2f\n")
		TEXT("  프레임당 평균 압력 반복 %.1f회 | 디버그 프리미티브 %.0f개 | 웹 요약 생성 프레임당 평균 %.4f ms\n")
		TEXT("  배치 ms (중앙/95%%/최대)  편집 %.2f/%.2f/%.2f | 설비 마당 %.2f/%.2f/%.2f | 방 Tick %.2f/%.2f/%.2f\n")
		TEXT("  측정값이다. 보장값이 아니다 (D10)."),
		*CaptureLabel, Samples.Num(), WarmupFrames, FrameMs.Num(),
		PercentileMs(FrameMs, 0.5), PercentileMs(GameMs, 0.5),
		PercentileMs(DrawMs, 0.5), PercentileMs(GpuMs, 0.5),
		PercentileMs(FrameMs, 0.95), PercentileMs(GameMs, 0.95),
		PercentileMs(DrawMs, 0.95), PercentileMs(GpuMs, 0.95),
		PercentileMs(SolverMs, 0.5), PercentileMs(PressureMs, 0.5), PercentileMs(DebugDrawMs, 0.5),
		IterationSum / Counted, DrawnSum / Counted, DashboardSum / Counted,
		PercentileMs(LayoutEditMs, 0.5), PercentileMs(LayoutEditMs, 0.95), MaxMs(LayoutEditMs),
		PercentileMs(PlantDisplayMs, 0.5), PercentileMs(PlantDisplayMs, 0.95), MaxMs(PlantDisplayMs),
		PercentileMs(RoomTickMs, 0.5), PercentileMs(RoomTickMs, 0.95), MaxMs(RoomTickMs));

	const FString Directory = FPaths::ProjectSavedDir() / TEXT("Profiling");
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Stem = Directory / FString::Printf(TEXT("VrgPerf-%s"), *CaptureLabel);
	// UTF-8로 못 박는다. 기본값은 한글이 섞이면 UTF-16으로 저장해서
	// 단계 간 비교를 위해 파일을 열어 볼 때 읽기 어렵다.
	FFileHelper::SaveStringToFile(Csv, *(Stem + TEXT(".csv")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(Summary, *(Stem + TEXT(".txt")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	UE_LOG(LogTemp, Display, TEXT("%s"), *Summary);
	UE_LOG(LogTemp, Display, TEXT("[VrgPerf] 기록: %s.csv / %s.txt"), *Stem, *Stem);
	// Capture-end physics report (after timed samples). The warm-up report alone cannot
	// establish conservation/convergence during a leak that starts at capture time.
	if (auto* Sim = CaptureSim.Get(); Sim && Sim->GetSimMode() == ESimMode::Simulation)
	{
		FString PhysicsSummary;
		const bool bOk = Sim->WriteEvaluationReport(CaptureLabel + TEXT("_End"), PhysicsSummary);
		UE_LOG(LogTemp, Display, TEXT("[VrgPerf] capture-end physics report %s: %s"), bOk ? TEXT("OK") : TEXT("FAILED"), *PhysicsSummary);
	}

	// 같은 라벨로 화면도 한 장 남긴다. 1단계 완료 조건이 "박스 모드와 볼륨 모드가
	// 같은 그림인가"라서, 숫자만으로는 확인할 수 없다.
	// UI를 포함해 찍는다. 화면 오버레이에만 뜨는 열수지·수렴 여부를 그림 한 장으로
	// 같이 남겨야 나중에 "그때 물리가 맞았나"를 확인할 수 있다.
	FScreenshotRequest::RequestScreenshot(
		FString::Printf(TEXT("VrgShot-%s"), *CaptureLabel), true, false);

	if (bQuitAfterCapture)
	{
		// 스크린샷이 디스크에 떨어질 시간을 준다.
		PendingQuitFrames = 8;
	}
}
