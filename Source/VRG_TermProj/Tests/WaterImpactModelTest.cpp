#include "Misc/AutomationTest.h"
#include "Simulation/Water/WaterImpactModel.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
 constexpr EAutomationTestFlags ImpactFlags=EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
 FWaterExposureSnapshot ImpactBaseline()
 {
  FWaterExposureSnapshot S;S.EquipmentId={TEXT("A"),TEXT("A"),TEXT("B")};S.Slot={0,1,INDEX_NONE};S.Available.Init(15,3);
  for(auto* A:{&S.SprayTouchedM3,&S.FloodImmersionM,&S.InteriorPassedM3,&S.InteriorHeldM3,&S.InteriorMovingM3,&S.RearMovingM3,&S.RearPassedM3}) A->Init(0,3);
  return S;
 }
 FWaterImpactPolicy ImpactFixturePolicy()
 {
  // SYNTHETIC TEST ONLY. Zero arrival/immersion thresholds and 2/3 s durations are chosen for exact event checks.
  // They are not measurements, engineering estimates or published trip/damage limits for real equipment.
  FWaterImpactPolicy P;P.ContactSources=15;P.SprayArrivalM3=P.FloodImmersionM=P.InteriorPresentM3=P.RearPresentM3=0;
  P.TripPoweredContactS=2;P.DamageContactAfterTripS=3;P.MaxObservationGapS=10;
  P.WetRecovery=EWaterWetRecovery::LatchUntilRestart;P.Timing=EWaterContactTiming::HoldPreviousObservation;
  P.Basis=EWaterThresholdBasis::SyntheticTest;P.BasisNote=TEXT("Controlled test fixture, not hardware failure criteria");return P;
 }
 bool SameState(const FWaterImpactState& A,const FWaterImpactState& B)
 {
  return A.Phase==B.Phase && A.FirstWetS==B.FirstWetS && A.TripS==B.TripS && A.DamageS==B.DamageS && A.PoweredContactS==B.PoweredContactS && A.ContactAfterTripS==B.ContactAfterTripS && A.LastContact==B.LastContact && A.LastPowered==B.LastPowered;
 }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactCumulativeTest,"VRG.Water.Impact.CumulativeAndExempt",ImpactFlags)
bool FWaterImpactCumulativeTest::RunTest(const FString& Parameters)
{
 // Exact hand values: trip after 2 s, duration damage far away, cumulative damage at 1 m3 of one source.
 FString Error;auto S=ImpactBaseline();auto P=ImpactFixturePolicy();const TArray<bool> Power={true,true,true};
 P.DamageContactAfterTripS=1e6;P.DamageCumulativeM3=1;P.ExemptTargets={0,0,1};
 S.InteriorPassedM3[1]=7;S.RearPassedM3[1]=7;   // history from before the baseline, and rear-only, must not count
 FWaterImpactModel M;TestTrue(TEXT("Reset"),M.Reset(S,Power,P,Error));
 auto Bad=P;Bad.ExemptTargets={1};FWaterImpactModel Rejected;TestFalse(TEXT("Exempt list length checked"),Rejected.Reset(S,Power,Bad,Error));
 Bad=P;Bad.DamageCumulativeM3=std::numeric_limits<double>::quiet_NaN();TestFalse(TEXT("NaN volume rejected"),Bad.IsValid(Error));
 auto At=[&](double T,double Spray0,double Spray2){auto N=S;N.SimTimeS=T;N.SprayTouchedM3[0]=Spray0;N.SprayTouchedM3[2]=Spray2;return N;};
 TestTrue(TEXT("Step 1"),M.Step(At(1,.1,.1),Power,Error));
 TestTrue(TEXT("Wet with its cause"),M.GetState().Phase[0]==EWaterImpactPhase::Wet);
 if(TestEqual(TEXT("One event so far"),M.GetEvents().Num(),1)) TestEqual(TEXT("Cause recorded"),M.GetEvents()[0].Cause,uint8(EWaterEvidence::Spray));
 TestTrue(TEXT("Exempt target ignores water"),M.GetState().Phase[2]==EWaterImpactPhase::Normal);
 TestTrue(TEXT("Step 2"),M.Step(At(2,.2,.2),Power,Error));TestTrue(TEXT("Step 3"),M.Step(At(3,.3,.3),Power,Error));
 TestTrue(TEXT("Tripped at 3 s"),M.GetState().Phase[0]==EWaterImpactPhase::Tripped);TestEqual(TEXT("Trip time"),M.GetState().TripS[0],3.0);
 TestTrue(TEXT("Below the volume"),M.Step(At(4,.9,.9),Power,Error) && M.GetState().Phase[0]==EWaterImpactPhase::Tripped);
 TestTrue(TEXT("At the volume"),M.Step(At(5,1.0,1.0),Power,Error) && M.GetState().Phase[0]==EWaterImpactPhase::Damaged);
 TestEqual(TEXT("Damage time"),M.GetState().DamageS[0],5.0);
 TestTrue(TEXT("Rear-only history stays Normal"),M.GetState().Phase[1]==EWaterImpactPhase::Normal);
 TestTrue(TEXT("Exempt target never changes"),M.GetState().Phase[2]==EWaterImpactPhase::Normal);
 TestEqual(TEXT("Names"),WaterEvidenceNames(uint8(EWaterEvidence::Spray)|uint8(EWaterEvidence::Interior)),FString(TEXT("spray+interior")));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactHistoryTest,"VRG.Water.Impact.StateHistory",ImpactFlags)
bool FWaterImpactHistoryTest::RunTest(const FString& Parameters)
{
 FString Error;auto S=ImpactBaseline();const auto P=ImpactFixturePolicy();const TArray<bool> Power={true,true,true};
 FWaterImpactModel A,B;TestTrue(TEXT("Explicit reset"),A.Reset(S,Power,P,Error));TestTrue(TEXT("Second reset"),B.Reset(S,Power,P,Error));
 S.SimTimeS=.5;TestTrue(TEXT("No physical arrival"),A.Step(S,Power,Error));TestTrue(TEXT("Repeat history"),B.Step(S,Power,Error));
 TestTrue(TEXT("No leak-hole-driven transition"),A.GetState().Phase[0]==EWaterImpactPhase::Normal);
 // Three overlapping kinds of contact are evidence, not three copies of elapsed wet time.
 S.SimTimeS=1;S.InteriorPassedM3[0]=S.InteriorHeldM3[0]=.00002;S.FloodImmersionM[0]=.01;S.SprayTouchedM3[0]=.001;
 TestTrue(TEXT("Arrival"),A.Step(S,Power,Error));TestTrue(TEXT("Deterministic arrival"),B.Step(S,Power,Error));
 TestEqual(TEXT("New arrival not retroactively integrated"),A.GetState().PoweredContactS[0],0.0);
 double MaxTimeError=0;
 for(int32 I=5;I<=24;++I) {
  S.SimTimeS=I*.25;TestTrue(TEXT("History step"),A.Step(S,Power,Error));TestTrue(TEXT("Same input history"),B.Step(S,Power,Error));
  TestTrue(TEXT("Exact state determinism"),SameState(A.GetState(),B.GetState()));
  TestTrue(TEXT("Other targets stay normal"),A.GetState().Phase[1]==EWaterImpactPhase::Normal && A.GetState().Phase[2]==EWaterImpactPhase::Normal);
  if(S.SimTimeS<3) TestTrue(TEXT("Before trip duration"),A.GetState().Phase[0]==EWaterImpactPhase::Wet);
  else if(S.SimTimeS<6) TestTrue(TEXT("Trip before damage duration"),A.GetState().Phase[0]==EWaterImpactPhase::Tripped);
 }
 TestTrue(TEXT("Damage reached"),A.GetState().Phase[0]==EWaterImpactPhase::Damaged);
 MaxTimeError=FMath::Max(FMath::Abs(A.GetState().TripS[0]-3),FMath::Abs(A.GetState().DamageS[0]-6));
 TestEqual(TEXT("Exact synthetic timing"),MaxTimeError,0.0);
 TestEqual(TEXT("No duplicate state events"),A.GetEvents().Num(),3);
 TestEqual(TEXT("Equal event count"),A.GetEvents().Num(),B.GetEvents().Num());
 for(int32 I=0;I<A.GetEvents().Num();++I) TestTrue(TEXT("Event history deterministic"),A.GetEvents()[I].SimTimeS==B.GetEvents()[I].SimTimeS && A.GetEvents()[I].To==B.GetEvents()[I].To);
 // One longer observation interval has the same hold assumption and crosses both thresholds exactly.
 FWaterImpactModel Long;auto C=ImpactBaseline();Long.Reset(C,Power,P,Error);C.SimTimeS=1;C.InteriorHeldM3[0]=.00002;Long.Step(C,Power,Error);C.SimTimeS=6;Long.Step(C,Power,Error);
 TestEqual(TEXT("Long interval trip time"),Long.GetState().TripS[0],A.GetState().TripS[0]);TestEqual(TEXT("Long interval damage time"),Long.GetState().DamageS[0],A.GetState().DamageS[0]);
 S.SimTimeS=7;S.InteriorHeldM3[0]=S.FloodImmersionM[0]=0;A.Step(S,Power,Error);
 TestTrue(TEXT("Dry/repair cannot undo damage"),A.GetState().Phase[0]==EWaterImpactPhase::Damaged);
 TestTrue(TEXT("Explicit restart"),A.Reset(S,Power,P,Error));TestTrue(TEXT("Old cumulative arrival not replayed"),A.GetState().Phase[0]==EWaterImpactPhase::Normal);
 TestEqual(TEXT("Restart clears events"),A.GetEvents().Num(),0);
 AddInfo(FString::Printf(TEXT("W4-1 trip/damage timing max error=%.17g s; normalized by 2 s trip duration=%.17g; exact repeated-history state/event difference=0; long vs short held-observation time difference=%.17g s"),MaxTimeError,MaxTimeError/2,FMath::Abs(Long.GetState().TripS[0]-3)));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactSourcesTest,"VRG.Water.Impact.SourcePolicies",ImpactFlags)
bool FWaterImpactSourcesTest::RunTest(const FString& Parameters)
{
 FString Error;const TArray<bool> Power={true,true,true};auto P=ImpactFixturePolicy();auto S=ImpactBaseline();
 // Rear bypass history is not contact with an internal board. Flood/rear are also not spray evidence.
 P.ContactSources=uint8(EWaterEvidence::Interior);FWaterImpactModel Model;Model.Reset(S,Power,P,Error);
 S.SimTimeS=1;S.InteriorPassedM3[0]=S.RearPassedM3[0]=S.RearMovingM3[0]=.001;S.FloodImmersionM[0]=1;S.SprayTouchedM3[0]=1;
 TestTrue(TEXT("Observe distinct sources"),Model.Step(S,Power,Error));TestTrue(TEXT("Rear-only bypass not internal board contact"),Model.GetState().Phase[0]==EWaterImpactPhase::Normal);
 // Front arrival that passed through between observations must still warn, without treating rear as front.
 S.SimTimeS=2;S.InteriorPassedM3[0]+=.00002;
 TestTrue(TEXT("Transient front arrival observed"),Model.Step(S,Power,Error));TestTrue(TEXT("Passed-through water not missed"),Model.GetState().Phase[0]==EWaterImpactPhase::Wet);
 S.SimTimeS=1;S.InteriorPassedM3[0]=S.RearPassedM3[0];
 P.ContactSources=uint8(EWaterEvidence::Rear);Model.Reset(ImpactBaseline(),Power,P,Error);TestTrue(TEXT("Rear source observed"),Model.Step(S,Power,Error));TestTrue(TEXT("Rear contact warning"),Model.GetState().Phase[0]==EWaterImpactPhase::Wet);
 P=ImpactFixturePolicy();P.ContactSources=uint8(EWaterEvidence::Spray);P.WetRecovery=EWaterWetRecovery::ClearWarningWhenDry;
 Model.Reset(ImpactBaseline(),Power,P,Error);S=ImpactBaseline();S.SimTimeS=1;S.SprayTouchedM3[0]=.001;Model.Step(S,Power,Error);
 TestTrue(TEXT("Actual spray arrival warns"),Model.GetState().Phase[0]==EWaterImpactPhase::Wet);
 S.SimTimeS=2;Model.Step(S,Power,Error);TestTrue(TEXT("Cumulative spray not permanent wetness"),Model.GetState().Phase[0]==EWaterImpactPhase::Normal);
 TestEqual(TEXT("No repeated arrival duration"),Model.GetState().PoweredContactS[0],0.0);
 // Positive thresholds and power observation are explicit caller inputs.
 P=ImpactFixturePolicy();P.ContactSources=uint8(EWaterEvidence::Flood);P.FloodImmersionM=.01;
 Model.Reset(ImpactBaseline(),Power,P,Error);S=ImpactBaseline();S.FloodImmersionM[0]=.005;S.SimTimeS=1;Model.Step(S,Power,Error);
 TestTrue(TEXT("Below threshold"),Model.GetState().Phase[0]==EWaterImpactPhase::Normal);
 S.FloodImmersionM[0]=.02;S.SimTimeS=2;Model.Step(S,{false,true,true},Error);S.SimTimeS=5;Model.Step(S,{false,true,true},Error);
 TestTrue(TEXT("Unpowered wet device not tripped"),Model.GetState().Phase[0]==EWaterImpactPhase::Wet);
 S.SimTimeS=6;Model.Step(S,Power,Error);S.SimTimeS=8;Model.Step(S,Power,Error);
 TestTrue(TEXT("Powered duration causes trip"),Model.GetState().Phase[0]==EWaterImpactPhase::Tripped);
 S.SimTimeS=9;S.FloodImmersionM[0]=0;Model.Step(S,Power,Error);S.SimTimeS=10;Model.Step(S,Power,Error);
 TestTrue(TEXT("Trip remains after water disappears"),Model.GetState().Phase[0]==EWaterImpactPhase::Tripped);
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterImpactValidationTest,"VRG.Water.Impact.StateValidation",ImpactFlags)
bool FWaterImpactValidationTest::RunTest(const FString& Parameters)
{
 FString Error;auto S=ImpactBaseline();auto P=ImpactFixturePolicy();const TArray<bool> Power={true,true,true};FWaterImpactModel Model;
 TestFalse(TEXT("Operational defaults intentionally unset"),Model.Reset(S,Power,FWaterImpactPolicy(),Error));
 auto Invalid=P;Invalid.BasisNote.Reset();TestFalse(TEXT("Missing threshold basis"),Model.Reset(S,Power,Invalid,Error));
 Invalid=P;Invalid.Basis=EWaterThresholdBasis::PublicSource;TestFalse(TEXT("Unsupported public claim"),Model.Reset(S,Power,Invalid,Error));
 Invalid=P;Invalid.SprayArrivalM3=std::numeric_limits<double>::quiet_NaN();TestFalse(TEXT("NaN policy"),Model.Reset(S,Power,Invalid,Error));
 TestTrue(TEXT("Valid policy"),Model.Reset(S,Power,P,Error));S.SimTimeS=1;S.InteriorHeldM3[0]=.00002;S.InteriorPassedM3[0]=.00002;Model.Step(S,Power,Error);
 const auto Before=Model.GetState();const int32 Events=Model.GetEvents().Num();
 auto Reject=[&](const FWaterExposureSnapshot& Bad,const TArray<bool>& Pins) {TestFalse(TEXT("Invalid step rejected"),Model.Step(Bad,Pins,Error));TestTrue(TEXT("All targets unchanged"),SameState(Before,Model.GetState()));TestEqual(TEXT("Event ledger unchanged"),Model.GetEvents().Num(),Events);TestEqual(TEXT("SimTime unchanged"),Model.GetSimTimeS(),1.0);TestFalse(TEXT("Error reason"),Error.IsEmpty());};
 Reject(S,Power);auto Bad=S;Bad.SimTimeS=.5;Reject(Bad,Power);Bad=S;Bad.SimTimeS=12;Reject(Bad,Power);
 Bad=S;Bad.SimTimeS=2;Bad.InteriorPassedM3[0]=0;Reject(Bad,Power);
 Bad=S;Bad.SimTimeS=2;Bad.EquipmentId[2]=TEXT("Changed");Reject(Bad,Power);
 Bad=S;Bad.SimTimeS=2;Bad.InteriorHeldM3[2]=-1;Reject(Bad,Power);
 Bad=S;Bad.SimTimeS=2;Bad.Available[2]=0;Reject(Bad,Power);Bad=S;Bad.SimTimeS=2;Reject(Bad,{});
 // Failed reset must not discard an existing run either.
 TestFalse(TEXT("Invalid reset rejected"),Model.Reset(S,Power,FWaterImpactPolicy(),Error));TestTrue(TEXT("Reset failure atomic"),SameState(Before,Model.GetState()));
 TestTrue(TEXT("Valid next observation after rejection"),Model.Step(Bad,Power,Error));
 TestEqual(TEXT("Input snapshot unchanged"),Bad.InteriorHeldM3[0],.00002);
 return true;
}
#endif
