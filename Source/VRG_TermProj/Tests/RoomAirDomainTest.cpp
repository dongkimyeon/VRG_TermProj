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
	TestFalse(TEXT("Invalid corner/parallel start do not start gesture"),C->BeginRoomCeilingCornerDrag(5,Eye,Down) || C->BeginRoomCeilingCornerDrag(0,Eye,FVector(1,0,0)));
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
#endif
