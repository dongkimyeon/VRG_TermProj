#include "Simulation/Water/WaterImpactModel.h"

bool FWaterImpactPolicy::IsValid(FString& Error) const
{
 auto Fail=[&](const TCHAR* Why){Error=Why;return false;};
 if(ContactSources==0 || (ContactSources&~uint8(15))) return Fail(TEXT("Explicit contact-source policy required"));
 const double Thresholds[]={SprayArrivalM3,FloodImmersionM,InteriorPresentM3,RearPresentM3};
 for(int32 S=0;S<4;++S) if((ContactSources&(1<<S)) && (!FMath::IsFinite(Thresholds[S]) || Thresholds[S]<0)) return Fail(TEXT("Contact thresholds are not configured"));
 for(double V:{TripPoweredContactS,DamageContactAfterTripS,MaxObservationGapS}) if(!FMath::IsFinite(V) || V<=0) return Fail(TEXT("Explicit positive duration/gap policy required"));
 if(WetRecovery!=EWaterWetRecovery::LatchUntilRestart && WetRecovery!=EWaterWetRecovery::ClearWarningWhenDry) return Fail(TEXT("Warning recovery policy required"));
 if(Timing!=EWaterContactTiming::HoldPreviousObservation) return Fail(TEXT("Observation timing policy required"));
 if(Basis==EWaterThresholdBasis::Unspecified || BasisNote.IsEmpty()) return Fail(TEXT("Threshold basis/estimate note required"));
 if(Basis!=EWaterThresholdBasis::SyntheticTest && Basis!=EWaterThresholdBasis::UncalibratedEstimate && Basis!=EWaterThresholdBasis::PublicSource) return Fail(TEXT("Invalid threshold basis"));
 if(Basis==EWaterThresholdBasis::PublicSource && (PublicSources.IsEmpty() || PublicSources.ContainsByPredicate([](const FString& S){return S.IsEmpty();}))) return Fail(TEXT("Public source citation required"));
 Error.Reset();return true;
}

uint8 FWaterImpactModel::Contact(const FWaterExposureSnapshot& Now,const FWaterExposureSnapshot& Before,int32 I) const
{
 uint8 Mask=0;
 // Cumulative contact is an arrival pulse, never evidence of perpetual wetness.
 if(Now.SprayTouchedM3[I]-Before.SprayTouchedM3[I]>Policy.SprayArrivalM3) Mask|=uint8(EWaterEvidence::Spray);
 if(Now.FloodImmersionM[I]>Policy.FloodImmersionM) Mask|=uint8(EWaterEvidence::Flood);
 // Passed includes rear bypass: compare new front arrival only, not the lifetime total.
 // This is contact evidence, not a new transferred-volume or thermal ledger.
 const double PassedArrival=Now.InteriorPassedM3[I]-Before.InteriorPassedM3[I];
 const double RearArrival=Now.RearPassedM3[I]-Before.RearPassedM3[I];
 if(Now.InteriorHeldM3[I]>Policy.InteriorPresentM3 || Now.InteriorMovingM3[I]>Policy.InteriorPresentM3 || PassedArrival>RearArrival+Policy.InteriorPresentM3) Mask|=uint8(EWaterEvidence::Interior);
 if(Now.RearMovingM3[I]>Policy.RearPresentM3 || Now.RearPassedM3[I]-Before.RearPassedM3[I]>Policy.RearPresentM3) Mask|=uint8(EWaterEvidence::Rear);
 return Mask & Now.Available[I] & Policy.ContactSources;
}

bool FWaterImpactModel::Reset(const FWaterExposureSnapshot& Baseline,const TArray<bool>& Powered,const FWaterImpactPolicy& InPolicy,FString& Error)
{
 if(!Baseline.IsValid(Error) || !InPolicy.IsValid(Error)) return false;
 const int32 N=Baseline.EquipmentId.Num();
 if(Powered.Num()!=N) {Error=TEXT("Power observation length mismatch");return false;}
 FWaterImpactModel Next;Next.Policy=InPolicy;Next.Previous=Baseline;
 auto& S=Next.State;S.Phase.Init(EWaterImpactPhase::Normal,N);
 for(auto* A:{&S.FirstWetS,&S.TripS,&S.DamageS}) A->Init(-1,N);
 S.PoweredContactS.Init(0,N);S.ContactAfterTripS.Init(0,N);S.LastContact.Init(0,N);S.LastPowered.Init(0,N);
 for(int32 I=0;I<N;++I) {
  S.LastContact[I]=Next.Contact(Baseline,Baseline,I);S.LastPowered[I]=Powered[I]?1:0;
  if(S.LastContact[I]) {S.Phase[I]=EWaterImpactPhase::Wet;S.FirstWetS[I]=Baseline.SimTimeS;}
 }
 Next.bInitialized=true;*this=MoveTemp(Next);Error.Reset();return true;
}

bool FWaterImpactModel::Step(const FWaterExposureSnapshot& Now,const TArray<bool>& Powered,FString& Error)
{
 auto Fail=[&](const TCHAR* Why){Error=Why;return false;};
 if(!bInitialized) return Fail(TEXT("Impact model is not configured"));
 if(!Now.IsValid(Error)) return false;
 const double Dt=Now.SimTimeS-Previous.SimTimeS;const int32 N=Previous.EquipmentId.Num();
 if(!FMath::IsFinite(Dt) || Dt<=0 || Dt>Policy.MaxObservationGapS) return Fail(TEXT("Duplicate/backward/stale observation; state unchanged"));
 if(Now.EquipmentId!=Previous.EquipmentId || Now.Slot!=Previous.Slot || Now.Available!=Previous.Available || Powered.Num()!=N) return Fail(TEXT("Target/source mapping changed; explicit Reset required"));
 for(int32 I=0;I<N;++I)
  if(Now.SprayTouchedM3[I]<Previous.SprayTouchedM3[I] || Now.InteriorPassedM3[I]<Previous.InteriorPassedM3[I] || Now.RearPassedM3[I]<Previous.RearPassedM3[I]) return Fail(TEXT("Cumulative source restarted; explicit baseline Reset required"));
 FWaterImpactState Next=State;TArray<FWaterImpactEvent> NewEvents=Events;
 auto Transition=[&](int32 I,EWaterImpactPhase To,double Time) {NewEvents.Add({I,Next.Phase[I],To,Time});Next.Phase[I]=To;if(To==EWaterImpactPhase::Damaged) Next.DamageS[I]=Time;};
 for(int32 I=0;I<N;++I) {
  // Explicit zero-order hold of PREVIOUS observation, not retroactive exposure of a new arrival.
  // This is a sampling assumption selected by the caller, not a calibrated electrical-contact duration.
  const bool Wet=State.LastContact[I]!=0;
  if(!Wet) {Next.PoweredContactS[I]=0;Next.ContactAfterTripS[I]=0;}
  else if(Next.Phase[I]==EWaterImpactPhase::Tripped) {
   const double Need=Policy.DamageContactAfterTripS-Next.ContactAfterTripS[I];
   if(Dt>=Need) Transition(I,EWaterImpactPhase::Damaged,Previous.SimTimeS+Need);
   Next.ContactAfterTripS[I]+=Dt;
  } else if(Next.Phase[I]==EWaterImpactPhase::Wet && State.LastPowered[I]) {
   const double Need=Policy.TripPoweredContactS-Next.PoweredContactS[I];
   Next.PoweredContactS[I]+=Dt;
   if(Dt>=Need) {
    const double TripAt=Previous.SimTimeS+Need;Next.TripS[I]=TripAt;Transition(I,EWaterImpactPhase::Tripped,TripAt);
    const double Remaining=Dt-Need;Next.ContactAfterTripS[I]=Remaining;
    if(Remaining>=Policy.DamageContactAfterTripS) Transition(I,EWaterImpactPhase::Damaged,TripAt+Policy.DamageContactAfterTripS);
   }
  } else if(!State.LastPowered[I]) Next.PoweredContactS[I]=0;
  const uint8 Current=Contact(Now,Previous,I);
  if(Current && Next.Phase[I]==EWaterImpactPhase::Normal) {if(Next.FirstWetS[I]<0) Next.FirstWetS[I]=Now.SimTimeS;Transition(I,EWaterImpactPhase::Wet,Now.SimTimeS);}
  if(!Current && Next.Phase[I]==EWaterImpactPhase::Wet && Policy.WetRecovery==EWaterWetRecovery::ClearWarningWhenDry) Transition(I,EWaterImpactPhase::Normal,Now.SimTimeS);
  // Trip/damage latch until an explicit new baseline Reset; no repair-hole input exists.
  Next.LastContact[I]=Current;Next.LastPowered[I]=Powered[I]?1:0;
  if(!Current) {Next.PoweredContactS[I]=0;Next.ContactAfterTripS[I]=0;}
  if(!FMath::IsFinite(Next.PoweredContactS[I]) || !FMath::IsFinite(Next.ContactAfterTripS[I])) return Fail(TEXT("Impact duration overflow; state unchanged"));
 }
 State=MoveTemp(Next);Events=MoveTemp(NewEvents);Previous=Now;Error.Reset();return true;
}
