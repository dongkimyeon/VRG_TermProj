// W4 follow-up (2026-11-11, Tools/누수_실시간_계획.md "W4 후속: 마당 펌프 침수"): water on the plant yard ground spreads
// (the W3 shallow-water model over the yard, building footprint solid), and a placed yard pump whose footprint the water
// covers, or that the spray hits, can trip under vrg.Water.Impact through the plant's existing pump fault path
// (FFacilityPlant::TripPump: standby start and network re-solve, D4). Nothing is decided here about the plant's response.

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Simulation/Core/SimUnits.h"

void ADataCenterSimulationActor::ResetYardWater()
{
	YardOffGridM3 = 0.0;
	// Extent: every box standing outside the building and every plant leak site, plus a margin.
	const FSprayGeometry& G = Spray.GetGeometry();
	FBox2D Yard(ForceInit);
	auto Outside = [&G](const FVector& P)
	{
		return !G.RoomM.IsValid || P.X < G.RoomM.Min.X || P.X > G.RoomM.Max.X || P.Y < G.RoomM.Min.Y || P.Y > G.RoomM.Max.Y;
	};
	for (const FSprayObstacle& O : G.Obstacles)
	{
		if (O.Kind == ESprayObstacleKind::Ceiling || !Outside(O.CenterM)) continue;
		const double R = FMath::Sqrt(O.HalfM.X * O.HalfM.X + O.HalfM.Y * O.HalfM.Y);
		Yard += FVector2D(O.CenterM.X - R, O.CenterM.Y - R);
		Yard += FVector2D(O.CenterM.X + R, O.CenterM.Y + R);
	}
	for (const FLiveLeakSite& S : LiveLeakSites)
		if (S.bPlant && Outside(S.PositionM)) Yard += FVector2D(S.PositionM.X, S.PositionM.Y);
	if (!Yard.bIsValid)
	{
		YardWater.ResetForYard(G, FBox2D(ForceInit), FloorWaterSettings);   // no yard: every ground deposit stays off the grid
		return;
	}
	Yard = Yard.ExpandBy(FMath::Max(YardMarginM, 0.0));
	FFloorWaterSettings S = FloorWaterSettings;
	S.CellM = FMath::Max(YardCellM, 1e-3);
	S.GpuMode = 0;   // CPU reference path (the yard is sparse); the GPU stays a room-floor option
	YardWater.ResetForYard(G, Yard, S);
	UE_LOG(LogTemp, Display, TEXT("[Water] 설비 마당 땅 물 격자: %d x %d (%.2f m 칸), 침수 기록 %d개(마당 장비)"),
		YardWater.GetNx(), YardWater.GetNy(), S.CellM, YardWater.GetFloods().Num());
}

bool ADataCenterSimulationActor::WaterBindingPlantPump(int32 Index, EPlantLoopId& OutLoop, int32& OutPump) const
{
	if (!WaterExposureBindings.IsValidIndex(Index) || WaterExposureBindings[Index].Slot != INDEX_NONE || !bPlantActive || !Plant.IsThermalActive()) return false;
	const FString& Id = WaterExposureBindings[Index].EquipmentId;
	const APlantEquipmentActor* Unit = nullptr;
	for (const FWaterSourceTarget& T : WaterSourceTargets)
		if (T.bNative && T.Id == Id) { Unit = Cast<APlantEquipmentActor>(T.Actor.Get()); break; }
	if (Unit == nullptr || GetWorld() == nullptr) return false;
	switch (Unit->Kind)
	{
	case EPlantUnitKind::FwsPump: OutLoop = EPlantLoopId::Fws; break;
	case EPlantUnitKind::ChwPump: OutLoop = EPlantLoopId::Chw; break;
	case EPlantUnitKind::CwPump: OutLoop = EPlantLoopId::Cw; break;
	default: return false;
	}
	// Unit K of the plant model is the K-th placed actor of the kind (VrgPlantEquipment::Gather).
	OutPump = VrgPlantEquipment::Gather(GetWorld(), Unit->Kind).IndexOfByKey(const_cast<APlantEquipmentActor*>(Unit));
	const FFacilityWaterLoop* L = Plant.GetLoopFor(OutLoop);
	return L != nullptr && OutPump != INDEX_NONE && OutPump < L->GetHandles().Pumps.Num();
}
