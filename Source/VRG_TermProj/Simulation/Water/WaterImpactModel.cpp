#include "Simulation/Water/WaterImpactModel.h"

FWaterImpactPolicy WaterImpactEstimates::MakePolicy(double TripS,double DamageS)
{
 // ESTIMATES, not hardware failure limits. No public trip/damage data was found (plan L4, 2026-11-03: one article's
 // unverifiable "under 10 s"); replace them when a source appears. This path is the "detection/protection was late or
 // absent" scenario, real sites shut down before water arrives (L5).
 FWaterImpactPolicy P;
 // Rear manifold water is not a contact source: it runs behind the trays and is only reported (W4-1 decision).
 P.ContactSources=uint8(EWaterEvidence::Spray)|uint8(EWaterEvidence::Flood)|uint8(EWaterEvidence::Interior);
 P.SprayArrivalM3=1e-7;      // ESTIMATE: per shared 20 ms step, about 5 mL/s on one slot/box; finer mist does not count
 P.FloodImmersionM=.01;      // ESTIMATE: 10 mm above the bottom of the slot/box (units on the floor keep live parts above it)
 P.InteriorPresentM3=2e-6;   // ESTIMATE: same 2 mL as the W5 tray detectors
 P.TripPoweredContactS=TripS;     // ESTIMATE: longer than the W5 compute-tray timer + sensor delays (10 + 2 s), so protection can act first
 P.DamageContactAfterTripS=DamageS; // ESTIMATE
 P.DamageCumulativeM3=1e-3;  // ESTIMATE: 1 L through or onto one target
 P.MaxObservationGapS=1;     // not physics: rejects stale observations
 P.WetRecovery=EWaterWetRecovery::LatchUntilRestart;P.Timing=EWaterContactTiming::HoldPreviousObservation; // no drying model
 P.Basis=EWaterThresholdBasis::UncalibratedEstimate;P.BasisNote=TEXT("Estimated thresholds with no public failure data, not calibrated (plan L4)");
 return P;
}

const TCHAR* WaterImpactPhaseName(EWaterImpactPhase Phase)
{
 switch(Phase) {
 case EWaterImpactPhase::Wet: return TEXT("wet");
 case EWaterImpactPhase::Tripped: return TEXT("tripped");
 case EWaterImpactPhase::Damaged: return TEXT("damaged");
 default: return TEXT("normal");
 }
}

FString WaterEvidenceNames(uint8 Mask)
{
 TArray<FString> Names;
 if(Mask&uint8(EWaterEvidence::Spray)) Names.Add(TEXT("spray"));
 if(Mask&uint8(EWaterEvidence::Flood)) Names.Add(TEXT("flood"));
 if(Mask&uint8(EWaterEvidence::Interior)) Names.Add(TEXT("interior"));
 if(Mask&uint8(EWaterEvidence::Rear)) Names.Add(TEXT("rear"));
 return FString::Join(Names,TEXT("+"));
}

bool FWaterImpactPolicy::IsValid(FString& Error) const
{
 auto Fail=[&](const TCHAR* Why){Error=Why;return false;};
 if(!FMath::IsFinite(DamageCumulativeM3)) return Fail(TEXT("Invalid cumulative damage volume"));
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
 if(Policy.ExemptTargets.IsValidIndex(I) && Policy.ExemptTargets[I]) return 0;
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
 if(!InPolicy.ExemptTargets.IsEmpty() && InPolicy.ExemptTargets.Num()!=N) {Error=TEXT("Exempt target list length mismatch");return false;}
 FWaterImpactModel Next;Next.Policy=InPolicy;Next.Previous=Baseline;Next.BaselineSnapshot=Baseline;
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
 auto Transition=[&](int32 I,EWaterImpactPhase To,double Time,uint8 Cause) {NewEvents.Add({I,Next.Phase[I],To,Time,Cause});Next.Phase[I]=To;if(To==EWaterImpactPhase::Damaged) Next.DamageS[I]=Time;};
 for(int32 I=0;I<N;++I) {
  // Explicit zero-order hold of PREVIOUS observation, not retroactive exposure of a new arrival.
  // This is a sampling assumption selected by the caller, not a calibrated electrical-contact duration.
  const bool Wet=State.LastContact[I]!=0;
  if(!Wet) {Next.PoweredContactS[I]=0;Next.ContactAfterTripS[I]=0;}
  else if(Next.Phase[I]==EWaterImpactPhase::Tripped) {
   const double Need=Policy.DamageContactAfterTripS-Next.ContactAfterTripS[I];
   if(Dt>=Need) Transition(I,EWaterImpactPhase::Damaged,Previous.SimTimeS+Need,State.LastContact[I]);
   Next.ContactAfterTripS[I]+=Dt;
  } else if(Next.Phase[I]==EWaterImpactPhase::Wet && State.LastPowered[I]) {
   const double Need=Policy.TripPoweredContactS-Next.PoweredContactS[I];
   Next.PoweredContactS[I]+=Dt;
   if(Dt>=Need) {
    const double TripAt=Previous.SimTimeS+Need;Next.TripS[I]=TripAt;Transition(I,EWaterImpactPhase::Tripped,TripAt,State.LastContact[I]);
    const double Remaining=Dt-Need;Next.ContactAfterTripS[I]=Remaining;
    if(Remaining>=Policy.DamageContactAfterTripS) Transition(I,EWaterImpactPhase::Damaged,TripAt+Policy.DamageContactAfterTripS,State.LastContact[I]);
   }
  } else if(!State.LastPowered[I]) Next.PoweredContactS[I]=0;
  const uint8 Current=Contact(Now,Previous,I);
  if(Policy.DamageCumulativeM3>=0 && Next.Phase[I]==EWaterImpactPhase::Tripped && !(Policy.ExemptTargets.IsValidIndex(I) && Policy.ExemptTargets[I])) {
   // Each source on its own (they can see the same water); rear-only water never reached the board.
   const auto& Base=BaselineSnapshot;double Spray=0,Front=0;
   if(Policy.ContactSources&uint8(EWaterEvidence::Spray)) Spray=Now.SprayTouchedM3[I]-Base.SprayTouchedM3[I];
   if(Policy.ContactSources&uint8(EWaterEvidence::Interior)) Front=(Now.InteriorPassedM3[I]-Now.RearPassedM3[I])-(Base.InteriorPassedM3[I]-Base.RearPassedM3[I]);
   if(FMath::Max(Spray,Front)>=Policy.DamageCumulativeM3) Transition(I,EWaterImpactPhase::Damaged,Now.SimTimeS,Spray>=Front?uint8(EWaterEvidence::Spray):uint8(EWaterEvidence::Interior));
  }
  if(Current && Next.Phase[I]==EWaterImpactPhase::Normal) {if(Next.FirstWetS[I]<0) Next.FirstWetS[I]=Now.SimTimeS;Transition(I,EWaterImpactPhase::Wet,Now.SimTimeS,Current);}
  if(!Current && Next.Phase[I]==EWaterImpactPhase::Wet && Policy.WetRecovery==EWaterWetRecovery::ClearWarningWhenDry) Transition(I,EWaterImpactPhase::Normal,Now.SimTimeS,0);
  // Trip/damage latch until an explicit new baseline Reset; no repair-hole input exists.
  Next.LastContact[I]=Current;Next.LastPowered[I]=Powered[I]?1:0;
  if(!Current) {Next.PoweredContactS[I]=0;Next.ContactAfterTripS[I]=0;}
  if(!FMath::IsFinite(Next.PoweredContactS[I]) || !FMath::IsFinite(Next.ContactAfterTripS[I])) return Fail(TEXT("Impact duration overflow; state unchanged"));
 }
 State=MoveTemp(Next);Events=MoveTemp(NewEvents);Previous=Now;Error.Reset();return true;
}


bool FWaterImpactModel::SetPowerAtCurrentTime(double Time,const TArray<bool>& Powered,FString& Error)
{
 if(!bInitialized || !FMath::IsFinite(Time) || Time!=Previous.SimTimeS || Powered.Num()!=State.LastPowered.Num()) {Error=TEXT("Power update requires the current observed endpoint and target layout");return false;}
 TArray<uint8> Next;Next.Reserve(Powered.Num());for(bool Value:Powered) Next.Add(Value?1:0);
 State.LastPowered=MoveTemp(Next);Error.Reset();return true;
}
