#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
enum class EMixedKind { Rack, Spine, Cdu, Crah };
struct FMixedRecord
{
	EMixedKind Kind;
	FString Id;
	FName Name;
	FTransform Transform;
	FRackLayoutSettings Rack;
	FSpineLayoutSettings Spine;
	FCduLayoutSettings Cdu;
	FCrahLayoutSettings Crah;
	bool bRuntime = false, bConverted = false;
	FTransform AutomaticTransform;
	FVector AutomaticSizeM = FVector::ZeroVector;
};
struct FMixedState
{
	TArray<FMixedRecord> Equipment;
	TArray<FString> Routes, Devices, Cables, Unconnected;
	int32 Pipes = 0, Pumps = 0, Rejected = 0;
};
struct FMixedFixture
{
	UWorld* World;
	ADataCenterSimulationActor* Sim;
	ADataCenterDebugController* Controller;
	ADataCenterRoomActor* Room;
	AAirFieldDebugActor* Air;
	FMixedFixture()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
		Controller = World->SpawnActor<ADataCenterDebugController>();
		Room = World->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections = true;
		// Remove only the test's real-time debounce wait; exercise the normal room Tick rebuild path.
		Room->LayoutSettleS = 0; Room->bShowSummary = false;
		Air = World->SpawnActor<AAirFieldDebugActor>();
		FActorSpawnParameters P; P.Name = TEXT("MixedPlacedRack");
		auto* Rack = World->SpawnActor<ARackThermalActor>(P);
		Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels = true; Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		// Synthetic authored workload/poses/configurations: not new measured or fitted hardware defaults.
		Rack->Input.Utilization = .67; Rack->Input.SlotUtilization.Init(.53, Rack->Params.Slots.Num());
		Rack->SetActorScale3D(FVector(1.05, .95, 1));
		P.Name = TEXT("MixedPlacedSpine");
		auto* Spine = World->SpawnActor<ASpineCabinetActor>(FVector(0, 200, 0), FRotator::ZeroRotator, P);
		Spine->NumSwitches = 2; Spine->FirstSwitchSlot = 5; Spine->RefreshVisuals();
		P.Name = TEXT("MixedPlacedCdu");
		auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 0, 0), FRotator::ZeroRotator, P);
		Cdu->PumpEfficiency = .65;
		P.Name = TEXT("MixedPlacedCrah");
		auto* Crah = World->SpawnActor<ACrahUnitActor>(FVector(300, 300, 0), FRotator::ZeroRotator, P);
		Crah->Kind = ECrahUnitKind::Perimeter; Crah->RefreshVisuals();
		Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	}
	~FMixedFixture() { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
	FMixedState Capture() const
	{
		FMixedState S;
		for (TActorIterator<ARackThermalActor> It(World); It; ++It) {
			FMixedRecord R; R.Kind = EMixedKind::Rack; R.Id = VrgRackLifecycle::Identity(*It);
			R.Name = It->GetFName(); R.Transform = It->GetActorTransform(); R.Rack = It->CaptureLayoutSettings();
			R.bRuntime = !R.Id.StartsWith(TEXT("placed/")); S.Equipment.Add(R);
		}
		for (TActorIterator<ASpineCabinetActor> It(World); It; ++It) {
			FMixedRecord R; R.Kind = EMixedKind::Spine; R.Id = VrgSpineLifecycle::Identity(*It);
			R.Name = It->GetFName(); R.Transform = It->GetActorTransform(); R.Spine = It->CaptureLayoutSettings();
			R.bRuntime = It->bRuntimeLayoutSpine; S.Equipment.Add(R);
		}
		for (TActorIterator<ACoolingDistributionUnitActor> It(World); It; ++It) {
			FMixedRecord R; R.Kind = EMixedKind::Cdu; R.Id = VrgCduLifecycle::Identity(*It);
			R.Name = It->GetFName(); R.Transform = It->GetActorTransform(); R.Cdu = It->CaptureLayoutSettings();
			R.bRuntime = It->bRuntimeLayoutCdu; S.Equipment.Add(R);
		}
		for (TActorIterator<ACrahUnitActor> It(World); It; ++It) {
			FMixedRecord R; R.Kind = EMixedKind::Crah; R.Id = VrgCrahLifecycle::Identity(*It);
			R.Name = It->GetFName(); R.Transform = It->GetActorTransform(); R.Crah = It->CaptureLayoutSettings();
			R.bRuntime = It->bRuntimeLayoutCrah; R.bConverted = It->bConvertedAutomaticCrah;
			R.AutomaticTransform = It->AutomaticLayoutTransform; R.AutomaticSizeM = It->AutomaticLayoutSizeM;
			S.Equipment.Add(R);
		}
		S.Equipment.Sort([](const auto& A, const auto& B) { return A.Id < B.Id; });
		const auto Racks = Room->GetRoutedRacks(); const auto Rows = Room->GetRoutedRackRows();
		for (int32 I = 0; I < Racks.Num(); ++I) {
			const auto* Cdu = Rows.IsValidIndex(I) ? Room->GetRowCdu(Rows[I]) : nullptr;
			S.Routes.Add(VrgRackLifecycle::Identity(Racks[I]) + TEXT("->") + (Cdu ? VrgCduLifecycle::Identity(Cdu) : TEXT("fallback")));
		}
		const auto& Devices = Room->GetPatchDevices(); const auto& Plan = Room->GetPatchPlan();
		for (const auto& D : Devices) S.Devices.Add(D.Label);
		for (const auto& Cable : Plan.Cables) {
			S.Cables.Add(FString::Printf(TEXT("%s:%d->%s:%d"), *Devices[Cable.FromDevice].Label, Cable.FromPort,
				*Devices[Cable.ToDevice].Label, Cable.ToPort));
		}
		for (const auto& U : Plan.Unconnected) S.Unconnected.Add(Devices[U.Device].Label + TEXT(":") + U.Reason);
		S.Routes.Sort(); S.Devices.Sort(); S.Cables.Sort(); S.Unconnected.Sort();
		S.Pipes = Room->GetMutableNetwork().GetNumPipes(); S.Pumps = Room->GetMutableNetwork().GetNumPumps();
		S.Rejected = Plan.RejectedClaims; return S;
	}
	AActor* Find(const FMixedRecord& R) const
	{
		switch (R.Kind) {
		case EMixedKind::Rack: return VrgRackLifecycle::Find(World, R.Id);
		case EMixedKind::Spine: return VrgSpineLifecycle::Find(World, R.Id);
		case EMixedKind::Cdu: return VrgCduLifecycle::Find(World, R.Id);
		default: return VrgCrahLifecycle::Find(World, R.Id);
		}
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutMixedHistoryTest, "VRG.Interaction.Layout.MixedHistoryPersistenceCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutMixedHistoryTest::RunTest(const FString& Parameters)
{
	FMixedFixture F;
	const FVector Down(0, 0, -1);
	const FString Prefix = TEXT("automation_mixed_history_") + FGuid::NewGuid().ToString();
	const FString BaseFile = Prefix + TEXT("_base"), MidFile = Prefix + TEXT("_mid"), FinalFile = Prefix + TEXT("_final"); FString Summary;
	TArray<FMixedState> States; States.Add(F.Capture());
	TMap<FString, FMixedRecord> Targets;
	for (const auto& R : States[0].Equipment) {
		const TCHAR* Key = R.Kind == EMixedKind::Rack ? TEXT("rack") : R.Kind == EMixedKind::Spine ? TEXT("spine")
			: R.Kind == EMixedKind::Cdu ? TEXT("cdu") : TEXT("crah"); Targets.Add(Key, R);
	}
	double MaxPositionCm = 0, MaxAngleDeg = 0, MaxScale = 0;
	auto Check = [&](FMixedFixture& X, const FMixedState& Expected, const FString& Stage) {
		const auto Current = X.Capture(); bool Equal = Current.Equipment.Num() == Expected.Equipment.Num();
		for (const auto& R : Expected.Equipment) {
			const auto* Got = Current.Equipment.FindByPredicate([&](const auto& A) { return A.Kind == R.Kind && A.Id == R.Id; });
			if (!Got) { Equal = false; continue; }
			const double Position = FVector::Distance(Got->Transform.GetLocation(), R.Transform.GetLocation());
			// atan2 of the relative quaternion avoids acos(dot(q,q)) cancellation near zero.
			const FQuat Delta = R.Transform.GetRotation().Inverse() * Got->Transform.GetRotation();
			const double Angle = FMath::RadiansToDegrees(2 * FMath::Atan2(FVector(Delta.X, Delta.Y, Delta.Z).Size(), FMath::Abs(Delta.W)));
			const double Scale = (Got->Transform.GetScale3D() - R.Transform.GetScale3D()).GetAbsMax();
			MaxPositionCm = FMath::Max(MaxPositionCm, Position); MaxAngleDeg = FMath::Max(MaxAngleDeg, Angle);
			MaxScale = FMath::Max(MaxScale, Scale);
			bool Settings = false;
			switch (R.Kind) {
			case EMixedKind::Rack: Settings = FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&Got->Rack, &R.Rack, 0); break;
			case EMixedKind::Spine: Settings = Got->Spine == R.Spine; break;
			case EMixedKind::Cdu: Settings = Got->Cdu == R.Cdu; break;
			case EMixedKind::Crah: Settings = Got->Crah == R.Crah; break;
			}
			Equal &= Settings && Got->Name == R.Name && Got->bRuntime == R.bRuntime && Got->bConverted == R.bConverted
				&& Position <= 1e-6 && Angle <= 1e-6 && Scale <= 1e-9
				&& (!R.bConverted || (Got->AutomaticTransform.Equals(R.AutomaticTransform, 1e-9) && Got->AutomaticSizeM == R.AutomaticSizeM));
		}
		TestTrue(*FString::Printf(TEXT("%s: all identities/settings/provenance/full transforms"), *Stage), Equal);
		const bool Connections = Current.Routes == Expected.Routes && Current.Devices == Expected.Devices && Current.Cables == Expected.Cables
			&& Current.Unconnected == Expected.Unconnected && Current.Rejected == Expected.Rejected
			&& Current.Pipes == Expected.Pipes && Current.Pumps == Expected.Pumps;
		if (!Connections) AddInfo(FString::Printf(TEXT("%s connection difference: routes %s/%s; cables %s/%s; unconnected %s/%s; devices%d/%d pipes%d/%d pumps%d/%d rejected%d/%d"),
			*Stage, *FString::Join(Current.Routes,TEXT(",")), *FString::Join(Expected.Routes,TEXT(",")),
			*FString::Join(Current.Cables,TEXT(",")), *FString::Join(Expected.Cables,TEXT(",")),
			*FString::Join(Current.Unconnected,TEXT(",")), *FString::Join(Expected.Unconnected,TEXT(",")),
			Current.Devices.Num(),Expected.Devices.Num(),Current.Pipes,Expected.Pipes,Current.Pumps,Expected.Pumps,Current.Rejected,Expected.Rejected));
		TestTrue(*FString::Printf(TEXT("%s: rebuilt row/CDU, branches and patch endpoints"), *Stage), Connections);
	};
	auto Select = [&](const FString& Key) {
		auto* Actor = F.Find(Targets.FindChecked(Key));
		if (!TestNotNull(*FString::Printf(TEXT("Live target %s"), *Key), Actor)) return false;
		F.Controller->SelectLayoutAlongRay(Actor->GetActorLocation() + FVector(0, 0, 500), Down);
		return TestTrue(TEXT("Selection is the intended live identity"), F.Controller->GetSelectedLayoutActor() == Actor);
	};
	auto Record = [&](const TCHAR* Stage) {
		F.Room->Tick(0); F.Room->Tick(0); // Observe changed hash, then rebuild once after settling.
		States.Add(F.Capture()); TestEqual(Stage, F.Controller->GetLayoutUndoCount(), States.Num() - 1);
		TestEqual(TEXT("Fresh edit has no Redo"), F.Controller->GetLayoutRedoCount(), 0);
	};
	auto Move = [&](const FString& Key) {
		if (!Select(Key)) return false;
		auto* Actor = F.Find(Targets.FindChecked(Key)); const FVector Eye = Actor->GetActorLocation() + FVector(0, 0, 500);
		const FTransform Before = Actor->GetActorTransform();
		F.Controller->BeginLayoutDragAlongRay(Eye, Down);
		F.Controller->UpdateLayoutDragAlongRay(Eye + FVector(0, 20, 0), Down, true);
		F.Controller->EndLayoutDrag(true);
		return TestTrue(TEXT("Committed move, height and scale retained"), Actor->GetActorLocation() != Before.GetLocation()
			&& Actor->GetActorLocation().Z == Before.GetLocation().Z && Actor->GetActorScale3D() == Before.GetScale3D());
	};
	auto Rotate = [&](const FString& Key) {
		if (!Select(Key)) return false;
		auto* Actor = F.Find(Targets.FindChecked(Key)); const auto Before = Actor->GetActorTransform();
		F.Controller->OnResetSim();
		return TestTrue(TEXT("Committed R, position and scale retained"), Actor->GetActorLocation() == Before.GetLocation()
			&& Actor->GetActorScale3D() == Before.GetScale3D() && !Actor->GetActorQuat().Equals(Before.GetRotation()));
	};
	auto Create = [&](EMixedKind Kind, const FString& Key, const FVector& Point, const FString& Source = FString()) {
		const bool Duplicate = !Source.IsEmpty(); bool Began = true;
		if (Duplicate && !Select(Source)) return false;
		FMixedRecord SourceRecord;
		if (Duplicate) {
			const auto Before = F.Capture();
			SourceRecord = *Before.Equipment.FindByPredicate([&](const auto& R) { return R.Id == Targets.FindChecked(Source).Id; });
		}
		switch (Kind) {
		case EMixedKind::Rack: if (Duplicate) Began = F.Controller->BeginRackDuplication(); else F.Controller->BeginRackAddition(); break;
		case EMixedKind::Spine: if (Duplicate) Began = F.Controller->BeginSpineDuplication(); else F.Controller->BeginSpineAddition(); break;
		case EMixedKind::Cdu: if (Duplicate) Began = F.Controller->BeginCduDuplication(); else F.Controller->BeginCduAddition(); break;
		case EMixedKind::Crah: if (Duplicate) Began = F.Controller->BeginCrahDuplication(); else F.Controller->BeginPerimeterCrahAddition(); break;
		}
		if (!TestTrue(TEXT("Value-only preview starts"), Began && F.Controller->IsAddingRack())) return false;
		F.Controller->UpdateRackAdditionAlongRay(Point + FVector(0, 0, 500), Down);
		AActor* Added = nullptr;
		switch (Kind) {
		case EMixedKind::Rack: Added = F.Controller->ConfirmRackAddition(); break;
		case EMixedKind::Spine: Added = F.Controller->ConfirmSpineAddition(); break;
		case EMixedKind::Cdu: Added = F.Controller->ConfirmCduAddition(); break;
		case EMixedKind::Crah: Added = F.Controller->ConfirmCrahAddition(); break;
		}
		if (!TestNotNull(TEXT("Exactly one addition/clone confirmed"), Added)) return false;
		const auto Now = F.Capture(); const auto* R = Now.Equipment.FindByPredicate([&](const auto& A) { return A.Name == Added->GetFName(); });
		if (!TestNotNull(TEXT("Created identity captured"), R)) return false;
		Targets.Add(Key, *R);
		if (Duplicate) {
			const auto& Original = Targets.FindChecked(Source);
			TestTrue(TEXT("Clone independent runtime ID, original still present"), R->Id != Original.Id && R->bRuntime && F.Find(Original));
			bool Same = false;
			switch (Kind) {
			case EMixedKind::Rack: Same = FRackLayoutSettings::StaticStruct()->CompareScriptStruct(&SourceRecord.Rack, &R->Rack, 0); break;
			case EMixedKind::Spine: Same = SourceRecord.Spine == R->Spine; break;
			case EMixedKind::Cdu: Same = SourceRecord.Cdu == R->Cdu; break;
			case EMixedKind::Crah: Same = SourceRecord.Crah == R->Crah && !R->bConverted; break;
			}
			TestTrue(TEXT("Clone keeps current source configuration/direction/scale/height"), Same
				&& R->Transform.GetRotation().Equals(SourceRecord.Transform.GetRotation(), 1e-9)
				&& R->Transform.GetScale3D() == SourceRecord.Transform.GetScale3D()
				&& R->Transform.GetLocation().Z == SourceRecord.Transform.GetLocation().Z);
		}
		return true;
	};
	auto Delete = [&](const FString& Key) {
		if (!Select(Key)) return false;
		switch (Targets.FindChecked(Key).Kind) {
		case EMixedKind::Rack: return F.Controller->TryDeleteLayoutRack();
		case EMixedKind::Spine: return F.Controller->TryDeleteLayoutSpine();
		case EMixedKind::Cdu: return F.Controller->TryDeleteLayoutCdu();
		default: return F.Controller->TryDeleteLayoutCrah();
		}
	};
	double MaxIt = 0, MaxSizing = 0, MaxDuct = 0, MaxCoolant = 0, MaxGlobal = 0, MaxHydraulic = 0, MaxDiv = 0;
	double MinInletK = DBL_MAX, MaxInletK = 0, MinCoilW = DBL_MAX, MaxCoilW = 0;
	auto Physics = [&](FMixedFixture& X, const FMixedState& Expected, const TCHAR* Stage) {
		// Restore only authored input controls, never a saved live temperature/integral/clock.
		int32 RackCount = 0, SpineCount = 0, CrahCount = 0;
		for (const auto& R : Expected.Equipment) {
			if (R.Kind == EMixedKind::Rack) { ++RackCount; CastChecked<ARackThermalActor>(X.Find(R))->Input = R.Rack.Input; }
			if (R.Kind == EMixedKind::Spine) ++SpineCount;
			if (R.Kind == EMixedKind::Crah) ++CrahCount;
		}
		X.Sim->EnterSimulationMode();
		// Independent fresh per-device thermal references use existing parameters and current authored controls.
		// CRAH fan and CDU pump are excluded from rack/spine IT and checked in their own categories below.
		double ReferenceItW = 0;
		for (TActorIterator<ARackThermalActor> It(X.World); It; ++It) {
			FRackThermalModel Reference; Reference.Initialize(It->Params, FRackThermalState());
			Reference.Step(It->Input, X.Sim->FixedStepS); ReferenceItW += Reference.GetLastTotalHeatW();
		}
		for (TActorIterator<ASpineCabinetActor> It(X.World); It; ++It) {
			It->EnsureThermalModel(); FRackThermalModel Reference;
			Reference.Initialize(It->GetMutableModel().GetParams(), FRackThermalState());
			Reference.Step(It->GetMutableInput(), X.Sim->FixedStepS); ReferenceItW += Reference.GetLastTotalHeatW();
		}
		X.Sim->Tick(.021f); const auto First = X.Sim->GetSnapshot();
		const double ItError = FMath::Abs(First.TotalItHeatW - ReferenceItW) / ReferenceItW;
		MaxIt = FMath::Max(MaxIt, ItError);
		TestTrue(TEXT("Live inventory IT equals fresh rack/spine references, no cooler/pump IT"), ReferenceItW > 0 && ItError < 1e-9);
		TestTrue(TEXT("Rebuild restarts clock and heat input includes fan exactly once"), First.SimTimeS == X.Sim->FixedStepS
			&& FMath::Abs(First.EnergyInJ - (First.TotalItHeatW + First.CrahFanPowerW) * First.SimTimeS) / First.EnergyInJ < 1e-12);
		for (int32 I = 0; I < 25; ++I) X.Sim->Tick(.04f);
		const auto S = X.Sim->GetSnapshot(); const auto AS = X.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
		const auto& GP = X.Air->GetMutableSolver().GetGrid().GetParams();
		const double DesignFlow = X.Sim->GetDesignAirLoadW() / (GP.DensityKgM3 * GP.SpecificHeatJKgK * X.Sim->CrahDesignAirDeltaTK);
		const double Eps = X.Sim->CrahDesignAirDeltaTK / (X.Sim->CrahDesignAirDeltaTK + X.Sim->CrahDesignApproachK);
		const double DesignUA = -FMath::Loge(1 - Eps) * GP.DensityKgM3 * GP.SpecificHeatJKgK * DesignFlow;
		const double FanW = X.Sim->CrahFanPressurePa * DesignFlow / X.Sim->CrahFanEfficiency;
		const double Sizing = FMath::Max3(FMath::Abs(S.CrahFlowM3s - DesignFlow) / DesignFlow,
			FMath::Abs(X.Sim->GetCrahTotalUAWK() - DesignUA) / DesignUA, FMath::Abs(S.CrahFanPowerW - FanW) / FanW);
		MaxSizing = FMath::Max(MaxSizing, Sizing);
		const auto Power = X.Sim->GetEvaluation().GetWindowPower();
		TestTrue(TEXT("All placed coolers active, shared design flow/UA/fan policy retained"), X.Sim->IsUsingPlacedCrahs()
			&& X.Sim->GetNumCrahUnits() == CrahCount && X.Sim->GetActiveCrahUnitCount() == CrahCount && Sizing < 1e-12
			&& FMath::Abs(Power.CrahFanW - FanW) / FanW < 1e-12);
		TestTrue(TEXT("No stale/deleted thermal or air targets"), S.RackHeatToAirW.Num() == RackCount + SpineCount
			&& S.RackAirMassFlowKgs.Num() == RackCount + SpineCount && S.RackDuctMassFlowKgs.Num() == RackCount + SpineCount);
		double TargetFlowM3s = 0, Duct = 0;
		for (int32 I = 0; I < S.RackAirMassFlowKgs.Num(); ++I) {
			const double Flow = S.RackAirMassFlowKgs[I]; TargetFlowM3s += Flow / GP.DensityKgM3;
			TestTrue(TEXT("Live rack/spine air flow positive"), Flow > 0);
			if (Flow > 0 && S.RackDuctMassFlowKgs.IsValidIndex(I)) Duct = FMath::Max(Duct, FMath::Abs(Flow - S.RackDuctMassFlowKgs[I]) / Flow);
		}
		const double ExpectedDuct = TargetFlowM3s + DesignFlow;
		Duct = FMath::Max(Duct, FMath::Abs(AS.DuctFlowM3s - ExpectedDuct) / ExpectedDuct); MaxDuct = FMath::Max(MaxDuct, Duct);
		TestTrue(TEXT("Live air targets and coolers share actual ducts"), !S.bAirDuctsRejected && !AS.bDuctsRejected && Duct < 1e-9);
		TestTrue(TEXT("Positive coil cooling, one air sink ledger"), S.CrahCoilRemovedW > 0
			&& FMath::Abs(S.TotalAirCoolingRemovedW - S.CrahCoilRemovedW) / S.CrahCoilRemovedW < 1e-9);
		const auto Routed = X.Room->GetRoutedRacks(); const auto Rows = X.Room->GetRoutedRackRows();
		TestEqual(TEXT("All and only live racks have branches"), Routed.Num(), RackCount);
		TestEqual(TEXT("Branch index count matches live racks"), X.Room->GetRackBranchPipeIndices().Num(), RackCount);
		TArray<double> HeatCapacityFlow; HeatCapacityFlow.Init(0, S.CduDutyW.Num());
		double RackRemovedW = 0;
		for (int32 I = 0; I < Routed.Num(); ++I) {
			auto* Rack = Routed[I]; const int32 Row = Rows[I];
			TestTrue(TEXT("Every live rack receives positive coolant mass flow"), Rack->Input.CoolantMassFlowKgs > 0);
			RackRemovedW += Rack->GetMutableModel().GetLastHeatRemovedByCoolantW();
			if (!TestTrue(TEXT("Live row/CDU heat group exists"), HeatCapacityFlow.IsValidIndex(Row) && X.Room->GetRowCdu(Row))) continue;
			double BoundaryFlowM3s = Rack->Input.CoolantMassFlowKgs / X.Room->GetNetwork().GetFluidDensityKgM3();
			if (const auto* Internal = X.Room->GetInRackHandles(Rack)) {
				// The thermal boundary is the sum of actual internal supply edges. The external
				// pipe differs by Newton's separately tested mass residual, not an energy error.
				BoundaryFlowM3s = 0;
				const auto& Net = X.Room->GetNetwork();
				for (int32 P : Internal->Pipes) {
					if (Net.GetPipeFromNodeIndex(P) == Internal->SupplyNode) BoundaryFlowM3s += Net.GetPipeFlowM3s(P);
					if (Net.GetPipeToNodeIndex(P) == Internal->SupplyNode) BoundaryFlowM3s -= Net.GetPipeFlowM3s(P);
				}
			}
			HeatCapacityFlow[Row] += BoundaryFlowM3s * X.Room->GetNetwork().GetFluidDensityKgM3() * Rack->Params.CoolantCpJKgK;
			// Reuse the existing GB200 15..35C transient inlet safety net, not a new hardware range.
			for (double K : Rack->Input.SlotAirInletTempK) {
				MinInletK = FMath::Min(MinInletK, K); MaxInletK = FMath::Max(MaxInletK, K);
				TestTrue(TEXT("Existing inlet plausibility"), FMath::IsFinite(K) && K > 288.15 && K < 308.15);
			}
		}
		double Coolant = FMath::Abs(RackRemovedW - S.TotalRadiatorRejectedW) / S.TotalRadiatorRejectedW;
		for (int32 Row = 0; Row < S.CduDutyW.Num(); ++Row) {
			const double Duty = HeatCapacityFlow[Row] * (S.CduReturnTempK[Row] - S.CduSupplyTempK[Row]);
			Coolant = FMath::Max(Coolant, FMath::Abs(Duty - S.CduDutyW[Row]) / S.CduDutyW[Row]);
			const auto* Cdu = X.Room->GetRowCdu(Row);
			TestTrue(TEXT("Existing coolant temperature and duty safety net"), Cdu && S.CduDutyW[Row] > 0
				&& S.CduReturnTempK[Row] > S.CduSupplyTempK[Row] && S.CduSupplyTempK[Row] >= Cdu->FacilitySupplyTempK);
		}
		MaxCoolant = FMath::Max(MaxCoolant, Coolant);
		const double PumpW = X.Room->GetTotalPumpElectricalW();
		const auto& HS = X.Room->GetLastSolveResult();
		TestTrue(TEXT("Existing coolant closure, pump power scale and electrical category"), Coolant < 1e-12
			&& PumpW > 0 && PumpW / S.TotalItHeatW < .05 && FMath::Abs(Power.PumpW - PumpW) / PumpW < 1e-12);
		TestTrue(TEXT("Existing Newton and air projection targets"), HS.bConverged && !HS.bIllPosed && !HS.bLinearSolveFailed
			&& HS.ReferenceFlowM3s > 0 && HS.ReferencePressurePa > 0 && HS.NormalizedResidual < X.Room->SolveSettings.NormalizedResidualTarget
			&& S.bHydraulicsConverged && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence < 1e-5);
		TestTrue(TEXT("Existing global conservation criterion"), FMath::Abs(S.NormalizedEnergyResidual) < 1e-9);
		MaxHydraulic = FMath::Max(MaxHydraulic, HS.NormalizedResidual); MaxDiv = FMath::Max(MaxDiv, AS.NormalizedDivergence);
		MaxGlobal = FMath::Max(MaxGlobal, FMath::Abs(S.NormalizedEnergyResidual));
		MinCoilW = FMath::Min(MinCoilW, S.CrahCoilRemovedW); MaxCoilW = FMath::Max(MaxCoilW, S.CrahCoilRemovedW);
		AddInfo(FString::Printf(TEXT("Mixed physics %s: rack%d spine%d crah%d IT %.6f expected%.6fW err%.3e sizing%.3e duct%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e flow%.6fm3/s fan%.6fW pump%.6fW t%.2fs"),
			Stage, RackCount, SpineCount, CrahCount, First.TotalItHeatW, ReferenceItW, ItError, Sizing, Duct, Coolant,
			S.NormalizedEnergyResidual, HS.NormalizedResidual, AS.NormalizedDivergence, DesignFlow, FanW, PumpW, S.SimTimeS));
		X.Sim->EnterLayoutMode();
	};
	if (!TestTrue(TEXT("Save initial mixed inventory"), VrgLayoutFile::Save(F.World, BaseFile, Summary))) return false;
	Physics(F, States[0], TEXT("original"));
	// Fixed interleaving is a reproducible command trace, not random/fuzz coverage. Positions are UE cm.
	if (!Move(TEXT("rack"))) return false; Record(TEXT("1 rack move"));
	if (!Rotate(TEXT("spine"))) return false; Record(TEXT("2 spine R"));
	if (!Create(EMixedKind::Cdu, TEXT("new-cdu"), FVector(-300, -250, 0))) return false; Record(TEXT("3 CDU add"));
	if (!Move(TEXT("crah"))) return false; Record(TEXT("4 CRAH move"));
	if (!Create(EMixedKind::Rack, TEXT("clone-rack"), FVector(0, -700, 0), TEXT("rack"))) return false; Record(TEXT("5 rack clone"));
	if (!Rotate(TEXT("cdu"))) return false; Record(TEXT("6 CDU R"));
	if (!Create(EMixedKind::Spine, TEXT("new-spine"), FVector(0, -450, 0))) return false; Record(TEXT("7 spine add"));
	if (!Move(TEXT("spine"))) return false; Record(TEXT("8 spine move"));
	if (!Create(EMixedKind::Rack, TEXT("new-rack"), FVector(0, -250, 0))) return false; Record(TEXT("9 rack add"));
	if (!Rotate(TEXT("crah"))) return false; Record(TEXT("10 CRAH R"));
	if (!Create(EMixedKind::Cdu, TEXT("clone-cdu"), FVector(-300, -500, 0), TEXT("cdu"))) return false; Record(TEXT("11 CDU clone"));
	if (!Move(TEXT("clone-rack"))) return false; Record(TEXT("12 runtime rack move"));
	if (!Create(EMixedKind::Crah, TEXT("new-crah"), FVector(300, -200, 0))) return false; Record(TEXT("13 perimeter add"));
	if (!Rotate(TEXT("rack"))) return false; Record(TEXT("14 rack R"));
	if (!Create(EMixedKind::Spine, TEXT("clone-spine"), FVector(0, -900, 0), TEXT("spine"))) return false; Record(TEXT("15 spine clone"));
	if (!Create(EMixedKind::Crah, TEXT("clone-crah"), FVector(300, -450, 0), TEXT("crah"))) return false; Record(TEXT("16 CRAH clone"));
	if (!Delete(TEXT("rack"))) return false; Record(TEXT("17 placed rack delete"));
	if (!Delete(TEXT("spine"))) return false; Record(TEXT("18 placed spine delete"));
	const int32 MidIndex = States.Num() - 1;
	if (!TestTrue(TEXT("Save mid-trace with additions, clones and authored deletions"), VrgLayoutFile::Save(F.World, MidFile, Summary))) return false;
	if (!Delete(TEXT("cdu"))) return false; Record(TEXT("19 placed CDU delete"));
	if (!Delete(TEXT("crah"))) return false; Record(TEXT("20 placed CRAH delete"));
	if (!Move(TEXT("new-spine"))) return false; Record(TEXT("21 runtime spine move"));
	if (!Rotate(TEXT("clone-cdu"))) return false; Record(TEXT("22 runtime CDU R"));
	TestTrue(TEXT("Save final trace includes all four authored deletion intents"), VrgLayoutFile::Save(F.World, FinalFile, Summary));

	// All public kind-specific entry points must reject a mixed selection without changing either stack.
	Select(TEXT("new-rack"));
	for (const auto& Key : {TEXT("new-spine"), TEXT("new-cdu"), TEXT("new-crah")}) {
		auto* A = F.Find(Targets.FindChecked(Key));
		F.Controller->SelectLayoutAlongRay(A->GetActorLocation() + FVector(0, 0, 500), Down, true);
	}
	TestEqual(TEXT("Four-kind mixed selection"), F.Controller->GetSelectedLayoutActors().Num(), 4);
	TestFalse(TEXT("Mixed rack delete"), F.Controller->TryDeleteLayoutRack());
	TestFalse(TEXT("Mixed spine delete"), F.Controller->TryDeleteLayoutSpine());
	TestFalse(TEXT("Mixed CDU delete"), F.Controller->TryDeleteLayoutCdu());
	TestFalse(TEXT("Mixed CRAH delete"), F.Controller->TryDeleteLayoutCrah());
	TestFalse(TEXT("Mixed rack clone"), F.Controller->BeginRackDuplication());
	TestFalse(TEXT("Mixed spine clone"), F.Controller->BeginSpineDuplication());
	TestFalse(TEXT("Mixed CDU clone"), F.Controller->BeginCduDuplication());
	TestFalse(TEXT("Mixed CRAH clone"), F.Controller->BeginCrahDuplication());
	TestTrue(TEXT("Mixed rejection preserves history and preview state"), F.Controller->GetLayoutUndoCount() == 22
		&& F.Controller->GetLayoutRedoCount() == 0 && !F.Controller->IsAddingRack());
	Check(F, States.Last(), TEXT("mixed rejection"));
	Physics(F, States.Last(), TEXT("final"));
	TestEqual(TEXT("Mode switch preserves all mixed commands"), F.Controller->GetLayoutUndoCount(), 22);
	for (int32 I = States.Num() - 2; I >= 0; --I) {
		if (!TestTrue(*FString::Printf(TEXT("Undo mixed command %d"), I + 1), F.Controller->TryUndoLayout())) return false;
		Check(F, States[I], FString::Printf(TEXT("undo%d"), I + 1));
		TestEqual(TEXT("Undo prefix count"), F.Controller->GetLayoutUndoCount(), I);
		TestEqual(TEXT("Redo suffix count"), F.Controller->GetLayoutRedoCount(), 22 - I);
	}
	TestFalse(TEXT("No command before original layout"), F.Controller->TryUndoLayout());
	Physics(F, States[0], TEXT("all-undo"));
	for (int32 I = 1; I < States.Num(); ++I) {
		if (!TestTrue(*FString::Printf(TEXT("Redo mixed command %d"), I), F.Controller->TryRedoLayout())) return false;
		Check(F, States[I], FString::Printf(TEXT("redo%d"), I));
		TestEqual(TEXT("Redo prefix count"), F.Controller->GetLayoutUndoCount(), I);
		TestEqual(TEXT("Undo suffix count"), F.Controller->GetLayoutRedoCount(), 22 - I);
	}
	TestFalse(TEXT("No command beyond final layout"), F.Controller->TryRedoLayout());
	Physics(F, States.Last(), TEXT("all-redo"));
	AddInfo(FString::Printf(TEXT("Mixed history22: max position %.3e cm, quaternion angle %.3e deg, scale %.3e dimensionless; every Undo/Redo prefix inventory/config/provenance/row/CDU/pipe/patch checked."), MaxPositionCm, MaxAngleDeg, MaxScale));

	// Load is a history boundary. Both stacks must be populated before proving that it clears them.
	TestTrue(TEXT("Undo one final command before Load"), F.Controller->TryUndoLayout());
	TestTrue(TEXT("Both mixed stacks populated before Load"), F.Controller->GetLayoutUndoCount() == 21 && F.Controller->GetLayoutRedoCount() == 1);
	TestTrue(TEXT("Mid-trace Load and repeat"), VrgLayoutFile::Load(F.World, MidFile, Summary) && VrgLayoutFile::Load(F.World, MidFile, Summary));
	Check(F, States[MidIndex], TEXT("mid-load-twice"));
	TestTrue(TEXT("Load clears both stacks and prevents stale Undo/Redo"), F.Controller->GetLayoutUndoCount() == 0
		&& F.Controller->GetLayoutRedoCount() == 0 && !F.Controller->TryUndoLayout() && !F.Controller->TryRedoLayout());
	Physics(F, States[MidIndex], TEXT("mid-load"));
	{
		FMixedFixture Fresh;
		TestTrue(TEXT("Fresh fixture has same authored base"), Fresh.Capture().Equipment.Num() == States[0].Equipment.Num());
		auto* A = Fresh.Find(Targets.FindChecked(TEXT("cdu")));
		Fresh.Controller->SelectLayoutAlongRay(A->GetActorLocation() + FVector(0, 0, 500), Down); Fresh.Controller->OnResetSim();
		TestEqual(TEXT("Fresh world has prior history"), Fresh.Controller->GetLayoutUndoCount(), 1);
		TestTrue(TEXT("Fresh mid-trace Load and repeat"), VrgLayoutFile::Load(Fresh.World, MidFile, Summary) && VrgLayoutFile::Load(Fresh.World, MidFile, Summary));
		Check(Fresh, States[MidIndex], TEXT("fresh-mid-load-twice"));
		TestTrue(TEXT("Fresh Load deletes authored rack/spine and clears prior history/clock"), !Fresh.Find(Targets.FindChecked(TEXT("rack")))
			&& !Fresh.Find(Targets.FindChecked(TEXT("spine"))) && Fresh.Controller->GetLayoutUndoCount() == 0
			&& Fresh.Controller->GetLayoutRedoCount() == 0 && Fresh.Sim->GetSnapshot().SimTimeS == 0);
		Physics(Fresh, States[MidIndex], TEXT("fresh-mid-load"));
		const double PausedTimeS = Fresh.Sim->GetSnapshot().SimTimeS;
		TestTrue(TEXT("Fresh final Load/repeat includes all four deletion intents"), VrgLayoutFile::Load(Fresh.World, FinalFile, Summary)
			&& VrgLayoutFile::Load(Fresh.World, FinalFile, Summary));
		Check(Fresh, States.Last(), TEXT("fresh-final-load-twice"));
		for (const auto& Key : {TEXT("rack"), TEXT("spine"), TEXT("cdu"), TEXT("crah")})
			TestNull(TEXT("Final file removes each authored source without touching clones"), Fresh.Find(Targets.FindChecked(Key)));
		// This world has now run: Load clears edit history but the paused clock remains until the next simulation entry.
		TestTrue(TEXT("Warm-world final Load clears history without advancing paused clock"), Fresh.Controller->GetLayoutUndoCount() == 0
			&& Fresh.Controller->GetLayoutRedoCount() == 0 && Fresh.Sim->GetSnapshot().SimTimeS == PausedTimeS);
		AddInfo(FString::Printf(TEXT("Mixed warm final Load: paused clock %.2f -> %.2fs, undo%d redo%d; next run checked from t=0."),
			PausedTimeS, Fresh.Sim->GetSnapshot().SimTimeS, Fresh.Controller->GetLayoutUndoCount(), Fresh.Controller->GetLayoutRedoCount()));
		Physics(Fresh, States.Last(), TEXT("fresh-final-load"));
	}
	TestTrue(TEXT("Original file restores base and repeat"), VrgLayoutFile::Load(F.World, BaseFile, Summary) && VrgLayoutFile::Load(F.World, BaseFile, Summary));
	Check(F, States[0], TEXT("base-load-twice"));
	TestTrue(TEXT("Base Load leaves no stale mixed history"), F.Controller->GetLayoutUndoCount() == 0 && F.Controller->GetLayoutRedoCount() == 0);
	AddInfo(FString::Printf(TEXT("Mixed maxima: IT%.3e sizing%.3e duct%.3e coolant%.3e global%.3e hydraulic%.3e div%.3e inlet%.6f..%.6fK coil%.6f..%.6fW; pose%.3ecm angle%.3edeg scale%.3e; fixed22 commands, repeated/current/fresh Load, not steady state/FPS."),
		MaxIt, MaxSizing, MaxDuct, MaxCoolant, MaxGlobal, MaxHydraulic, MaxDiv, MinInletK, MaxInletK, MinCoilW, MaxCoilW,
		MaxPositionCm, MaxAngleDeg, MaxScale));
	for (const auto& File : {BaseFile, MidFile, FinalFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	return true;
}
#endif
