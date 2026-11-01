#include "Misc/AutomationTest.h"
#include <limits>
#include "Simulation/Water/WaterExposure.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
 constexpr EAutomationTestFlags ExposureFlags=EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterExposureViewsTest,"VRG.Water.Impact.ExposureViews",ExposureFlags)
bool FWaterExposureViewsTest::RunTest(const FString& Parameters)
{
 // Synthetic geometry/volumes for observer verification, not equipment damage limits.
 FSprayGeometry G;G.RoomM=FBox(FVector(-2,-2,0),FVector(5,5,4));
 for(int32 I=0;I<2;++I) {auto& O=G.Obstacles.AddDefaulted_GetRef();O.Name=I==0?TEXT("A"):TEXT("B");O.CenterM=FVector(I*2,0,1);O.HalfM=FVector(.5,.5,1);}
 FLeakSpray Spray;Spray.Reset(G,FSpraySettings(),FSprayFluid());
 FSpraySource Source;Source.Id=0;Source.OriginM=FVector(0,0,1);Source.VolumeM3=.001;Source.GaugePressurePa=10000;
 Spray.Step({Source},.02,0);
 FFloorWaterSettings Settings;Settings.CellM=.1;
 FFloorWater Floor;Floor.Reset(FVector2D::ZeroVector,2,2,0,Settings);Floor.AddFlood(TEXT("A"),{0,1});Floor.AddFlood(TEXT("B"),{2,3});
 for(int32 J=0;J<2;++J) for(int32 I=0;I<2;++I) TestTrue(TEXT("Floor arrival"),Floor.AddWater(FVector((I+.5)*.1,(J+.5)*.1,0),.0004));
 Floor.Step(.01,0);
 FRackInteriorWater Water;Water.Initialize({3,3},{0,1,2,0,1,2});Water.AddWater(0,2,.00002,true,.01);
 TArray<FWaterExposureBinding> Bindings;
 auto& Root=Bindings.AddDefaulted_GetRef();Root.EquipmentId=TEXT("A");Root.SprayObstacle=0;Root.FloorFlood=0;Root.FloorContactDepthM=0;
 for(int32 Slot:{0,2}) {auto& B=Bindings.AddDefaulted_GetRef();B.EquipmentId=TEXT("A");B.Slot=Slot;B.InteriorRack=0;B.SprayObstacle=0;B.FloorFlood=0;B.FloorContactDepthM=Slot+1;}
 auto& Other=Bindings.AddDefaulted_GetRef();Other.EquipmentId=TEXT("B");Other.Slot=2;Other.InteriorRack=1;
 const auto DepthBefore=Floor.GetDepthM();const auto HeldBefore=Water.HeldM3,RearBefore=Water.GetRearTransitM3();
 const double ReceivedBefore=Water.ReceivedM3,SprayBefore=Spray.GetWetting()[0].WaterM3;
 FWaterExposureSnapshot View,Repeat;FString Error;
 if(!TestTrue(TEXT("Collect existing views"),WaterExposure::Collect(Bindings,.02,Spray.GetGeometry(),Spray.GetWetting(),Floor.GetFloods(),Water,{TEXT("A"),TEXT("B")},View,Error))) {AddError(Error);return false;}
 TestEqual(TEXT("Whole-box spray contact"),View.SprayTouchedM3[0],SprayBefore);
 TestTrue(TEXT("Actual flood immersion"),FMath::Abs(View.FloodImmersionM[0]-.04)<1e-14);
 TestEqual(TEXT("No invented per-slot spray"),View.SprayTouchedM3[2],0.0);
 TestEqual(TEXT("One unlocalized diagnostic per source"),View.Diagnostics.Num(),1);
 TestEqual(TEXT("Unlocalized volume retained as diagnostic only"),View.Diagnostics[0].ObservedContactM3,SprayBefore);
 TestEqual(TEXT("Upper slot above flood"),View.FloodImmersionM[2],0.0);
 TestEqual(TEXT("Rear arrival exact native slot"),View.RearPassedM3[2],.00002);
 TestEqual(TEXT("Rear is subset, not additive water"),View.InteriorPassedM3[2],View.RearPassedM3[2]);
 TestEqual(TEXT("Rear transit separate from front"),View.InteriorMovingM3[2],0.0);
 TestEqual(TEXT("No lower-slot early arrival"),View.InteriorPassedM3[1],0.0);
 TestEqual(TEXT("No other-rack contamination"),View.InteriorPassedM3[3],0.0);
 TestTrue(TEXT("Repeated observation accepted without accumulating"),WaterExposure::Collect(Bindings,.02,Spray.GetGeometry(),Spray.GetWetting(),Floor.GetFloods(),Water,{TEXT("A"),TEXT("B")},Repeat,Error));
 TestTrue(TEXT("Snapshot idempotent"),Repeat.RearPassedM3==View.RearPassedM3 && Repeat.SprayTouchedM3==View.SprayTouchedM3);
 TestTrue(TEXT("Sources unchanged"),DepthBefore==Floor.GetDepthM() && HeldBefore==Water.HeldM3 && RearBefore==Water.GetRearTransitM3() && ReceivedBefore==Water.ReceivedM3 && SprayBefore==Spray.GetWetting()[0].WaterM3);
 const double ObserverDelta=FMath::Abs(Water.ReceivedM3-ReceivedBefore),FloorResidual=Floor.GetTotals().Residual,InteriorResidual=FMath::Abs(Water.BalanceErrorM3())/.00002;
 AddInfo(FString::Printf(TEXT("W4-1 observer delta=%.17g m3 (normalized by 2e-5 m3=%.17g); floor residual=%.17g (received 0.0016 m3); interior residual=%.17g (received 2e-5 m3); flood depth error=%.17g m"),ObserverDelta,ObserverDelta/.00002,FloorResidual,InteriorResidual,FMath::Abs(View.FloodImmersionM[0]-.04)));
 TestTrue(TEXT("Existing source conservation"),FloorResidual<1e-12 && InteriorResidual<1e-12);
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaterExposureValidationTest,"VRG.Water.Impact.ExposureValidation",ExposureFlags)
bool FWaterExposureValidationTest::RunTest(const FString& Parameters)
{
 FSprayGeometry G;auto& O=G.Obstacles.AddDefaulted_GetRef();O.Name=TEXT("A");
 TArray<FSprayWetting> Spray;Spray.AddDefaulted();TArray<FFloorFlood> Flood;auto& F=Flood.AddDefaulted_GetRef();F.Name=TEXT("A");
 FRackInteriorWater Water;Water.Initialize({1},{0});FWaterExposureBinding B;B.EquipmentId=TEXT("A");B.SprayObstacle=0;
 FWaterExposureSnapshot Out;Out.SimTimeS=123;FString Error;
 auto Reject=[&](const TArray<FWaterExposureBinding>& Bs,double Time) {TestFalse(TEXT("Invalid binding rejected"),WaterExposure::Collect(Bs,Time,G,Spray,Flood,Water,{TEXT("A")},Out,Error));TestEqual(TEXT("Failed collect atomic"),Out.SimTimeS,123.0);TestFalse(TEXT("Reason present"),Error.IsEmpty());};
 auto Bad=B;Bad.EquipmentId=TEXT("wrong");Reject({Bad},1);Bad=B;Bad.SprayObstacle=2;Reject({Bad},1);
 Bad=B;Bad.FloorFlood=0;Reject({Bad},1);Reject({B,B},1);Reject({B},-1);
 Spray[0].WaterM3=-.1;Reject({B},1);Spray[0].WaterM3=0;
 Bad=B;Bad.InteriorRack=0;Bad.Slot=3;Reject({Bad},1);
 TestTrue(TEXT("Valid baseline"),WaterExposure::Collect({B},1,G,Spray,Flood,Water,{TEXT("A")},Out,Error));
 auto Malformed=Out;Malformed.FloodImmersionM.Reset();TestFalse(TEXT("Bad SoA rejected"),Malformed.IsValid(Error));
 Malformed=Out;Malformed.SprayTouchedM3[0]=std::numeric_limits<double>::infinity();TestFalse(TEXT("Nonfinite exposure rejected"),Malformed.IsValid(Error));
 Malformed=Out;Malformed.Available[0]=0;Malformed.SprayTouchedM3[0]=1;TestFalse(TEXT("Value without source rejected"),Malformed.IsValid(Error));
 return true;
}
#endif
