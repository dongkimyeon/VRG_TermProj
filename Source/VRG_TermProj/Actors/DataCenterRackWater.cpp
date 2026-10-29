#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"

void ADataCenterSimulationActor::InitializeRackInteriorWater()
{
	RackInterior = FRackInteriorCoupling(); bRackInteriorStepRejected = false;
	TArray<int32> Counts;
	TArray<double> Heights;
	TArray<FRackWaterSite> Sites;
	for (int32 R = 0; R < Racks.Num(); ++R)
	{
		const ARackThermalActor* Rack = Racks[R];
		Counts.Add(Rack ? Rack->Params.Slots.Num() : 0);
		if (!Rack) continue;
		for (const auto& Slot : Rack->Params.Slots)
		{
			const FVector LocalM(0.0, 0.0, Slot.BottomZM + 0.5 * Slot.HeightM);
			Heights.Add(SimUnits::UnrealToMeters(Rack->GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(LocalM))).Z);
		}
		if (const FInRackHandles* H = Room ? Room->GetInRackHandles(Rack) : nullptr)
			for (int32 T = 0; T < H->Slots.Num(); ++T)
				for (int32 K = 0; K < 5; ++K) Sites.Add({H->Leaks[T * 5 + K], R, H->Slots[T], K == 0 || K == 4});
	}
	if (!RackInterior.Initialize(Counts, Heights, Sites))
		UE_LOG(LogTemp, Warning, TEXT("[RackWater] Invalid slot/site mapping; input remains in the inventory ledger."));
}

void ADataCenterSimulationActor::RackInteriorAfterStep(double DtS)
{
	const bool bAccepted = RackInterior.Step(GetInternalLeakedM3(), DtS, Clock.GetSimTimeS());
	if (!bAccepted && !bRackInteriorStepRejected)
		UE_LOG(LogTemp, Warning, TEXT("[RackWater] Rejected input interval; retained pending %.12g m3."), RackInterior.PendingInputM3(GetInternalLeakedM3()));
	bRackInteriorStepRejected = !bAccepted;
}
