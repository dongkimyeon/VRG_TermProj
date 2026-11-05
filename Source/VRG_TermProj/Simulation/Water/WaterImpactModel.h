#pragma once
#include "Simulation/Water/WaterExposure.h"

enum class EWaterImpactPhase : uint8 { Normal, Wet, Tripped, Damaged };
enum class EWaterWetRecovery : uint8 { Unspecified, LatchUntilRestart, ClearWarningWhenDry };
enum class EWaterContactTiming : uint8 { Unspecified, HoldPreviousObservation };
enum class EWaterThresholdBasis : uint8 { Unspecified, SyntheticTest, UncalibratedEstimate, PublicSource };
struct FWaterImpactPolicy
{
 // No operational defaults: -1/Unspecified is invalid. Units are m3, m and seconds.
 // No public hardware failure calibration is established here (plan L4, assumptions).
 uint8 ContactSources=0;
 // Interior/rear thresholds apply to current presence OR new per-observation arrival.
 double SprayArrivalM3=-1, FloodImmersionM=-1, InteriorPresentM3=-1, RearPresentM3=-1;
 double TripPoweredContactS=-1, DamageContactAfterTripS=-1, MaxObservationGapS=-1;
 EWaterWetRecovery WetRecovery=EWaterWetRecovery::Unspecified;
 EWaterContactTiming Timing=EWaterContactTiming::Unspecified;
 EWaterThresholdBasis Basis=EWaterThresholdBasis::Unspecified;
 FString BasisNote;
 TArray<FString> PublicSources;
 // Optional second damage branch of plan L4: a tripped target whose largest single source history since the baseline
 // reaches this volume. Negative = off. Sources are compared one by one, never added (they can overlap).
 double DamageCumulativeM3=-1;
 // Optional, one per target: 1 = stays Normal (no actuator, or outdoor equipment, plan L4). Empty = none exempt.
 TArray<uint8> ExemptTargets;
 bool IsValid(FString& Error) const;
};
struct FWaterImpactEvent
{
 int32 Target=INDEX_NONE;
 EWaterImpactPhase From=EWaterImpactPhase::Normal, To=EWaterImpactPhase::Normal;
 double SimTimeS=0;
 uint8 Cause=0; // EWaterEvidence mask behind this transition
};
struct FWaterImpactState
{
 TArray<EWaterImpactPhase> Phase;
 TArray<double> FirstWetS, TripS, DamageS, PoweredContactS, ContactAfterTripS;
 TArray<uint8> LastContact, LastPowered;
};
// The game's policy (vrg.Water.Impact). Every value is an uncalibrated ESTIMATE, see the .cpp and plan L4.
namespace WaterImpactEstimates
{
 constexpr double TripPoweredContactS=15, DamageContactAfterTripS=60;
 FWaterImpactPolicy MakePolicy(double TripS=TripPoweredContactS,double DamageS=DamageContactAfterTripS);
}
const TCHAR* WaterImpactPhaseName(EWaterImpactPhase Phase);
FString WaterEvidenceNames(uint8 Mask);
class FWaterImpactModel
{
public:
 // Explicit baseline prevents cumulative histories from being replayed on restart.
 // Reset does not drain/repair any physical source; current held/flooded water may warn again.
 bool Reset(const FWaterExposureSnapshot& Baseline,const TArray<bool>& Powered,const FWaterImpactPolicy& Policy,FString& Error);
 bool Step(const FWaterExposureSnapshot& Now,const TArray<bool>& Powered,FString& Error);
 // Known electrical change at the already observed endpoint; no retroactive interval or water replay.
 bool SetPowerAtCurrentTime(double SimTimeS,const TArray<bool>& Powered,FString& Error);
 const FWaterImpactState& GetState() const {return State;}
 const TArray<FWaterImpactEvent>& GetEvents() const {return Events;}
 double GetSimTimeS() const {return Previous.SimTimeS;}
 bool IsInitialized() const {return bInitialized;}
private:
 FWaterImpactPolicy Policy;
 FWaterExposureSnapshot Previous,BaselineSnapshot;
 FWaterImpactState State;
 TArray<FWaterImpactEvent> Events;
 bool bInitialized=false;
 uint8 Contact(const FWaterExposureSnapshot& Now,const FWaterExposureSnapshot& Before,int32 I) const;
};
