#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// W4 game path (vrg.Water.Impact): console -> estimated policy -> real water -> power cut -> event, report and fault records.
// Trip/damage durations given on the console here are shorter than the estimates only to keep the runs short.
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
 constexpr EAutomationTestFlags GameImpactFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter;
 struct FImpactRoom
 {
  UWorld* World=nullptr;ADataCenterRoomActor* Room=nullptr;ADataCenterSimulationActor* Sim=nullptr;
  ARackThermalActor* RackA=nullptr;ARackThermalActor* RackB=nullptr;ACoolingDistributionUnitActor* CduA=nullptr;ACrahUnitActor* Crah=nullptr;ASpineCabinetActor* Spine=nullptr;
  explicit FImpactRoom(int32 Variant)
  {
   World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
   Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
   RackA=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));RackA->ApplyGb200Nvl72Preset();RackA->LayoutId=TEXT("game-A");RackA->bShowReadout=false;
   RackB=World->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));RackB->ApplyGb200Nvl72Preset();RackB->LayoutId=TEXT("game-B");RackB->bShowReadout=false;
   CduA=World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);
   World->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);
   if(Variant==1) {
    Crah=World->SpawnActor<ACrahUnitActor>(FVector(400,150,0),FRotator(0,180,0));Crah->LayoutId=TEXT("game-crah");Crah->Kind=ECrahUnitKind::InRow;Crah->bRuntimeLayoutCrah=true;
    auto* Dry=World->SpawnActor<ACrahUnitActor>(FVector(400,450,0),FRotator(0,180,0));Dry->LayoutId=TEXT("game-dry-crah");Dry->bRuntimeLayoutCrah=true;
   }
   if(Variant==2) {Spine=World->SpawnActor<ASpineCabinetActor>(FVector(315,70,0),FRotator::ZeroRotator);Spine->LayoutId=TEXT("game-spine");}
   World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
   Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->MaxStepsPerFrame=16;
   Room->RebuildRoomNow();
  }
  ~FImpactRoom() {Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);}
  void Console(const TCHAR* Command) {GEngine->Exec(World,Command);}
  void Run(int32 Steps) {const double End=Sim->GetSnapshot().SimTimeS+Steps*Sim->FixedStepS;for(int32 I=0;I<Steps+4 && Sim->GetSnapshot().SimTimeS<End-1e-9;++I) Sim->Tick(float(Sim->FixedStepS));}
  void AimOutsideLeakAt(const FString& Id,double HoleM)
  {
   // The nearest room leak site that is not inside any box, aimed at the box centre: the real W1 -> W2 path.
   int32 Box=INDEX_NONE;for(const auto& B:Sim->GetWaterExposureBindings()) if(B.EquipmentId==Id && B.Slot==INDEX_NONE) Box=B.SprayObstacle;
   const auto& Geometry=Sim->GetLeakSpray().GetGeometry();const auto& Sites=Sim->GetLiveLeakSites();
   int32 Site=INDEX_NONE;double Nearest=TNumericLimits<double>::Max();
   if(Box==INDEX_NONE) return;
   for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {
    bool bInside=false;
    for(const auto& O:Geometry.Obstacles) {const FVector L=O.Rotation.UnrotateVector(Sites[I].PositionM-O.CenterM);bInside|=FMath::Abs(L.X)<=O.HalfM.X && FMath::Abs(L.Y)<=O.HalfM.Y && FMath::Abs(L.Z)<=O.HalfM.Z;}
    const double D=FVector::Dist(Sites[I].PositionM,Geometry.Obstacles[Box].CenterM);
    if(!bInside && D<Nearest) {Nearest=D;Site=I;}
   }
   if(Site==INDEX_NONE) return;
   Sim->SetLiveLeakDirection(Site,Geometry.Obstacles[Box].CenterM-Sites[Site].PositionM);Sim->SetLiveLeak(Site,HoleM,TEXT("W4 game path"));
  }
 };
 void CheckLedgers(FAutomationTestBase& T,const FImpactRoom& R,const TCHAR* What)
 {
  const auto Snap=R.Sim->GetSnapshot();
  T.TestTrue(FString::Printf(TEXT("%s: heat ledger"),What),FMath::Abs(Snap.NormalizedEnergyResidual)<1e-9);
  T.TestTrue(FString::Printf(TEXT("%s: hydraulics and air converged"),What),Snap.bHydraulicsConverged && Snap.bAirPressureConverged);
  T.TestTrue(FString::Printf(TEXT("%s: spray and floor volume"),What),R.Sim->GetLeakSpray().GetTotals().Residual<1e-12 && R.Sim->GetFloorWater().GetTotals().Residual<1e-12);
  T.TestTrue(FString::Printf(TEXT("%s: interior water volume"),What),FMath::Abs(R.Sim->GetRackInteriorCoupling().GetWater().BalanceErrorM3())<1e-15);
  for(double K:Snap.RackAirInletTempK) T.TestTrue(FString::Printf(TEXT("%s: inlet plausibility"),What),K>288.15 && K<308.15);
 }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactConsolePath,"VRG.Water.Impact.GameConsolePath",GameImpactFlags)
bool FWaterImpactConsolePath::RunTest(const FString& Parameters)
{
 FImpactRoom R(0);auto* Sim=R.Sim;
 Sim->EnterSimulationMode();
 TestFalse(TEXT("Default off: no policy after entry"),Sim->HasWaterImpactPolicy());
 TestFalse(TEXT("Default off: no response"),Sim->IsRackWaterTripResponseEnabled() || Sim->IsCduWaterTripResponseEnabled() || Sim->IsSpineWaterTripResponseEnabled());
 const auto Default=WaterImpactEstimates::MakePolicy();FString Error;
 TestTrue(TEXT("Estimated policy valid"),Default.IsValid(Error));TestTrue(TEXT("Marked as an estimate"),Default.Basis==EWaterThresholdBasis::UncalibratedEstimate);
 TestFalse(TEXT("Rear water is not a contact source"),(Default.ContactSources&uint8(EWaterEvidence::Rear))!=0);
 R.Console(TEXT("vrg.Water.Impact 1 0.3 0.4"));
 TestTrue(TEXT("Console applies at once in a run"),Sim->HasWaterImpactPolicy() && Sim->IsRackWaterTripResponseEnabled() && Sim->IsCduWaterTripResponseEnabled() && Sim->IsSpineWaterTripResponseEnabled());
 R.Console(TEXT("vrg.Water.Impact 1 -1 5"));
 TestEqual(TEXT("Bad duration rejected, previous kept"),Sim->WaterImpactRequest.TripS,.3);
 const auto Exposure=[&]()->const FWaterExposureSnapshot& {return Sim->GetWaterExposure();};

 for(int32 Run=0;Run<2;++Run) {
  // Run 0 is the first entry above; run 1 is the R-restart path, the request must survive it with a clean state.
  if(Run==1) {
   Sim->EnterSimulationMode();
   TestTrue(TEXT("Request survives restart"),Sim->HasWaterImpactPolicy() && Sim->IsRackWaterTripResponseEnabled());
   for(bool P:R.RackA->GetInput().SlotPowered) TestTrue(TEXT("Restart restores power"),P);
   for(EWaterImpactPhase P:Sim->GetWaterImpactObservation().GetState().Phase) TestTrue(TEXT("Restart clears states"),P==EWaterImpactPhase::Normal);
   TestEqual(TEXT("Restart clears records"),Sim->GetFaultMonitor().GetWaterImpacts().Num(),0);
  }
  const auto H=*R.Room->GetInRackHandles(R.RackA);const int32 Slot=H.Slots[0];
  TestTrue(TEXT("Plate leak"),R.Room->SetInternalLeakHoleDiameterM(R.RackA,0,2,.001));
  R.Run(150);
  CheckLedgers(*this,R,TEXT("console path"));
  const auto& M=Sim->GetFaultMonitor();
  const FVrgWaterImpactRecord* Trip=M.GetWaterImpacts().FindByPredicate([&](const FVrgWaterImpactRecord& X){return X.Slot==Slot && X.To==TEXT("tripped");});
  const FVrgWaterImpactRecord* Damage=M.GetWaterImpacts().FindByPredicate([&](const FVrgWaterImpactRecord& X){return X.Slot==Slot && X.To==TEXT("damaged");});
  if(!TestNotNull(TEXT("Trip record"),Trip) || !TestNotNull(TEXT("Damage record"),Damage)) return false;
  TestEqual(TEXT("Record names the rack"),Trip->EquipmentId,FString(TEXT("rack/game-A")));TestTrue(TEXT("Record names the water"),Trip->Cause.Contains(TEXT("interior")));TestFalse(TEXT("Record names the device"),Trip->Device.IsEmpty());
  TestFalse(TEXT("Slot actually off"),R.RackA->GetInput().SlotPowered[Slot]);
  TestTrue(TEXT("Cut applied at the next shared step"),Trip->CutAppliedS>=Trip->TimeS && Trip->CutAppliedS-Trip->TimeS<=Sim->FixedStepS+1e-12);
  TestTrue(TEXT("IT heat drops with the cut"),Trip->ItAfterW<Trip->ItBeforeW);
  const auto* Fault=M.GetFaults().FindByPredicate([](const FVrgFaultRecord& F){return F.Kind==EVrgFaultKind::WaterTrip;});
  if(TestNotNull(TEXT("Fault monitor names flooding"),Fault)) {TestEqual(TEXT("Fault at the trip time"),Fault->TimeS,Trip->TimeS);TestEqual(TEXT("Fault slot"),int32(Fault->Value),Slot);}
  TestNotNull(TEXT("Fault monitor names damage"),M.GetFaults().FindByPredicate([](const FVrgFaultRecord& F){return F.Kind==EVrgFaultKind::WaterDamage;}));
  // Whole-rack box has no actuator of its own: it never pretends to trip.
  const auto& X=Exposure();const auto& Phase=Sim->GetWaterImpactObservation().GetState().Phase;
  for(int32 I=0;I<X.EquipmentId.Num();++I) {
   if(X.Slot[I]==INDEX_NONE && X.EquipmentId[I].StartsWith(TEXT("rack/"))) TestTrue(TEXT("Rack box stays Normal"),Phase[I]==EWaterImpactPhase::Normal);
   if(X.EquipmentId[I]==TEXT("rack/game-B")) TestTrue(TEXT("Other rack stays Normal"),Phase[I]==EWaterImpactPhase::Normal);
  }
  for(bool P:R.RackB->GetInput().SlotPowered) TestTrue(TEXT("Other rack keeps power"),P);
  // Repairing the hole does not bring it back.
  R.Room->SetInternalLeakHoleDiameterM(R.RackA,0,2,0);R.Run(10);
  TestFalse(TEXT("Repair keeps the cut"),R.RackA->GetInput().SlotPowered[Slot]);
  TestTrue(TEXT("Report keeps the records through JSON"),[&]{FVrgFaultMonitor Back;return FVrgFaultMonitor::FromJsonObject(M.ToJsonObject(),Back) && Back==M;}());
  AddInfo(FString::Printf(TEXT("W4 console run%d trip=%.17g s damage=%.17g s applied=%.17g s IT=%.17g -> %.17g W cause=%s device=%s records=%d"),Run,Trip->TimeS,Damage->TimeS,Trip->CutAppliedS,Trip->ItBeforeW,Trip->ItAfterW,*Trip->Cause,*Trip->Device,M.GetWaterImpacts().Num()));
 }
 // Off during a run stops new judgements, what is cut stays cut until the restart.
 const auto H=*R.Room->GetInRackHandles(R.RackA);
 R.Console(TEXT("vrg.Water.Impact 0"));
 TestFalse(TEXT("Off stops the policy"),Sim->HasWaterImpactPolicy());
 const int32 Records=Sim->GetFaultMonitor().GetWaterImpacts().Num();
 TestTrue(TEXT("Second plate leak"),R.Room->SetInternalLeakHoleDiameterM(R.RackB,0,2,.001));R.Run(50);
 TestEqual(TEXT("Off: no new records"),Sim->GetFaultMonitor().GetWaterImpacts().Num(),Records);
 TestFalse(TEXT("Off keeps the earlier cut"),R.RackA->GetInput().SlotPowered[H.Slots[0]]);
 for(bool P:R.RackB->GetInput().SlotPowered) TestTrue(TEXT("Off: a wet rack keeps power"),P);
 Sim->EnterSimulationMode();
 TestFalse(TEXT("Restart with the request off"),Sim->HasWaterImpactPolicy());
 for(bool P:R.RackA->GetInput().SlotPowered) TestTrue(TEXT("Restart restores power"),P);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactAllEquipment,"VRG.Water.Impact.GameAllEquipment",GameImpactFlags)
bool FWaterImpactAllEquipment::RunTest(const FString& Parameters)
{
 // One real 5 mm W1 leak aimed at each kind of equipment; the same console policy in every case.
 for(int32 Case=0;Case<4;++Case) {
  FImpactRoom R(Case==2?1:Case==3?2:0);auto* Sim=R.Sim;
  R.Console(TEXT("vrg.Water.Impact 1 0.2 100"));   // before entry: applied at the entry
  Sim->EnterSimulationMode();
  TestTrue(TEXT("Request applied at entry"),Sim->HasWaterImpactPolicy());
  const FString Id=Case==0?TEXT("rack/game-A"):Case==1?Sim->GetWaterExposureBindings().FindByPredicate([](const FWaterExposureBinding& B){return B.EquipmentId.StartsWith(TEXT("cdu/"));})->EquipmentId:Case==2?TEXT("crah/game-crah"):TEXT("spine/game-spine");
  if(Case==2) TestTrue(TEXT("Native CRAH response"),Sim->IsCrahWaterTripResponseEnabled());
  else {TestFalse(TEXT("Virtual CRAH has no response"),Sim->IsCrahWaterTripResponseEnabled());TestTrue(TEXT("Virtual CRAHs listed, not faked"),Sim->GetWaterImpactNoActuator().ContainsByPredicate([](const FString& S){return S.Contains(TEXT("CRAH")) || S.StartsWith(TEXT("crah"));}));}
  const auto Before=Sim->GetSnapshot();
  R.AimOutsideLeakAt(Id,.005);
  auto Cuts=[&]{return Sim->GetWaterRackPowerCuts().Num()+Sim->GetWaterCduPowerCuts().Num()+Sim->GetWaterCrahPowerCuts().Num()+Sim->GetWaterSpinePowerCuts().Num();};
  for(int32 I=0;I<200 && Cuts()==0;++I) R.Run(5);
  R.Run(25);
  if(!TestTrue(FString::Printf(TEXT("Case %d: water cut something"),Case),Cuts()>0)) continue;
  CheckLedgers(*this,R,*Id);
  const auto& Records=Sim->GetFaultMonitor().GetWaterImpacts();
  const auto* Trip=Records.FindByPredicate([&](const FVrgWaterImpactRecord& X){return X.EquipmentId==Id && X.To==TEXT("tripped");});
  if(!TestNotNull(TEXT("Trip record for the aimed equipment"),Trip)) continue;
  TestTrue(TEXT("Cause is the spray"),Trip->Cause.Contains(TEXT("spray")));TestTrue(TEXT("Cut applied"),Trip->CutAppliedS>=0);
  const auto Snap=Sim->GetSnapshot();
  if(Case==0) {
   // Only slots whose height band the jet actually hit.
   const auto& E=Sim->GetWaterExposure();const auto& P=Sim->GetWaterImpactObservation().GetState().Phase;int32 Wet=0,Dry=0;double ZoneSum=0;
   for(int32 I=0;I<E.EquipmentId.Num();++I) if(E.EquipmentId[I]==Id && E.Slot[I]!=INDEX_NONE) {
    ZoneSum+=E.SprayTouchedM3[I];
    if(E.SprayTouchedM3[I]>0) ++Wet;
    else {++Dry;TestTrue(TEXT("A slot the jet missed stays Normal"),P[I]==EWaterImpactPhase::Normal);TestTrue(TEXT("A slot the jet missed keeps power"),R.RackA->GetInput().SlotPowered[E.Slot[I]]);}
   }
   TestTrue(TEXT("Some slots hit, some not"),Wet>0 && Dry>0);
   TestFalse(TEXT("The tripped slot is off"),R.RackA->GetInput().SlotPowered[Trip->Slot]);
   AddInfo(FString::Printf(TEXT("W4 rack spray wet slots=%d dry=%d slot sum=%.17g m3"),Wet,Dry,ZoneSum));
  }
  if(Case==1) TestEqual(TEXT("A CDU cut leaves IT alone"),Trip->ItBeforeW,Trip->ItAfterW);
  if(Case==2) {TestFalse(TEXT("CRAH actually off"),R.Crah->IsPowered());TestEqual(TEXT("One CRAH cut"),Sim->GetWaterCrahPowerCuts().Num(),1);}
  if(Case==3) {TestFalse(TEXT("Spine actually off"),R.Spine->IsPowered());TestEqual(TEXT("One spine cut"),Sim->GetWaterSpinePowerCuts().Num(),1);}
  for(bool P:R.RackB->GetInput().SlotPowered) TestTrue(TEXT("The dry rack keeps power"),P);
  AddInfo(FString::Printf(TEXT("W4 case %s trip=%.17g s applied=%.17g s spray=%.17g m3 IT=%.17g -> %.17g W, start IT=%.17g W, now %.17g W, records=%d, no actuator=%d"),*Id,Trip->TimeS,Trip->CutAppliedS,Trip->SprayM3,Trip->ItBeforeW,Trip->ItAfterW,Before.TotalItHeatW,Snap.TotalItHeatW,Records.Num(),Sim->GetWaterImpactNoActuator().Num()));
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactSamePaths,"VRG.Water.Impact.GameTickAndRunSteps",GameImpactFlags)
bool FWaterImpactSamePaths::RunTest(const FString& Parameters)
{
 // The same console run through the two step loops gives the same state changes at the same times.
 TArray<FVrgWaterImpactRecord> First;TArray<double> FirstTemp;
 for(int32 Path=0;Path<2;++Path) {
  FImpactRoom R(0);auto* Sim=R.Sim;
  R.Console(TEXT("vrg.Water.Impact 1 0.3 0.4"));Sim->EnterSimulationMode();
  TestTrue(TEXT("Plate leak"),R.Room->SetInternalLeakHoleDiameterM(R.RackA,0,2,.001));
  if(Path==0) R.Run(100);
  else {Sim->QueueWarmupSteps(100);for(int32 I=0;I<40 && Sim->GetSnapshot().SimTimeS<2-1e-9;++I) Sim->Tick(0.f);}
  TestEqual(TEXT("Same time reached"),Sim->GetSnapshot().SimTimeS,Sim->FixedStepS*100,1e-9);
  const auto& Records=Sim->GetFaultMonitor().GetWaterImpacts();
  TestTrue(TEXT("Something happened"),Records.Num()>0);
  if(Path==0) {First=Records;FirstTemp=R.RackA->GetModel().GetState().ComponentTempK;}
  else {
   TestTrue(TEXT("Same records (times, causes, IT)"),First==Records);
   TestTrue(TEXT("Same temperatures"),FirstTemp==R.RackA->GetModel().GetState().ComponentTempK);
  }
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactProtectionFirst,"VRG.Water.Impact.GameProtectionFirst",GameImpactFlags)
bool FWaterImpactProtectionFirst::RunTest(const FString& Parameters)
{
 // W5 and W4 both on: a protective shutdown that comes first leaves a wet, unpowered tray with no water fault (causes stay apart).
 // Trip 4 s against the tray detector (about 2 s) + 0.5 s timer; the estimated 15 s vs 10 s + 2 s keeps the same order.
 FImpactRoom R(0);auto* Sim=R.Sim;
 R.Console(TEXT("vrg.Water.Impact 1 4 100"));R.Console(TEXT("vrg.Leak.AutoIsolate 1 0.5"));
 Sim->EnterSimulationMode();
 const auto H=*R.Room->GetInRackHandles(R.RackA);const int32 Slot=H.Slots[0];
 TestTrue(TEXT("Plate leak"),R.Room->SetInternalLeakHoleDiameterM(R.RackA,0,2,.001));
 R.Run(300);
 CheckLedgers(*this,R,TEXT("protection first"));
 TestFalse(TEXT("Protection cut the tray"),R.RackA->GetInput().SlotPowered[Slot]);
 TestNotNull(TEXT("Protection event"),Sim->GetLeakProtectionEvents().FindByPredicate([&](const auto& E){return E.Rack==0 && E.Slot==Slot && E.Action==TEXT("slot power off");}));
 for(const auto& X:Sim->GetFaultMonitor().GetWaterImpacts()) if(X.Slot==Slot) TestEqual(TEXT("The unpowered tray only gets wet"),X.To,FString(TEXT("wet")));
 TestNull(TEXT("No water fault for it"),Sim->GetFaultMonitor().GetFaults().FindByPredicate([&](const FVrgFaultRecord& F){return F.Kind==EVrgFaultKind::WaterTrip && int32(F.Value)==Slot;}));
 R.Console(TEXT("vrg.Leak.AutoIsolate 0"));
 return true;
}
#endif
