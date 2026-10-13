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
