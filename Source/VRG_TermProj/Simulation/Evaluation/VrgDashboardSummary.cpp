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
