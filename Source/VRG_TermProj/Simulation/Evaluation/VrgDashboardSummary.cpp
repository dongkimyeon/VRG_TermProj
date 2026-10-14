// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Evaluation/VrgDashboardSummary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <charconv>

FString FVrgDashboardSummary::ToJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	Root->SetStringField(TEXT("mode"), bSimulating ? TEXT("simulation") : TEXT("layout"));
	Root->SetBoolField(TEXT("evalReady"), bEvalReady);
	Root->SetNumberField(TEXT("windowS"), WindowS);
	Root->SetNumberField(TEXT("itWindowW"), ItWindowW);
	Root->SetNumberField(TEXT("itNowW"), ItNowW);
	Root->SetNumberField(TEXT("coolantRemovedNowW"), CoolantRemovedNowW);
	Root->SetNumberField(TEXT("airRemovedNowW"), AirRemovedNowW);
	Root->SetNumberField(TEXT("removedNowW"), CoolantRemovedNowW + AirRemovedNowW);
	if (bPartialPueValid) Root->SetNumberField(TEXT("partialPueWindow"), PartialPueWindow);
	else Root->SetField(TEXT("partialPueWindow"), MakeShared<FJsonValueNull>());
	Root->SetStringField(TEXT("partialPueScope"), TEXT("(IT + CRAH fans + pumps) / IT; chillers, cooling towers, UPS/distribution losses and lighting are not modelled"));
	Root->SetNumberField(TEXT("inletCount"), InletCount);
	Root->SetNumberField(TEXT("inletWindowMeanMaxK"), InletWindowMeanMaxK);
	Root->SetNumberField(TEXT("inletWindowMeanAvgK"), InletWindowMeanAvgK);
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

bool FVrgDashboardSummary::FromJson(const FString& Json, FVrgDashboardSummary& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardSummary S;
	FString Mode;
	double Count = 0.0;
	const bool bOk = Root->TryGetNumberField(TEXT("simTimeS"), S.SimTimeS) && Root->TryGetStringField(TEXT("mode"), Mode)
		&& Root->TryGetBoolField(TEXT("evalReady"), S.bEvalReady) && Root->TryGetNumberField(TEXT("windowS"), S.WindowS)
		&& Root->TryGetNumberField(TEXT("itWindowW"), S.ItWindowW) && Root->TryGetNumberField(TEXT("itNowW"), S.ItNowW)
		&& Root->TryGetNumberField(TEXT("coolantRemovedNowW"), S.CoolantRemovedNowW) && Root->TryGetNumberField(TEXT("airRemovedNowW"), S.AirRemovedNowW)
		&& Root->TryGetNumberField(TEXT("inletCount"), Count) && Root->TryGetNumberField(TEXT("inletWindowMeanMaxK"), S.InletWindowMeanMaxK)
		&& Root->TryGetNumberField(TEXT("inletWindowMeanAvgK"), S.InletWindowMeanAvgK);
	if (!bOk || (Mode != TEXT("simulation") && Mode != TEXT("layout"))) return false;
	S.bSimulating = Mode == TEXT("simulation");
	S.InletCount = int32(Count);
	S.bPartialPueValid = Root->TryGetNumberField(TEXT("partialPueWindow"), S.PartialPueWindow);
	Out = S;
	return true;
}

FString FVrgDashboardSeries::ToJson(double SinceS) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("generation"), Generation);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	Root->SetNumberField(TEXT("intervalS"), 1.0);
	Root->SetNumberField(TEXT("lengthS"), LengthS);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FVrgSeriesPoint& P : Points)
	{
		if (!(P.TimeS > SinceS)) continue;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("t"), P.TimeS);
		O->SetNumberField(TEXT("inletMaxK"), P.InletMaxK);
		O->SetNumberField(TEXT("inletMeanK"), P.InletMeanK);
		O->SetNumberField(TEXT("itW"), P.ItW);
		O->SetNumberField(TEXT("coolantRemovedW"), P.CoolantRemovedW);
		O->SetNumberField(TEXT("airRemovedW"), P.AirRemovedW);
		if (P.bPartialPueValid) O->SetNumberField(TEXT("partialPue"), P.PartialPue);
		else O->SetField(TEXT("partialPue"), MakeShared<FJsonValueNull>());
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("points"), Items);
	TArray<TSharedPtr<FJsonValue>> EventItems;
	for (const FVrgDashboardEvent& E : Events)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("t"), E.TimeS);
		O->SetStringField(TEXT("kind"), E.Kind);
		O->SetNumberField(TEXT("index"), E.Index);
		O->SetNumberField(TEXT("value"), E.Value);
		O->SetBoolField(TEXT("applied"), E.bApplied);
		EventItems.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("events"), EventItems);
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

bool FVrgDashboardSeries::FromJson(const FString& Json, FVrgDashboardSeries& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardSeries S;
	double Gen = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* EventItems = nullptr;
	if (!Root->TryGetNumberField(TEXT("generation"), Gen) || !Root->TryGetNumberField(TEXT("simTimeS"), S.SimTimeS)
		|| !Root->TryGetNumberField(TEXT("lengthS"), S.LengthS) || !Root->TryGetArrayField(TEXT("points"), Items)
		|| !Root->TryGetArrayField(TEXT("events"), EventItems)) return false;
	S.Generation = int32(Gen);
	for (const TSharedPtr<FJsonValue>& V : *Items)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgSeriesPoint P;
		if (!O.IsValid() || !O->TryGetNumberField(TEXT("t"), P.TimeS) || !O->TryGetNumberField(TEXT("inletMaxK"), P.InletMaxK)
			|| !O->TryGetNumberField(TEXT("inletMeanK"), P.InletMeanK) || !O->TryGetNumberField(TEXT("itW"), P.ItW)
			|| !O->TryGetNumberField(TEXT("coolantRemovedW"), P.CoolantRemovedW)
			|| !O->TryGetNumberField(TEXT("airRemovedW"), P.AirRemovedW)) return false;
		P.bPartialPueValid = O->TryGetNumberField(TEXT("partialPue"), P.PartialPue);
		S.Points.Add(P);
	}
	for (const TSharedPtr<FJsonValue>& V : *EventItems)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardEvent E;
		double Index = 0.0;
		if (!O.IsValid() || !O->TryGetNumberField(TEXT("t"), E.TimeS) || !O->TryGetStringField(TEXT("kind"), E.Kind)
			|| !O->TryGetNumberField(TEXT("index"), Index) || !O->TryGetNumberField(TEXT("value"), E.Value)
			|| !O->TryGetBoolField(TEXT("applied"), E.bApplied)) return false;
		E.Index = int32(Index);
		S.Events.Add(E);
	}
	Out = MoveTemp(S);
	return true;
}

FString FVrgDashboardLayout::ToJson() const
{
	FString Json;
	FJsonSerializer::Serialize(ToJsonObject(), TJsonWriterFactory<>::Create(&Json));
	return Json;
}

TSharedRef<FJsonObject> FVrgDashboardLayout::ToJsonObject() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	if (bHasDomain)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetNumberField(TEXT("minXM"), DomainMinXM); D->SetNumberField(TEXT("minYM"), DomainMinYM);
		D->SetNumberField(TEXT("maxXM"), DomainMaxXM); D->SetNumberField(TEXT("maxYM"), DomainMaxYM);
		D->SetNumberField(TEXT("minZM"), DomainMinZM); D->SetNumberField(TEXT("maxZM"), DomainMaxZM);
		Root->SetObjectField(TEXT("domain"), D);
	}
	else Root->SetField(TEXT("domain"), MakeShared<FJsonValueNull>());
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FVrgDashboardEquipment& E : Equipment)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("kind"), E.Kind);
		O->SetStringField(TEXT("name"), E.Name);
		O->SetNumberField(TEXT("xM"), E.CenterXM);
		O->SetNumberField(TEXT("yM"), E.CenterYM);
		O->SetNumberField(TEXT("yawDeg"), E.YawDeg);
		O->SetNumberField(TEXT("depthM"), E.DepthM);
		O->SetNumberField(TEXT("widthM"), E.WidthM);
		O->SetNumberField(TEXT("baseZM"), E.BaseZM);
		O->SetNumberField(TEXT("heightM"), E.HeightM);
		O->SetNumberField(TEXT("inlet"), E.InletIndex);
		O->SetBoolField(TEXT("auto"), E.bAutoPlaced);
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("equipment"), Items);
	TArray<TSharedPtr<FJsonValue>> Inlets;
	for (const double K : InletWindowMeanK) Inlets.Add(MakeShared<FJsonValueNumber>(K));
	Root->SetArrayField(TEXT("inletWindowMeanK"), Inlets);
	Root->SetNumberField(TEXT("recommendedInletK"), RecommendedInletK);
	Root->SetBoolField(TEXT("hydraulicsConverged"), bHydraulicsConverged);
	TArray<TSharedPtr<FJsonValue>> PipeItems;
	for (const FVrgDashboardPipe& P : Pipes)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("edge"), P.Edge);
		O->SetBoolField(TEXT("supply"), P.bSupply);
		O->SetBoolField(TEXT("branch"), P.bBranch);
		O->SetNumberField(TEXT("flowM3s"), P.FlowM3s);
		O->SetNumberField(TEXT("diameterM"), P.DiameterM);
		if (P.bTempValid) O->SetNumberField(TEXT("tempK"), P.TempK); else O->SetField(TEXT("tempK"), MakeShared<FJsonValueNull>());
		TArray<TSharedPtr<FJsonValue>> Xyz;
		for (const FVector& V : P.PointsM) { Xyz.Add(MakeShared<FJsonValueNumber>(V.X)); Xyz.Add(MakeShared<FJsonValueNumber>(V.Y)); Xyz.Add(MakeShared<FJsonValueNumber>(V.Z)); }
		O->SetArrayField(TEXT("pointsM"), Xyz);
		PipeItems.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("pipes"), PipeItems);
	return Root;
}

bool FVrgDashboardLayout::FromJson(const FString& Json, FVrgDashboardLayout& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardLayout L;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Inlets = nullptr;
	if (!Root->TryGetNumberField(TEXT("simTimeS"), L.SimTimeS) || !Root->TryGetArrayField(TEXT("equipment"), Items)
		|| !Root->TryGetArrayField(TEXT("inletWindowMeanK"), Inlets) || !Root->TryGetNumberField(TEXT("recommendedInletK"), L.RecommendedInletK)) return false;
	const TSharedPtr<FJsonObject>* Domain = nullptr;
	if (Root->TryGetObjectField(TEXT("domain"), Domain))
	{
		L.bHasDomain = (*Domain)->TryGetNumberField(TEXT("minXM"), L.DomainMinXM) && (*Domain)->TryGetNumberField(TEXT("minYM"), L.DomainMinYM)
			&& (*Domain)->TryGetNumberField(TEXT("maxXM"), L.DomainMaxXM) && (*Domain)->TryGetNumberField(TEXT("maxYM"), L.DomainMaxYM);
		// Z range since the 3D view (optional: plan views inside older reports lack it).
		(*Domain)->TryGetNumberField(TEXT("minZM"), L.DomainMinZM); (*Domain)->TryGetNumberField(TEXT("maxZM"), L.DomainMaxZM);
		if (!L.bHasDomain) return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Items)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardEquipment E;
		double Inlet = 0.0;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("kind"), E.Kind) || !O->TryGetStringField(TEXT("name"), E.Name)
			|| !O->TryGetNumberField(TEXT("xM"), E.CenterXM) || !O->TryGetNumberField(TEXT("yM"), E.CenterYM)
			|| !O->TryGetNumberField(TEXT("yawDeg"), E.YawDeg) || !O->TryGetNumberField(TEXT("depthM"), E.DepthM)
			|| !O->TryGetNumberField(TEXT("widthM"), E.WidthM) || !O->TryGetNumberField(TEXT("inlet"), Inlet)
			|| !O->TryGetBoolField(TEXT("auto"), E.bAutoPlaced)) return false;
		O->TryGetNumberField(TEXT("baseZM"), E.BaseZM);     // optional (older reports)
		O->TryGetNumberField(TEXT("heightM"), E.HeightM);
		E.InletIndex = int32(Inlet);
		L.Equipment.Add(E);
	}
	for (const TSharedPtr<FJsonValue>& V : *Inlets) L.InletWindowMeanK.Add(V->AsNumber());
	// Pipes since 3D stage 2 (optional: older reports lack them).
	Root->TryGetBoolField(TEXT("hydraulicsConverged"), L.bHydraulicsConverged);
	const TArray<TSharedPtr<FJsonValue>>* PipeItems = nullptr;
	if (Root->TryGetArrayField(TEXT("pipes"), PipeItems))
		for (const TSharedPtr<FJsonValue>& V : *PipeItems)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FVrgDashboardPipe P;
			double Edge = 0.0;
			const TArray<TSharedPtr<FJsonValue>>* Xyz = nullptr;
			if (!O.IsValid() || !O->TryGetNumberField(TEXT("edge"), Edge) || !O->TryGetBoolField(TEXT("supply"), P.bSupply)
				|| !O->TryGetBoolField(TEXT("branch"), P.bBranch) || !O->TryGetNumberField(TEXT("flowM3s"), P.FlowM3s)
				|| !O->TryGetNumberField(TEXT("diameterM"), P.DiameterM) || !O->TryGetArrayField(TEXT("pointsM"), Xyz) || Xyz->Num() % 3) return false;
			P.Edge = int32(Edge);
			P.bTempValid = O->TryGetNumberField(TEXT("tempK"), P.TempK);
			for (int32 I = 0; I + 2 < Xyz->Num(); I += 3)
				P.PointsM.Add(FVector((*Xyz)[I]->AsNumber(), (*Xyz)[I + 1]->AsNumber(), (*Xyz)[I + 2]->AsNumber()));
			L.Pipes.Add(MoveTemp(P));
		}
	Out = MoveTemp(L);
	return true;
}

namespace
{
	// Rack tab field order is shared by the writer and the reader so the two cannot drift.
	template <typename FVisit>
	void VisitRowNumbers(FVrgDashboardRackRow& R, FVisit&& Visit)
	{
		Visit(TEXT("xM"), R.XM); Visit(TEXT("yM"), R.YM); Visit(TEXT("utilization"), R.Utilization); Visit(TEXT("heatW"), R.HeatW);
		Visit(TEXT("toCoolantW"), R.ToCoolantW); Visit(TEXT("toRoomAirW"), R.ToRoomAirW); Visit(TEXT("inletNowK"), R.InletNowK);
		Visit(TEXT("coolantInK"), R.CoolantInK); Visit(TEXT("coolantOutK"), R.CoolantOutK); Visit(TEXT("coolantFlowM3s"), R.CoolantFlowM3s);
		Visit(TEXT("fanSpeedMean"), R.FanSpeedMean); Visit(TEXT("fanPowerW"), R.FanPowerW); Visit(TEXT("hottestPartK"), R.HottestPartK);
	}
	template <typename FVisit>
	void VisitSlotNumbers(FVrgDashboardRackSlot& S, FVisit&& Visit)
	{
		Visit(TEXT("bottomZM"), S.BottomZM); Visit(TEXT("heightM"), S.HeightM); Visit(TEXT("inletK"), S.InletK); Visit(TEXT("heatW"), S.HeatW);
		Visit(TEXT("heatToAirW"), S.HeatToAirW); Visit(TEXT("airFlowKgs"), S.AirFlowKgs); Visit(TEXT("outletAirK"), S.OutletAirK);
		Visit(TEXT("fanSpeed"), S.FanSpeed);
	}
	template <typename FVisit>
	void VisitPartNumbers(FVrgDashboardRackPart& P, FVisit&& Visit)
	{
		Visit(TEXT("tempK"), P.TempK); Visit(TEXT("heatW"), P.HeatW); Visit(TEXT("toCoolantW"), P.ToCoolantW); Visit(TEXT("toAirW"), P.ToAirW);
	}
	FString SerializeJson(const TSharedRef<FJsonObject>& Root)
	{
		FString Json;
		FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}

	// Minimal direct JSON writer for the rack tab: no object tree, one growing string. The caller places the commas.
	struct FJsonText
	{
		FString S;
		void Raw(const TCHAR* T) { S.Append(T); }
		void Key(const TCHAR* K) { S.AppendChar(TEXT('"')); S.Append(K); S.Append(TEXT("\":")); }
		void Num(double V)
		{
			if (!FMath::IsFinite(V)) { S.Append(TEXT("null")); return; }
			// Shortest text that reads back to the same double (C++17 to_chars); printf %.17g was most of the detail's cost.
			char Buf[32];
			const std::to_chars_result R = std::to_chars(Buf, Buf + sizeof(Buf), V);
			for (const char* C = Buf; C < R.ptr; ++C) S.AppendChar(TCHAR(*C));
		}
		void Int(int64 V) { S.Append(LexToString(V)); }
		void Bool(bool V) { S.Append(V ? TEXT("true") : TEXT("false")); }
		void Str(const FString& V)
		{
			S.AppendChar(TEXT('"'));
			for (const TCHAR C : V)
			{
				if (C == TEXT('"') || C == TEXT('\\')) { S.AppendChar(TEXT('\\')); S.AppendChar(C); }
				else if (C < 0x20) S.Appendf(TEXT("\\u%04x"), int32(C));
				else S.AppendChar(C);
			}
			S.AppendChar(TEXT('"'));
		}
		// "key":number pairs from a Visit* function, each preceded by a comma (the object is already open).
		auto NumberFields() { return [this](const TCHAR* K, double& V) { Raw(TEXT(",")); Key(K); Num(V); }; }
	};
}

FString FVrgDashboardRacks::RowsToJson() const
{
	FJsonText J;
	J.S.Reserve(256 + Rows.Num() * 480);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("simTimeS")); J.Num(SimTimeS);
	J.Raw(TEXT(",")); J.Key(TEXT("mode")); J.Str(bSimulating ? TEXT("simulation") : TEXT("layout"));
	J.Raw(TEXT(",")); J.Key(TEXT("racks")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Rows.Num(); ++I)
	{
		FVrgDashboardRackRow& R = const_cast<FVrgDashboardRackRow&>(Rows[I]);   // the visitor only reads
		J.Raw(I ? TEXT(",{") : TEXT("{"));
		J.Key(TEXT("inlet")); J.Int(R.Inlet);
		J.Raw(TEXT(",")); J.Key(TEXT("kind")); J.Str(R.Kind);
		J.Raw(TEXT(",")); J.Key(TEXT("name")); J.Str(R.Name);
		J.Raw(TEXT(",")); J.Key(TEXT("hottestPart")); J.Str(R.HottestPart);
		J.Raw(TEXT(",")); J.Key(TEXT("dockedSlots")); J.Int(R.DockedSlots);
		J.Raw(TEXT(",")); J.Key(TEXT("numSlots")); J.Int(R.NumSlots);
		VisitRowNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]}"));
	return MoveTemp(J.S);
}

FString FVrgDashboardRacks::DetailToJson() const
{
	if (!bHasDetail) return FString();
	FJsonText J;
	int32 NumParts = 0;
	for (const FVrgDashboardRackSlot& S : Detail.Slots) NumParts += S.Parts.Num();
	J.S.Reserve(512 + Detail.Slots.Num() * 320 + NumParts * 180);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("simTimeS")); J.Num(SimTimeS);
	J.Raw(TEXT(",")); J.Key(TEXT("inlet")); J.Int(Detail.Inlet);
	J.Raw(TEXT(",")); J.Key(TEXT("totalHeatW")); J.Num(Detail.TotalHeatW);
	J.Raw(TEXT(",")); J.Key(TEXT("toCoolantW")); J.Num(Detail.ToCoolantW);
	J.Raw(TEXT(",")); J.Key(TEXT("toAirW")); J.Num(Detail.ToAirW);
	J.Raw(TEXT(",")); J.Key(TEXT("toAmbientW")); J.Num(Detail.ToAmbientW);
	J.Raw(TEXT(",")); J.Key(TEXT("mixedAirOutletK")); J.Num(Detail.MixedAirOutletK);
	J.Raw(TEXT(",")); J.Key(TEXT("coolantOutletK")); J.Num(Detail.CoolantOutletK);
	J.Raw(TEXT(",")); J.Key(TEXT("slots")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Detail.Slots.Num(); ++I)
	{
		FVrgDashboardRackSlot& S = const_cast<FVrgDashboardRackSlot&>(Detail.Slots[I]);
		J.Raw(I ? TEXT(",{") : TEXT("{"));
		J.Key(TEXT("index")); J.Int(S.Index);
		J.Raw(TEXT(",")); J.Key(TEXT("device")); J.Str(S.Device);
		J.Raw(TEXT(",")); J.Key(TEXT("docked")); J.Bool(S.bDocked);
		VisitSlotNumbers(S, J.NumberFields());
		J.Raw(TEXT(",")); J.Key(TEXT("parts")); J.Raw(TEXT("["));
		for (int32 K = 0; K < S.Parts.Num(); ++K)
		{
			FVrgDashboardRackPart& P = S.Parts[K];
			J.Raw(K ? TEXT(",{") : TEXT("{"));
			J.Key(TEXT("name")); J.Str(P.Name);
			J.Raw(TEXT(",")); J.Key(TEXT("kind")); J.Str(P.Kind);
			VisitPartNumbers(P, J.NumberFields());
			J.Raw(TEXT("}"));
		}
		J.Raw(TEXT("]}"));
	}
	J.Raw(TEXT("]}"));
	return MoveTemp(J.S);
}

FString FVrgDashboardRacks::RowsToJsonDom() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	Root->SetStringField(TEXT("mode"), bSimulating ? TEXT("simulation") : TEXT("layout"));
	TArray<TSharedPtr<FJsonValue>> Items;
	for (FVrgDashboardRackRow R : Rows)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("inlet"), R.Inlet);
		O->SetStringField(TEXT("kind"), R.Kind);
		O->SetStringField(TEXT("name"), R.Name);
		O->SetStringField(TEXT("hottestPart"), R.HottestPart);
		O->SetNumberField(TEXT("dockedSlots"), R.DockedSlots);
		O->SetNumberField(TEXT("numSlots"), R.NumSlots);
		VisitRowNumbers(R, [&O](const TCHAR* Key, double& V) { O->SetNumberField(Key, V); });
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("racks"), Items);
	return SerializeJson(Root);
}

FString FVrgDashboardRacks::DetailToJsonDom() const
{
	if (!bHasDetail) return FString();
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	Root->SetNumberField(TEXT("inlet"), Detail.Inlet);
	Root->SetNumberField(TEXT("totalHeatW"), Detail.TotalHeatW);
	Root->SetNumberField(TEXT("toCoolantW"), Detail.ToCoolantW);
	Root->SetNumberField(TEXT("toAirW"), Detail.ToAirW);
	Root->SetNumberField(TEXT("toAmbientW"), Detail.ToAmbientW);
	Root->SetNumberField(TEXT("mixedAirOutletK"), Detail.MixedAirOutletK);
	Root->SetNumberField(TEXT("coolantOutletK"), Detail.CoolantOutletK);
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (FVrgDashboardRackSlot S : Detail.Slots)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("index"), S.Index);
		O->SetStringField(TEXT("device"), S.Device);
		O->SetBoolField(TEXT("docked"), S.bDocked);
		VisitSlotNumbers(S, [&O](const TCHAR* Key, double& V) { O->SetNumberField(Key, V); });
		TArray<TSharedPtr<FJsonValue>> Parts;
		for (FVrgDashboardRackPart P : S.Parts)
		{
			TSharedRef<FJsonObject> Q = MakeShared<FJsonObject>();
			Q->SetStringField(TEXT("name"), P.Name);
			Q->SetStringField(TEXT("kind"), P.Kind);
			VisitPartNumbers(P, [&Q](const TCHAR* Key, double& V) { Q->SetNumberField(Key, V); });
			Parts.Add(MakeShared<FJsonValueObject>(Q));
		}
		O->SetArrayField(TEXT("parts"), Parts);
		Slots.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("slots"), Slots);
	return SerializeJson(Root);
}

bool FVrgDashboardRacks::RowsFromJson(const FString& Json, FVrgDashboardRacks& Out)
{
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	FString Mode;
	FVrgDashboardRacks R;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
		|| !Root->TryGetNumberField(TEXT("simTimeS"), R.SimTimeS) || !Root->TryGetStringField(TEXT("mode"), Mode)
		|| !Root->TryGetArrayField(TEXT("racks"), Items)) return false;
	R.bSimulating = Mode == TEXT("simulation");
	for (const TSharedPtr<FJsonValue>& V : *Items)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardRackRow Row;
		double Inlet = 0.0, Docked = 0.0, NumSlots = 0.0;
		bool bOk = O.IsValid() && O->TryGetNumberField(TEXT("inlet"), Inlet) && O->TryGetStringField(TEXT("kind"), Row.Kind)
			&& O->TryGetStringField(TEXT("name"), Row.Name) && O->TryGetStringField(TEXT("hottestPart"), Row.HottestPart)
			&& O->TryGetNumberField(TEXT("dockedSlots"), Docked) && O->TryGetNumberField(TEXT("numSlots"), NumSlots);
		if (bOk) VisitRowNumbers(Row, [&](const TCHAR* Key, double& Value) { bOk = bOk && O->TryGetNumberField(Key, Value); });
		if (!bOk) return false;
		Row.Inlet = int32(Inlet); Row.DockedSlots = int32(Docked); Row.NumSlots = int32(NumSlots);
		R.Rows.Add(Row);
	}
	Out = MoveTemp(R);
	return true;
}

bool FVrgDashboardRacks::DetailFromJson(const FString& Json, FVrgDashboardRackDetail& Out)
{
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	FVrgDashboardRackDetail D;
	double Inlet = 0.0;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
		|| !Root->TryGetNumberField(TEXT("inlet"), Inlet) || !Root->TryGetNumberField(TEXT("totalHeatW"), D.TotalHeatW)
		|| !Root->TryGetNumberField(TEXT("toCoolantW"), D.ToCoolantW) || !Root->TryGetNumberField(TEXT("toAirW"), D.ToAirW)
		|| !Root->TryGetNumberField(TEXT("toAmbientW"), D.ToAmbientW) || !Root->TryGetNumberField(TEXT("mixedAirOutletK"), D.MixedAirOutletK)
		|| !Root->TryGetNumberField(TEXT("coolantOutletK"), D.CoolantOutletK) || !Root->TryGetArrayField(TEXT("slots"), Slots)) return false;
	D.Inlet = int32(Inlet);
	for (const TSharedPtr<FJsonValue>& V : *Slots)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		FVrgDashboardRackSlot S;
		double Index = 0.0;
		bool bOk = O.IsValid() && O->TryGetNumberField(TEXT("index"), Index) && O->TryGetStringField(TEXT("device"), S.Device)
			&& O->TryGetBoolField(TEXT("docked"), S.bDocked) && O->TryGetArrayField(TEXT("parts"), Parts);
		if (bOk) VisitSlotNumbers(S, [&](const TCHAR* Key, double& Value) { bOk = bOk && O->TryGetNumberField(Key, Value); });
		if (!bOk) return false;
		S.Index = int32(Index);
		for (const TSharedPtr<FJsonValue>& PV : *Parts)
		{
			const TSharedPtr<FJsonObject> Q = PV->AsObject();
			FVrgDashboardRackPart P;
			bool bPart = Q.IsValid() && Q->TryGetStringField(TEXT("name"), P.Name) && Q->TryGetStringField(TEXT("kind"), P.Kind);
			if (bPart) VisitPartNumbers(P, [&](const TCHAR* Key, double& Value) { bPart = bPart && Q->TryGetNumberField(Key, Value); });
			if (!bPart) return false;
			S.Parts.Add(P);
		}
		D.Slots.Add(MoveTemp(S));
	}
	Out = MoveTemp(D);
	return true;
}
