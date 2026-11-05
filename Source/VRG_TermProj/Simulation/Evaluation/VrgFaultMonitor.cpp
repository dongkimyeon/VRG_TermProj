// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Evaluation/VrgFaultMonitor.h"

void FVrgFaultMonitor::Reset(const FVrgFaultSettings& InSettings)
{
	Settings = InSettings;
	Faults.Reset(); Seen.Reset(); Leaks.Reset(); Valves.Reset(); TankSamples.Reset(); WaterImpacts.Reset();
}

void FVrgFaultMonitor::Record(double TimeS, int32 Rack, EVrgFaultKind Kind, double Value, int32& InOutNew)
{
	const uint32 Key = uint32(Rack + 1) * 8u + uint32(Kind);
	if (Seen.Contains(Key)) return;
	Seen.Add(Key);
	FVrgFaultRecord R;
	R.TimeS = TimeS; R.Rack = Rack; R.Kind = Kind; R.Value = Value;
	Faults.Add(R);
	++InOutNew;
}

int32 FVrgFaultMonitor::Update(const FVrgFaultInput& In)
{
	int32 New = 0;
	if (!In.bHydraulicsConverged) Record(In.TimeS, INDEX_NONE, EVrgFaultKind::HydraulicNotConverged, In.HydraulicResidual, New);
	for (int32 R = 0; R < In.CoolantMassFlowKgs.Num(); ++R)
	{
		const double Design = In.DesignMassFlowKgs.IsValidIndex(R) ? In.DesignMassFlowKgs[R] : 0.0;
		if (Design > 0.0)
		{
			const double Ratio = In.CoolantMassFlowKgs[R] / Design;
			if (Ratio < Settings.LowFlowRatio) Record(In.TimeS, R, EVrgFaultKind::LowCoolantFlow, Ratio, New);
		}
		if (In.MaxComponentTempK.IsValidIndex(R) && In.MaxComponentTempK[R] > Settings.MaxComponentTempK)
			Record(In.TimeS, R, EVrgFaultKind::OverTemperature, In.MaxComponentTempK[R], New);
		if (In.LoopDepleted.IsValidIndex(R) && In.LoopDepleted[R]) Record(In.TimeS, R, EVrgFaultKind::InventoryDepleted, 0.0, New);
	}
	return New;
}

int32 FVrgFaultMonitor::AddWaterImpact(const FVrgWaterImpactRecord& R)
{
	int32 New = 0;
	if (R.Rack != INDEX_NONE && R.To == TEXT("tripped")) Record(R.TimeS, R.Rack, EVrgFaultKind::WaterTrip, R.Slot, New);
	if (R.Rack != INDEX_NONE && R.To == TEXT("damaged")) Record(R.TimeS, R.Rack, EVrgFaultKind::WaterDamage, R.Slot, New);
	return WaterImpacts.Add(R);
}

void FVrgFaultMonitor::SetWaterImpactCut(int32 Index, double AppliedS, double ItBeforeW, double ItAfterW)
{
	if (!WaterImpacts.IsValidIndex(Index)) return;
	FVrgWaterImpactRecord& R = WaterImpacts[Index];
	R.CutAppliedS = AppliedS; R.ItBeforeW = ItBeforeW; R.ItAfterW = ItAfterW;
}

double FVrgFaultMonitor::GetFirstFaultTimeS(int32 Rack) const
{
	double T = -1.0;
	for (const FVrgFaultRecord& F : Faults) if (F.Rack == Rack && (T < 0.0 || F.TimeS < T)) T = F.TimeS;
	return T;
}

const TCHAR* FVrgFaultMonitor::KindName(EVrgFaultKind Kind)
{
	switch (Kind)
	{
	case EVrgFaultKind::LowCoolantFlow: return TEXT("lowCoolantFlow");
	case EVrgFaultKind::OverTemperature: return TEXT("overTemperature");
	case EVrgFaultKind::InventoryDepleted: return TEXT("inventoryDepleted");
	case EVrgFaultKind::WaterTrip: return TEXT("waterTrip");
	case EVrgFaultKind::WaterDamage: return TEXT("waterDamage");
	default: return TEXT("hydraulicNotConverged");
	}
}

namespace
{
	TArray<TSharedPtr<FJsonValue>> IntArray(const TArray<int32>& A)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const int32 V : A) Out.Add(MakeShared<FJsonValueNumber>(V));
		return Out;
	}
	TArray<int32> ReadInts(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
	{
		TArray<int32> Out;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (O->TryGetArrayField(Field, A)) for (const TSharedPtr<FJsonValue>& V : *A) Out.Add(int32(V->AsNumber()));
		return Out;
	}
}

TSharedRef<FJsonObject> FVrgFaultMonitor::ToJsonObject() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("lowFlowRatio"), Settings.LowFlowRatio);
	Root->SetNumberField(TEXT("maxComponentTempK"), Settings.MaxComponentTempK);
	Root->SetStringField(TEXT("basis"), TEXT("judged from the computed flows and temperatures only (D4); thresholds are display settings, the temperature limit is an estimate"));
	TArray<TSharedPtr<FJsonValue>> F, L, V, T;
	for (const FVrgFaultRecord& R : Faults)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("timeS"), R.TimeS); O->SetNumberField(TEXT("rack"), R.Rack);
		O->SetStringField(TEXT("kind"), KindName(R.Kind)); O->SetNumberField(TEXT("value"), R.Value);
		F.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const FVrgLeakRecord& R : Leaks)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("timeS"), R.TimeS); O->SetNumberField(TEXT("rack"), R.Rack);
		O->SetBoolField(TEXT("returnBranch"), R.bReturnBranch); O->SetNumberField(TEXT("holeDiameterM"), R.HoleDiameterM);
		if (!R.Site.IsEmpty())
		{
			// W1 live leak (optional fields; older reports lack them).
			O->SetStringField(TEXT("site"), R.Site);
			O->SetArrayField(TEXT("pointM"), { MakeShared<FJsonValueNumber>(R.PointM.X), MakeShared<FJsonValueNumber>(R.PointM.Y), MakeShared<FJsonValueNumber>(R.PointM.Z) });
		}
		L.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const FVrgValveRecord& R : Valves)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("timeS"), R.TimeS); O->SetNumberField(TEXT("rack"), R.Rack); O->SetBoolField(TEXT("closed"), R.bClosed);
		O->SetArrayField(TEXT("isolatedRacks"), IntArray(R.IsolatedRacks)); O->SetArrayField(TEXT("impactedRacks"), IntArray(R.ImpactedRacks));
		V.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const FVrgTankSample& S : TankSamples)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("timeS"), S.TimeS); O->SetNumberField(TEXT("row"), S.Row); O->SetNumberField(TEXT("pressurePa"), S.PressurePa);
		O->SetNumberField(TEXT("waterM3"), S.WaterM3); O->SetNumberField(TEXT("leakedM3"), S.LeakedM3); O->SetBoolField(TEXT("depleted"), S.bDepleted);
		T.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("faults"), F);
	Root->SetArrayField(TEXT("leaks"), L);
	Root->SetArrayField(TEXT("valves"), V);
	Root->SetArrayField(TEXT("tank"), T);
	// W4 (optional field; older reports lack it).
	TArray<TSharedPtr<FJsonValue>> W;
	for (const FVrgWaterImpactRecord& R : WaterImpacts)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("timeS"), R.TimeS); O->SetStringField(TEXT("equipmentId"), R.EquipmentId); O->SetNumberField(TEXT("rack"), R.Rack); O->SetNumberField(TEXT("slot"), R.Slot);
		O->SetStringField(TEXT("device"), R.Device); O->SetStringField(TEXT("from"), R.From); O->SetStringField(TEXT("to"), R.To); O->SetStringField(TEXT("cause"), R.Cause);
		O->SetNumberField(TEXT("immersionM"), R.ImmersionM); O->SetNumberField(TEXT("sprayM3"), R.SprayM3); O->SetNumberField(TEXT("interiorM3"), R.InteriorM3);
		O->SetNumberField(TEXT("cutAppliedS"), R.CutAppliedS); O->SetNumberField(TEXT("itBeforeW"), R.ItBeforeW); O->SetNumberField(TEXT("itAfterW"), R.ItAfterW);
		W.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("waterImpacts"), W);
	return Root;
}

bool FVrgFaultMonitor::FromJsonObject(const TSharedPtr<FJsonObject>& Object, FVrgFaultMonitor& Out)
{
	Out.Reset(FVrgFaultSettings());
	if (!Object.IsValid()) return true;   // older report without the S2b object
	Object->TryGetNumberField(TEXT("lowFlowRatio"), Out.Settings.LowFlowRatio);
	Object->TryGetNumberField(TEXT("maxComponentTempK"), Out.Settings.MaxComponentTempK);
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (Object->TryGetArrayField(TEXT("faults"), A))
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return false;
			FVrgFaultRecord R;
			R.TimeS = O->GetNumberField(TEXT("timeS")); R.Rack = int32(O->GetNumberField(TEXT("rack"))); R.Value = O->GetNumberField(TEXT("value"));
			const FString Kind = O->GetStringField(TEXT("kind"));
			bool bKnown = false;
			for (const EVrgFaultKind K : { EVrgFaultKind::LowCoolantFlow, EVrgFaultKind::OverTemperature, EVrgFaultKind::InventoryDepleted, EVrgFaultKind::HydraulicNotConverged, EVrgFaultKind::WaterTrip, EVrgFaultKind::WaterDamage })
				if (Kind == KindName(K)) { R.Kind = K; bKnown = true; }
			if (!bKnown) return false;
			Out.Faults.Add(R);
			Out.Seen.Add(uint32(R.Rack + 1) * 8u + uint32(R.Kind));
		}
	if (Object->TryGetArrayField(TEXT("leaks"), A))
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return false;
			FVrgLeakRecord R;
			R.TimeS = O->GetNumberField(TEXT("timeS")); R.Rack = int32(O->GetNumberField(TEXT("rack")));
			R.bReturnBranch = O->GetBoolField(TEXT("returnBranch")); R.HoleDiameterM = O->GetNumberField(TEXT("holeDiameterM"));
			O->TryGetStringField(TEXT("site"), R.Site);
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (O->TryGetArrayField(TEXT("pointM"), P) && P->Num() == 3) R.PointM = FVector((*P)[0]->AsNumber(), (*P)[1]->AsNumber(), (*P)[2]->AsNumber());
			Out.Leaks.Add(R);
		}
	if (Object->TryGetArrayField(TEXT("valves"), A))
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return false;
			FVrgValveRecord R;
			R.TimeS = O->GetNumberField(TEXT("timeS")); R.Rack = int32(O->GetNumberField(TEXT("rack"))); R.bClosed = O->GetBoolField(TEXT("closed"));
			R.IsolatedRacks = ReadInts(O, TEXT("isolatedRacks")); R.ImpactedRacks = ReadInts(O, TEXT("impactedRacks"));
			Out.Valves.Add(R);
		}
	if (Object->TryGetArrayField(TEXT("tank"), A))
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return false;
			FVrgTankSample S;
			S.TimeS = O->GetNumberField(TEXT("timeS")); S.Row = int32(O->GetNumberField(TEXT("row"))); S.PressurePa = O->GetNumberField(TEXT("pressurePa"));
			S.WaterM3 = O->GetNumberField(TEXT("waterM3")); S.LeakedM3 = O->GetNumberField(TEXT("leakedM3")); S.bDepleted = O->GetBoolField(TEXT("depleted"));
			Out.TankSamples.Add(S);
		}
	if (Object->TryGetArrayField(TEXT("waterImpacts"), A))
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) return false;
			FVrgWaterImpactRecord R;
			R.TimeS = O->GetNumberField(TEXT("timeS")); R.EquipmentId = O->GetStringField(TEXT("equipmentId")); R.Rack = int32(O->GetNumberField(TEXT("rack"))); R.Slot = int32(O->GetNumberField(TEXT("slot")));
			R.Device = O->GetStringField(TEXT("device")); R.From = O->GetStringField(TEXT("from")); R.To = O->GetStringField(TEXT("to")); R.Cause = O->GetStringField(TEXT("cause"));
			R.ImmersionM = O->GetNumberField(TEXT("immersionM")); R.SprayM3 = O->GetNumberField(TEXT("sprayM3")); R.InteriorM3 = O->GetNumberField(TEXT("interiorM3"));
			R.CutAppliedS = O->GetNumberField(TEXT("cutAppliedS")); R.ItBeforeW = O->GetNumberField(TEXT("itBeforeW")); R.ItAfterW = O->GetNumberField(TEXT("itAfterW"));
			Out.WaterImpacts.Add(R);
		}
	return true;
}

bool FVrgFaultMonitor::operator==(const FVrgFaultMonitor& O) const
{
	return Settings.LowFlowRatio == O.Settings.LowFlowRatio && Settings.MaxComponentTempK == O.Settings.MaxComponentTempK
		&& Faults == O.Faults && Leaks == O.Leaks && Valves == O.Valves && TankSamples == O.TankSamples && WaterImpacts == O.WaterImpacts;
}
