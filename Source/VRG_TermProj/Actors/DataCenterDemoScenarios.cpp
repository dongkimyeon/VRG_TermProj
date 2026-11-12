// Fill out your copyright notice in the Description page of Project Settings.

// 2026-11-12 (발표 시연, 사용자 요청 "예측 못 하는 상황을 실시간으로, 2~4배속"): 시간 배속과 장애 시나리오 예약.
// 시나리오는 기존 이벤트(vrg.Eval.Event)와 기존 정책 명령만 쓴다. 결과(퍼지는 범위·순서·시각)는 모델이 계산하며 여기서 정하지 않는다(D4).

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<float> CVarSimSpeed(TEXT("vrg.Sim.Speed"), 1.0f,
		TEXT("시뮬레이션 배속(실제 1초에 시뮬레이션 몇 초, 0.25~8, 기본 1). 스텝 크기는 그대로이고 프레임마다 스텝을 더 돌린다. 키 [ / ] = 0.5·1·2·4 순환."));

	struct FDemoScenarioInfo { const TCHAR* Name; const TCHAR* Title; const TCHAR* Watch; };
	const FDemoScenarioInfo DemoScenarios[] = {
		{ TEXT("leak"), TEXT("랙 0 트레이 0 냉각판 3 mm 누수"),
			TEXT("분사·트레이·바닥 물 → 냉각판·드립 트레이 감지기 경보 → 탱크 수위 공정값 경보 → 줄 재고 고갈 시 그 줄 랙 전부 냉각수 유량 0 → 칩 온도 상승·과열 판정") },
		{ TEXT("leakprotect"), TEXT("같은 누수 + 보호 차단 정책 켬(vrg.Leak.AutoIsolate 1)"),
			TEXT("감지기 → 트레이 셧다운 타이머 → 랙 격리 밸브 → 고갈 전에 막는지, 막힌 랙만 꺼지고 나머지는 사는지(leak 시나리오와 비교)") },
		{ TEXT("pump"), TEXT("줄 0을 맡은 CDU의 펌프 고장 → 15 s 뒤 대기 펌프도 고장"),
			TEXT("첫 고장: 대기 펌프 전환(2 s)으로 유량 회복 → 두 번째: 그 CDU가 맡은 모든 줄 유량 0(L_Test는 CDU 1대가 4줄 32랙 = 단일 고장점) → 칩 온도가 열용량만큼 오르다 과열 판정") },
		{ TEXT("flood"), TEXT("랙 2 공급 분기 3 mm 누수 + 냉각수 자동 보충 켬 + 침수 영향 켬(vrg.Water.Impact 1)"),
			TEXT("보충수가 재고를 채워 물이 계속 샌다 → 물줄기·바닥으로 퍼짐 → 물에 닿은 장비 젖음 → 15 s 통전 접촉 뒤 트립(전원 차단) → 장비 테두리 색·사건 목록 (보충 없는 leak 시나리오와 비교)") },
		{ TEXT("valve"), TEXT("운전자 실수: 랙 3 분기 밸브를 잘못 닫음"),
			TEXT("그 랙만 냉각수 유량 0, 나머지 줄은 펌프 차압 제어로 유지(격리 범위 ≠ 수력 영향 범위, D4) → 그 랙 칩 온도 상승") },
		{ TEXT("heatwave"), TEXT("폭염 외기 45 °C + FWS 드라이쿨러 8대 고장(설비 냉각 켬 필요)"),
			TEXT("설비수 공급 온도 상승 → CDU 공급 온도 상승 → 칩 온도·흡기 상승, 냉각 PUE 변화(vrg.Plant.Enable 1 후 R 재시작 상태에서)") },
	};
}

double ADataCenterSimulationActor::GetRequestedSimSpeed()
{
	return FMath::Clamp(double(CVarSimSpeed.GetValueOnGameThread()), 0.25, 8.0);
}

void ADataCenterSimulationActor::SetRequestedSimSpeed(double Speed)
{
	CVarSimSpeed->Set(float(FMath::Clamp(Speed, 0.25, 8.0)), ECVF_SetByConsole);
}

FString ADataCenterSimulationActor::StartDemoScenario(const FString& InName, bool& bOk)
{
	bOk = false;
	const FString Name = InName.ToLower();
	if (Name == TEXT("clear"))
	{
		// Removes the scheduled events and switches the scenario policies off; R restarts a clean run.
		EvalEvents.Reset();
		if (GEngine && GetWorld())
		{
			GEngine->Exec(GetWorld(), TEXT("vrg.Leak.AutoIsolate 0"));
			GEngine->Exec(GetWorld(), TEXT("vrg.Water.Impact 0"));
		}
		if (Room != nullptr) Room->SetCoolantMakeup(false);
		bOk = true;
		return TEXT("시연 이벤트와 정책을 지웠다(누수 보호·침수 영향·자동 보충 끔). R로 처음부터 다시 시작한다.");
	}
	const FDemoScenarioInfo* Info = nullptr;
	for (const FDemoScenarioInfo& S : DemoScenarios) if (Name == S.Name) Info = &S;
	if (Info == nullptr)
	{
		FString List;
		for (const FDemoScenarioInfo& S : DemoScenarios) List += FString::Printf(TEXT("\n  %s: %s"), S.Name, S.Title);
		return TEXT("시나리오: vrg.Demo <이름> | clear") + List;
	}
	if (Mode != ESimMode::Simulation) return TEXT("시뮬레이션 모드에서만 예약한다(Enter).");
	if (Name == TEXT("heatwave") && !(bPlantActive && Plant.IsThermalActive()))
		return TEXT("heatwave는 설비 냉각 모델이 돌 때만: vrg.Plant.Enable 1 입력 후 R(재시작)한 다음 다시 실행한다.");

	const double T0 = Clock.GetSimTimeS() + 2.0;
	auto Add = [this](double TimeS, const TCHAR* Kind, int32 Index, double Value, const TArray<FString>& Args = {})
	{
		FEvalEvent E;
		E.TimeS = TimeS; E.Kind = Kind; E.Index = Index; E.Value = Value; E.Args = Args;
		EvalEvents.Add(E);
	};
	auto Exec = [this](const TCHAR* Cmd) { if (GEngine && GetWorld()) GEngine->Exec(GetWorld(), Cmd); };
	if (Name == TEXT("leak") || Name == TEXT("leakprotect"))
	{
		if (Name == TEXT("leakprotect")) Exec(TEXT("vrg.Leak.AutoIsolate 1"));
		Add(T0, TEXT("leakat"), INDEX_NONE, 3.0e-3, { TEXT("rack:0:tray:0:plate") });
	}
	else if (Name == TEXT("pump"))
	{
		Add(T0, TEXT("cdupump"), 0, 0.0);
		Add(T0 + 15.0, TEXT("cdupump"), 0, 0.0);
	}
	else if (Name == TEXT("flood"))
	{
		Exec(TEXT("vrg.Water.Impact 1"));
		if (Room != nullptr) Room->SetCoolantMakeup(true);
		Add(T0, TEXT("leakat"), INDEX_NONE, 3.0e-3, { TEXT("room:2:supply") });
	}
	else if (Name == TEXT("valve"))
	{
		Add(T0, TEXT("valve"), 3, 1.0);
	}
	else if (Name == TEXT("heatwave"))
	{
		Add(T0, TEXT("ambient"), INDEX_NONE, 45.0 + 273.15);
		Add(T0 + 5.0, TEXT("plant"), INDEX_NONE, 0.0, { TEXT("drycooler"), TEXT("fws"), TEXT("8") });
	}
	EvalEvents.StableSort([](const FEvalEvent& L, const FEvalEvent& R) { return L.TimeS < R.TimeS; });
	bOk = true;
	return FString::Printf(TEXT("시연 '%s': %s (t=%.1f s부터) | 볼 것: %s"), Info->Name, Info->Title, T0, Info->Watch);
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GVrgLeakMakeupCommand(TEXT("vrg.Leak.Makeup"),
		TEXT("CDU 냉각수 자동 보충(기본 0=끔). vrg.Leak.Makeup 0|1 - 켜면 탱크가 비기 전에 보충해 누수가 계속된다(보충 유량 0.1 L/s·온도는 추정값)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (World == nullptr) return;
			for (TActorIterator<ADataCenterRoomActor> It(World); It; ++It) It->SetCoolantMakeup(Args.Num() > 0 && FCString::Atoi(*Args[0]) != 0);
		}));

	FAutoConsoleCommandWithWorldAndArgs GVrgDemoCommand(TEXT("vrg.Demo"),
		TEXT("발표 시연 장애 시나리오를 지금 SimTime + 2 s부터 예약한다. vrg.Demo leak | leakprotect | pump | flood | valve | heatwave | clear (인자 없으면 목록)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (World == nullptr) return;
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
			{
				bool bOk = false;
				const FString Text = It->StartDemoScenario(Args.Num() > 0 ? Args[0] : FString(), bOk);
				UE_LOG(LogTemp, Display, TEXT("[Demo] %s"), *Text);
				// Presenter banner (8 s): the scenario and what to watch. Display only.
				if (GEngine && Args.Num() > 0) GEngine->AddOnScreenDebugMessage(static_cast<uint64>(It->GetUniqueID()) + 77, 8.0f,
					bOk ? FColor(255, 214, 120) : FColor::Red, Text);
			}
		}));
}
