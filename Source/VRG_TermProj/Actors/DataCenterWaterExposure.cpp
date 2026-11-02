#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/GjRackLayout.h"

namespace
{
 FString WaterActorIdentity(const AActor* Actor)
 {
  if(const auto* R=Cast<ARackThermalActor>(Actor)) return TEXT("rack/")+VrgRackLifecycle::Identity(R);
  if(const auto* S=Cast<ASpineCabinetActor>(Actor)) return TEXT("spine/")+VrgSpineLifecycle::Identity(S);
  if(const auto* C=Cast<ACoolingDistributionUnitActor>(Actor)) return TEXT("cdu/")+VrgCduLifecycle::Identity(C);
  if(const auto* C=Cast<ACrahUnitActor>(Actor)) return TEXT("crah/")+VrgCrahLifecycle::Identity(C);
  if(const auto* P=Cast<APlantEquipmentActor>(Actor)) return TEXT("plant/")+VrgPlantEquipment::Identity(P);
  return FString();
 }
 FVector WaterActorSizeM(const AActor* Actor)
 {
  FVector Size=FVector::ZeroVector;
  if(const auto* R=Cast<ARackThermalActor>(Actor)) Size=R->RackSizeM;
  else if(Cast<ASpineCabinetActor>(Actor)) Size=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
  else if(const auto* C=Cast<ACoolingDistributionUnitActor>(Actor)) Size=C->SizeM;
  // Placed CRAH uses its existing planned solver box; preserve that geometry policy.
  else if(const auto* Crah=Cast<ACrahUnitActor>(Actor)) return Crah->SizeM;
  else if(const auto* P=Cast<APlantEquipmentActor>(Actor)) Size=P->SizeM;
  return Size*Actor->GetActorScale3D().GetAbs();
 }
 double WaterBoxBottomM(const FSprayObstacle& Box)
 {
  double Down=0;
  for(int32 Axis=0;Axis<3;++Axis) {FVector V=FVector::ZeroVector;V[Axis]=Box.HalfM[Axis];Down+=FMath::Abs(Box.Rotation.RotateVector(V).Z);}
  return Box.CenterM.Z-Down;
 }
 double WaterSlotBottomM(const ARackThermalActor* Rack,double BottomM,double HeightM)
 {
  // Bottom of the entire rotated/scaled slot box, not just its centre or the rack base.
  double MinZ=TNumericLimits<double>::Max();
  for(int32 X:{-1,1}) for(int32 Y:{-1,1}) for(int32 Z:{0,1}) {
   const FVector Local(X*.5*Rack->RackSizeM.X,Y*.5*Rack->RackSizeM.Y,BottomM+Z*HeightM);
   MinZ=FMath::Min(MinZ,SimUnits::UnrealToMeters(Rack->GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(Local))).Z);
  }
  return MinZ;
 }
}

void ADataCenterSimulationActor::ClearWaterObservation()
{
 WaterImpactObservation=FWaterImpactModel();WaterImpactPowered.Reset();bWaterImpactConfigured=false;
 bRackWaterTripResponse=false;WaterRackPowerCuts.Reset();
 WaterSourceTargets.Reset();WaterExposureBindings.Reset();WaterRackIds.Reset();WaterMappingDiagnostics.Reset();
 WaterExposureSnapshot=FWaterExposureSnapshot();WaterObservationCount=0;bWaterExposureCurrent=false;bWaterRegistryReady=false;
 WaterObservationDiagnostic=TEXT("Water observation requires simulation-entry source initialization");
}

void ADataCenterSimulationActor::InitializeWaterObservation()
{
 if(!bWaterExposureObservation) {ClearWaterObservation();WaterObservationDiagnostic=TEXT("Water observation disabled");return;}
 WaterImpactObservation=FWaterImpactModel();WaterImpactPowered.Reset();bWaterImpactConfigured=false;
 bRackWaterTripResponse=false;WaterRackPowerCuts.Reset();
 WaterExposureBindings.Reset();WaterMappingDiagnostics.Reset();WaterExposureSnapshot=FWaterExposureSnapshot();
 WaterObservationCount=0;bWaterExposureCurrent=false;WaterRackIds.SetNum(Racks.Num());
 for(int32 R=0;R<Racks.Num();++R) WaterRackIds[R]=IsValid(Racks[R])?WaterActorIdentity(Racks[R]):FString();
 const auto& Geometry=Spray.GetGeometry();const auto& Floods=FloorWater.GetFloods();
 for(auto& Target:WaterSourceTargets) {
  if(!Geometry.Obstacles.IsValidIndex(Target.Obstacle)) continue;
  const auto& Box=Geometry.Obstacles[Target.Obstacle];
  const int32 Flood=Floods.IndexOfByPredicate([&](const FFloorFlood& F){return F.SourceObstacle==Target.Obstacle;});
  const bool HasFloor=Flood!=INDEX_NONE && !Floods[Flood].Cells.IsEmpty();
  double BedWorldM=FloorWater.GetFloorZM();
  if(HasFloor) {double Sum=0;for(int32 Cell:Floods[Flood].Cells) Sum+=FloorWater.GetBedM()[Cell];BedWorldM+=Sum/Floods[Flood].Cells.Num();}
  auto Add=[&](int32 Slot,double BottomM) {
   auto& B=WaterExposureBindings.AddDefaulted_GetRef();B.EquipmentId=Target.Id;B.SourceName=Box.Name;B.Slot=Slot;B.SprayObstacle=Target.Obstacle;
   if(HasFloor) {B.FloorFlood=Flood;B.FloorContactDepthM=FMath::Max(0.0,BottomM-BedWorldM);}
   if(Slot!=INDEX_NONE) B.InteriorRack=Target.Rack;
  };
  Add(INDEX_NONE,WaterBoxBottomM(Box));
  if(!HasFloor) WaterMappingDiagnostics.Add({Target.Id,TEXT("No W3 footprint observation (elevated/outside floor/empty footprint); flood remains unavailable"),Target.Obstacle,0});
  Target.SlotBottomM.Reset();Target.SlotHeightM.Reset();
  if(Target.Rack!=INDEX_NONE && Racks.IsValidIndex(Target.Rack) && IsValid(Racks[Target.Rack])) {
   const ARackThermalActor* Rack=Racks[Target.Rack];
   for(int32 Slot=0;Slot<Rack->Params.Slots.Num();++Slot) {
    double Bottom,Height;Rack->GetSlotHeightRangeM(Slot,Bottom,Height);Target.SlotBottomM.Add(Bottom);Target.SlotHeightM.Add(Height);
    Add(Slot,WaterSlotBottomM(Rack,Bottom,Height));
   }
  }
 }
 bWaterRegistryReady=true;ObserveWaterExposure(Clock.GetSimTimeS(),true);
}


bool ADataCenterSimulationActor::ValidateWaterObservationTargets(FString& Reason) const
{
 auto Reject=[&](const TCHAR* Message) {Reason=Message;return false;};
 for(const auto& Target:WaterSourceTargets) {
  if(Target.bNative && (!Target.Actor.IsValid() || WaterActorIdentity(Target.Actor.Get())!=Target.Id
   || !Target.Transform.Equals(Target.Actor->GetActorTransform()) || Target.SizeM!=WaterActorSizeM(Target.Actor.Get()))) return Reject(TEXT("Equipment identity/geometry changed; re-enter simulation to rebuild water mapping"));
  if(Target.Rack!=INDEX_NONE) {
   if(!Racks.IsValidIndex(Target.Rack) || Racks[Target.Rack]!=Target.Actor.Get() || !IsValid(Racks[Target.Rack]) || Racks[Target.Rack]->Params.Slots.Num()!=Target.SlotBottomM.Num()) return Reject(TEXT("Rack/slot registry changed; water observation rejected"));
   for(int32 Slot=0;Slot<Target.SlotBottomM.Num();++Slot) {double Bottom,Height;Racks[Target.Rack]->GetSlotHeightRangeM(Slot,Bottom,Height);if(Bottom!=Target.SlotBottomM[Slot] || Height!=Target.SlotHeightM[Slot]) return Reject(TEXT("Slot geometry changed; water observation rejected"));}
  }
 }
 Reason.Reset();return true;
}

bool ADataCenterSimulationActor::ObserveWaterExposure(double EndTimeS,bool bBaseline)
{
 auto Fail=[&](const FString& Reason) {bWaterExposureCurrent=false;if(Reason!=WaterObservationDiagnostic) UE_LOG(LogTemp,Display,TEXT("[WaterExposure] %s"),*Reason);WaterObservationDiagnostic=Reason;return false;};
 if(!bWaterExposureObservation) return Fail(TEXT("Water observation disabled"));
 if(!bWaterRegistryReady) return Fail(TEXT("Water registry invalidated; re-enter simulation before observing rebuilt sources"));
 if(bRackInteriorStepRejected) return Fail(TEXT("Interior water interval rejected; exposure/state not partially published"));
 if(!bBaseline && EndTimeS<=WaterExposureSnapshot.SimTimeS) return Fail(TEXT("Duplicate/backward water observation rejected"));
 FString MappingError;if(!ValidateWaterObservationTargets(MappingError)) return Fail(MappingError);

 FWaterExposureSnapshot Next;FString Error;
 if(!WaterExposure::Collect(WaterExposureBindings,EndTimeS,Spray.GetGeometry(),Spray.GetWetting(),FloorWater.GetFloods(),RackInterior.GetWater(),WaterRackIds,Next,Error)) return Fail(Error);
 Next.Diagnostics.Append(WaterMappingDiagnostics);
 // Publish exposure and impact together. A rejected state sample cannot advance either ledger.
 FWaterImpactModel NextImpact=WaterImpactObservation;
 TArray<bool> ActualPower=WaterImpactPowered;
 if(bRackWaterTripResponse && !ActualRackWaterPower(WaterImpactPowered,ActualPower,Error)) return Fail(Error);
 if(bWaterImpactConfigured && !NextImpact.Step(Next,ActualPower,Error)) return Fail(Error);
 WaterExposureSnapshot=MoveTemp(Next);if(bWaterImpactConfigured) WaterImpactObservation=MoveTemp(NextImpact);bWaterExposureCurrent=true;if(!bBaseline) ++WaterObservationCount;
 WaterObservationDiagnostic=bWaterImpactConfigured?(bRackWaterTripResponse?TEXT("Water exposure/state current; explicit rack-only trip response enabled"):TEXT("Water exposure/state current; observational policy (no new shutdown commands)")):TEXT("Water exposure current; impact policy unset (state not evaluated)");return true;
}


bool ADataCenterSimulationActor::ConfigureWaterImpactObservation(const FWaterImpactPolicy& Policy,const TArray<bool>& Powered,FString& Reason)
{
 if(Mode!=ESimMode::Simulation || !bWaterRegistryReady || !bWaterExposureCurrent || WaterExposureSnapshot.SimTimeS!=Clock.GetSimTimeS()) {Reason=TEXT("Fresh simulation water baseline required before policy configuration");return false;}
 if(!Policy.IsValid(Reason)) return false;
 if(Policy.MaxObservationGapS<Clock.GetFixedStepS()) {Reason=TEXT("Policy observation gap is shorter than the shared fixed step");return false;}
 FWaterImpactModel Next;
 if(!Next.Reset(WaterExposureSnapshot,Powered,Policy,Reason)) return false;
 WaterImpactObservation=MoveTemp(Next);WaterImpactPowered=Powered;bWaterImpactConfigured=true;bRackWaterTripResponse=false;
 WaterObservationDiagnostic=TEXT("Explicit water-impact policy initialized; observational state only");return true;
}

bool ADataCenterSimulationActor::SetWaterImpactPowerObservation(const TArray<bool>& Powered,FString& Reason)
{
 if(!bWaterImpactConfigured || !bWaterExposureCurrent || Powered.Num()!=WaterExposureSnapshot.EquipmentId.Num()) {Reason=TEXT("Configured current target mapping and explicit power array required");return false;}
 // The existing hold policy applies this observation at the next endpoint, never retroactively.
 WaterImpactPowered=Powered;Reason.Reset();return true;
}


bool ADataCenterSimulationActor::ActualRackWaterPower(const TArray<bool>& OtherPowered,TArray<bool>& Actual,FString& Reason) const
{
 if(OtherPowered.Num()!=WaterExposureBindings.Num()) {Reason=TEXT("Explicit non-rack power observations must match the full target layout");return false;}
 if(!ValidateWaterObservationTargets(Reason)) return false;
 TArray<bool> Next=OtherPowered;
 for(int32 I=0;I<WaterExposureBindings.Num();++I) {
  const auto& B=WaterExposureBindings[I];if(B.InteriorRack==INDEX_NONE || B.Slot==INDEX_NONE) continue;
  if(!Racks.IsValidIndex(B.InteriorRack) || !IsValid(Racks[B.InteriorRack])) {Reason=TEXT("Stale rack power mapping");return false;}
  const auto* Rack=Racks[B.InteriorRack].Get();const int32 Count=FRackThermalModel::NumSlotsOf(Rack->Params);
  if(B.Slot<0 || B.Slot>=Count || !FRackThermalModel::IsSlotPowerInputValid(Rack->GetInput(),Count)) {Reason=TEXT("Invalid native slot electrical input");return false;}
  Next[I]=FRackThermalModel::SlotPoweredOf(Rack->GetInput(),B.Slot,Count) && Rack->IsSledDocked(B.Slot);
 }
 Actual=MoveTemp(Next);Reason.Reset();return true;
}

bool ADataCenterSimulationActor::ConfigureRackWaterTripResponse(const FWaterImpactPolicy& Policy,const TArray<bool>& OtherPowered,FString& Reason)
{
 TArray<bool> Actual;if(!ActualRackWaterPower(OtherPowered,Actual,Reason)) return false;
 if(!ConfigureWaterImpactObservation(Policy,Actual,Reason)) return false;
 bRackWaterTripResponse=true;WaterObservationDiagnostic=TEXT("Explicit rack water-trip response enabled; other equipment remains observation-only");return true;
}

void ADataCenterSimulationActor::RackWaterPowerBeforeStep()
{
 if(!bRackWaterTripResponse || !bWaterImpactConfigured) return;
 auto Reject=[&](const FString& Reason) {bWaterExposureCurrent=false;WaterObservationDiagnostic=Reason;};
 if(!bWaterExposureObservation || !bWaterExposureCurrent || !bWaterRegistryReady || WaterExposureSnapshot.SimTimeS!=Clock.GetSimTimeS()) {Reject(TEXT("No fresh water state for rack power application"));return;}
 TArray<bool> Actual;FString Error;if(!ActualRackWaterPower(WaterImpactPowered,Actual,Error)) {Reject(Error);return;}
 const auto& State=WaterImpactObservation.GetState();TArray<int32> Pending;
 for(int32 I=0;I<WaterExposureBindings.Num();++I) {
  const auto& B=WaterExposureBindings[I];if(B.InteriorRack==INDEX_NONE || B.Slot==INDEX_NONE) continue;
  if(State.Phase[I]!=EWaterImpactPhase::Tripped && State.Phase[I]!=EWaterImpactPhase::Damaged) continue;
  const auto* Rack=Racks[B.InteriorRack].Get();
  if(FRackThermalModel::SlotPoweredOf(Rack->GetInput(),B.Slot,FRackThermalModel::NumSlotsOf(Rack->Params))) {Pending.Add(I);Actual[I]=false;}
 }
 // Preflight the entire update before touching any actuator. No clock advance, event replay or water mutation.
 FWaterImpactModel Next=WaterImpactObservation;
 if(!Next.SetPowerAtCurrentTime(Clock.GetSimTimeS(),Actual,Error)) {Reject(Error);return;}
 for(int32 I:Pending) {
  const auto& B=WaterExposureBindings[I];auto* Rack=Racks[B.InteriorRack].Get();auto& Input=Rack->GetMutableInput();
  if(Input.SlotPowered.IsEmpty()) Input.SlotPowered.Init(true,FRackThermalModel::NumSlotsOf(Rack->Params));
  Input.SlotPowered[B.Slot]=false;
  WaterRackPowerCuts.Add({B.EquipmentId,B.Slot,State.TripS[I]>=0?State.TripS[I]:State.DamageS[I],WaterExposureSnapshot.SimTimeS,Clock.GetSimTimeS()});
 }
 WaterImpactObservation=MoveTemp(Next);WaterImpactPowered=MoveTemp(Actual);
}
