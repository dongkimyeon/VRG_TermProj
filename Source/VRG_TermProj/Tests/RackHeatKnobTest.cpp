// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/RackThermalActor.h"
#include "Engine/World.h"
#include "Simulation/Thermal/RackThermalModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// 2026-10-13: the CPU/GPU heat knob (keys 1/2, 3/4, and applied once on every start by the debug controller) wrote the
// first CPU node's watts (a Grace CPU, 280 W) into every CPU node, including the management switch's 25 W control CPU
// (UA 1.5 W/K -> ~200 C and rising in L_Test). The knob now scales every node of the kind by the same multiple of its
// own default, and a saved layout-file array carrying exactly the old trace is repaired on load. Values are the GB200
// NVL72 preset defaults (no new physical numbers).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRackHeatKnobTest, "VRG.Thermal.Rack.HeatKnobKeepsPartShares",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRackHeatKnobTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams P = FRackThermalModel::MakeGb200Nvl72Params();
	const TArray<double> Default = FRackThermalModel::MakeDefaultHeatW(P);
	TArray<FRackComponentParams> Nodes; TArray<int32> Starts;
	FRackThermalModel::ResolveLayout(P, Nodes, Starts);
	const int32 FirstCpu = Nodes.IndexOfByPredicate([](const FRackComponentParams& N) { return N.Kind == ERackPartKind::Cpu; });
	TArray<int32> OtherCpus;   // CPU nodes whose part default differs from the first CPU's (the management switch control CPUs)
	for (int32 N = 0; N < Nodes.Num(); ++N)
		if (Nodes[N].Kind == ERackPartKind::Cpu && Nodes[N].DefaultHeatW != Nodes[FirstCpu].DefaultHeatW) OtherCpus.Add(N);
	if (!TestTrue(TEXT("preset has a first CPU and other CPU parts"), FirstCpu != INDEX_NONE && OtherCpus.Num() == 2)) return false;
	const double GraceW = Nodes[FirstCpu].DefaultHeatW, ControlW = Nodes[OtherCpus[0]].DefaultHeatW;
	AddInfo(FString::Printf(TEXT("first CPU %s %.1f W, other CPU %s %.1f W x%d"), *Nodes[FirstCpu].Name.ToString(), GraceW,
		*Nodes[OtherCpus[0]].Name.ToString(), ControlW, OtherCpus.Num()));

	// 1) The exact L_Test trace (control CPUs at the first CPU's watts) is restored bit-exactly to the defaults.
	TArray<double> H = Default;
	for (const int32 N : OtherCpus) H[N] = GraceW;
	TestEqual(TEXT("old trace: both control CPUs repaired"), FRackThermalModel::RepairKindHeatSpread(P, H), 2);
	TestTrue(TEXT("old trace: array equals the defaults exactly"), H == Default);

	// 2) The same trace after the knob moved the first CPU to 300 W: control CPUs get 25 W x 300/280.
	H = Default;
	for (int32 N = 0; N < Nodes.Num(); ++N) if (Nodes[N].Kind == ERackPartKind::Cpu) H[N] = 300.0;
	TestEqual(TEXT("scaled trace repaired"), FRackThermalModel::RepairKindHeatSpread(P, H), 2);
	TestTrue(TEXT("scaled trace: control CPU = default x 300 / first default"),
		FMath::Abs(H[OtherCpus[0]] - ControlW * 300.0 / GraceW) <= 1e-15 * ControlW && H[FirstCpu] == 300.0);

	// 3) Deliberate settings are kept: a control CPU at its own chosen value, or every Grace raised but the control CPU default.
	H = Default; H[OtherCpus[0]] = 30.0;
	TestEqual(TEXT("deliberate control CPU value kept"), FRackThermalModel::RepairKindHeatSpread(P, H), 0);
	TestEqual(TEXT("deliberate value unchanged"), H[OtherCpus[0]], 30.0);
	H = Default; H[FirstCpu] = 320.0;
	TestEqual(TEXT("first CPU changed alone: nothing to repair"), FRackThermalModel::RepairKindHeatSpread(P, H), 0);
	H = Default;
	TestEqual(TEXT("defaults: nothing to repair"), FRackThermalModel::RepairKindHeatSpread(P, H), 0);

	// 4) The knob keeps each part's share: +10 % on the first CPU is +10 % on every CPU part; GPUs and other kinds untouched.
	H = Default;
	TestTrue(TEXT("knob applies"), FRackThermalModel::ApplyKindHeatScale(P, H, ERackPartKind::Cpu, GraceW * 1.1));
	bool bShares = true;
	for (int32 N = 0; N < Nodes.Num(); ++N)
	{
		const double Expected = Nodes[N].Kind == ERackPartKind::Cpu ? Default[N] * 1.1 : Default[N];
		bShares &= FMath::Abs(H[N] - Expected) <= 1e-12 * FMath::Max(1.0, Expected);
	}
	TestTrue(TEXT("knob: every CPU part x1.1 of its own default, others unchanged (relative 1e-12)"), bShares);
	TArray<double> Empty;
	TestFalse(TEXT("knob refuses a mismatched array"), FRackThermalModel::ApplyKindHeatScale(P, Empty, ERackPartKind::Cpu, 300.0));

	// 5) The rack actor repairs a saved array when it conforms its inputs (level and layout-file loads go through it).
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>();
	Rack->Params = P;
	Rack->Input.ComponentHeatW = Default;
	for (const int32 N : OtherCpus) Rack->Input.ComponentHeatW[N] = GraceW;
	Rack->ConformInputs();
	TestTrue(TEXT("actor conform repairs the saved trace"), Rack->Input.ComponentHeatW == Default);
	World->DestroyWorld(false);
	return true;
}

#endif
