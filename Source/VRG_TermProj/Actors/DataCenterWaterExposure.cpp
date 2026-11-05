#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/GjRackLayout.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
 FAutoConsoleCommandWithWorldAndArgs GVrgWaterImpact(TEXT("vrg.Water.Impact"),
  TEXT("침수 영향(W4, 기본 0=끔, 문턱은 추정). 켜면 젖은 랙 슬롯·CDU·CRAH·스파인이 트립·손상되어 전원이 끊기고 R 재시작까지 유지. vrg.Water.Impact 0|1 [트립 s] [손상 s], 인자 없으면 상태"),
  FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args,UWorld* World)
  {
   if(!World) return;
   for(TActorIterator<ADataCenterSimulationActor> It(World);It;++It) {
    if(Args.Num()==0) {UE_LOG(LogTemp,Display,TEXT("[WaterImpact] %s"),*It->DescribeWaterImpact());continue;}
    auto Request=It->WaterImpactRequest;Request.bEnabled=FCString::Atoi(*Args[0])!=0;
    if(Args.Num()>1) Request.TripS=FCString::Atod(*Args[1]);
    if(Args.Num()>2) Request.DamageS=FCString::Atod(*Args[2]);
    FString Reason;const bool bOk=It->SetWaterImpactRequest(Request,Reason);
    UE_LOG(LogTemp,Display,TEXT("[WaterImpact] %s%s%s"),*It->DescribeWaterImpact(),bOk?TEXT(""):TEXT(" 거절: "),bOk?TEXT(""):*Reason);
   }
  }));
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
 WaterImpactEventCursor=0;WaterImpactTargetRecord.Reset();WaterCutsAwaitingIt.Reset();WaterImpactNoActuator.Reset();
 WaterImpactObservation=FWaterImpactModel();WaterImpactPowered.Reset();bWaterImpactConfigured=false;
 WaterSpinePowerCuts.Reset();
 bSpineWaterTripResponse=bRackWaterTripResponse=bCduWaterTripResponse=bCrahWaterTripResponse=false;WaterRackPowerCuts.Reset();WaterCduPowerCuts.Reset();WaterCrahPowerCuts.Reset();
 WaterSourceTargets.Reset();WaterExposureBindings.Reset();WaterRackIds.Reset();WaterMappingDiagnostics.Reset();
 WaterExposureSnapshot=FWaterExposureSnapshot();WaterObservationCount=0;bWaterExposureCurrent=false;bWaterRegistryReady=false;
 WaterObservationDiagnostic=TEXT("Water observation requires simulation-entry source initialization");
}

void ADataCenterSimulationActor::InitializeWaterObservation()
{
 if(!bWaterExposureObservation) {ClearWaterObservation();WaterObservationDiagnostic=TEXT("Water observation disabled");return;}
 WaterImpactEventCursor=0;WaterImpactTargetRecord.Reset();WaterCutsAwaitingIt.Reset();WaterImpactNoActuator.Reset();
 WaterImpactObservation=FWaterImpactModel();WaterImpactPowered.Reset();bWaterImpactConfigured=false;
 WaterSpinePowerCuts.Reset();
 bSpineWaterTripResponse=bRackWaterTripResponse=bCduWaterTripResponse=bCrahWaterTripResponse=false;WaterRackPowerCuts.Reset();WaterCduPowerCuts.Reset();WaterCrahPowerCuts.Reset();
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
 // vrg.Water.Impact outlives a restart: the new run starts from t=0 with every target Normal and powered.
 if(WaterImpactRequest.bEnabled) {FString Reason;if(!ApplyWaterImpactRequest(Reason)) UE_LOG(LogTemp,Warning,TEXT("[WaterImpact] 침수 영향을 켜지 못함: %s"),*Reason);}
}

bool ADataCenterSimulationActor::ApplyWaterImpactRequest(FString& Reason)
{
 if(WaterImpactRequest.TripS<=0 || WaterImpactRequest.DamageS<=0) {Reason=TEXT("Trip and damage durations must be positive");return false;}
 FString CrahReason;const bool bCrah=ValidateCrahPowerTargets(CrahReason);
 FWaterImpactPolicy Policy=WaterImpactEstimates::MakePolicy(WaterImpactRequest.TripS,WaterImpactRequest.DamageS);
 // A target nothing can switch off stays Normal: a "tripped" entry with the equipment still running would be false.
 // That covers whole-rack boxes (their slots are the targets), outdoor yard equipment (plan L4) and virtual automatic CRAHs.
 TArray<FString> NoActuator;Policy.ExemptTargets.Init(0,WaterExposureBindings.Num());
 for(int32 I=0;I<WaterExposureBindings.Num();++I) {
  const auto& B=WaterExposureBindings[I];
  const bool bActuator=B.Slot!=INDEX_NONE?B.InteriorRack!=INDEX_NONE:(WaterBindingCdu(I)!=nullptr || WaterBindingSpine(I)!=nullptr || (bCrah && WaterBindingCrah(I)!=nullptr));
  if(bActuator) continue;
  Policy.ExemptTargets[I]=1;
  if(!B.EquipmentId.StartsWith(TEXT("rack/"))) NoActuator.AddUnique(B.EquipmentId);
 }
 TArray<bool> Power;Power.Init(true,WaterExposureBindings.Num());
 if(!ConfigureSpineWaterTripResponse(Policy,Power,Reason,true,true,bCrah)) return false;
 WaterImpactPolicyInUse=MoveTemp(Policy);WaterImpactNoActuator=MoveTemp(NoActuator);
 WaterImpactEventCursor=0;WaterImpactTargetRecord.Reset();WaterCutsAwaitingIt.Reset();
 WaterObservationDiagnostic=TEXT("vrg.Water.Impact: estimated policy, every available power response enabled");
 UE_LOG(LogTemp,Display,TEXT("[WaterImpact] 침수 영향 켬(문턱 추정): 트립 %.1f s, 손상 %.1f s, 대상 %d개, 차단 수단 없어 제외 %d개"),WaterImpactRequest.TripS,WaterImpactRequest.DamageS,WaterExposureBindings.Num(),WaterImpactNoActuator.Num());
 return true;
}

bool ADataCenterSimulationActor::SetWaterImpactRequest(const FWaterImpactRequest& Request,FString& Reason)
{
 if(Request.bEnabled && (!FMath::IsFinite(Request.TripS) || !FMath::IsFinite(Request.DamageS) || Request.TripS<=0 || Request.DamageS<=0)) {Reason=TEXT("Trip and damage durations must be positive");return false;}
 WaterImpactRequest=Request;Reason.Reset();
 if(Mode!=ESimMode::Simulation || !bWaterRegistryReady) return true; // applied at the next simulation entry
 if(Request.bEnabled) return ApplyWaterImpactRequest(Reason);
 // Off stops new judgements only: what is already cut stays cut until the R restart, and its records stay.
 bWaterImpactConfigured=bRackWaterTripResponse=bCduWaterTripResponse=bCrahWaterTripResponse=bSpineWaterTripResponse=false;
 WaterCutsAwaitingIt.Reset();WaterObservationDiagnostic=TEXT("vrg.Water.Impact off");
 return true;
}

FString ADataCenterSimulationActor::DescribeWaterImpact() const
{
 int32 Counts[4]={0,0,0,0};
 for(EWaterImpactPhase P:WaterImpactObservation.GetState().Phase) ++Counts[int32(P)];
 return FString::Printf(TEXT("요청 %s(트립 %.1f s, 손상 %.1f s, 문턱 추정), 적용 %s, 젖음 %d, 트립 %d, 손상 %d, 차단 랙 슬롯 %d·CDU %d·CRAH %d·스파인 %d. %s"),
  WaterImpactRequest.bEnabled?TEXT("켬"):TEXT("끔"),WaterImpactRequest.TripS,WaterImpactRequest.DamageS,bWaterImpactConfigured?TEXT("중"):TEXT("안 함"),Counts[1],Counts[2],Counts[3],
  WaterRackPowerCuts.Num(),WaterCduPowerCuts.Num(),WaterCrahPowerCuts.Num(),WaterSpinePowerCuts.Num(),*WaterObservationDiagnostic);
}

void ADataCenterSimulationActor::WaterImpactAfterStep()
{
 // The whole-site IT heat around a cut: read at the start of the step that applied it and at the end of that step.
 if(!WaterCutsAwaitingIt.IsEmpty()) {
  const double ItAfterW=GetSnapshot().TotalItHeatW;
  for(int32 Index:WaterCutsAwaitingIt) {
   FaultMonitor.SetWaterImpactCut(Index,WaterCutAppliedS,WaterCutItBeforeW,ItAfterW);
   const auto& R=FaultMonitor.GetWaterImpacts()[Index];
   UE_LOG(LogTemp,Warning,TEXT("[WaterImpact] t=%.2f s %s 슬롯 %d(%s) 전원 차단 적용, 그 스텝 전체 IT %.1f → %.1f kW"),WaterCutAppliedS,*R.EquipmentId,R.Slot,*R.Device,WaterCutItBeforeW*1e-3,ItAfterW*1e-3);
  }
  WaterCutsAwaitingIt.Reset();
 }
 if(!bWaterImpactConfigured) return;
 const auto& Events=WaterImpactObservation.GetEvents();
 for(;WaterImpactEventCursor<Events.Num();++WaterImpactEventCursor) {
  const auto& E=Events[WaterImpactEventCursor];
  if(!WaterExposureBindings.IsValidIndex(E.Target) || !WaterExposureSnapshot.EquipmentId.IsValidIndex(E.Target)) continue;
  const auto& B=WaterExposureBindings[E.Target];const auto& X=WaterExposureSnapshot;
  FVrgWaterImpactRecord R;
  R.TimeS=E.SimTimeS;R.EquipmentId=B.EquipmentId;R.Rack=B.Slot!=INDEX_NONE?B.InteriorRack:INDEX_NONE;R.Slot=B.Slot;
  if(R.Rack!=INDEX_NONE && Racks.IsValidIndex(R.Rack) && IsValid(Racks[R.Rack]) && Racks[R.Rack]->Params.Slots.IsValidIndex(R.Slot)) {
   const auto& P=Racks[R.Rack]->Params;const int32 Type=P.Slots[R.Slot].DeviceType;
   if(P.DeviceTypes.IsValidIndex(Type)) R.Device=P.DeviceTypes[Type].Name.ToString();
  }
  R.From=WaterImpactPhaseName(E.From);R.To=WaterImpactPhaseName(E.To);R.Cause=WaterEvidenceNames(E.Cause);
  R.ImmersionM=X.FloodImmersionM[E.Target];R.SprayM3=X.SprayTouchedM3[E.Target];R.InteriorM3=X.InteriorPassedM3[E.Target]-X.RearPassedM3[E.Target];
  const int32 Index=FaultMonitor.AddWaterImpact(R);
  if((E.To==EWaterImpactPhase::Tripped || E.To==EWaterImpactPhase::Damaged) && !WaterImpactTargetRecord.Contains(E.Target)) WaterImpactTargetRecord.Add(E.Target,Index);
  UE_LOG(LogTemp,Display,TEXT("[WaterImpact] t=%.2f s %s 슬롯 %d(%s) %s → %s, 원인 %s, 침수 %.1f mm, 분사 %.1f mL, 내부 %.1f mL (문턱 추정)"),
   R.TimeS,*R.EquipmentId,R.Slot,*R.Device,*R.From,*R.To,*R.Cause,R.ImmersionM*1e3,R.SprayM3*1e6,R.InteriorM3*1e6);
 }
}

TSharedRef<FJsonObject> ADataCenterSimulationActor::MakeWaterImpactReportJson() const
{
 auto Root=MakeShared<FJsonObject>();
 Root->SetBoolField(TEXT("enabled"),WaterImpactRequest.bEnabled);Root->SetBoolField(TEXT("applied"),bWaterImpactConfigured);
 Root->SetBoolField(TEXT("thresholdsEstimated"),true);Root->SetStringField(TEXT("basis"),WaterImpactEstimates::MakePolicy().BasisNote);
 const FWaterImpactPolicy Shown=WaterImpactEstimates::MakePolicy(WaterImpactRequest.TripS,WaterImpactRequest.DamageS);
 Root->SetNumberField(TEXT("tripPoweredContactS"),Shown.TripPoweredContactS);Root->SetNumberField(TEXT("damageContactAfterTripS"),Shown.DamageContactAfterTripS);
 Root->SetNumberField(TEXT("damageCumulativeM3"),Shown.DamageCumulativeM3);Root->SetNumberField(TEXT("sprayArrivalM3"),Shown.SprayArrivalM3);
 Root->SetNumberField(TEXT("floodImmersionM"),Shown.FloodImmersionM);Root->SetNumberField(TEXT("interiorPresentM3"),Shown.InteriorPresentM3);
 int32 Counts[4]={0,0,0,0};
 for(EWaterImpactPhase P:WaterImpactObservation.GetState().Phase) ++Counts[int32(P)];
 Root->SetNumberField(TEXT("wetTargets"),Counts[1]);Root->SetNumberField(TEXT("trippedTargets"),Counts[2]);Root->SetNumberField(TEXT("damagedTargets"),Counts[3]);
 Root->SetNumberField(TEXT("rackSlotCuts"),WaterRackPowerCuts.Num());Root->SetNumberField(TEXT("cduCuts"),WaterCduPowerCuts.Num());
 Root->SetNumberField(TEXT("crahCuts"),WaterCrahPowerCuts.Num());Root->SetNumberField(TEXT("spineCuts"),WaterSpinePowerCuts.Num());
 TArray<TSharedPtr<FJsonValue>> NoActuator;for(const FString& Id:WaterImpactNoActuator) NoActuator.Add(MakeShared<FJsonValueString>(Id));
 Root->SetArrayField(TEXT("noActuatorEquipment"),NoActuator);
 return Root;
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
 if((bRackWaterTripResponse || bCduWaterTripResponse || bCrahWaterTripResponse || bSpineWaterTripResponse) && !ActualWaterPower(WaterImpactPowered,ActualPower,Error,bRackWaterTripResponse,bCduWaterTripResponse,bCrahWaterTripResponse,bSpineWaterTripResponse)) return Fail(Error);
 if(bWaterImpactConfigured && !NextImpact.Step(Next,ActualPower,Error)) return Fail(Error);
 WaterExposureSnapshot=MoveTemp(Next);if(bWaterImpactConfigured) WaterImpactObservation=MoveTemp(NextImpact);bWaterExposureCurrent=true;if(!bBaseline) ++WaterObservationCount;
 WaterObservationDiagnostic=bWaterImpactConfigured?((bRackWaterTripResponse || bCduWaterTripResponse || bCrahWaterTripResponse || bSpineWaterTripResponse)?TEXT("Water exposure/state current; explicit equipment trip response enabled"):TEXT("Water exposure/state current; observational policy (no new shutdown commands)")):TEXT("Water exposure current; impact policy unset (state not evaluated)");return true;
}


bool ADataCenterSimulationActor::ConfigureWaterImpactObservation(const FWaterImpactPolicy& Policy,const TArray<bool>& Powered,FString& Reason)
{
 if(Mode!=ESimMode::Simulation || !bWaterRegistryReady || !bWaterExposureCurrent || WaterExposureSnapshot.SimTimeS!=Clock.GetSimTimeS()) {Reason=TEXT("Fresh simulation water baseline required before policy configuration");return false;}
 if(!Policy.IsValid(Reason)) return false;
 if(Policy.MaxObservationGapS<Clock.GetFixedStepS()) {Reason=TEXT("Policy observation gap is shorter than the shared fixed step");return false;}
 FWaterImpactModel Next;
 if(!Next.Reset(WaterExposureSnapshot,Powered,Policy,Reason)) return false;
 WaterImpactObservation=MoveTemp(Next);WaterImpactPowered=Powered;bWaterImpactConfigured=true;bSpineWaterTripResponse=bRackWaterTripResponse=bCduWaterTripResponse=bCrahWaterTripResponse=false;
 WaterObservationDiagnostic=TEXT("Explicit water-impact policy initialized; observational state only");return true;
}

bool ADataCenterSimulationActor::SetWaterImpactPowerObservation(const TArray<bool>& Powered,FString& Reason)
{
 if(!bWaterImpactConfigured || !bWaterExposureCurrent || Powered.Num()!=WaterExposureSnapshot.EquipmentId.Num()) {Reason=TEXT("Configured current target mapping and explicit power array required");return false;}
 // The existing hold policy applies this observation at the next endpoint, never retroactively.
 WaterImpactPowered=Powered;Reason.Reset();return true;
}


ACoolingDistributionUnitActor* ADataCenterSimulationActor::WaterBindingCdu(int32 Index) const
{
 if(!WaterExposureBindings.IsValidIndex(Index) || WaterExposureBindings[Index].Slot!=INDEX_NONE) return nullptr;
 for(const auto& T:WaterSourceTargets) if(T.bNative && T.Id==WaterExposureBindings[Index].EquipmentId) return Cast<ACoolingDistributionUnitActor>(T.Actor.Get());
 return nullptr;
}

bool ADataCenterSimulationActor::ActualWaterPower(const TArray<bool>& OtherPowered,TArray<bool>& Actual,FString& Reason,bool bRack,bool bCdu,bool bCrah,bool bSpine) const
{
 if(OtherPowered.Num()!=WaterExposureBindings.Num()) {Reason=TEXT("Explicit non-rack power observations must match the full target layout");return false;}
 if(!ValidateWaterObservationTargets(Reason) || (bCrah && !ValidateCrahPowerTargets(Reason))) return false;
 TArray<bool> Next=OtherPowered;
 for(int32 I=0;I<WaterExposureBindings.Num();++I) {
  const auto& B=WaterExposureBindings[I];
  if(bCdu) if(const auto* Cdu=WaterBindingCdu(I)) Next[I]=Cdu->IsPumpPowered();
  if(bCrah) if(const auto* Crah=WaterBindingCrah(I)) Next[I]=Crah->IsPowered();
  if(bSpine) if(const auto* Spine=WaterBindingSpine(I)) Next[I]=Spine->IsPowered();
  if(!bRack || B.InteriorRack==INDEX_NONE || B.Slot==INDEX_NONE) continue;
  if(!Racks.IsValidIndex(B.InteriorRack) || !IsValid(Racks[B.InteriorRack])) {Reason=TEXT("Stale rack power mapping");return false;}
  const auto* Rack=Racks[B.InteriorRack].Get();const int32 Count=FRackThermalModel::NumSlotsOf(Rack->Params);
  if(B.Slot<0 || B.Slot>=Count || !FRackThermalModel::IsSlotPowerInputValid(Rack->GetInput(),Count)) {Reason=TEXT("Invalid native slot electrical input");return false;}
  // 2026-11-03 (P4): a tripped rack busbar de-energizes every slot whatever its own power command.
  Next[I]=Rack->GetModel().IsSlotLive(Rack->GetInput(),B.Slot,Count) && Rack->IsSledDocked(B.Slot);
 }
 Actual=MoveTemp(Next);Reason.Reset();return true;
}

bool ADataCenterSimulationActor::ConfigureRackWaterTripResponse(const FWaterImpactPolicy& Policy,const TArray<bool>& OtherPowered,FString& Reason)
{
 TArray<bool> Actual;if(!ActualWaterPower(OtherPowered,Actual,Reason,true,false)) return false;
 if(!ConfigureWaterImpactObservation(Policy,Actual,Reason)) return false;
 bRackWaterTripResponse=true;WaterObservationDiagnostic=TEXT("Explicit rack water-trip response enabled; other equipment remains observation-only");return true;
}

bool ADataCenterSimulationActor::ConfigureCduWaterTripResponse(const FWaterImpactPolicy& Policy,const TArray<bool>& OtherPowered,FString& Reason,bool bIncludeRackResponse)
{
 TArray<bool> Actual;if(!ActualWaterPower(OtherPowered,Actual,Reason,bIncludeRackResponse,true)) return false;
 if(!ConfigureWaterImpactObservation(Policy,Actual,Reason)) return false;
 bRackWaterTripResponse=bIncludeRackResponse;bCduWaterTripResponse=true;
 WaterObservationDiagnostic=TEXT("Explicit CDU pump trip response enabled; no valve/topology commands");return true;
}

ACrahUnitActor* ADataCenterSimulationActor::WaterBindingCrah(int32 Index) const
{
 if(!WaterExposureBindings.IsValidIndex(Index) || WaterExposureBindings[Index].Slot!=INDEX_NONE) return nullptr;
 for(const auto& T:WaterSourceTargets) if(T.bNative && T.Id==WaterExposureBindings[Index].EquipmentId) return Cast<ACrahUnitActor>(T.Actor.Get());
 return nullptr;
}

bool ADataCenterSimulationActor::ValidateCrahPowerTargets(FString& Reason) const
{
 if(CrahUnitActors.Num()!=Bridge.GetNumCrahUnits()) {Reason=TEXT("CRAH duct mapping changed; re-enter simulation");return false;}
 int32 Native=0;
 for(const auto& Target:WaterSourceTargets) if(const auto* Actor=Cast<ACrahUnitActor>(Target.Actor.Get())) {
  const int32 Unit=CrahUnitActors.IndexOfByPredicate([&](const auto& A){return A.Get()==Actor;});
  const auto* Placement=CrahPlacements.FindByPredicate([&](const FCrahPlacement& P){return P.SourceActor.Get()==Actor;});
  if(Unit==INDEX_NONE || !Placement || Placement->FlowM3s!=Actor->VolumeFlowM3s || Placement->bTopIntake!=(Actor->Kind==ECrahUnitKind::Perimeter)) {Reason=TEXT("Native CRAH has no current duct/placement mapping; ceiling/filtered units unsupported, re-enter simulation after setting changes");return false;}
  ++Native;
 }
 if(Native==0) {Reason=TEXT("Native coupled CRAH required; virtual automatic CRAH remains observation-only");return false;}
 Reason.Reset();return true;
}

bool ADataCenterSimulationActor::ConfigureCrahWaterTripResponse(const FWaterImpactPolicy& Policy,const TArray<bool>& OtherPowered,FString& Reason,bool bIncludeRackResponse,bool bIncludeCduResponse)
{
 TArray<bool> Actual;if(!ActualWaterPower(OtherPowered,Actual,Reason,bIncludeRackResponse,bIncludeCduResponse,true)) return false;
 if(!ConfigureWaterImpactObservation(Policy,Actual,Reason)) return false;
 bRackWaterTripResponse=bIncludeRackResponse;bCduWaterTripResponse=bIncludeCduResponse;bCrahWaterTripResponse=true;
 WaterObservationDiagnostic=TEXT("Explicit native CRAH water-trip response enabled; no resize or valve isolation");return true;
}

ASpineCabinetActor* ADataCenterSimulationActor::WaterBindingSpine(int32 Index) const
{
 if(!WaterExposureBindings.IsValidIndex(Index) || WaterExposureBindings[Index].Slot!=INDEX_NONE) return nullptr;
 for(const auto& T:WaterSourceTargets) if(T.bNative && T.Id==WaterExposureBindings[Index].EquipmentId) return Cast<ASpineCabinetActor>(T.Actor.Get());
 return nullptr;
}

bool ADataCenterSimulationActor::ConfigureSpineWaterTripResponse(const FWaterImpactPolicy& Policy,const TArray<bool>& OtherPowered,FString& Reason,bool bRack,bool bCdu,bool bCrah)
{
 TArray<bool> Actual;
 if(!ActualWaterPower(OtherPowered,Actual,Reason,bRack,bCdu,bCrah,true)) return false;
 if(!ConfigureWaterImpactObservation(Policy,Actual,Reason)) return false;
 bSpineWaterTripResponse=true; bRackWaterTripResponse=bRack; bCduWaterTripResponse=bCdu; bCrahWaterTripResponse=bCrah;
 return true;
}

void ADataCenterSimulationActor::WaterPowerBeforeStep()
{
 if((!bRackWaterTripResponse && !bCduWaterTripResponse && !bCrahWaterTripResponse && !bSpineWaterTripResponse) || !bWaterImpactConfigured) return;
 auto Reject=[&](const FString& Reason) {bWaterExposureCurrent=false;WaterObservationDiagnostic=Reason;};
 if(!bWaterExposureObservation || !bWaterExposureCurrent || !bWaterRegistryReady || WaterExposureSnapshot.SimTimeS!=Clock.GetSimTimeS()) {Reject(TEXT("No fresh water state for power application"));return;}
 TArray<bool> Actual;FString Error;if(!ActualWaterPower(WaterImpactPowered,Actual,Error,bRackWaterTripResponse,bCduWaterTripResponse,bCrahWaterTripResponse,bSpineWaterTripResponse)) {Reject(Error);return;}
 const auto& State=WaterImpactObservation.GetState();TArray<int32> PendingRack,PendingCdu,PendingCrah,PendingSpine;
 for(int32 I=0;I<WaterExposureBindings.Num();++I) {
  if(State.Phase[I]!=EWaterImpactPhase::Tripped && State.Phase[I]!=EWaterImpactPhase::Damaged) continue;
  const auto& B=WaterExposureBindings[I];
  if(bCduWaterTripResponse) if(const auto* Cdu=WaterBindingCdu(I)) if(Cdu->IsPumpPowered()) {PendingCdu.Add(I);Actual[I]=false;}
  if(bCrahWaterTripResponse) if(const auto* Crah=WaterBindingCrah(I)) if(Crah->IsPowered()) {PendingCrah.Add(I);Actual[I]=false;}
  if(bSpineWaterTripResponse) if(const auto* Spine=WaterBindingSpine(I)) if(Spine->IsPowered()) {PendingSpine.Add(I);Actual[I]=false;}
  if(!bRackWaterTripResponse || B.InteriorRack==INDEX_NONE || B.Slot==INDEX_NONE) continue;
  const auto* Rack=Racks[B.InteriorRack].Get();
  if(FRackThermalModel::SlotPoweredOf(Rack->GetInput(),B.Slot,FRackThermalModel::NumSlotsOf(Rack->Params))) {PendingRack.Add(I);Actual[I]=false;}
 }
 // Preflight both actuator types together before changing anything. No event replay or water mutation.
 FWaterImpactModel Next=WaterImpactObservation;
 if(!Next.SetPowerAtCurrentTime(Clock.GetSimTimeS(),Actual,Error)) {Reject(Error);return;}
 if(!PendingRack.IsEmpty() || !PendingCdu.IsEmpty() || !PendingCrah.IsEmpty() || !PendingSpine.IsEmpty()) {
  // Report: which state record caused each cut, and the IT heat just before it (read by WaterImpactAfterStep).
  WaterCutItBeforeW=GetSnapshot().TotalItHeatW;WaterCutAppliedS=Clock.GetSimTimeS();
  for(const TArray<int32>* List:{&PendingRack,&PendingCdu,&PendingCrah,&PendingSpine}) for(int32 I:*List) if(const int32* Record=WaterImpactTargetRecord.Find(I)) WaterCutsAwaitingIt.AddUnique(*Record);
 }
 for(int32 I:PendingRack) {
  const auto& B=WaterExposureBindings[I];auto* Rack=Racks[B.InteriorRack].Get();auto& Input=Rack->GetMutableInput();
  if(Input.SlotPowered.IsEmpty()) Input.SlotPowered.Init(true,FRackThermalModel::NumSlotsOf(Rack->Params));
  Input.SlotPowered[B.Slot]=false;
  WaterRackPowerCuts.Add({B.EquipmentId,B.Slot,State.TripS[I]>=0?State.TripS[I]:State.DamageS[I],WaterExposureSnapshot.SimTimeS,Clock.GetSimTimeS()});
 }
 for(int32 I:PendingCdu) {
  WaterBindingCdu(I)->SetPumpPowered(false);
  WaterCduPowerCuts.Add({WaterExposureBindings[I].EquipmentId,State.TripS[I]>=0?State.TripS[I]:State.DamageS[I],WaterExposureSnapshot.SimTimeS,Clock.GetSimTimeS()});
 }
 for (int32 I : PendingSpine) {
  WaterBindingSpine(I)->SetPowered(false);
  const double DecisionS = State.TripS[I] >= 0 ? State.TripS[I] : State.DamageS[I];
  WaterSpinePowerCuts.Add({WaterExposureBindings[I].EquipmentId, DecisionS, WaterExposureSnapshot.SimTimeS, Clock.GetSimTimeS()});
 }
 for(int32 I:PendingCrah) {
  WaterBindingCrah(I)->SetPowered(false);
  WaterCrahPowerCuts.Add({WaterExposureBindings[I].EquipmentId,State.TripS[I]>=0?State.TripS[I]:State.DamageS[I],WaterExposureSnapshot.SimTimeS,Clock.GetSimTimeS()});
 }
 WaterImpactObservation=MoveTemp(Next);WaterImpactPowered=MoveTemp(Actual);
}
