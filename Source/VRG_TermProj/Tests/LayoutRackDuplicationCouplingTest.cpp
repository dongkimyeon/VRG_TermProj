#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutRackDuplicationCouplingTest, "VRG.Interaction.Layout.RackDuplicationCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutRackDuplicationCouplingTest::RunTest(const FString& Parameters)
{
	for (int32 Case = 0; Case < 2; ++Case)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("World"), World)) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		auto* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		auto* Controller = World->SpawnActor<ADataCenterDebugController>();
		auto* Room = World->SpawnActor<ADataCenterRoomActor>();
		Room->bUseGjConnections = true;
		auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
		auto* Air = World->SpawnActor<AAirFieldDebugActor>();
		auto* Source = World->SpawnActor<ARackThermalActor>();
		Source->ApplyGb200Nvl72Preset();
		Source->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Source->bUseGjModels = true; Source->bShowReadout = false;
		Sim->bShowSummary = false;
		if (Case == 1)
		{
			// Synthetic controls for this fixture only, not measured physical defaults.
			Source->Params.Slots.Pop(); Source->ConformInputs(); // Omit one management device.
			Source->Input.Utilization = .68;
			Source->Input.SlotUtilization.Init(.55, Source->Params.Slots.Num());
			Source->Input.SlotUtilization[1] = .8;
			Source->Input.FanSpeedOverride = .65;
			Source->Input.ComponentHeatW[0] *= 1.1;
			Source->Input.SledDocked[2] = false;
		}
		const auto Settings = Source->CaptureLayoutSettings();
		Room->RebuildRoomNow(); Sim->EnterSimulationMode();
		Sim->Tick(.021f); // float Tick must exceed double .02 s, as established by the addition test.
		const auto Before = Sim->GetSnapshot();
		TestTrue(TEXT("Baseline has one integrated rack"), Before.RackAirMassFlowKgs.Num() == 1 && Before.SimTimeS >= Sim->FixedStepS);
		Sim->EnterLayoutMode();
		const int32 PipesBefore = Room->GetMutableNetwork().GetNumPipes();
		const auto SourceState = Source->GetSnapshot();
		const double SourceEnergyJ = Source->GetMutableModel().GetEnergyInJ();
		const FTransform SourceTransform = Source->GetActorTransform();
		Controller->SelectLayoutAlongRay(FVector(0, 0, 500), FVector(0, 0, -1));
		TestTrue(TEXT("Start source duplication"), Controller->BeginRackDuplication());
		Controller->UpdateRackAdditionAlongRay(FVector(0, 85, 500), FVector(0, 0, -1));
		auto* Clone = Controller->ConfirmRackAddition();
		if (!TestNotNull(TEXT("Confirm one clone"), Clone))
		{
			GEngine->DestroyWorldContext(World); World->DestroyWorld(false); continue;
		}
		int32 Count = 0; for (TActorIterator<ARackThermalActor> It(World); It; ++It) ++Count;
		TestEqual(TEXT("One extra rack"), Count, 2);
		TestNull(TEXT("Repeated confirmation cannot add"), Controller->ConfirmRackAddition());
		TestTrue(TEXT("Original thermal state/transform unchanged before new simulation"), Source->GetSnapshot().ComponentTempK == SourceState.ComponentTempK
			&& Source->GetSnapshot().SimTimeS == SourceState.SimTimeS && Source->GetMutableModel().GetEnergyInJ() == SourceEnergyJ
			&& Source->GetActorTransform().Equals(SourceTransform, 0));
		TestTrue(TEXT("Unique name/model/input storage"), Source->GetFName() != Clone->GetFName()
			&& &Source->GetMutableModel() != &Clone->GetMutableModel()
			&& Source->Input.ComponentHeatW.GetData() != Clone->Input.ComponentHeatW.GetData());
		TestTrue(TEXT("Slot and control settings retained"), Clone->Params.Slots.Num() == Settings.Params.Slots.Num()
			&& Clone->Input.ComponentHeatW == Settings.Input.ComponentHeatW && Clone->Input.SledDocked == Settings.Input.SledDocked
			&& Clone->Input.Utilization == Settings.Input.Utilization && Clone->Input.SlotUtilization == Settings.Input.SlotUtilization
			&& Clone->Input.FanSpeedOverride == Settings.Input.FanSpeedOverride && Clone->bUseGjModels);
		const auto Routed = Room->GetRoutedRacks(); const auto Rows = Room->GetRoutedRackRows();
		const int32 SourceIndex = Routed.IndexOfByKey(Source), CloneIndex = Routed.IndexOfByKey(Clone);
		const auto& Branches = Room->GetRackBranchPipeIndices();
		const bool bBothRouted = SourceIndex != INDEX_NONE && CloneIndex != INDEX_NONE;
		TestTrue(TEXT("Same row with separate branches and CDU"), bBothRouted && Rows[SourceIndex] == Rows[CloneIndex]
			&& Branches[CloneIndex] != INDEX_NONE && Branches[CloneIndex] != Branches[SourceIndex]
			&& Room->GetRowCdu(Rows[CloneIndex]) == Cdu && Room->GetMutableNetwork().GetNumPipes() > PipesBefore);
		TestEqual(TEXT("Both GJ racks in patch display"), Room->GetPatchDevices().Num(), 2);
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		const auto After = Sim->GetSnapshot();
		const double DeltaItW = After.TotalItHeatW - Before.TotalItHeatW;
		FRackThermalModel Reference;
		Reference.Initialize(Settings.Params, FRackThermalState());
		Reference.Step(Settings.Input, Sim->FixedStepS);
		const double ExpectedW = Reference.GetLastTotalHeatW();
		const double RelativeError = FMath::Abs(DeltaItW - ExpectedW) / ExpectedW;
		TestTrue(TEXT("IT increase matches one independently initialized rack at same controls/initial step"), RelativeError < 1e-9);
		// Reuse the existing NVIDIA ~120 kW +/-5% full-load envelope (Gb200Nvl72Test).
		// Reduced-load fixture is bounded by zero and that same rated envelope, not by a new physical estimate.
		TestTrue(TEXT("IT plausibility separate from conservation"), Case == 0 ? (DeltaItW > 1.14e5 && DeltaItW < 1.26e5)
			: (DeltaItW > 0 && DeltaItW < 1.26e5 && DeltaItW < 1.14e5));
		for (int32 Frame = 0; Frame < 20; ++Frame) Sim->Tick(.04f);
		const auto Snapshot = Sim->GetSnapshot();
		const auto AirSnapshot = Air->GetMutableSolver().MakeSnapshot(Snapshot.SimTimeS);
		TestEqual(TEXT("Two coolant targets"), Snapshot.RackCoolantMassFlowKgs.Num(), 2);
		TestEqual(TEXT("Two thermal/air targets"), Snapshot.RackHeatToAirW.Num(), 2);
		TestEqual(TEXT("Two air/duct targets"), Snapshot.RackAirMassFlowKgs.Num(), 2);
		TestTrue(TEXT("No rejected ducts"), !Snapshot.bAirDuctsRejected && !AirSnapshot.bDuctsRejected);
		double MaxDuctError = 0;
		if (Snapshot.RackAirMassFlowKgs.Num() == 2 && Snapshot.RackDuctMassFlowKgs.Num() == 2)
		{
			for (int32 I = 0; I < 2; ++I)
			{
				TestTrue(TEXT("Positive fan flow"), Snapshot.RackAirMassFlowKgs[I] > 0);
				MaxDuctError = FMath::Max(MaxDuctError, FMath::Abs(Snapshot.RackAirMassFlowKgs[I] - Snapshot.RackDuctMassFlowKgs[I]) / Snapshot.RackAirMassFlowKgs[I]);
			}
		}
		else AddError(TEXT("Missing paired duct targets"));
		TestTrue(TEXT("Existing flow criterion"), MaxDuctError < 1e-9);
		TestTrue(TEXT("Existing global energy criterion"), FMath::Abs(Snapshot.NormalizedEnergyResidual) < 1e-9);
		TestTrue(TEXT("Existing pressure/divergence criterion"), AirSnapshot.bPressureConverged && AirSnapshot.NormalizedDivergence < 1e-5);
		AddInfo(FString::Printf(TEXT("Duplication %s: pipes %d -> %d | IT delta %.6f W expected %.6f W rel %.3e | t %.2f s global residual %.3e | duct rel %.3e | divergence %.3e"),
			Case == 0 ? TEXT("default") : TEXT("custom"), PipesBefore, Room->GetMutableNetwork().GetNumPipes(), DeltaItW, ExpectedW,
			RelativeError, Snapshot.SimTimeS, Snapshot.NormalizedEnergyResidual, MaxDuctError, AirSnapshot.NormalizedDivergence));
		GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	}
	return true;
}
#endif
