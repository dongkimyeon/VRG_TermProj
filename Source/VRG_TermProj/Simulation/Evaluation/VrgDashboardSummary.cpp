// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Evaluation/VrgDashboardSummary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetNumberField(TEXT("simTimeS"), SimTimeS);
	if (bHasDomain)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetNumberField(TEXT("minXM"), DomainMinXM); D->SetNumberField(TEXT("minYM"), DomainMinYM);
		D->SetNumberField(TEXT("maxXM"), DomainMaxXM); D->SetNumberField(TEXT("maxYM"), DomainMaxYM);
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
		O->SetNumberField(TEXT("inlet"), E.InletIndex);
		O->SetBoolField(TEXT("auto"), E.bAutoPlaced);
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("equipment"), Items);
	TArray<TSharedPtr<FJsonValue>> Inlets;
	for (const double K : InletWindowMeanK) Inlets.Add(MakeShared<FJsonValueNumber>(K));
	Root->SetArrayField(TEXT("inletWindowMeanK"), Inlets);
	Root->SetNumberField(TEXT("recommendedInletK"), RecommendedInletK);
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	return Json;
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
		E.InletIndex = int32(Inlet);
		L.Equipment.Add(E);
	}
	for (const TSharedPtr<FJsonValue>& V : *Inlets) L.InletWindowMeanK.Add(V->AsNumber());
	Out = MoveTemp(L);
	return true;
}
