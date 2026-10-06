#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "Interaction/VrgLayoutFile.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/RoomSettingsWidget.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingCouplingTest, "VRG.Interaction.Layout.RoomCeilingCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingCouplingTest::RunTest(const FString& Parameters)
{
	double MaxEnergy=0,MaxPressure=0,MaxWater=0,MaxDuct=0,MaxHydraulic=0,MaxIt=0,MaxSizing=0;
	double MinInlet=TNumericLimits<double>::Max(),MaxInlet=0;
	for(int32 Mode=0;Mode<2;++Mode)
	{
		FRoomFixture F; FString Reason; F.Sim->CrahLayout=Mode==0?ECrahLayout::InRow:ECrahLayout::Perimeter;
		F.Sim->Containment=EAisleContainment::BlankingAndColdAisleRoof;
		auto Room=ManualRoom(); Room.CeilingHeightM=4; // Synthetic plate clearance fixture, not facility data.
		TestTrue(TEXT("Manual baseline accepted"),F.Sim->TrySetRoomAirDomain(Room,Reason));
		auto* Wall=F.World->SpawnActor<ALayoutBarrierActor>(FVector(-400,320,0),FRotator::ZeroRotator);
		Wall->SetActorScale3D(FVector(.2,1,3.5)); // Synthetic wall intersects only the remote plate, not a duct.
		const auto InitialInput=F.Racks[0]->Input;
		F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const auto Reference=F.Sim->GetSnapshot(); F.Sim->EnterLayoutMode();
		const auto Baseline=F.Air->GetMutableSolver().GetGrid().bSolidCell;
		const int32 RoofCells=F.Sim->GetNumContainmentCells(),WallCells=F.Sim->GetNumBarrierCells();
		TestTrue(TEXT("Existing roof and wall represented before ceiling"),RoofCells>0 && WallCells>0);
		FRoomCeilingPlate P; P.MinXYM=FVector2D(-2.6,-2); P.MaxXYM=FVector2D(2.6,2); P.HeightM=2.83;
		FRoomCeilingPlate Remote; Remote.MinXYM=FVector2D(-4.3,3); Remote.MaxXYM=FVector2D(-3.7,3.4); Remote.HeightM=2.83;
		TestTrue(TEXT("Plate and wall overlap allowed; no immediate solid changes"),F.Sim->TrySetRoomCeilingPlates({P,Remote,P},Reason)
			&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Baseline && F.Sim->GetNumCeilingCells()==0);
		FAirGridParams GP; FBox Actual(ForceInit); TArray<FRoomCeilingPlan> Plans;
		TestTrue(TEXT("Plan accepted for all existing paths"),F.Sim->ValidateRoomAirDomain(Room,GP,Actual,Reason)
			&& VrgRoomAirDomain::BuildCeilingPlans(Room,F.Sim->GetRoomCeilingPlates(),GP,Actual,Plans,Reason));
		if (Plans.Num()!=3) { AddError(Reason); return false; }
		// Existing Load must include saved plate data in FINAL equipment validation.
		const auto Pose=F.Racks[0]->GetActorTransform(); F.Racks[0]->SetActorLocation(Pose.GetLocation()+FVector(0,0,100));
		TestTrue(TEXT("Save collision fixture"),VrgLayoutFile::Save(F.World,TEXT("AutomationCeilingBlockedLoad"),Reason));
		F.Racks[0]->SetActorTransform(Pose);
		TestFalse(TEXT("Load cannot bypass ceiling using a final elevated rack"),VrgLayoutFile::Load(F.World,TEXT("AutomationCeilingBlockedLoad"),Reason));
		TestTrue(TEXT("Load rejection has reason and retains geometry/grid/clock"),Reason.Contains(TEXT("천장 판"))
			&& F.Racks[0]->GetActorTransform().Equals(Pose,0) && F.Sim->GetSnapshot().SimTimeS==Reference.SimTimeS
			&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Baseline && F.Sim->GetRoomCeilingPlates().Num()==3);
		auto Run=[&](bool bPlaced)
		{
			for(auto* Rack:F.Racks) Rack->Input=InitialInput;
			F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const auto First=F.Sim->GetSnapshot();
			const auto& G=F.Air->GetMutableSolver().GetGrid(); TArray<bool> Expected=Baseline; int32 Added=0;
			for(const auto& Plan:Plans) for(int32 Z=Plan.MinCell.Z;Z<Plan.MaxCellExclusive.Z;++Z)
				for(int32 Y=Plan.MinCell.Y;Y<Plan.MaxCellExclusive.Y;++Y) for(int32 X=Plan.MinCell.X;X<Plan.MaxCellExclusive.X;++X)
				{ const int32 I=G.CellIndex(X,Y,Z); if(!Expected[I]) { Expected[I]=true; ++Added; } }
			TestTrue(TEXT("Exact mask union; overlaps once; equipment/wall/roof retained"),G.bSolidCell==Expected
				&& F.Sim->GetNumCeilingCells()==Added && F.Sim->GetNumContainmentCells()==RoofCells && F.Sim->GetNumBarrierCells()==WallCells);
			const auto& PP=Plans[0]; TestTrue(TEXT("Upper air is left fluid"),!G.IsSolidCell(PP.MinCell.X,PP.MinCell.Y,PP.MaxCellExclusive.Z));
			TestTrue(TEXT("CRAH conversion keeps count and geometry"),F.Sim->IsUsingPlacedCrahs()==bPlaced && F.Sim->GetNumCrahUnits()==(Mode==0?4:2));
			TestFalse(TEXT("Cannot change ceiling while simulating"),F.Sim->TrySetRoomCeilingPlates({},Reason));
			for(int32 I=0;I<25;++I) F.Sim->Tick(.04f);
			const auto S=F.Sim->GetSnapshot(); const auto AS=F.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
			const double It=FMath::Abs(First.TotalItHeatW-Reference.TotalItHeatW)/Reference.TotalItHeatW;
			const double Sizing=FMath::Max3(FMath::Abs(S.CrahFlowM3s-Reference.CrahFlowM3s)/Reference.CrahFlowM3s,
				FMath::Abs(F.Sim->GetCrahTotalUAWK()-Reference.CrahFlowM3s*G.GetParams().DensityKgM3*G.GetParams().SpecificHeatJKgK
				*(-FMath::Loge(1-F.Sim->CrahDesignAirDeltaTK/(F.Sim->CrahDesignAirDeltaTK+F.Sim->CrahDesignApproachK))))/F.Sim->GetCrahTotalUAWK(),
				FMath::Abs(S.CrahFanPowerW-Reference.CrahFanPowerW)/Reference.CrahFanPowerW);
			double RackFlow=0,Duct=0; for(int32 I=0;I<S.RackAirMassFlowKgs.Num();++I)
			{ if(!TestTrue(TEXT("Rack has a matching positive duct coupling"),S.RackAirMassFlowKgs[I]>0 && S.RackDuctMassFlowKgs.IsValidIndex(I))) continue;
				RackFlow+=S.RackAirMassFlowKgs[I]/G.GetParams().DensityKgM3;
				Duct=FMath::Max(Duct,FMath::Abs(S.RackAirMassFlowKgs[I]-S.RackDuctMassFlowKgs[I])/S.RackAirMassFlowKgs[I]); }
			Duct=FMath::Max(Duct,FMath::Abs(AS.DuctFlowM3s-RackFlow-S.CrahFlowM3s)/(RackFlow+S.CrahFlowM3s));
			double Removed=0; for(auto* Rack:F.Racks) { Removed+=Rack->GetMutableModel().GetLastHeatRemovedByCoolantW();
				TestTrue(TEXT("Coolant flow positive"),Rack->Input.CoolantMassFlowKgs>0);
				for(double K:Rack->Input.SlotAirInletTempK) { MinInlet=FMath::Min(MinInlet,K); MaxInlet=FMath::Max(MaxInlet,K);
					TestTrue(TEXT("Existing15..35C transient inlet safety"),K>288.15 && K<308.15); } }
			const double Water=FMath::Abs(Removed-S.TotalRadiatorRejectedW)/S.TotalRadiatorRejectedW;
			const auto HS=F.Room->GetLastSolveResult();
			TestTrue(TEXT("Existing IT/sizing/duct/coolant/coil/energy criteria"),It<1e-9 && Sizing<1e-12 && Duct<1e-9 && Water<1e-12
				&& !AS.bDuctsRejected && AS.NumDuctFaces>0 && S.CrahCoilRemovedW>0 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9);
			TestTrue(TEXT("Pressure/hydraulic convergence and pump scale"),S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5
				&& HS.bConverged && S.bHydraulicsConverged && HS.NormalizedResidual<F.Room->SolveSettings.NormalizedResidualTarget
				&& F.Room->GetTotalPumpElectricalW()>0 && F.Room->GetTotalPumpElectricalW()/S.TotalItHeatW<.05);
			MaxIt=FMath::Max(MaxIt,It); MaxSizing=FMath::Max(MaxSizing,Sizing); MaxDuct=FMath::Max(MaxDuct,Duct); MaxWater=FMath::Max(MaxWater,Water);
			MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxPressure=FMath::Max(MaxPressure,AS.NormalizedDivergence);
			MaxHydraulic=FMath::Max(MaxHydraulic,HS.NormalizedResidual);
			AddInfo(FString::Printf(TEXT("Ceiling mode%d placed%d grid43x37x16 added%d roof%d wall%d t%.2fs energy%.3e pressure%.3e"),Mode,bPlaced,Added,RoofCells,WallCells,S.SimTimeS,S.NormalizedEnergyResidual,AS.NormalizedDivergence));
			F.Sim->EnterLayoutMode();
		};
		Run(false);
		TArray<FCrahLayoutRecord> Auto; F.Sim->CaptureAutomaticCrahLayout(Auto);
		for(const auto& R:Auto) { auto* Unit=F.World->SpawnActor<ACrahUnitActor>(R.Transform.GetLocation(),R.Transform.Rotator()); Unit->ApplyLayoutSettings(R.Settings); }
		Run(true);
		const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
		TestTrue(TEXT("Clearing ceiling leaves live mask until entry"),F.Sim->TrySetRoomCeilingPlates({},Reason) && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask);
		F.Sim->EnterSimulationMode(); TestTrue(TEXT("Next entry removes only ceiling; preserves existing geometry"),F.Sim->GetNumCeilingCells()==0 && F.Air->GetMutableSolver().GetGrid().bSolidCell==Baseline);
	}
	AddInfo(FString::Printf(TEXT("Ceiling maxima IT%.3e sizing%.3e duct%.3e coolant%.3e global%.3e pressure%.3e hydraulic%.3e; inlet%.9f..%.9fK"),MaxIt,MaxSizing,MaxDuct,MaxWater,MaxEnergy,MaxPressure,MaxHydraulic,MinInlet,MaxInlet));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingBlocksFlowTest, "VRG.Air.Solid.CeilingBlocksFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingBlocksFlowTest::RunTest(const FString& Parameters)
{
	// Reuse BlocksFlow's 12-cell/h0.25m/297.15K/340K synthetic buoyancy fixture and existing criteria.
	FAirGridParams GP; GP.NumCellsX=12; GP.NumCellsY=12; GP.NumCellsZ=12; GP.CellSizeM=.25;
	FRoomAirDomainSettings Room; Room.bManual=true; Room.MinXYM=FVector2D(0,0); Room.MaxXYM=FVector2D(3,3); Room.CeilingHeightM=3;
	FRoomCeilingPlate P; P.MinXYM=Room.MinXYM; P.MaxXYM=Room.MaxXYM; P.HeightM=1.55;
	TArray<FRoomCeilingPlan> Plans; FString Reason;
	TestTrue(TEXT("Horizontal single layer plan"),VrgRoomAirDomain::BuildCeilingPlans(Room,{P},GP,FBox(FVector(0),FVector(3)),Plans,Reason));
	FAirSolver Solver; Solver.Initialize(GP,297.15); Solver.GetMutablePressureSettings().NormalizedResidualTarget=1e-7;
	Solver.GetMutablePressureSettings().MaxIterations=3000;
	auto& G=Solver.GetMutableGrid(); TestEqual(TEXT("One144-cell layer from production rasterizer"),VrgRoomAirDomain::MarkCeilingCells(G,Plans).Num(),144);
	TestEqual(TEXT("Repeated plan adds no duplicate solids"),VrgRoomAirDomain::MarkCeilingCells(G,Plans).Num(),0); Solver.OnSolidCellsChanged();
	for(int32 Y=4;Y<=7;++Y) for(int32 X=2;X<=4;++X) G.TemperatureK[G.CellIndex(X,Y,1)]=340;
	FAirSolverInput Input; Input.CellHeatW.Init(0,G.GetNumCells()); for(int32 I=0;I<120;++I) Solver.Step(Input,.02);
	double MaxNormal=0,MaxUpperError=0;
	for(int32 Y=0;Y<12;++Y) for(int32 X=0;X<12;++X)
	{
		MaxNormal=FMath::Max3(MaxNormal,FMath::Abs(G.VelocityZMs[G.FaceZIndex(X,Y,6)]),FMath::Abs(G.VelocityZMs[G.FaceZIndex(X,Y,7)]));
		for(int32 Z=7;Z<12;++Z) { TestFalse(TEXT("Air above remains fluid"),G.IsSolidCell(X,Y,Z));
			MaxUpperError=FMath::Max(MaxUpperError,FMath::Abs(G.TemperatureK[G.CellIndex(X,Y,Z)]-297.15)); }
	}
	const auto S=Solver.MakeSnapshot(2.4);
	TestTrue(TEXT("Existing solid flow/temperature/pressure criteria"),MaxNormal==0 && MaxUpperError<1e-9 && S.MaxSpeedMs>.01
		&& S.bPressureConverged && S.NormalizedDivergence<1e-5);
	AddInfo(FString::Printf(TEXT("Ceiling layer144 cells Z[1.5,1.75]m: normal%.3e m/s upper temperature error%.3e K maxspeed%.6f m/s pressure%.3e at2.4s"),MaxNormal,MaxUpperError,S.MaxSpeedMs,S.NormalizedDivergence));
	// AirSolver returns zero normalized residual for zero EnergyInJ; manually seeded temperature above
	// is a flow isolation fixture, not a conservation proof. Reuse EnergyConservation's synthetic1500W.
	Solver.Initialize(GP,297.15); VrgRoomAirDomain::MarkCeilingCells(Solver.GetMutableGrid(),Plans); Solver.OnSolidCellsChanged();
	Input.CellHeatW.Init(0,Solver.GetGrid().GetNumCells());
	for(int32 Y=4;Y<=7;++Y) for(int32 X=2;X<=4;++X) Input.CellHeatW[Solver.GetGrid().CellIndex(X,Y,1)]=1500.0/12;
	for(int32 I=0;I<120;++I) Solver.Step(Input,.02);
	const auto Heat=Solver.MakeSnapshot(2.4);
	TestTrue(TEXT("Positive known source makes ceiling energy criterion meaningful"),FMath::IsNearlyEqual(Solver.GetEnergyInJ(),3600.0,1e-9)
		&& Solver.GetEnergyStoredJ()>0 && FMath::Abs(Heat.NormalizedEnergyResidual)<1e-9 && Heat.bPressureConverged && Heat.NormalizedDivergence<1e-5);
	AddInfo(FString::Printf(TEXT("Ceiling positive heat fixture input%.9fJ stored%.9fJ absolute energy residual%.3eJ normalized%.3e pressure%.3e at2.4s"),
		Solver.GetEnergyInJ(),Solver.GetEnergyStoredJ(),Solver.GetEnergyResidualJ(),Heat.NormalizedEnergyResidual,Heat.NormalizedDivergence));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingUiTest,"VRG.Interaction.Layout.RoomCeilingUiInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingUiTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	FString Message; TestFalse(TEXT("Automatic room cannot open ceiling modal"),C->OpenRoomCeilingSettings());
	TestTrue(TEXT("Manual room"),F.Sim->TrySetRoomAirDomain(ManualRoom(),Message));
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Grid=F.Air->GridParams; const auto Origin=F.Air->GetActorLocation();
	const auto Pose=F.Racks[0]->GetActorTransform(); const auto Camera=Pawn->GetActorLocation();
	TestTrue(TEXT("Ceiling modal shares numeric focus gate"),C->OpenRoomCeilingSettings());
	TestFalse(TEXT("Duplicate/different modal blocked"),C->OpenRoomSettings());
	FRoomAirDomainSettings Draft; const TArray<FString> Fields={TEXT("-5.13"),TEXT("-4.61"),TEXT("3.13"),TEXT("3.62"),TEXT("2.63")};
	TestTrue(TEXT("Reuse strict five-field metre parser"),URoomSettingsWidget::ParseDraft(true,Fields,Draft,Message));
	auto BadFields=Fields; BadFields[4]=TEXT("nan"); const auto Seed=Draft;
	TestFalse(TEXT("Invalid numeric draft leaves value output"),URoomSettingsWidget::ParseDraft(true,BadFields,Draft,Message));
	TestTrue(TEXT("Parsing/draft did not apply"),Draft==Seed && F.Sim->GetRoomCeilingPlates().IsEmpty());
	C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true); C->BeginRackAddition();
	for(auto Key:{EKeys::R,EKeys::Delete,EKeys::D,EKeys::Z,EKeys::Y,EKeys::Enter}) { FInputKeyEventArgs A; A.Key=Key; A.Event=IE_Pressed; C->InputKey(A); }
	TestTrue(TEXT("Modal blocks camera/equipment/mode/history"),Pawn->GetActorLocation()==Camera && F.Racks[0]->GetActorTransform().Equals(Pose,0)
		&& !C->IsAddingRack() && F.Sim->GetSimMode()==ESimMode::Layout && C->GetLayoutUndoCount()==0);
	TestFalse(TEXT("Ceiling modal cannot change room"),C->ApplyRoomSettings(ManualRoom(),Message));
	FRoomCeilingPlate P; P.MinXYM=Draft.MinXYM; P.MaxXYM=Draft.MaxXYM; P.HeightM=Draft.CeilingHeightM;
	auto Bad=P; Bad.HeightM=0; TestFalse(TEXT("Invalid height refuses without creating plate"),C->AddRoomCeiling(Bad,Message));
	TestTrue(TEXT("Refusal includes requested geometry"),Message.Contains(TEXT("요청")) && F.Sim->GetRoomCeilingPlates().IsEmpty());
	Bad=P; Bad.MinXYM=FVector2D(-1.7,-.1); Bad.MaxXYM=FVector2D(-1.3,.1); Bad.HeightM=1;
	TestFalse(TEXT("Collision refuses and still reports quantized plan"),C->AddRoomCeiling(Bad,Message));
	TestTrue(TEXT("Collision reason/range/thickness available"),Message.Contains(TEXT("거절")) && Message.Contains(TEXT("두께")) && Message.Contains(TEXT("계산 최소")));
	TestTrue(TEXT("Explicit Add alone appends exactly one"),C->AddRoomCeiling(P,Message) && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P});
	TestTrue(TEXT("Geometry reported; physical grid delayed"),Message.Contains(TEXT("3x3x1 = 9")) && Message.Contains(TEXT("배치 저장에 포함"))
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetActorLocation()==Origin && F.Air->GridParams.NumCellsZ==Grid.NumCellsZ);
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc closes without removing confirmed plate"),!C->IsRoomSettingsOpen() && C->bShowMouseCursor && F.Sim->GetRoomCeilingPlates().Num()==1);
	C->ApplyLayoutCameraInput(1,FVector(1,0,0),0,true); TestTrue(TEXT("Close restores camera path"),Pawn->GetActorLocation()!=Camera);
	TestFalse(TEXT("Closed modal cannot add"),C->AddRoomCeiling(P,Message));
	C->OpenRoomCeilingSettings(); C->SetPatchMapOpen(true); TestFalse(TEXT("Patch closes modal and blocks reopening"),C->IsRoomSettingsOpen() || C->OpenRoomCeilingSettings()); C->SetPatchMapOpen(false);
	C->OpenRoomCeilingSettings(); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("Mode change closes modal and applies nine ceiling cells"),!C->IsRoomSettingsOpen() && F.Sim->GetNumCeilingCells()==9);
	TestFalse(TEXT("Simulation cannot open ceiling modal"),C->OpenRoomCeilingSettings());
	AddInfo(TEXT("Ceiling numeric modal: request2.63m -> single0.25m layer9 cells; strict parsing, blocked input/patch/mode, append only on Add and grid delayed. Actual widget render/OS focus are direct-check scope."));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHistoryTest,"VRG.Interaction.Layout.RoomCeilingHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHistoryTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	C->Possess(F.World->SpawnActor<ADefaultPawn>()); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	const auto InitialInput=F.Racks[0]->Input; FString Message; const auto Room=ManualRoom(); F.Sim->TrySetRoomAirDomain(Room,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); const double ReferenceIt=F.Sim->GetSnapshot().TotalItHeatW; F.Sim->EnterLayoutMode();
	const auto Warm=F.Sim->GetSnapshot(); const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Origin=F.Air->GetActorLocation(); const int32 Pipes=F.Room->GetMutableNetwork().GetNumPipes();
	auto Unchanged=[&]() { const auto S=F.Sim->GetSnapshot(); return S.SimTimeS==Warm.SimTimeS && S.EnergyInJ==Warm.EnergyInJ
		&& S.EnergyOutJ==Warm.EnergyOutJ && S.EnergyStoredJ==Warm.EnergyStoredJ && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetActorLocation()==Origin && F.Room->GetMutableNetwork().GetNumPipes()==Pipes; };
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Synthetic existing fixture.
	auto Add=[&](const FRoomCeilingPlate& Value) { if(!C->OpenRoomCeilingSettings()) return false; const bool Ok=C->AddRoomCeiling(Value,Message); C->CloseRoomSettings(); return Ok; };
	C->OpenRoomCeilingSettings(); C->CloseRoomSettings(); TestTrue(TEXT("Canceled empty draft records nothing"),C->GetLayoutUndoCount()==0 && Unchanged());
	TestTrue(TEXT("One Add one shared record without live rebuild"),Add(P) && C->GetLayoutUndoCount()==1 && Unchanged());
	TestTrue(TEXT("Whole list Undo/Redo value-only"),C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates().IsEmpty()
		&& C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P} && Unchanged());
	C->TryUndoLayout(); auto Bad=P; Bad.HeightM=0;
	TestFalse(TEXT("Rejected Add leaves Redo"),Add(Bad)); TestTrue(TEXT("Failure/cancel preserve stacks/live state"),C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==1 && Unchanged());
	C->OpenRoomCeilingSettings(); TestFalse(TEXT("Numeric modal blocks history"),C->TryRedoLayout()); C->CloseRoomSettings();
	const auto Pose=F.Racks[0]->GetActorTransform();
	F.Racks[0]->SetActorLocation(FVector(-465,340,100*(2.6-F.Racks[0]->RackSizeM.Z)));
	TestFalse(TEXT("Redo revalidates equipment against quantized plate"),C->TryRedoLayout());
	TestTrue(TEXT("Refusal keeps empty list/stacks/temperature/ledger"),F.Sim->GetRoomCeilingPlates().IsEmpty() && C->GetLayoutRedoCount()==1 && Unchanged()); F.Racks[0]->SetActorTransform(Pose);
	auto Other=P; Other.HeightM=2.53; F.Sim->TrySetRoomCeilingPlates({Other},Message);
	TestFalse(TEXT("Stale history cannot overwrite externally changed list"),C->TryRedoLayout());
	TestTrue(TEXT("External list and record retained"),F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Other} && C->GetLayoutRedoCount()==1);
	F.Sim->TrySetRoomCeilingPlates({},Message); TestTrue(TEXT("Redo after removing obstruction"),C->TryRedoLayout() && Unchanged());
	C->TryUndoLayout(); TestTrue(TEXT("New Add clears Redo"),Add(Other) && C->GetLayoutRedoCount()==0);
	// Interleave with the existing room command and keep BOTH command types in the shared stack.
	auto Larger=Room; Larger.CeilingHeightM=3.2; C->OpenRoomSettings(); C->ApplyRoomSettings(Larger,Message); C->CloseRoomSettings();
	TestTrue(TEXT("Interleaved shared room/list reverse and forward"),C->TryUndoLayout() && F.Sim->GetRoomAirDomain()==Room && C->TryUndoLayout()
		&& F.Sim->GetRoomCeilingPlates().IsEmpty() && C->TryRedoLayout() && C->TryRedoLayout() && F.Sim->GetRoomAirDomain()==Larger && Unchanged());
	for(auto* Rack:F.Racks) Rack->Input=InitialInput;
	F.Sim->EnterSimulationMode(); TestFalse(TEXT("Simulation history blocked"),C->TryUndoLayout()); F.Sim->Tick(.021f); const auto First=F.Sim->GetSnapshot();
	for(int32 I=0;I<25;++I) F.Sim->Tick(.04f);
	const auto S=F.Sim->GetSnapshot(); const auto AS=F.Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
	const double It=FMath::Abs(First.TotalItHeatW-ReferenceIt)/ReferenceIt;
	TestTrue(TEXT("Next entry integrates current plate and existing physics criteria"),F.Sim->GetNumCeilingCells()==9 && It<1e-9
		&& FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5
		&& S.bHydraulicsConverged && !AS.bDuctsRejected && S.CrahCoilRemovedW>0);
	for(auto* Rack:F.Racks) for(double K:Rack->Input.SlotAirInletTempK) TestTrue(TEXT("Existing transient15..35C inlet safety"),K>288.15 && K<308.15);
	F.Sim->EnterLayoutMode(); TestTrue(TEXT("Mode transition preserves shared history"),C->GetLayoutUndoCount()==2);
	C->SetPatchMapOpen(true); TestFalse(TEXT("Patch blocks history"),C->TryUndoLayout()); C->SetPatchMapOpen(false);
	const FString File=TEXT("AutomationCeilingHistory"); TestTrue(TEXT("Load restores saved plates and clears all history"),VrgLayoutFile::Save(F.World,File,Message)
		&& VrgLayoutFile::Load(F.World,File,Message) && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0 && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Other});
	F.Sim->TrySetRoomCeilingPlates({},Message);
	for(int32 I=0;I<105;++I) TestTrue(TEXT("Existing100-command cap with repeated individual Add"),Add(P) && C->GetLayoutUndoCount()<=100);
	for(int32 I=0;I<100;++I) TestTrue(TEXT("Undo retained ceiling addition"),C->TryUndoLayout());
	TestTrue(TEXT("Five evicted additions remain"),F.Sim->GetRoomCeilingPlates().Num()==5 && !C->TryUndoLayout() && C->GetLayoutRedoCount()==100);
	for(int32 I=0;I<100;++I) TestTrue(TEXT("Redo retained ceiling addition"),C->TryRedoLayout());
	TestEqual(TEXT("All retained additions restored"),F.Sim->GetRoomCeilingPlates().Num(),105);
	AddInfo(FString::Printf(TEXT("Ceiling history: one record/list exact restoration; cancel/refusal/stale state preserved at0.02s; shared cap100 and Load clears history but restores saved list; next-entry9cells IT%.3e energy%.3e pressure%.3e at%.2fs."),It,S.NormalizedEnergyResidual,AS.NormalizedDivergence,S.SimTimeS));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingSelectionTest,"VRG.Interaction.Layout.RoomCeilingSelectionDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingSelectionTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	TestFalse(TEXT("Automatic room cannot enable ceiling editing"),C->SetRoomCeilingEditing(true));
	TestTrue(TEXT("Manual room"),F.Sim->TrySetRoomAirDomain(ManualRoom(),Reason));
	// Existing synthetic corner geometry. First two identical plates must remain separate list entries.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63;
	auto Low=P; Low.HeightM=1.83; TestTrue(TEXT("Plate fixture"),F.Sim->TrySetRoomCeilingPlates({P,P,Low},Reason));
	const FVector Eye(-520,340,400),Down(0,0,-1); const auto Pose=F.Racks[0]->GetActorTransform();
	TestFalse(TEXT("Normal equipment mode cannot pick/delete a ceiling"),C->SelectRoomCeilingAlongRay(Eye,Down) || C->TryDeleteRoomCeiling());
	C->SelectLayoutAlongRay(FVector(-150,0,1000),Down); TestEqual(TEXT("Normal equipment selection retained"),C->GetSelectedLayoutActor(),static_cast<AActor*>(F.Racks[0]));
	TestTrue(TEXT("Enable exclusive ceiling mode clears old equipment selection"),C->SetRoomCeilingEditing(true) && !C->GetSelectedLayoutActor());
	TestTrue(TEXT("Pick actual expanded box, nearest plate and stable first duplicate"),C->SelectRoomCeilingAlongRay(Eye,Down) && C->GetSelectedRoomCeilingIndex()==0);
	TestFalse(TEXT("Malformed ray does not clear valid selection"),C->SelectRoomCeilingAlongRay(Eye,FVector::ZeroVector)); TestEqual(TEXT("Selection retained"),C->GetSelectedRoomCeilingIndex(),0);
	C->SelectLayoutAlongRay(FVector(-150,0,1000),Down); C->BeginRackAddition();
	TestTrue(TEXT("Ceiling click owns mode; equipment/addition disabled"),!C->GetSelectedLayoutActor() && !C->IsAddingRack() && !C->IsLayoutDragging());
	const auto Camera=Pawn->GetActorLocation(); C->ApplyLayoutCameraInput(1,FVector(1,0,0),0,true); TestTrue(TEXT("RMB camera path available during ceiling edit"),Pawn->GetActorLocation()!=Camera);
	FInputKeyEventArgs Key; Key.Key=EKeys::R; Key.Event=IE_Pressed; C->InputKey(Key);
	TestTrue(TEXT("Equipment R does not rotate hidden selection"),F.Racks[0]->GetActorTransform().Equals(Pose,0));
	Key.Key=EKeys::Delete; TestTrue(TEXT("Raw Delete handled once"),C->InputKey(Key));
	Key.Event=IE_Repeat; C->InputKey(Key); Key.Event=IE_Released; C->InputKey(Key);
	TestTrue(TEXT("Only selected duplicate removed, one shared command, selection cleared"),F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,Low}
		&& C->GetLayoutUndoCount()==1 && C->GetSelectedRoomCeilingIndex()==INDEX_NONE && F.Racks[0]->GetActorTransform().Equals(Pose,0));
	TestFalse(TEXT("No selection cannot delete again"),C->TryDeleteRoomCeiling());
	TestTrue(TEXT("Shared Undo/Redo restores exact ordered list"),C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,P,Low}
		&& C->TryRedoLayout() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,Low});
	C->SelectRoomCeilingAlongRay(Eye,Down); TestFalse(TEXT("Empty/behind ray clears selection"),C->SelectRoomCeilingAlongRay(FVector(-520,340,0),Down));
	TestEqual(TEXT("Empty click selection"),C->GetSelectedRoomCeilingIndex(),INDEX_NONE);
	C->SelectRoomCeilingAlongRay(Eye,Down); F.Sim->TrySetRoomCeilingPlates({Low},Reason);
	TestTrue(TEXT("External list change cannot delete shifted index"),C->GetSelectedRoomCeilingIndex()==INDEX_NONE && !C->TryDeleteRoomCeiling() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Low});
	C->SetRoomCeilingEditing(false); C->SetRoomBoundsEditing(true); TestFalse(TEXT("Room handles block ceiling mode"),C->SetRoomCeilingEditing(true)); C->SetRoomBoundsEditing(false);
	C->SetRoomCeilingEditing(true); C->OpenRoomCeilingSettings(); TestTrue(TEXT("Numeric modal ends ceiling edit and blocks picking/deletion"),!C->IsRoomCeilingEditing()
		&& !C->SelectRoomCeilingAlongRay(Eye,Down) && !C->TryDeleteRoomCeiling() && !C->SetRoomCeilingEditing(true)); C->CloseRoomSettings();
	C->SetRoomCeilingEditing(true); C->SetPatchMapOpen(true); TestFalse(TEXT("Patch blocks reentry/deletion"),C->IsRoomCeilingEditing() || C->SetRoomCeilingEditing(true) || C->TryDeleteRoomCeiling()); C->SetPatchMapOpen(false);
	C->SetRoomCeilingEditing(true); Key.Key=EKeys::Escape; Key.Event=IE_Pressed; C->InputKey(Key);
	TestTrue(TEXT("Esc exits mode, preserving list and enabling equipment"),!C->IsRoomCeilingEditing() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Low});
	C->SelectLayoutAlongRay(FVector(-150,0,1000),Down); TestEqual(TEXT("Equipment picking resumes after Esc"),C->GetSelectedLayoutActor(),static_cast<AActor*>(F.Racks[0]));
	C->SetRoomCeilingEditing(true); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestFalse(TEXT("Simulation disables ceiling mode/selection/deletion"),C->IsRoomCeilingEditing() || C->SetRoomCeilingEditing(true) || C->TryDeleteRoomCeiling());
	AddInfo(TEXT("Ceiling selection: exclusive mode/actual expanded box; nearest ray and exact ties first index; one of duplicate entries deleted; one shared command; stale/miss/modal/patch/Esc/mode guards; camera/equipment resumed. Render/real button input remains direct-check scope."));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingCornerPreviewTest,"VRG.Interaction.Layout.RoomCeilingCornerPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingCornerPreviewTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture metres.
	F.Sim->TrySetRoomAirDomain(ManualRoom(),Reason); F.Sim->TrySetRoomCeilingPlates({P,P},Reason);
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Pose=F.Racks[0]->GetActorTransform(), Camera=Pawn->GetActorTransform();
	const FVector Eye(-490,340,400),Down(0,0,-1);
	auto Frozen=[&]() { return F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,P} && F.Sim->GetRoomAirDomain()==ManualRoom()
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Racks[0]->GetActorTransform().Equals(Pose,0) && C->GetLayoutUndoCount()==0; };
	TestFalse(TEXT("No ceiling mode/selection cannot start handle"),C->BeginRoomCeilingCornerDrag(0,Eye,Down));
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(Eye,Down);
	TestFalse(TEXT("Invalid corner/parallel start do not start gesture"),C->BeginRoomCeilingCornerDrag(6,Eye,Down) || C->BeginRoomCeilingCornerDrag(0,Eye,FVector(1,0,0)));
	for(int32 Corner=0;Corner<4;++Corner)
	{
		TestTrue(TEXT("Selected plate corner begins"),C->BeginRoomCeilingCornerDrag(Corner,Eye,Down));
		C->UpdateRoomCeilingCornerDrag(Eye+FVector(12,8,0),Down);
		FRoomCeilingPlate Preview; bool Valid=false; TestTrue(TEXT("Preview available"),C->GetRoomCeilingPreview(Preview,Valid) && Valid);
		const double X=(Corner&1 ? P.MaxXYM.X : P.MinXYM.X),Y=(Corner&2 ? P.MaxXYM.Y : P.MinXYM.Y);
		TestTrue(TEXT("Shared5cm XY snap, exact opposite coordinates and height"),FMath::IsNearlyEqual(Corner&1?Preview.MaxXYM.X:Preview.MinXYM.X,FMath::RoundToDouble((X+.12)/.05)*.05,1e-12)
			&& FMath::IsNearlyEqual(Corner&2?Preview.MaxXYM.Y:Preview.MinXYM.Y,FMath::RoundToDouble((Y+.08)/.05)*.05,1e-12)
			&& (Corner&1?Preview.MinXYM.X:Preview.MaxXYM.X)==(Corner&1?P.MinXYM.X:P.MaxXYM.X)
			&& (Corner&2?Preview.MinXYM.Y:Preview.MaxXYM.Y)==(Corner&2?P.MinXYM.Y:P.MaxXYM.Y) && Preview.HeightM==P.HeightM && Frozen());
		TestTrue(TEXT("Calculated bounds/thickness/cells reported"),C->GetRoomCeilingEditMessage().Contains(TEXT("계산 최소")) && C->GetRoomCeilingEditMessage().Contains(TEXT("두께")));
		C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true); C->BeginRackAddition();
		TestTrue(TEXT("Camera/equipment/delete/history blocked during gesture"),Pawn->GetActorTransform().Equals(Camera,0) && !C->IsAddingRack()
			&& !C->TryDeleteRoomCeiling() && !C->TryUndoLayout() && !C->TryRedoLayout() && Frozen());
		FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
		TestTrue(TEXT("Esc cancels gesture only"),!C->IsLayoutDragging() && C->IsRoomCeilingEditing() && Frozen());
	}
	C->BeginRoomCeilingCornerDrag(3,Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,200,0),Down);
	FRoomCeilingPlate Preview; bool Valid=true; C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Outside room red/refusal leaves all live state/records"),!Valid && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingCornerDrag(0,Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(200,0,0),Down);
	C->GetRoomCeilingPreview(Preview,Valid); TestTrue(TEXT("Crossing opposite refuses rather than swapping"),!Valid && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingCornerDrag(3,Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye,FVector::ZeroVector);
	C->GetRoomCeilingPreview(Preview,Valid); TestTrue(TEXT("Invalid ray cannot commit stale preview"),!Valid && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingCornerDrag(3,Eye,Down); TestFalse(TEXT("Unchanged click creates no record or snap"),C->EndRoomCeilingCornerDrag(true)); TestTrue(TEXT("No-op exact state"),Frozen());
	C->BeginRoomCeilingCornerDrag(3,Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(10,10,0),Down);
	TestTrue(TEXT("Numeric modal cancels pending drag"),C->OpenRoomCeilingSettings() && !C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && Frozen()); C->CloseRoomSettings();
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(Eye,Down); C->BeginRoomCeilingCornerDrag(3,Eye,Down); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch cancels pending edit"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && Frozen()); C->SetPatchMapOpen(false);
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(Eye,Down); C->BeginRoomCeilingCornerDrag(3,Eye,Down);
	auto Other=P; Other.HeightM=1.83; F.Sim->TrySetRoomCeilingPlates({Other},Reason); C->RefreshInteractionInputMode();
	TestTrue(TEXT("External list invalidates index and entire gesture"),!C->IsLayoutDragging() && C->GetSelectedRoomCeilingIndex()==INDEX_NONE
		&& !C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Other} && C->GetLayoutUndoCount()==0);
	F.Sim->TrySetRoomCeilingPlates({P,P},Reason); C->SelectRoomCeilingAlongRay(Eye,Down); C->BeginRoomCeilingCornerDrag(3,Eye,Down);
	C->EndLayoutDrag(false); TestTrue(TEXT("Common focus/cancel path ends ceiling drag"),!C->IsLayoutDragging() && Frozen());
	C->BeginRoomCeilingCornerDrag(3,Eye,Down); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("Mode cancels pending and next entry applies original plates"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && F.Sim->GetNumCeilingCells()==9);
	AddInfo(TEXT("Ceiling corners: four absolute5cm XY snaps; opposite/height exact; delayed live arrays; crossing/outside/ray/no-op/Esc/modal/patch/stale/common focus/mode cancellation and input gates. Actual pixel handle selection/OS focus pending direct check.")); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHeightPreviewTest,"VRG.Interaction.Layout.RoomCeilingHeightPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHeightPreviewTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture.
	F.Sim->TrySetRoomAirDomain(ManualRoom(),Reason); F.Sim->TrySetRoomCeilingPlates({P,P},Reason);
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Pose=F.Racks[0]->GetActorTransform(),Camera=Pawn->GetActorTransform();
	const FVector PickEye(-490,340,400),Down(0,0,-1),Eye(-390,337.5,270),Side(-1,0,0);
	auto Frozen=[&]() { return F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,P} && F.Sim->GetRoomAirDomain()==ManualRoom()
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Racks[0]->GetActorTransform().Equals(Pose,0) && C->GetLayoutUndoCount()==0; };
	TestFalse(TEXT("No selection height blocked"),C->BeginRoomCeilingHeightDrag(Eye,Side));
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(PickEye,Down);
	TestFalse(TEXT("Parallel/near-parallel/backward axis rays cannot arm"),C->BeginRoomCeilingHeightDrag(PickEye,Down)
		|| C->BeginRoomCeilingHeightDrag(PickEye,FVector(.001,0,-1)) || C->BeginRoomCeilingHeightDrag(Eye,-Side));
	TestTrue(TEXT("Shared forward axis grab with offset"),C->BeginRoomCeilingHeightDrag(Eye,Side));
	C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side*3);
	FRoomCeilingPlate Preview; bool Valid=false; C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Height2.63+0.22->2.85m snapped, XY exact and all live state frozen"),Valid && FMath::IsNearlyEqual(Preview.HeightM,2.85,1e-12)
		&& Preview.MinXYM==P.MinXYM && Preview.MaxXYM==P.MaxXYM && Frozen());
	TestTrue(TEXT("Actual Z/thickness/count reported"),C->GetRoomCeilingEditMessage().Contains(TEXT("계산 최소")) && C->GetRoomCeilingEditMessage().Contains(TEXT("3x3x1 = 9")));
	C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true); C->BeginRackAddition();
	TestTrue(TEXT("Height drag blocks camera/equipment/delete/history"),Pawn->GetActorTransform().Equals(Camera,0) && !C->IsAddingRack()
		&& !C->TryDeleteRoomCeiling() && !C->TryUndoLayout() && !C->TryRedoLayout() && Frozen());
	C->UpdateRoomCeilingCornerDrag(PickEye,Down); C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Parallel update retains last height but forbids stale commit"),!Valid && FMath::IsNearlyEqual(Preview.HeightM,2.85,1e-12) && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingHeightDrag(Eye,Side); C->UpdateRoomCeilingCornerDrag(PickEye,FVector(.001,0,-1)); C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side);
	C->GetRoomCeilingPreview(Preview,Valid); TestTrue(TEXT("Stable ray recovery revalidates"),Valid && FMath::IsNearlyEqual(Preview.HeightM,2.85,1e-12));
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc cancels height only, no recording"),C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && Frozen());
	for(double Delta:{60.0,-300.0})
	{
		C->BeginRoomCeilingHeightDrag(Eye,Side); C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,Delta),Side); C->GetRoomCeilingPreview(Preview,Valid);
		TestTrue(TEXT("Above room/nonpositive heights red and rejected"),!Valid && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	}
	C->BeginRoomCeilingHeightDrag(Eye,Side); TestTrue(TEXT("No-op unaligned2.63m does not snap or record"),!C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingHeightDrag(Eye,Side); C->OpenRoomSettings(); TestTrue(TEXT("Numeric modal cancels height"),!C->IsLayoutDragging() && !C->IsRoomCeilingEditing() && Frozen()); C->CloseRoomSettings();
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(PickEye,Down); C->BeginRoomCeilingHeightDrag(Eye,Side); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch cancels height"),!C->IsLayoutDragging() && !C->IsRoomCeilingEditing() && Frozen()); C->SetPatchMapOpen(false);
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(PickEye,Down); C->BeginRoomCeilingHeightDrag(Eye,Side);
	auto Other=P; Other.HeightM=1.83; F.Sim->TrySetRoomCeilingPlates({Other},Reason); C->UpdateRoomCeilingCornerDrag(Eye+FVector(0,0,22),Side);
	TestTrue(TEXT("External list cannot apply captured index"),!C->IsLayoutDragging() && !C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Other} && C->GetLayoutUndoCount()==0);
	F.Sim->TrySetRoomCeilingPlates({P,P},Reason); C->SelectRoomCeilingAlongRay(PickEye,Down); C->BeginRoomCeilingHeightDrag(Eye,Side); C->EndLayoutDrag(false);
	TestTrue(TEXT("Common focus cancellation restores unchanged state"),!C->IsLayoutDragging() && Frozen());
	C->BeginRoomCeilingHeightDrag(Eye,Side); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("Mode cancels and applies original9cell union"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && F.Sim->GetNumCeilingCells()==9);
	AddInfo(TEXT("Ceiling height: shared axis conditioning1e-4 and5cm snap2.63->2.85m; XY exact, last-height stale commit blocked/recovered; cancel/no-op/reject/input/list gates. Actual height pixel/OS input pending direct check.")); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingMovePreviewTest,"VRG.Interaction.Layout.RoomCeilingMovePreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingMovePreviewTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.13,3.13); P.MaxXYM=FVector2D(-4.61,3.62); P.HeightM=2.63; // Existing synthetic fixture metres.
	F.Sim->TrySetRoomAirDomain(ManualRoom(),Reason); F.Sim->TrySetRoomCeilingPlates({P,P},Reason);
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Pose=F.Racks[0]->GetActorTransform(),Camera=Pawn->GetActorTransform(); const FVector Eye(-490,340,400),Down(0,0,-1);
	auto Frozen=[&]() { return F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{P,P} && F.Sim->GetRoomAirDomain()==ManualRoom()
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps
		&& F.Racks[0]->GetActorTransform().Equals(Pose,0) && C->GetLayoutUndoCount()==0; };
	TestFalse(TEXT("Normal mode cannot start plate body press"),C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down)); C->SetRoomCeilingEditing(true);
	TestTrue(TEXT("Body press picks first duplicate and arms shared gesture"),C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down) && C->GetSelectedRoomCeilingIndex()==0);
	TestTrue(TEXT("Unmoved click retains exact unsnapped geometry and selection"),!C->EndRoomCeilingCornerDrag(true) && C->GetSelectedRoomCeilingIndex()==0 && Frozen());
	TestFalse(TEXT("Picked invalid height ray owns press, no fallback body drag"),C->BeginRoomCeilingPointerDrag(C->RoomHeightHandle,Eye,Down)); TestTrue(TEXT("No gesture on rejected handle"),!C->IsLayoutDragging() && Frozen());
	TestTrue(TEXT("XY handle owns press before body"),C->BeginRoomCeilingPointerDrag(0,Eye,Down)); C->UpdateRoomCeilingCornerDrag(Eye+FVector(12,8,0),Down);
	FRoomCeilingPlate Preview; bool Valid=false; C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Handle gesture fixes opposite corner instead of translating"),Valid && Preview.MaxXYM==P.MaxXYM && Preview.MinXYM!=P.MinXYM && Frozen()); C->EndRoomCeilingCornerDrag(false);
	TestTrue(TEXT("Valid height handle priority"),C->BeginRoomCeilingPointerDrag(C->RoomHeightHandle,FVector(-390,337.5,270),FVector(-1,0,0)));
	C->UpdateRoomCeilingCornerDrag(FVector(-390,337.5,292),FVector(-1,0,0)); C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Height handler fixes XY rather than translating"),Valid && Preview.MinXYM==P.MinXYM && Preview.MaxXYM==P.MaxXYM && FMath::IsNearlyEqual(Preview.HeightM,2.85,1e-12)); C->EndRoomCeilingCornerDrag(false);
	C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(87,-23,0),Down); C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Minimum anchor snaps5cm, grab offset/size/height retained without mutation"),Valid && FMath::IsNearlyEqual(Preview.MinXYM.X,-4.25,1e-12)
		&& FMath::IsNearlyEqual(Preview.MinXYM.Y,2.9,1e-12) && (Preview.MaxXYM-Preview.MinXYM).Equals(P.MaxXYM-P.MinXYM,1e-12) && Preview.HeightM==P.HeightM && Frozen());
	TestTrue(TEXT("Requested/calculated geometry and cells shown"),C->GetRoomCeilingEditMessage().Contains(TEXT("계산 최소")) && C->GetRoomCeilingEditMessage().Contains(TEXT("3x3x1 = 9")));
	C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true); C->BeginRackAddition();
	TestTrue(TEXT("Movement blocks camera/equipment/Delete/Undo/Redo"),Pawn->GetActorTransform().Equals(Camera,0) && !C->IsAddingRack() && !C->TryDeleteRoomCeiling()
		&& !C->TryUndoLayout() && !C->TryRedoLayout() && Frozen());
	C->UpdateRoomCeilingCornerDrag(Eye,FVector(1,0,0)); C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Missing plane ray holds last point but rejects stale commit"),!Valid && FMath::IsNearlyEqual(Preview.MinXYM.X,-4.25,1e-12) && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingMoveDrag(Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(-300,0,0),Down); C->GetRoomCeilingPreview(Preview,Valid);
	TestTrue(TEXT("Outside room translation refused whole"),!Valid && !C->EndRoomCeilingCornerDrag(true) && Frozen());
	C->BeginRoomCeilingMoveDrag(Eye,Down); C->UpdateRoomCeilingCornerDrag(Eye+FVector(87,-23,0),Down);
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc); TestTrue(TEXT("Esc cancels move keeping edit mode"),!C->IsLayoutDragging() && C->IsRoomCeilingEditing() && Frozen());
	C->BeginRoomCeilingMoveDrag(Eye,Down); C->OpenRoomSettings(); TestTrue(TEXT("Numeric modal cancels move"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && Frozen()); C->CloseRoomSettings();
	C->SetRoomCeilingEditing(true); C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch cancels move"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && Frozen()); C->SetPatchMapOpen(false);
	C->SetRoomCeilingEditing(true); C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down); auto Other=P; Other.HeightM=1.83; F.Sim->TrySetRoomCeilingPlates({Other},Reason);
	C->RefreshInteractionInputMode(); TestTrue(TEXT("External list cannot move another index"),!C->IsLayoutDragging() && C->GetSelectedRoomCeilingIndex()==INDEX_NONE && !C->EndRoomCeilingCornerDrag(true) && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Other});
	F.Sim->TrySetRoomCeilingPlates({P,P},Reason); C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down); C->EndLayoutDrag(false);
	TestTrue(TEXT("Shared focus cancel keeps state/records"),!C->IsLayoutDragging() && Frozen());
	TestFalse(TEXT("Empty body press clears selection with no gesture"),C->BeginRoomCeilingPointerDrag(INDEX_NONE,FVector(5000,5000,400),Down)); TestEqual(TEXT("Empty selection"),C->GetSelectedRoomCeilingIndex(),INDEX_NONE);
	C->BeginRoomCeilingPointerDrag(INDEX_NONE,Eye,Down); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("Mode cancels move and applies original union"),!C->IsRoomCeilingEditing() && !C->IsLayoutDragging() && F.Sim->GetNumCeilingCells()==9);
	AddInfo(TEXT("Ceiling body movement: minimum XY anchor5cm, initial offset, size/height unchanged; handle priority including failed ray; live state frozen and no-op/miss/ray/refusal/cancel/input/list gates. Actual UI pixel press/drag pending direct check.")); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHolePlanTest, "VRG.Interaction.Layout.RoomCeilingHolePlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHolePlanTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; FString Reason; const auto Room=ManualRoom();
	TestTrue(TEXT("Manual fixture"), F.Sim->TrySetRoomAirDomain(Room,Reason));
	FAirGridParams GP; FBox Actual(ForceInit); TArray<FRoomCeilingPlan> Plans;
	TestTrue(TEXT("Reuse room/grid/resource validation"), F.Sim->ValidateRoomAirDomain(Room,GP,Actual,Reason));
	// Synthetic geometry, exact binary fractions isolate full-cell inclusion from measured hardware.
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.25,3); P.MaxXYM=FVector2D(-4.25,4); P.HeightM=2.625;
	FRoomCeilingHole Hole; Hole.LocalMinXYM=FVector2D(.125,.125); Hole.LocalMaxXYM=FVector2D(.875,.875); P.Holes.Add(Hole);
	TestTrue(TEXT("Local hole accepted"), F.Sim->TrySetRoomCeilingPlates({P},Reason)
		&& VrgRoomAirDomain::BuildCeilingPlans(Room,{P},GP,Actual,Plans,Reason));
	if (Plans.Num()!=1 || Plans[0].OpenCells.Num()!=1) return false;
	const auto Plan=Plans[0];
	TestTrue(TEXT("Only four fully included cells open"), Plan.OpenCells[0]==FIntRect(2,31,4,33)
		&& VrgRoomAirDomain::IsCeilingCellOpen(Plan,2,31) && !VrgRoomAirDomain::IsCeilingCellOpen(Plan,1,31));
	TestFalse(TEXT("Open passage excluded from equipment/duct overlap"), VrgRoomAirDomain::CeilingIntersectsBox(Plan,FBox(FVector(-5,3.25,2.5),FVector(-4.5,3.75,2.75)),GP.CellSizeM));
	TestTrue(TEXT("Remaining plate still obstructs duct"), VrgRoomAirDomain::CeilingIntersectsBox(Plan,FBox(FVector(-5.25,3,2.5),FVector(-5,3.25,2.75)),GP.CellSizeM));
	const auto List=F.Sim->GetRoomCeilingPlates(); const auto Clock=F.Sim->GetSnapshot().SimTimeS;
	auto Reject=[&](FRoomCeilingPlate Bad)
	{
		const auto Kept=Plans;
		TestFalse(TEXT("Invalid hole rejects complete plan"),VrgRoomAirDomain::BuildCeilingPlans(Room,{P,Bad},GP,Actual,Plans,Reason));
		TestTrue(TEXT("Reason and previous output preserved"),!Reason.IsEmpty() && Plans.Num()==Kept.Num() && Plans[0].OpenCells==Kept[0].OpenCells);
		TestFalse(TEXT("Invalid list rejected by existing API"),F.Sim->TrySetRoomCeilingPlates({Bad},Reason));
		TestTrue(TEXT("List/grid/clock unaffected"),F.Sim->GetRoomCeilingPlates()==List && F.Sim->GetSnapshot().SimTimeS==Clock);
	};
	auto Bad=P; Bad.Holes[0].LocalMaxXYM.X=.125; Reject(Bad);
	Bad=P; Bad.Holes[0].LocalMinXYM.X=-.01; Reject(Bad);
	Bad=P; Bad.Holes[0].LocalMaxXYM.Y=1.01; Reject(Bad);
	Bad=P; Bad.Holes[0].LocalMaxXYM.X=std::numeric_limits<double>::infinity(); Reject(Bad);
	Bad=P; Bad.Holes[0].LocalMaxXYM=FVector2D(.2,.2); Reject(Bad);
	Bad=P; Bad.MaxXYM.X=P.MinXYM.X+.75; Reject(Bad); // Resize cannot truncate the local hole.
	// A cell exactly on each face is allowed; the tiniest inward request must not round outwards.
	Bad=P; Bad.Holes[0].LocalMinXYM=FVector2D(.25,.25); Bad.Holes[0].LocalMaxXYM=FVector2D(.5,.5);
	TestTrue(TEXT("Exact one-cell hole allowed"),VrgRoomAirDomain::BuildCeilingPlans(Room,{Bad},GP,Actual,Plans,Reason) && Plans[0].OpenCells[0]==FIntRect(2,31,3,32));
	Bad.Holes[0].LocalMinXYM.X+=1e-12;
	TestFalse(TEXT("Inward face cannot open an almost-contained cell"),VrgRoomAirDomain::BuildCeilingPlans(Room,{Bad},GP,Actual,Plans,Reason));
	Bad=P; Bad.MinXYM+=FVector2D(.5,-.5); Bad.MaxXYM+=FVector2D(.5,-.5); Bad.HeightM=2.875;
	TestTrue(TEXT("Local data follows translated plate and height"),VrgRoomAirDomain::BuildCeilingPlans(Room,{Bad},GP,Actual,Plans,Reason)
		&& Plans[0].OpenCells[0]==FIntRect(4,29,6,31) && Plans[0].MinCell.Z==11 && Bad.Holes==P.Holes);
	FRoomCeilingPlate Legacy=P; Legacy.Holes.Reset();
	TestTrue(TEXT("No-hole geometry retains old plan"),VrgRoomAirDomain::BuildCeilingPlans(Room,{Legacy},GP,Actual,Plans,Reason)
		&& Plans[0].OpenCells.IsEmpty() && Plans[0].MinCell==FIntVector(1,30,10) && Plans[0].MaxCellExclusive==FIntVector(5,34,11));
	AddInfo(TEXT("Hole plan: local[0.125,0.875]^2m -> fully contained2x2 cells h0.25m; world[-5,-4.5]x[3.25,3.75]m Z[2.5,2.75]m. Validation only; rasterization checked separately."));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHoleFlowTest, "VRG.Air.Solid.CeilingHoleFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHoleFlowTest::RunTest(const FString& Parameters)
{
	// Same synthetic geometry/air/source/step/pressure settings as CeilingBlocksFlow; no exterior sink.
	FAirGridParams GP; GP.NumCellsX=12; GP.NumCellsY=12; GP.NumCellsZ=12; GP.CellSizeM=.25;
	FRoomAirDomainSettings Room; Room.bManual=true; Room.MinXYM=FVector2D(0); Room.MaxXYM=FVector2D(3); Room.CeilingHeightM=3;
	FRoomCeilingPlate P; P.MinXYM=Room.MinXYM; P.MaxXYM=Room.MaxXYM; P.HeightM=1.55;
	FRoomCeilingHole Hole; Hole.LocalMinXYM=FVector2D(.75); Hole.LocalMaxXYM=FVector2D(2.25); P.Holes={Hole,Hole};
	TArray<FRoomCeilingPlan> Plans; FString Reason;
	TestTrue(TEXT("Overlapping holes planned"),VrgRoomAirDomain::BuildCeilingPlans(Room,{P},GP,FBox(FVector(0),FVector(3)),Plans,Reason));
	FAirSolver MaskSolver; MaskSolver.Initialize(GP,297.15); auto& Mask=MaskSolver.GetMutableGrid();
	// Existing solid owners all share this bool mask; the rasterizer must never write false to any of them.
	for(int32 I=3;I<6;++I) Mask.bSolidCell[Mask.CellIndex(I,I,6)]=true;
	const auto Before=Mask.bSolidCell;
	TestEqual(TEXT("Duplicate holes union; only108 plate cells added"),VrgRoomAirDomain::MarkCeilingCells(Mask,Plans).Num(),108);
	for(int32 I=0;I<Before.Num();++I) if(Before[I]) TestTrue(TEXT("Every preexisting solid preserved"),Mask.bSolidCell[I]);
	TestFalse(TEXT("Unobstructed hole remains fluid"),Mask.IsSolidCell(7,7,6));
	FRoomCeilingPlate Cover; Cover.MinXYM=FVector2D(1.75); Cover.MaxXYM=FVector2D(2); Cover.HeightM=P.HeightM;
	TArray<FRoomCeilingPlan> Covered; VrgRoomAirDomain::BuildCeilingPlans(Room,{P,Cover,P},GP,FBox(FVector(0),FVector(3)),Covered,Reason);
	TestEqual(TEXT("Other plate fills exactly one hole cell"),VrgRoomAirDomain::MarkCeilingCells(Mask,Covered).Num(),1);
	TestTrue(TEXT("Other plate survives later holed plate"),Mask.IsSolidCell(7,7,6));
	TestEqual(TEXT("Repeated union adds nothing"),VrgRoomAirDomain::MarkCeilingCells(Mask,Covered).Num(),0);
	double MaxEnergy=0,MaxDiv=0,OpenNormal=0,OpenUpper=0,ClosedUpper=0;
	for(bool bOpen:{false,true})
	{
		auto Case=P; if(!bOpen) Case.Holes.Reset();
		TestTrue(TEXT("Closed/open plan"),VrgRoomAirDomain::BuildCeilingPlans(Room,{Case},GP,FBox(FVector(0),FVector(3)),Plans,Reason));
		FAirSolver Solver; Solver.Initialize(GP,297.15); Solver.GetMutablePressureSettings().NormalizedResidualTarget=1e-7; Solver.GetMutablePressureSettings().MaxIterations=3000;
		auto& G=Solver.GetMutableGrid(); TestEqual(TEXT("Production mask count"),VrgRoomAirDomain::MarkCeilingCells(G,Plans).Num(),bOpen?108:144); Solver.OnSolidCellsChanged();
		FAirSolverInput Input; Input.CellHeatW.Init(0,G.GetNumCells());
		for(int32 Y=4;Y<=7;++Y) for(int32 X=4;X<=6;++X) Input.CellHeatW[G.CellIndex(X,Y,5)]=1500.0/12;
		for(int32 I=0;I<120;++I) Solver.Step(Input,.02);
		double Normal=0,Upper=0,MinK=TNumericLimits<double>::Max(),MaxK=0;
		for(int32 Y=0;Y<12;++Y) for(int32 X=0;X<12;++X)
		{
			Normal=FMath::Max(Normal,FMath::Abs(G.VelocityZMs[G.FaceZIndex(X,Y,7)]));
			for(int32 Z=0;Z<12;++Z) if(!G.IsSolidCell(X,Y,Z))
			{ const double K=G.TemperatureK[G.CellIndex(X,Y,Z)]; MinK=FMath::Min(MinK,K); MaxK=FMath::Max(MaxK,K); if(Z>=7) Upper=FMath::Max(Upper,K-297.15); }
		}
		const auto S=Solver.MakeSnapshot(2.4); MaxEnergy=FMath::Max(MaxEnergy,FMath::Abs(S.NormalizedEnergyResidual)); MaxDiv=FMath::Max(MaxDiv,S.NormalizedDivergence);
		// Measured closed/open minimum undershoot1.54e-7K at2.4s (2026-10-06).
		// Conservative temperature flux retains the small pressure projection divergence; use1e-6K margin,
		// documented in CLAUDE, rather than a machine-precision minimum-principle assumption. No solver clamp.
		TestTrue(TEXT("Existing meaningful energy/pressure criteria and measured temperature bound"),FMath::IsNearlyEqual(Solver.GetEnergyInJ(),3600.0,1e-9)
			&& Solver.GetEnergyStoredJ()>0 && FMath::Abs(S.NormalizedEnergyResidual)<1e-9 && S.bPressureConverged && S.NormalizedDivergence<1e-5 && MinK>=297.15-1e-6 && MaxK<340);
		if(bOpen) { OpenNormal=Normal; OpenUpper=Upper; } else { ClosedUpper=Upper; TestEqual(TEXT("Closed plate has no normal flow"),Normal,0.0); }
		AddInfo(FString::Printf(TEXT("Ceiling hole open%d t2.4s input%.9fJ stored%.9fJ residual%.3eJ energy%.3e pressure%.3e normal%.9fm/s upperRise%.9fK fluid[%.9f,%.9f]K"),bOpen,Solver.GetEnergyInJ(),Solver.GetEnergyStoredJ(),Solver.GetEnergyResidualJ(),S.NormalizedEnergyResidual,S.NormalizedDivergence,Normal,Upper,MinK,MaxK));
	}
	TestTrue(TEXT("Hole connects upper/lower fluid, closed reference isolated"),OpenNormal>0 && OpenUpper>0 && ClosedUpper<1e-9);
	AddInfo(FString::Printf(TEXT("Ceiling hole mask144->108 cells; full inclusion36cells; other solids/plate preserved; maxEnergyAbs%.3e maxNormalizedDiv%.3e; no external mass/heat removal."),MaxEnergy,MaxDiv));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHoleUiTest,"VRG.Interaction.Layout.RoomCeilingHoleUiInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHoleUiTest::RunTest(const FString& Parameters)
{
	FRoomFixture F; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
	auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
	FString Message; TestFalse(TEXT("No manual room/plate cannot open hole modal"),C->OpenRoomCeilingHoleSettings());
	F.Sim->TrySetRoomAirDomain(ManualRoom(),Message);
	FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.25,3); P.MaxXYM=FVector2D(-4.25,4); P.HeightM=2.625; // Existing synthetic metre fixture.
	const TArray<FRoomCeilingPlate> Before{P,P}; F.Sim->TrySetRoomCeilingPlates(Before,Message);
	F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); F.Sim->EnterLayoutMode();
	const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell;
	const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK;
	const auto Clock=F.Sim->GetSnapshot().SimTimeS; const auto Pose=F.Racks[0]->GetActorTransform(); const auto Camera=Pawn->GetActorLocation();
	const FVector Eye(-475,350,400),Down(0,0,-1);
	TestFalse(TEXT("Editing without selected plate cannot open"),C->SetRoomCeilingEditing(true) && C->OpenRoomCeilingHoleSettings());
	TestTrue(TEXT("Selected snapshot opens hole modal"),C->SelectRoomCeilingAlongRay(Eye,Down) && C->OpenRoomCeilingHoleSettings());
	TestFalse(TEXT("No second modal/room Apply"),C->OpenRoomSettings() || C->ApplyRoomSettings(ManualRoom(),Message));
	FRoomCeilingHole H;
	TestTrue(TEXT("Four strict local metre fields"),URoomSettingsWidget::ParseHoleDraft({TEXT("0.125"),TEXT("0.875"),TEXT("0.125"),TEXT("0.875")},H,Message));
	const auto Parsed=H;
	for(const auto& Bad:TArray<TArray<FString>>{{TEXT("nan"),TEXT("1"),TEXT("0"),TEXT("1")},{TEXT("0"),TEXT("1"),TEXT("0")},{TEXT("0"),TEXT("1"),TEXT("inf"),TEXT("1")}})
		TestTrue(TEXT("Invalid parsing leaves draft output"),!URoomSettingsWidget::ParseHoleDraft(Bad,H,Message) && H==Parsed && !Message.IsEmpty());
	TestTrue(TEXT("Typing/parsing is metadata-free"),F.Sim->GetRoomCeilingPlates()==Before && C->GetLayoutUndoCount()==0);
	C->ApplyLayoutCameraInput(1,FVector(1,1,1),1,true); C->BeginRackAddition();
	for(auto Key:{EKeys::R,EKeys::Delete,EKeys::D,EKeys::Z,EKeys::Y,EKeys::Enter}) { FInputKeyEventArgs A; A.Key=Key; A.Event=IE_Pressed; C->InputKey(A); }
	TestTrue(TEXT("Modal blocks world gestures/history/camera/equipment"),Pawn->GetActorLocation()==Camera && F.Racks[0]->GetActorTransform().Equals(Pose,0)
		&& !C->IsAddingRack() && !C->BeginRoomCeilingMoveDrag(Eye,Down) && !C->TryDeleteRoomCeiling() && !C->TryUndoLayout() && F.Sim->GetRoomCeilingPlates()==Before);
	auto Reject=[&](FRoomCeilingHole Bad)
	{ TestFalse(TEXT("Rejected hole Add"),C->AddRoomCeilingHole(Bad,Message)); TestTrue(TEXT("Reason/list/history/grid/clock retained"),!Message.IsEmpty() && F.Sim->GetRoomCeilingPlates()==Before && C->GetLayoutUndoCount()==0
		&& F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Clock); };
	auto Bad=H; Bad.LocalMaxXYM.X=1.01; Reject(Bad); Bad=H; Bad.LocalMaxXYM=FVector2D(.2); Reject(Bad); Bad=H; Bad.LocalMaxXYM.X=Bad.LocalMinXYM.X; Reject(Bad);
	TestTrue(TEXT("Add one hole to selected duplicate only"),C->AddRoomCeilingHole(H,Message) && F.Sim->GetRoomCeilingPlates()[0].Holes==TArray<FRoomCeilingHole>{H}
		&& F.Sim->GetRoomCeilingPlates()[1]==P && C->GetLayoutUndoCount()==1 && C->GetSelectedRoomCeilingIndex()==0);
	TestTrue(TEXT("Readout distinguishes local/world/calculation/count/other obstruction"),Message.Contains(TEXT("구멍 로컬")) && Message.Contains(TEXT("요청 월드")) && Message.Contains(TEXT("개방 계산"))
		&& Message.Contains(TEXT("4셀")) && Message.Contains(TEXT("막힐 수")) && F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask);
	FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
	TestTrue(TEXT("Esc closes numeric modal first and retains edit/selection"),!C->IsRoomSettingsOpen() && C->IsRoomCeilingEditing() && C->GetSelectedRoomCeilingIndex()==0 && C->bShowMouseCursor);
	TestFalse(TEXT("Closed modal cannot Add"),C->AddRoomCeilingHole(H,Message));
	C->ApplyLayoutCameraInput(1,FVector(1,0,0),0,true); TestTrue(TEXT("Close restores camera path"),Pawn->GetActorLocation()!=Camera);
	TestTrue(TEXT("Reopen current selection"),C->OpenRoomCeilingHoleSettings()); auto External=F.Sim->GetRoomCeilingPlates(); External.RemoveAt(0); F.Sim->TrySetRoomCeilingPlates(External,Message); C->RefreshInteractionInputMode();
	TestTrue(TEXT("External list cannot repoint open dialog"),!C->AddRoomCeilingHole(H,Message) && Message.Contains(TEXT("다시 선택")) && F.Sim->GetRoomCeilingPlates()==External && C->GetLayoutUndoCount()==1); C->CloseRoomSettings();
	C->SelectRoomCeilingAlongRay(Eye,Down); C->OpenRoomCeilingHoleSettings(); C->SetPatchMapOpen(true);
	TestTrue(TEXT("Patch closes hole modal and blocks reopening"),!C->IsRoomSettingsOpen() && !C->OpenRoomCeilingHoleSettings()); C->SetPatchMapOpen(false);
	C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(Eye,Down);
	TestTrue(TEXT("Active drag blocks hole modal"),C->BeginRoomCeilingMoveDrag(Eye,Down) && !C->OpenRoomCeilingHoleSettings()); C->EndRoomCeilingCornerDrag(false);
	C->OpenRoomCeilingHoleSettings(); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
	TestTrue(TEXT("Mode closes hole draft and refuses reopening"),!C->IsRoomSettingsOpen() && !C->OpenRoomCeilingHoleSettings());
	AddInfo(TEXT("Hole modal: four local m fields, selected duplicate only, full-list refusal/0cells, actual4cell plan h0.25m, world input gates/stale protection and modal-first Esc; no actual widget pixels/OS focus measured."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomCeilingHoleSelectionTest,"VRG.Interaction.Layout.RoomCeilingHoleSelectionDelete",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomCeilingHoleSelectionTest::RunTest(const FString& Parameters)
{
 FRoomFixture F; auto* C=F.World->SpawnActor<ADataCenterDebugController>(); C->bFrameEquipmentOnStart=false;
 auto* Pawn=F.World->SpawnActor<ADefaultPawn>(); C->Possess(Pawn); C->PlayerInput=NewObject<UPlayerInput>(C); C->RefreshInteractionInputMode();
 FString Message; F.Sim->TrySetRoomAirDomain(ManualRoom(),Message);
 FRoomCeilingPlate P; P.MinXYM=FVector2D(-5.25,3); P.MaxXYM=FVector2D(-4.25,4); P.HeightM=2.625;
 FRoomCeilingHole H; H.LocalMinXYM=FVector2D(.125); H.LocalMaxXYM=FVector2D(.875); P.Holes={H,H};
 TestTrue(TEXT("Duplicate holes fixture accepted"),F.Sim->TrySetRoomCeilingPlates({P},Message));
 F.Sim->EnterSimulationMode(); F.Sim->Tick(.021f); F.Sim->EnterLayoutMode();
 const auto Mask=F.Air->GetMutableSolver().GetGrid().bSolidCell; const auto Temps=F.Air->GetMutableSolver().GetGrid().TemperatureK; const double Clock=F.Sim->GetSnapshot().SimTimeS;
 auto Frozen=[&]() { return F.Air->GetMutableSolver().GetGrid().bSolidCell==Mask && F.Air->GetMutableSolver().GetGrid().TemperatureK==Temps && F.Sim->GetSnapshot().SimTimeS==Clock; };
 const FVector Eye(-475,350,400),Down(0,0,-1);
 TestFalse(TEXT("No parent selection blocks hole mode"),C->SetRoomCeilingHoleEditing(true));
 TestTrue(TEXT("Parent selection then separate hole mode"),C->SetRoomCeilingEditing(true) && C->SelectRoomCeilingAlongRay(Eye,Down) && C->SetRoomCeilingHoleEditing(true));
 TestFalse(TEXT("No selected hole Delete is harmless"),C->TryDeleteRoomCeilingHole());
 TestTrue(TEXT("Duplicate tie picks first actual opening"),C->SelectRoomCeilingHoleAlongRay(Eye,Down) && C->GetSelectedRoomCeilingHoleIndex()==0);
 TestTrue(TEXT("Hole owns click; cannot manipulate/delete parent"),!C->SelectRoomCeilingAlongRay(Eye,Down) && !C->BeginRoomCeilingPointerDrag(0,Eye,Down) && !C->TryDeleteRoomCeiling());
 TestTrue(TEXT("Blank clears hole only"),!C->SelectRoomCeilingHoleAlongRay(FVector(-525,300,400),Down) && C->GetSelectedRoomCeilingHoleIndex()==INDEX_NONE && C->GetSelectedRoomCeilingIndex()==0 && C->IsRoomCeilingHoleEditing());
 C->SelectRoomCeilingHoleAlongRay(Eye,Down);
 TestTrue(TEXT("Invalid ray retains selection"),!C->SelectRoomCeilingHoleAlongRay(Eye,FVector::ZeroVector) && C->GetSelectedRoomCeilingHoleIndex()==0);
 FInputKeyEventArgs Delete; Delete.Key=EKeys::Delete; Delete.Event=IE_Pressed; C->InputKey(Delete);
 TestTrue(TEXT("Delete removes one duplicate, keeps plate/mode, clears hole only"),F.Sim->GetRoomCeilingPlates()[0].Holes==TArray<FRoomCeilingHole>{H} && C->GetLayoutUndoCount()==1 && C->GetSelectedRoomCeilingIndex()==0 && C->GetSelectedRoomCeilingHoleIndex()==INDEX_NONE && C->IsRoomCeilingHoleEditing() && Frozen());
 Delete.Event=IE_Repeat; C->InputKey(Delete); TestEqual(TEXT("Held Delete does not repeat"),F.Sim->GetRoomCeilingPlates()[0].Holes.Num(),1);
 FInputKeyEventArgs Esc; Esc.Key=EKeys::Escape; Esc.Event=IE_Pressed; C->InputKey(Esc);
 TestTrue(TEXT("Esc leaves parent editing/selection intact"),!C->IsRoomCeilingHoleEditing() && C->IsRoomCeilingEditing() && C->GetSelectedRoomCeilingIndex()==0);
 TestTrue(TEXT("Plate drag blocks hole mode"),C->BeginRoomCeilingMoveDrag(Eye,Down) && !C->SetRoomCeilingHoleEditing(true) && !C->TryDeleteRoomCeilingHole()); C->EndRoomCeilingCornerDrag(false);
 C->SetRoomCeilingHoleEditing(true); C->SelectRoomCeilingHoleAlongRay(Eye,Down);
 TestTrue(TEXT("Numeric dialog ends hole picking; Delete blocked"),C->OpenRoomCeilingHoleSettings() && !C->IsRoomCeilingHoleEditing() && !C->TryDeleteRoomCeilingHole()); C->CloseRoomSettings();
 C->SetRoomCeilingHoleEditing(true); C->SelectRoomCeilingHoleAlongRay(Eye,Down);
 auto External=F.Sim->GetRoomCeilingPlates(); External[0].Holes.Add(H); F.Sim->TrySetRoomCeilingPlates(External,Message);
 TestTrue(TEXT("External list invalidates index before refresh"),C->GetSelectedRoomCeilingHoleIndex()==INDEX_NONE && !C->TryDeleteRoomCeilingHole()); C->RefreshInteractionInputMode();
 TestFalse(TEXT("Refresh ends stale edit"),C->IsRoomCeilingHoleEditing());
 // Fully open synthetic plate through the native CDU. Height derives from its actual exterior, no new physical default.
 FRoomCeilingPlate Block; Block.MinXYM=FVector2D(-5,-3); Block.MaxXYM=FVector2D(-3,-1); Block.HeightM=F.Cdu->SizeM.Z*.5;
 FRoomCeilingHole Full; Full.LocalMinXYM=FVector2D(0); Full.LocalMaxXYM=Block.MaxXYM-Block.MinXYM; Block.Holes={Full};
 TestTrue(TEXT("Open plate permits native CDU exterior"),F.Sim->TrySetRoomCeilingPlates({Block},Message));
 const FVector CduEye(-400,-200,400); C->SelectRoomCeilingAlongRay(CduEye,Down); C->SetRoomCeilingHoleEditing(true); C->SelectRoomCeilingHoleAlongRay(CduEye,Down);
 const int32 Undo=C->GetLayoutUndoCount(),Redo=C->GetLayoutRedoCount();
 TestTrue(TEXT("Refilled CDU obstruction refused atomically"),!C->TryDeleteRoomCeilingHole() && F.Sim->GetRoomCeilingPlates()==TArray<FRoomCeilingPlate>{Block} && C->GetSelectedRoomCeilingHoleIndex()==0
  && C->GetLayoutUndoCount()==Undo && C->GetLayoutRedoCount()==Redo && C->GetRoomCeilingEditMessage().Contains(TEXT("거절")) && Frozen());
 C->SetPatchMapOpen(true); TestTrue(TEXT("Patch exits hole mode and blocks Delete"),!C->IsRoomCeilingHoleEditing() && !C->TryDeleteRoomCeilingHole()); C->SetPatchMapOpen(false);
 C->SetRoomCeilingEditing(true); C->SelectRoomCeilingAlongRay(CduEye,Down); C->SetRoomCeilingHoleEditing(true); F.Sim->EnterSimulationMode(); C->RefreshInteractionInputMode();
 TestTrue(TEXT("Simulation mode ends picking and blocks Delete"),!C->IsRoomCeilingHoleEditing() && !C->TryDeleteRoomCeilingHole());
 AddInfo(TEXT("Hole selection: actual-cell slab/tie first; duplicate RemoveAt one; parent/handle/drag/modal/patch/mode gates; stale snapshot; native CDU refill rejected with history/warm grid unchanged. No UI pixels/OS focus measured."));
 return true;
}
#endif
