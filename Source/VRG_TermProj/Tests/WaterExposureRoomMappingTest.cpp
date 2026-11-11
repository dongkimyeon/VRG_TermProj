#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterExposureRoomMapping,"VRG.Water.Impact.RoomMapping",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWaterExposureRoomMapping::RunTest(const FString& Parameters)
{
 UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
 auto* Room=World->SpawnActor<ADataCenterRoomActor>();Room->bUseGjConnections=false;
 auto* Rack=World->SpawnActor<ARackThermalActor>(FVector(200,150,0),FRotator(0,90,0));Rack->ApplyGb200Nvl72Preset();Rack->LayoutId=TEXT("map-rack");Rack->bShowReadout=false;Rack->SetActorScale3D(FVector(1.1,.9,1.2));
 auto* Cdu=World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-100,150,0),FRotator::ZeroRotator);Cdu->LayoutId=TEXT("map-cdu");
 auto* Spine=World->SpawnActor<ASpineCabinetActor>(FVector(450,150,0),FRotator::ZeroRotator);Spine->LayoutId=TEXT("map-spine");
 auto* Crah=World->SpawnActor<ACrahUnitActor>(FVector(200,250,0),FRotator(0,90,0));Crah->LayoutId=TEXT("map-crah");
 World->SpawnActor<AAirFieldDebugActor>()->bShowSummary=false;
 auto* Sim=World->SpawnActor<ADataCenterSimulationActor>();Sim->bShowSummary=false;Sim->bFacilityPlant=false;Sim->FloorWaterSettings.CellM=.1;Room->RebuildRoomNow();Sim->EnterSimulationMode();
 const auto& View=Sim->GetWaterExposure();const auto& Bindings=Sim->GetWaterExposureBindings();const auto& Geometry=Sim->GetLeakSpray().GetGeometry();
 TestTrue(TEXT("Entry produces valid zero-time baseline"),Sim->IsWaterExposureCurrent() && View.SimTimeS==0 && Sim->GetWaterObservationCount()==0);
 TestTrue(TEXT("Native identities retained"),View.EquipmentId.Contains(TEXT("rack/map-rack")) && View.EquipmentId.Contains(TEXT("cdu/map-cdu")) && View.EquipmentId.Contains(TEXT("spine/map-spine")) && View.EquipmentId.Contains(TEXT("crah/map-crah")));
 int32 Slots=0;double MaxHeightError=0;
 for(const auto& B:Bindings) {
  TestEqual(TEXT("Existing source label preserved"),B.SourceName,Geometry.Obstacles[B.SprayObstacle].Name);
  if(B.FloorFlood!=INDEX_NONE) {
   // 2026-11-11: boxes outside the building map to the plant yard's records (index past the room floor's).
   const auto& RoomFloods=Sim->GetFloorWater().GetFloods();
   const auto& F=B.FloorFlood<RoomFloods.Num()?RoomFloods[B.FloorFlood]:Sim->GetYardWater().GetFloods()[B.FloorFlood-RoomFloods.Num()];
   TestEqual(TEXT("Footprint maps by obstacle, not display-name guess"),F.SourceObstacle,B.SprayObstacle);
  }
  if(B.EquipmentId==TEXT("rack/map-rack") && B.Slot!=INDEX_NONE) {
   ++Slots;double Bottom,Height;Rack->GetSlotHeightRangeM(B.Slot,Bottom,Height);
   const double Expected=FMath::Max(0.0,Rack->GetActorLocation().Z*.01+Bottom*1.2-Sim->GetFloorWater().GetFloorZM());
   if(B.FloorFlood!=INDEX_NONE) MaxHeightError=FMath::Max(MaxHeightError,FMath::Abs(Expected-B.FloorContactDepthM));
  }
 }
 TestEqual(TEXT("All native rack slots including air-cooled devices"),Slots,Rack->Params.Slots.Num());
 TestTrue(TEXT("Slot geometry matches transformed bottom"),MaxHeightError<1e-12);
 TestTrue(TEXT("Policy explicitly unset"),Sim->GetWaterObservationDiagnostic().Contains(TEXT("policy unset")));
 const auto Before=View.SprayTouchedM3;Sim->Tick(0);TestEqual(TEXT("Zero-step frame has no duplicate observation"),Sim->GetWaterObservationCount(),uint64(0));
 TestTrue(TEXT("Zero-step frame preserves baseline"),Before==Sim->GetWaterExposure().SprayTouchedM3);
 AddInfo(FString::Printf(TEXT("W4 room mapping targets=%d slots=%d max transformed contact-height error=%.17g m (normalized by 1m)"),Bindings.Num(),Slots,MaxHeightError));
 Sim->EnterLayoutMode();TestFalse(TEXT("Layout invalidates old observation"),Sim->IsWaterExposureCurrent());
 Rack->LayoutId=TEXT("replacement");Sim->EnterSimulationMode();TestTrue(TEXT("New entry rebuilds ID mapping"),Sim->GetWaterExposure().EquipmentId.Contains(TEXT("rack/replacement")) && !Sim->GetWaterExposure().EquipmentId.Contains(TEXT("rack/map-rack")));
 Sim->EnterLayoutMode();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
 // Duplicate labels do not identify an equipment footprint; explicit source index is retained.
 FSprayGeometry G;G.RoomM=FBox(FVector(0,0,0),FVector(1,1,1));
 for(double X:{.25,.75}) {auto& O=G.Obstacles.AddDefaulted_GetRef();O.Name=TEXT("same label");O.CenterM=FVector(X,.5,.1);O.HalfM=FVector(.1,.1,.1);}
 FFloorWaterSettings Settings;Settings.CellM=.1;FFloorWater Floor;Floor.ResetForRoom(G,Settings);
 TestEqual(TEXT("Two identical names remain two footprint records"),Floor.GetFloods().Num(),2);
 TestEqual(TEXT("First obstacle metadata"),Floor.GetFloods()[0].SourceObstacle,0);TestEqual(TEXT("Second obstacle metadata"),Floor.GetFloods()[1].SourceObstacle,1);
 return true;
}
#endif
