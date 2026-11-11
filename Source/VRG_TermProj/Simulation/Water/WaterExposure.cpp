#include "Simulation/Water/WaterExposure.h"

bool FWaterExposureSnapshot::IsValid(FString& Error) const
{
 const int32 N=EquipmentId.Num();
 if(!FMath::IsFinite(SimTimeS) || SimTimeS<0 || Slot.Num()!=N || Available.Num()!=N) {Error=TEXT("Invalid time/identity layout");return false;}
 for(const auto* A:{&SprayTouchedM3,&FloodImmersionM,&InteriorPassedM3,&InteriorHeldM3,&InteriorMovingM3,&RearMovingM3,&RearPassedM3}) {
  if(A->Num()!=N) {Error=TEXT("Exposure SoA length mismatch");return false;}
  for(double V:*A) if(!FMath::IsFinite(V) || V<0) {Error=TEXT("Exposure must be finite and nonnegative");return false;}
 }
 // Hashed duplicate check: the pairwise string compare was O(N^2) and cost 23.6 ms per step at L_Test's 1227 targets (2026-11-06).
 TSet<TPair<FString,int32>> Seen;Seen.Reserve(N);
 for(int32 I=0;I<N;++I) {
  if(EquipmentId[I].IsEmpty() || Slot[I]<INDEX_NONE || (Available[I]&~uint8(15))) {Error=TEXT("Invalid equipment/slot/source mask");return false;}
  bool bDuplicate=false;Seen.Add(TPair<FString,int32>(EquipmentId[I],Slot[I]),&bDuplicate);
  if(bDuplicate) {Error=TEXT("Duplicate equipment/slot target");return false;}
  if(RearPassedM3[I]>InteriorPassedM3[I]) {Error=TEXT("Rear history is not a separate mass ledger");return false;}
  const bool Values[]={SprayTouchedM3[I]!=0,FloodImmersionM[I]!=0,InteriorPassedM3[I]!=0 || InteriorHeldM3[I]!=0 || InteriorMovingM3[I]!=0,RearPassedM3[I]!=0 || RearMovingM3[I]!=0};
  for(int32 K=0;K<4;++K) if(!(Available[I]&(1<<K)) && Values[K]!=0) {Error=TEXT("Exposure value without a known source");return false;}
 }
 Error.Reset();return true;
}

bool WaterExposure::Collect(const TArray<FWaterExposureBinding>& Bindings,double Now,
 const FSprayGeometry& Geometry,const TArray<FSprayWetting>& Spray,const TArray<FFloorFlood>& Flood,
 const FRackInteriorWater& Interior,const TArray<FString>& RackIds,FWaterExposureSnapshot& Out,FString& Error,
 const TArray<FFloorFlood>* YardFlood)
{
 FWaterExposureSnapshot Next;Next.SimTimeS=Now;const int32 N=Bindings.Num();
 Next.Available.Init(0,N);
 for(auto* A:{&Next.SprayTouchedM3,&Next.FloodImmersionM,&Next.InteriorPassedM3,&Next.InteriorHeldM3,&Next.InteriorMovingM3,&Next.RearMovingM3,&Next.RearPassedM3}) A->Init(0,N);
 TSet<int32> Unlocalized;
 auto Fail=[&](const TCHAR* Why){Error=Why;return false;};
 for(int32 I=0;I<N;++I) {
  const auto& B=Bindings[I];const FString& SourceName=B.SourceName.IsEmpty()?B.EquipmentId:B.SourceName;Next.EquipmentId.Add(B.EquipmentId);Next.Slot.Add(B.Slot);
  if(B.SprayObstacle<INDEX_NONE || B.FloorFlood<INDEX_NONE || B.InteriorRack<INDEX_NONE) return Fail(TEXT("Invalid source index"));
  if(B.SprayObstacle!=INDEX_NONE) {
   if(!Geometry.Obstacles.IsValidIndex(B.SprayObstacle) || !Spray.IsValidIndex(B.SprayObstacle)) return Fail(TEXT("Missing spray obstacle/contact view"));
   const auto& O=Geometry.Obstacles[B.SprayObstacle];const auto& W=Spray[B.SprayObstacle];
   if(O.Name!=SourceName || O.Kind!=ESprayObstacleKind::Equipment) return Fail(TEXT("Spray equipment identity mismatch"));
   if(!FMath::IsFinite(W.WaterM3) || W.WaterM3<0) return Fail(TEXT("Invalid spray contact history"));
   if(B.Slot!=INDEX_NONE) {
    // Exact impact-height band when the box has one (native racks). Without bands a box maximum impact height is not
    // per-slot arrival evidence: no volume split or broad wetting.
    if(W.ZoneWaterM3.IsValidIndex(B.Slot)) {Next.SprayTouchedM3[I]=W.ZoneWaterM3[B.Slot];Next.Available[I]|=uint8(EWaterEvidence::Spray);}
    else if(!Unlocalized.Contains(B.SprayObstacle)) {Next.Diagnostics.Add({B.EquipmentId,TEXT("Spray box contact cannot localize native slots"),B.SprayObstacle,W.WaterM3});Unlocalized.Add(B.SprayObstacle);}
   } else {Next.SprayTouchedM3[I]=W.WaterM3;Next.Available[I]|=uint8(EWaterEvidence::Spray);}
  }
  if(B.FloorFlood!=INDEX_NONE) {
   const bool bYard=B.FloorFlood>=Flood.Num();const int32 FI=bYard?B.FloorFlood-Flood.Num():B.FloorFlood;
   const TArray<FFloorFlood>* List=bYard?YardFlood:&Flood;
   if(!List || !List->IsValidIndex(FI) || (*List)[FI].Name!=SourceName) return Fail(TEXT("Flood equipment identity mismatch"));
   const auto& F=(*List)[FI];
   if(F.SourceObstacle!=INDEX_NONE && B.SprayObstacle!=INDEX_NONE && F.SourceObstacle!=B.SprayObstacle) return Fail(TEXT("Flood/spray source obstacle mismatch"));
   if(!FMath::IsFinite(B.FloorContactDepthM) || B.FloorContactDepthM<0 || !FMath::IsFinite(F.MeanDepthM) || F.MeanDepthM<0) return Fail(TEXT("Invalid explicit flood contact height/depth"));
   Next.FloodImmersionM[I]=FMath::Max(0.0,F.MeanDepthM-B.FloorContactDepthM);Next.Available[I]|=uint8(EWaterEvidence::Flood);
  }
  if(B.InteriorRack!=INDEX_NONE) {
   const int32 R=B.InteriorRack;
   if(!RackIds.IsValidIndex(R) || RackIds[R]!=B.EquipmentId || !Interior.Start.IsValidIndex(R+1) || B.Slot<0 || B.Slot>=Interior.Start[R+1]-Interior.Start[R]) return Fail(TEXT("Interior requires an exact rack/native-slot binding"));
   const int32 S=Interior.Start[R]+B.Slot;
   if(!Interior.HeldM3.IsValidIndex(S) || !Interior.TransitM3.IsValidIndex(S) || !Interior.PassedM3.IsValidIndex(S) || !Interior.RearExposureM3.IsValidIndex(S) || !Interior.GetRearTransitM3().IsValidIndex(S)) return Fail(TEXT("Incomplete interior SoA"));
   Next.InteriorPassedM3[I]=Interior.PassedM3[S];Next.InteriorHeldM3[I]=Interior.HeldM3[S];
   Next.InteriorMovingM3[I]=Interior.TransitM3[S];Next.RearMovingM3[I]=Interior.GetRearTransitM3()[S];
   Next.RearPassedM3[I]=Interior.RearExposureM3[S];Next.Available[I]|=uint8(EWaterEvidence::Interior)|uint8(EWaterEvidence::Rear);
  }
 }
 if(!Next.IsValid(Error)) return false;
 Out=MoveTemp(Next);return true;
}
