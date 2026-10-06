#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/RackThermalActor.h"
#include <limits>
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
struct FRoomFixture
{
	UWorld* World; ADataCenterSimulationActor* Sim; ADataCenterRoomActor* Room; AAirFieldDebugActor* Air;
	ARackThermalActor* Racks[2]; ACoolingDistributionUnitActor* Cdu;
	FRoomFixture()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
		Room = World->SpawnActor<ADataCenterRoomActor>(); Air = World->SpawnActor<AAirFieldDebugActor>();
		Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
		for (int32 I = 0; I < 2; ++I)
		{
			Racks[I] = World->SpawnActor<ARackThermalActor>(FVector(I == 0 ? -150 : 150, 0, 0), FRotator(0, I == 0 ? 0 : 180, 0));
			Racks[I]->ApplyGb200Nvl72Preset(); Racks[I]->bShowReadout = false;
		}
		Room->RebuildRoomNow(); Sim->EnterLayoutMode();
	}
	~FRoomFixture() { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
};
FRoomAirDomainSettings ManualRoom()
{
	// Synthetic geometry fixture, not a published facility/hardware dimension.
	FRoomAirDomainSettings S; S.bManual = true;
	S.MinXYM = FVector2D(-5.5, -4.5); S.MaxXYM = FVector2D(5.1, 4.6); S.CeilingHeightM = 3.1;
	return S;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomAirDomainFoundationTest, "VRG.Interaction.Layout.RoomAirDomainFoundation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomAirDomainFoundationTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason;
	TestFalse(TEXT("Unset room keeps automatic mode"), F.Sim->GetRoomAirDomain().bManual);
	F.Air->FitToRacks(); const FAirGridParams Auto = F.Air->GridParams; const FVector AutoOrigin = F.Air->GetActorLocation();
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); F.Sim->EnterLayoutMode();
	TestTrue(TEXT("Legacy auto counts/origin unchanged at simulation entry"), F.Air->GridParams.NumCellsX == Auto.NumCellsX
		&& F.Air->GridParams.NumCellsY == Auto.NumCellsY && F.Air->GridParams.NumCellsZ == Auto.NumCellsZ && F.Air->GetActorLocation() == AutoOrigin);
	const auto Manual = ManualRoom(); const auto Before = F.Sim->GetSnapshot();
	const int32 Pipes = F.Room->GetMutableNetwork().GetNumPipes();
	FAirGridParams Planned; FBox Actual(ForceInit);
	TestTrue(TEXT("Manual plan validates without applying it"), F.Sim->ValidateRoomAirDomain(Manual, Planned, Actual, Reason));
	TestTrue(TEXT("Counts are outward ceil, no span quantum or clipping"), Planned.NumCellsX == 43 && Planned.NumCellsY == 37 && Planned.NumCellsZ == 13
		&& Actual.Min == FVector(-5.5, -4.5, 0) && Actual.Max == FVector(5.25, 4.75, 3.25));
	FAirGridParams LimitGrid; FBox LimitBounds(ForceInit); auto Limit = Manual;
	Limit.MaxXYM.X = Limit.MinXYM.X + VrgRoomAirDomain::MaxFitCellsPerAxis * Auto.CellSizeM;
	TestTrue(TEXT("Exact resource boundary accepted"), VrgRoomAirDomain::BuildGrid(Limit, Auto, LimitGrid, LimitBounds, Reason) && LimitGrid.NumCellsX == 128);
	const auto KeptBounds = LimitBounds; Limit.MaxXYM.X += .01;
	TestFalse(TEXT("One partial cell beyond resource boundary rejected"), VrgRoomAirDomain::BuildGrid(Limit, Auto, LimitGrid, LimitBounds, Reason));
	TestTrue(TEXT("Rejected planner does not overwrite outputs"), LimitGrid.NumCellsX == 128 && LimitBounds.Min == KeptBounds.Min && LimitBounds.Max == KeptBounds.Max);
	TestTrue(TEXT("Setting acceptance leaves old grid/clock/routes untouched"), F.Sim->TrySetRoomAirDomain(Manual, Reason)
		&& F.Air->GetActorLocation() == AutoOrigin && F.Air->GridParams.NumCellsX == Auto.NumCellsX
		&& F.Sim->GetSnapshot().SimTimeS == Before.SimTimeS && F.Room->GetMutableNetwork().GetNumPipes() == Pipes);
	auto Reject = [&](const FRoomAirDomainSettings& Bad, const TCHAR* Message)
	{
		TestFalse(Message, F.Sim->TrySetRoomAirDomain(Bad, Reason)); TestTrue(TEXT("Reason provided"), !Reason.IsEmpty());
		const auto& Kept = F.Sim->GetRoomAirDomain();
		TestTrue(TEXT("Rejected setting preserves committed request and live grid/clock/rows"), Kept.bManual
			&& Kept.MinXYM == Manual.MinXYM && Kept.MaxXYM == Manual.MaxXYM && Kept.CeilingHeightM == Manual.CeilingHeightM
			&& F.Air->GetActorLocation() == AutoOrigin && F.Air->GridParams.NumCellsX == Auto.NumCellsX
			&& F.Sim->GetSnapshot().SimTimeS == Before.SimTimeS && F.Room->GetMutableNetwork().GetNumPipes() == Pipes);
	};
	auto Bad = Manual; Bad.MaxXYM.X = Bad.MinXYM.X; Reject(Bad, TEXT("Zero width rejected"));
	Bad = Manual; Bad.CeilingHeightM = -1; Reject(Bad, TEXT("Negative height rejected"));
	Bad = Manual; Bad.MaxXYM.Y = std::numeric_limits<double>::infinity(); Reject(Bad, TEXT("Infinity rejected"));
	Bad = Manual; Bad.MaxXYM.X = Bad.MinXYM.X + 32.01; Reject(Bad, TEXT("129-cell request rejected rather than truncated"));
	Bad = Manual; Bad.MinXYM.X = -4.1; Reject(Bad, TEXT("CDU footprint outside room rejected"));
	Bad = Manual; Bad.CeilingHeightM = 1; Reject(Bad, TEXT("Rack height outside room rejected"));
	// The automatic plan must be captured unfiltered even when the OLD live grid could not fit it.
	Bad = Manual; Bad.MaxXYM.Y = .5;
	Reject(Bad, TEXT("Automatic CRAH outside room rejected"));
	TestTrue(TEXT("Automatic CRAH is identified in reason"), Reason.Contains(TEXT("자동 CRAH")));
	const FTransform Original = F.Racks[0]->GetActorTransform();
	F.Racks[0]->SetActorTransform(FTransform(FRotator(0, 90, 0), FVector(-500, 0, 0), FVector(2, 3, 1)));
	Reject(Manual, TEXT("Rotated and scaled full box outside room rejected"));
	F.Racks[0]->SetActorTransform(FTransform(FRotator(15, 30, 20), FVector(-150, 0, 300), FVector::OneVector));
	Reject(Manual, TEXT("Elevated tilted box above ceiling rejected")); F.Racks[0]->SetActorTransform(Original);
	// Rack fits, but the front/rear sampling region does not; disable auto CRAH to isolate the cause.
	const auto Layout = F.Sim->CrahLayout; F.Sim->CrahLayout = ECrahLayout::CeilingCoil;
	Bad = Manual; Bad.MaxXYM.X = 2.11; Reject(Bad, TEXT("Required rack duct outside room rejected"));
	TestTrue(TEXT("Duct is identified in reason"), Reason.Contains(TEXT("덕트"))); F.Sim->CrahLayout = Layout;
	F.Air->GridParams.CellSizeM = 0.001; F.Sim->EnterSimulationMode(); F.Sim->QueueWarmupSteps(1);
	TestTrue(TEXT("Revalidation rejects stale request before simulation or warm-up mutates state"), F.Sim->GetSimMode() == ESimMode::Layout
		&& F.Sim->GetSnapshot().SimTimeS == Before.SimTimeS && F.Air->GetActorLocation() == AutoOrigin
		&& F.Room->GetMutableNetwork().GetNumPipes() == Pipes);
	F.Air->GridParams.CellSizeM = Auto.CellSizeM;
	F.Sim->EnterSimulationMode();
	F.Sim->Tick(.021f); // Duct diagnostics are populated by the first shared step.
	const auto ManualTemperatures = F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const int32 ManualDuctFaces = F.Air->GetMutableSolver().MakeSnapshot(0).NumDuctFaces;
	F.Air->DispatchBeginPlay();
	TestTrue(TEXT("Manual domain applied at entry, auto flag retained"), F.Air->bAutoFitToRacks
		&& F.Air->GetActorLocation() == FVector(-550, -450, 0) && F.Air->GetMutableSolver().GetGrid().GetNumCells() == 43 * 37 * 13);
	TestTrue(TEXT("Late air BeginPlay does not auto-fit/reset the integrated manual grid/ducts"), F.Air->GetMutableSolver().GetGrid().TemperatureK == ManualTemperatures
		&& ManualDuctFaces > 0 && F.Air->GetMutableSolver().MakeSnapshot(0).NumDuctFaces == ManualDuctFaces);
	TestFalse(TEXT("Cannot change room while simulating"), F.Sim->TrySetRoomAirDomain({}, Reason));
	F.Sim->Tick(.021f); F.Sim->EnterLayoutMode(); const auto Warm = F.Sim->GetSnapshot();
	const auto WarmTemps = F.Air->GetMutableSolver().GetGrid().TemperatureK;
	F.Racks[0]->SetActorLocation(FVector(600, 0, 0)); F.Sim->EnterSimulationMode();
	TestTrue(TEXT("Equipment change invalidates committed room; entry preserves warm state"), F.Sim->GetSimMode() == ESimMode::Layout
		&& F.Sim->GetSnapshot().SimTimeS == Warm.SimTimeS && F.Sim->GetSnapshot().EnergyInJ == Warm.EnergyInJ
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK == WarmTemps && F.Air->GetActorLocation() == FVector(-550, -450, 0));
	F.Racks[0]->SetActorTransform(Original);
	TestTrue(TEXT("Return to default automatic mode"), F.Sim->TrySetRoomAirDomain({}, Reason));
	F.Sim->EnterSimulationMode();
	TestTrue(TEXT("Automatic fit restored after manual mode"), F.Air->GetActorLocation() == AutoOrigin && F.Air->GridParams.NumCellsX == Auto.NumCellsX);
	AddInfo(TEXT("Manual request 10.6x9.1x3.1m; h0.25m; actual10.75x9.25x3.25m,43x37x13=20683 cells; no cropping; config/entry rejection preserves clock/grid/routes."));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingFoundationTest, "VRG.Interaction.Layout.RoomCeilingFoundation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingFoundationTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; const auto Room=ManualRoom();
	// Synthetic off-grid rectangle/height; these are test geometry, not facility/material parameters.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63;
	TestTrue(TEXT("Existing world has no ceiling plates"),F.Sim->GetRoomCeilingPlates().IsEmpty());
	TestFalse(TEXT("Plate requires explicit manual domain"),F.Sim->TrySetRoomCeilingPlates({P},Reason));
	TestTrue(TEXT("Manual domain accepted"),F.Sim->TrySetRoomAirDomain(Room,Reason));
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); F.Sim->EnterLayoutMode();
	const auto Warm=F.Sim->GetSnapshot(); const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Solids=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Origin=F.Air->GetActorLocation();
	const auto Pose=F.Racks[0]->GetActorTransform(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	TestTrue(TEXT("Value acceptance leaves warm grid/geometry/clock intact"),F.Sim->TrySetRoomCeilingPlates({P},Reason));
	auto Unchanged=[&]() { const auto S=F.Sim->GetSnapshot(); return F.Sim->GetRoomAirDomain()==Room
		&& F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P} && S.SimTimeS==Warm.SimTimeS
		&& S.EnergyInJ==Warm.EnergyInJ && S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ
		&& F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Air->GetMutableSolver().GetGrid().bSolidCell==Solids
		&& F.Air->GetActorLocation()==Origin && F.Racks[0]->GetActorTransform().Equals(Pose,0) && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	TestTrue(TEXT("No live mutation before next entry"),Unchanged());
	FAirGridParams GP; FBox Actual(ForceInit); TArray<FRoomCeilingPlan> Plans;
	TestTrue(TEXT("Reported plate plan"),F.Sim->ValidateRoomAirDomain(Room,GP,Actual,Reason)
		&& VrgRoomAirDomain::BuildCeilingPlans(Room,{P},GP,Actual,Plans,Reason));
	if (TestEqual(TEXT("One complete plan"),Plans.Num(),1))
	{
		TestTrue(TEXT("Containing single Z layer and conservative XY"),Plans[0].MinCell==FIntVector(1,30,10)
			&& Plans[0].MaxCellExclusive==FIntVector(4,33,11) && Plans[0].ActualBoundsM.Min==FVector(-5.25,3,2.5)
			&& Plans[0].ActualBoundsM.Max==FVector(-4.5,3.75,2.75));
	}
	auto Reject=[&](const FRoomCeilingPlate& Bad) { TestFalse(TEXT("Invalid candidate refused atomically"),F.Sim->TrySetRoomCeilingPlates({P,Bad},Reason));
		TestTrue(TEXT("Reason and all live values retained"),!Reason.IsEmpty() && Unchanged()); };
	auto Bad=P; Bad.MaxXYM.X=Bad.MinXYM.X; Reject(Bad);
	Bad=P; Bad.HeightM=std::numeric_limits<double>::infinity(); Reject(Bad);
	Bad=P; Bad.HeightM=0; Reject(Bad); Bad=P; Bad.HeightM=Room.CeilingHeightM; Reject(Bad);
	Bad=P; Bad.MinXYM.X=Room.MinXYM.X-.01; Reject(Bad);
	const auto Kept=Plans; Bad=P; Bad.HeightM=-1;
	TestFalse(TEXT("Planner failure retains complete previous output"),VrgRoomAirDomain::BuildCeilingPlans(Room,{P,Bad},GP,Actual,Plans,Reason));
	TestTrue(TEXT("No partial plan published"),Plans.Num()==Kept.Num() && Plans[0].ActualBoundsM==Kept[0].ActualBoundsM);
	Bad=P; Bad.MinXYM=FVector2D(-1.7,-.1); Bad.MaxXYM=FVector2D(-1.3,.1); Bad.HeightM=1;
	Reject(Bad); TestTrue(TEXT("Equipment identified"),Reason.Contains(TEXT("외형")));
	// A grid column outside the rack body but inside its existing front sample/duct envelope.
	const double Front=-1.5+.5*F.Racks[0]->RackSizeM.X;
	const double Edge=Room.MinXYM.X+FMath::CeilToDouble((Front-Room.MinXYM.X)/GP.CellSizeM)*GP.CellSizeM;
	Bad.MinXYM=FVector2D(Edge,-.1); Bad.MaxXYM=FVector2D(Edge+.01,.1);
	Reject(Bad); TestTrue(TEXT("Required rack duct identified"),Reason.Contains(TEXT("덕트")));
	TArray<FCrahLayoutRecord> Auto; F.Sim->CaptureAutomaticCrahLayout(Auto);
	if (TestTrue(TEXT("Automatic CRAHs present without conversion"),!Auto.IsEmpty()))
	{
		const auto Base=Auto[0].Transform.GetLocation()/100;
		Bad.MinXYM=FVector2D(Base.X-.01,Base.Y-.01); Bad.MaxXYM=FVector2D(Base.X+.01,Base.Y+.01); Bad.HeightM=.5*Auto[0].Settings.SizeM.Z;
		Reject(Bad); TestTrue(TEXT("Automatic CRAH identified"),Reason.Contains(TEXT("자동 CRAH")));
	}
	auto Small=Room; Small.MaxXYM.Y=3.5;
	TestFalse(TEXT("Room edits cannot clip existing ceiling"),F.Sim->TrySetRoomAirDomain(Small,Reason));
	TestFalse(TEXT("Automatic mode cannot silently drop ceiling"),F.Sim->TrySetRoomAirDomain({},Reason));
	TestTrue(TEXT("Room refusals unchanged"),Unchanged());
	// Equipment moving into a quantized plate invalidates entry before mode/grid/bridge mutation.
	F.Racks[0]->SetActorLocation(FVector(-465,340,100*(2.6-F.Racks[0]->RackSizeM.Z))); F.Sim->EnterSimulationMode();
	TestTrue(TEXT("Entry revalidates plate before changing clock/grid"),F.Sim->GetSimMode()==ESimMode::Layout
		&& F.Sim->GetRoomAirDomainMessage().Contains(TEXT("천장 판"))
		&& F.Sim->GetSnapshot().SimTimeS==Warm.SimTimeS && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps);
	F.Racks[0]->SetActorTransform(Pose);
	TestTrue(TEXT("Clearing list is value-only"),F.Sim->TrySetRoomCeilingPlates({},Reason) && F.Air->GetMutableSolver().GetGrid().bSolidCell==Solids);
	AddInfo(TEXT("Ceiling fixture request XY[-5.13,-4.61]x[3.13,3.62]m at2.63m -> XY[-5.25,-4.5]x[3,3.75]m, Z[2.5,2.75]m, nine cells in one0.25m layer; warm0.02s state unchanged. No holes/vents/history/persistence."));
	return true;
}
#endif
