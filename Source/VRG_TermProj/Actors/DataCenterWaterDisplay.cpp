// W6 (Tools/누수_실시간_계획.md L6, 2026-11-10): the computed water drawn as water, and the W4 equipment states.
// Display only: nothing here changes a model or the clock (rule 4); wall time only paces the floor mesh upload.

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/WaterDisplayActor.h"
#include "DrawDebugHelpers.h"
#include "HAL/PlatformTime.h"
#include "Simulation/Core/SimUnits.h"

void ADataCenterSimulationActor::BuildWaterDisplay(bool bUpdateMarks)
{
	if (bUpdateMarks) WaterDisplay::UpdateEverWet(FloorWater, WaterEverWet, WaterDisplaySettings.MinDepthM);
	WaterDisplay::Build(FloorWater, Spray, &WaterEverWet, WaterDisplaySettings, WaterDisplaySnapshot);
}

void ADataCenterSimulationActor::UpdateWaterDisplay()
{
	const bool bAnyWater = Spray.HasWater() || FloorWater.HasWater();
	const bool bWant = bShowSpray && WaterShowStyle == 1 && Mode == ESimMode::Simulation && GetWorld() != nullptr;
	if (!bAnyWater)
	{
		// A restart / new entry: the models are empty again, so is the display memory.
		WaterEverWet.Reset();
		LastWaterDisplaySimS = -1.0;
	}
	if (!bWant || !bAnyWater)
	{
		if (WaterDisplayActor != nullptr) WaterDisplayActor->Clear();
		WaterDisplaySnapshot = FWaterDisplaySnapshot();
		if (bWant) DrawWaterImpactOutlines();
		return;
	}
	const double SimS = Clock.GetSimTimeS();
	if (SimS == LastWaterDisplaySimS && WaterDisplayActor != nullptr) { DrawWaterImpactOutlines(); return; }   // nothing stepped
	LastWaterDisplaySimS = SimS;
	if (WaterDisplayActor == nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		WaterDisplayActor = GetWorld()->SpawnActor<AWaterDisplayActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		LastWaterFloorWallS = -1.0;
	}
	BuildWaterDisplay(true);
	// The floor mesh is the expensive upload: at most 10 times a second (wall time paces the drawing only).
	const double NowS = FPlatformTime::Seconds();
	const bool bFloor = LastWaterFloorWallS < 0.0 || NowS - LastWaterFloorWallS >= 0.1;
	if (bFloor) LastWaterFloorWallS = NowS;
	if (WaterDisplayActor != nullptr) WaterDisplayActor->Show(WaterDisplaySnapshot, bFloor);
	DrawWaterImpactOutlines();
}

void ADataCenterSimulationActor::GetWaterImpactViews(TArray<FWaterImpactView>& Out) const
{
	Out.Reset();
	const TArray<EWaterImpactPhase>& Phase = WaterImpactObservation.GetState().Phase;
	const FSprayGeometry& G = Spray.GetGeometry();
	for (int32 I = 0; I < FMath::Min(Phase.Num(), WaterExposureBindings.Num()); ++I)
	{
		if (Phase[I] == EWaterImpactPhase::Normal) continue;
		const FWaterExposureBinding& B = WaterExposureBindings[I];
		if (!G.Obstacles.IsValidIndex(B.SprayObstacle)) continue;
		const FSprayObstacle& O = G.Obstacles[B.SprayObstacle];
		FWaterImpactView& V = Out.AddDefaulted_GetRef();
		V.EquipmentId = B.EquipmentId; V.Slot = B.Slot; V.Phase = uint8(Phase[I]);
		V.CenterM = O.CenterM; V.Rotation = O.Rotation; V.HalfM = O.HalfM;
		// A rack slot: the slot's height band of the rack box.
		if (B.Slot != INDEX_NONE && Racks.IsValidIndex(B.InteriorRack) && IsValid(Racks[B.InteriorRack]))
		{
			double Bottom = 0.0, Height = 0.0;
			Racks[B.InteriorRack]->GetSlotHeightRangeM(B.Slot, Bottom, Height);
			const double BaseZ = O.CenterM.Z - O.HalfM.Z;
			V.CenterM.Z = BaseZ + Bottom + .5 * Height;
			V.HalfM.Z = .5 * Height;
		}
	}
}

void ADataCenterSimulationActor::GetWaterImpactEventLines(int32 MaxLines, TArray<FString>& Out) const
{
	Out.Reset();
	const TArray<FVrgWaterImpactRecord>& R = FaultMonitor.GetWaterImpacts();
	for (int32 I = FMath::Max(0, R.Num() - MaxLines); I < R.Num(); ++I)
	{
		const FVrgWaterImpactRecord& E = R[I];
		Out.Add(FString::Printf(TEXT("t=%.1f s %s%s %s → %s (%s)%s"), E.TimeS, *E.EquipmentId,
			E.Slot != INDEX_NONE ? *FString::Printf(TEXT(" 슬롯 %d"), E.Slot) : TEXT(""), *E.From, *E.To, *E.Cause,
			E.CutAppliedS >= 0.0 ? *FString::Printf(TEXT(", 차단 %.1f s"), E.CutAppliedS) : TEXT("")));
	}
}

void ADataCenterSimulationActor::DrawWaterImpactOutlines() const
{
	UWorld* World = GetWorld();
	if (World == nullptr) return;
	TArray<FWaterImpactView> Views;
	GetWaterImpactViews(Views);
	for (const FWaterImpactView& V : Views)
	{
		// Wet = cyan, tripped = orange, damaged = red (display colours, the states are W4's).
		const FColor C = V.Phase >= 3 ? FColor(255, 40, 40) : V.Phase == 2 ? FColor(255, 150, 20) : FColor(60, 220, 255);
		DrawDebugBox(World, SimUnits::MetersToUnreal(V.CenterM), SimUnits::MetersToUnreal(V.HalfM + FVector(.01)), V.Rotation, C, false, -1.0f, SDPG_World, V.Slot != INDEX_NONE ? 1.0f : 2.0f);
	}
}
