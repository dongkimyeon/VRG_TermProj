#pragma once
#include "CoreMinimal.h"
#include "Simulation/Water/LeakSpray.h"
#include "Simulation/Water/FloorWater.h"
#include "Simulation/Water/RackInteriorWater.h"

// W4-1 observations only. These are contact records, NOT a transferable water/enthalpy ledger.
enum class EWaterEvidence : uint8 { Spray=1, Flood=2, Interior=4, Rear=8 };
struct FWaterExposureBinding
{
 FString EquipmentId;
 int32 Slot=INDEX_NONE; // equipment as a whole, or an exact native slot index
 int32 SprayObstacle=INDEX_NONE, FloorFlood=INDEX_NONE, InteriorRack=INDEX_NONE;
 // Explicit height above the floor-water bed [m], including slot height/clearance.
 // Unset is invalid when a flood binding is supplied; no guessed rack/slot geometry.
 double FloorContactDepthM=-1;
};
struct FWaterExposureDiagnostic
{
 FString EquipmentId, Reason;
 int32 SprayObstacle=INDEX_NONE;
 double ObservedContactM3=0;
};
struct FWaterExposureSnapshot
{
 double SimTimeS=0;
 TArray<FString> EquipmentId;
 TArray<int32> Slot;
 TArray<uint8> Available;
 // Flat SoA. Flood is current immersion, spray/interior/rear are cumulative contact histories.
 // Rear is a SUBSET of Passed, never add the two; spray may observe water later seen on the floor.
 TArray<double> SprayTouchedM3, FloodImmersionM, InteriorPassedM3, InteriorHeldM3, InteriorMovingM3, RearMovingM3, RearPassedM3;
 TArray<FWaterExposureDiagnostic> Diagnostics;
 bool IsValid(FString& Error) const;
};
namespace WaterExposure
{
 // Supply existing GetGeometry()/GetWetting()/GetFloods()/GetWater() views and the explicit rack-name map.
 // Pure read: no UObject, hydraulic flow integration, source mutation or hidden cumulative baseline.
 bool Collect(const TArray<FWaterExposureBinding>& Bindings,double SimTimeS,
  const FSprayGeometry& Geometry,const TArray<FSprayWetting>& Spray,const TArray<FFloorFlood>& Flood,
  const FRackInteriorWater& Interior,const TArray<FString>& RackIds,FWaterExposureSnapshot& Out,FString& Error);
}
