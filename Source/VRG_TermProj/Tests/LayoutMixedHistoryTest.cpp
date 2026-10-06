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
	const FString BaseFile = Prefix + TEXT("_base"), MidFile = Prefix + TEXT("_mid"); FString Summary;
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
	if (!TestTrue(TEXT("Save initial mixed inventory"), VrgLayoutFile::Save(F.World, BaseFile, Summary))) return false;
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
	for (int32 I = States.Num() - 2; I >= 0; --I) {
		if (!TestTrue(*FString::Printf(TEXT("Undo mixed command %d"), I + 1), F.Controller->TryUndoLayout())) return false;
		Check(F, States[I], FString::Printf(TEXT("undo%d"), I + 1));
		TestEqual(TEXT("Undo prefix count"), F.Controller->GetLayoutUndoCount(), I);
		TestEqual(TEXT("Redo suffix count"), F.Controller->GetLayoutRedoCount(), 22 - I);
	}
	TestFalse(TEXT("No command before original layout"), F.Controller->TryUndoLayout());
	for (int32 I = 1; I < States.Num(); ++I) {
		if (!TestTrue(*FString::Printf(TEXT("Redo mixed command %d"), I), F.Controller->TryRedoLayout())) return false;
		Check(F, States[I], FString::Printf(TEXT("redo%d"), I));
		TestEqual(TEXT("Redo prefix count"), F.Controller->GetLayoutUndoCount(), I);
		TestEqual(TEXT("Undo suffix count"), F.Controller->GetLayoutRedoCount(), 22 - I);
	}
	TestFalse(TEXT("No command beyond final layout"), F.Controller->TryRedoLayout());
	AddInfo(FString::Printf(TEXT("Mixed history22: max position %.3e cm, quaternion angle %.3e deg, scale %.3e dimensionless; every Undo/Redo prefix inventory/config/provenance/row/CDU/pipe/patch checked."), MaxPositionCm, MaxAngleDeg, MaxScale));

	// Persistence/physical follow-up is added below; never transfer live integrals through a layout record.
	for (const auto& File : {BaseFile, MidFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(File));
	return true;
}
#endif
