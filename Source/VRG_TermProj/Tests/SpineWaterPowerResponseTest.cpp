#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpineWaterPowerResponse,"VRG.Water.Impact.SpinePowerResponse",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::CommandletContext|EAutomationTestFlags::EngineFilter)
bool FSpineWaterPowerResponse::RunTest(const FString& Parameters)
{
 // Synthetic timing stimulus inherited from the rack/CDU response tests, NOT 스파인 failure data.
 FWaterImpactPolicy Policy;Policy.ContactSources=uint8(EWaterEvidence::Spray);Policy.SprayArrivalM3=0;
 Policy.TripPoweredContactS=.115;Policy.DamageContactAfterTripS=.1;Policy.MaxObservationGapS=1;
 Policy.WetRecovery=EWaterWetRecovery::LatchUntilRestart;Policy.Timing=EWaterContactTiming::HoldPreviousObservation;
 Policy.Basis=EWaterThresholdBasis::SyntheticTest;Policy.BasisNote=TEXT("Controlled native 스파인 trip fixture, not operating equipment failure criteria");
 double TickTrip=-1,TickApplied=-1;
 for(int32 Path=0;Path<2;++Path) {
   UWorld* W=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W);
   auto* Room=W->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
   auto* Rack=W->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->Rename(TEXT("CrahRackA"));Rack->ApplyGb200Nvl72Preset();Rack->bShowReadout=false;
   auto* Other=W->SpawnActor<ARackThermalActor>(FVector(200,450,0),FRotator(0,90,0));Other->Rename(TEXT("CrahRackB"));Other->ApplyGb200Nvl72Preset();Other->bShowReadout=false;
   W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,60,0),FRotator::ZeroRotator);
   W->SpawnActor<ACoolingDistributionUnitActor>(FVector(70,450,0),FRotator::ZeroRotator);
   auto* Wet=W->SpawnActor<ASpineCabinetActor>(FVector(400,150,0),FRotator::ZeroRotator);
   auto* Dry=W->SpawnActor<ASpineCabinetActor>(FVector(400,450,0),FRotator::ZeroRotator);
   Wet->LayoutId=TEXT("wet-spine"); Dry->LayoutId=TEXT("dry-spine");
   W->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
   auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
   Room->RebuildRoomNow(); Sim->EnterSimulationMode();
   auto Cleanup=[&]() {Sim->EnterLayoutMode();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);};
   FString Reason; TArray<bool> Powered; Powered.Init(true,Sim->GetWaterExposure().EquipmentId.Num());
   if(!TestTrue(TEXT("침수 정책 활성화"),Sim->ConfigureSpineWaterTripResponse(Policy,Powered,Reason))) {AddError(Reason); Cleanup(); return false;}
   int32 Box=INDEX_NONE,Binding=INDEX_NONE,Site=INDEX_NONE;
   for(int32 I=0;I<Sim->GetWaterExposureBindings().Num();++I) {const auto& B=Sim->GetWaterExposureBindings()[I];if(B.EquipmentId==TEXT("spine/wet-spine")) {Box=B.SprayObstacle;Binding=I;}}
   const auto& Geometry=Sim->GetLeakSpray().GetGeometry();const auto& Sites=Sim->GetLiveLeakSites();double Nearest=TNumericLimits<double>::Max();
   if(Box!=INDEX_NONE) for(int32 I=0;I<Sites.Num();++I) if(!Sites[I].bPlant && Sites[I].InternalSite==INDEX_NONE) {
    bool Inside=false;for(const auto& O:Geometry.Obstacles) {const FVector L=O.Rotation.UnrotateVector(Sites[I].PositionM-O.CenterM);Inside|=FMath::Abs(L.X)<=O.HalfM.X && FMath::Abs(L.Y)<=O.HalfM.Y && FMath::Abs(L.Z)<=O.HalfM.Z;}
    const double Dist=FVector::DistSquared(Sites[I].PositionM,Geometry.Obstacles[Box].CenterM);if(!Inside && Dist<Nearest) {Nearest=Dist;Site=I;}
   }
   if(!TestTrue(TEXT("Actual W1 jet and native 스파인 box in fluid domain"),Site!=INDEX_NONE && Box!=INDEX_NONE && Geometry.RoomM.IsInsideOrOn(Geometry.Obstacles[Box].CenterM))) {Cleanup();return false;}
   Sim->SetLiveLeakDirection(Site,Geometry.Obstacles[Box].CenterM-Sites[Site].PositionM);Sim->SetLiveLeak(Site,.005,TEXT("Synthetic 스파인 exposure fixture"));
   auto Run=[&](int32 Count) {for(int32 I=0;I<Count;++I) {if(Path==0) Sim->Tick(float(Sim->FixedStepS)); else {Sim->QueueWarmupSteps(1);Sim->Tick(0);}}};
   Run(100);
   TestFalse(TEXT("노출 스파인 차단"),Wet->IsPowered());
   TestTrue(TEXT("건조 스파인 통전"),Dry->IsPowered());
   Sim->SetLiveLeak(Site,0,TEXT("수리")); Run(150);
   TestFalse(TEXT("수리 후 차단 유지"),Wet->IsPowered());
   const auto& Cuts=Sim->GetWaterSpinePowerCuts();
   if(TestEqual(TEXT("단일 차단 기록"),Cuts.Num(),1)) {
    const auto& Cut=Cuts[0];
    TestEqual(TEXT("대상"),Cut.EquipmentId,FString(TEXT("spine/wet-spine")));
    TestTrue(TEXT("판정 뒤 한 스텝 안에 적용"),Cut.AppliedTimeS==Cut.ObservedTimeS && Cut.AppliedTimeS>=Cut.TripTimeS && Cut.AppliedTimeS-Cut.TripTimeS<=Sim->FixedStepS+1e-12);
    if(Path==0) {TickTrip=Cut.TripTimeS; TickApplied=Cut.AppliedTimeS;}
    else {TestEqual(TEXT("판정 시각 일치"),Cut.TripTimeS,TickTrip);TestEqual(TEXT("차단 시각 일치"),Cut.AppliedTimeS,TickApplied);}
   }
   Sim->EnterLayoutMode(); Sim->EnterSimulationMode();
   TestTrue(TEXT("재시작 통전"),Wet->IsPowered() && Dry->IsPowered());
   TestEqual(TEXT("재시작 사건 초기화"),Sim->GetWaterSpinePowerCuts().Num(),0);
   Cleanup();
 }
 return true;
}
#endif
