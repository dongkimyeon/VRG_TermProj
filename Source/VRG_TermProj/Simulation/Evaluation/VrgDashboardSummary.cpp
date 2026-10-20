// Fill out your copyright notice in the Description page of Project Settings.

#include "Simulation/Evaluation/VrgDashboardSummary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <charconv>
#include "Simulation/Air/AirSolver.h"

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

FVrgDashboardAirflow FVrgDashboardAirflow::Sample(const FAirSolver& Solver, const FVector& OriginM, int32 InStride, const TArray<double>& HeightsM)
{
	FVrgDashboardAirflow A;
	A.bOctree = Solver.IsUsingOctree();
	const FUniformAirGrid& G = Solver.GetGrid();
	A.CellM = G.GetCellSizeM();
	A.StrideCells = FMath::Max(1, InStride);
	if (A.bOctree || G.GetNumCells() == 0 || G.VelocityXMs.Num() != G.GetNumFacesX()) return A;
	TArray<int32> Layers;
	for (const double H : HeightsM)
	{
		const int32 Z = FMath::FloorToInt32((H - OriginM.Z) / A.CellM);
		if (Z >= 0 && Z < G.GetNumCellsZ()) Layers.AddUnique(Z);
	}
	Layers.Sort();
	for (const int32 Z : Layers)
	{
		A.LayerHeightsM.Add(OriginM.Z + G.CellCenterM(0, 0, Z).Z);
		// Start half a stride in so the samples sit symmetrically in the domain.
		for (int32 Y = A.StrideCells / 2; Y < G.GetNumCellsY(); Y += A.StrideCells)
			for (int32 X = A.StrideCells / 2; X < G.GetNumCellsX(); X += A.StrideCells)
			{
				if (!G.IsFluidCell(X, Y, Z)) continue;
				const FVector V = Solver.CellCenterVelocityMs(X, Y, Z);
				A.PositionsM.Add(OriginM + G.CellCenterM(X, Y, Z));
				A.VelocitiesMs.Add(V);
				A.MaxSpeedMs = FMath::Max(A.MaxSpeedMs, V.Size());
			}
	}
	return A;
}

FString FVrgDashboardAirflow::ToJson() const
{
	FJsonText J;
	J.S.Reserve(256 + PositionsM.Num() * 140);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("simTimeS")); J.Num(SimTimeS);
	J.Raw(TEXT(",")); J.Key(TEXT("mode")); J.Str(bSimulating ? TEXT("simulation") : TEXT("layout"));
	J.Raw(TEXT(",")); J.Key(TEXT("octree")); J.Bool(bOctree);
	J.Raw(TEXT(",")); J.Key(TEXT("cellM")); J.Num(CellM);
	J.Raw(TEXT(",")); J.Key(TEXT("strideCells")); J.Int(StrideCells);
	J.Raw(TEXT(",")); J.Key(TEXT("maxSpeedMs")); J.Num(MaxSpeedMs);
	J.Raw(TEXT(",")); J.Key(TEXT("layerHeightsM")); J.Raw(TEXT("["));
	for (int32 I = 0; I < LayerHeightsM.Num(); ++I) { if (I) J.Raw(TEXT(",")); J.Num(LayerHeightsM[I]); }
	auto Vectors = [&J](const TCHAR* Name, const TArray<FVector>& V)
	{
		J.Raw(TEXT(",")); J.Key(Name); J.Raw(TEXT("["));
		for (int32 I = 0; I < V.Num(); ++I)
		{
			if (I) J.Raw(TEXT(","));
			J.Num(V[I].X); J.Raw(TEXT(",")); J.Num(V[I].Y); J.Raw(TEXT(",")); J.Num(V[I].Z);
		}
		J.Raw(TEXT("]"));
	};
	J.Raw(TEXT("]"));
	Vectors(TEXT("positionsM"), PositionsM);
	Vectors(TEXT("velocitiesMs"), VelocitiesMs);
	J.Raw(TEXT("}"));
	return MoveTemp(J.S);
}

bool FVrgDashboardAirflow::FromJson(const FString& Json, FVrgDashboardAirflow& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardAirflow A;
	FString Mode;
	double Stride = 0.0;
	const TArray<TSharedPtr<FJsonValue>> *Layers = nullptr, *P = nullptr, *V = nullptr;
	if (!Root->TryGetNumberField(TEXT("simTimeS"), A.SimTimeS) || !Root->TryGetStringField(TEXT("mode"), Mode)
		|| !Root->TryGetBoolField(TEXT("octree"), A.bOctree) || !Root->TryGetNumberField(TEXT("cellM"), A.CellM)
		|| !Root->TryGetNumberField(TEXT("strideCells"), Stride) || !Root->TryGetNumberField(TEXT("maxSpeedMs"), A.MaxSpeedMs)
		|| !Root->TryGetArrayField(TEXT("layerHeightsM"), Layers) || !Root->TryGetArrayField(TEXT("positionsM"), P)
		|| !Root->TryGetArrayField(TEXT("velocitiesMs"), V) || P->Num() != V->Num() || P->Num() % 3) return false;
	A.bSimulating = Mode == TEXT("simulation");
	A.StrideCells = int32(Stride);
	for (const TSharedPtr<FJsonValue>& L : *Layers) A.LayerHeightsM.Add(L->AsNumber());
	for (int32 I = 0; I + 2 < P->Num(); I += 3)
	{
		A.PositionsM.Add(FVector((*P)[I]->AsNumber(), (*P)[I + 1]->AsNumber(), (*P)[I + 2]->AsNumber()));
		A.VelocitiesMs.Add(FVector((*V)[I]->AsNumber(), (*V)[I + 1]->AsNumber(), (*V)[I + 2]->AsNumber()));
	}
	Out = MoveTemp(A);
	return true;
}

FVrgDashboardSensors FVrgDashboardSensors::FromEvaluation(const FVrgEvaluation& Eval)
{
	FVrgDashboardSensors D;
	D.SimTimeS = Eval.GetSimTimeS();
	D.Spec = Eval.GetSettings().Sensor;
	for (int32 I = 0; I < Eval.GetNumInlets(); ++I)
	{
		const FVrgEvaluation::FInletStats S = Eval.GetInlet(I);
		FVrgDashboardSensor& R = D.Sensors.AddDefaulted_GetRef();
		R.Inlet = I;
		R.AirNowK = S.AirNowK;
		R.ElementK = S.SensorElementK;
		R.bHasReading = S.bSensorHasReading;
		R.ReadingK = S.SensorLastK;
		R.ReadingTimeS = S.SensorLastReadingTimeS;
		R.Readings = S.SensorReadings;
		R.bAlarm = S.bSensorAlarm;
		R.FirstAlarmS = S.SensorFirstAlarmS;
		R.Alarms = S.SensorAlarms;
	}
	return D;
}

FString FVrgDashboardSensors::ToJson() const
{
	FJsonText J;
	J.S.Reserve(512 + Sensors.Num() * 220);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("simTimeS")); J.Num(SimTimeS);
	J.Raw(TEXT(",")); J.Key(TEXT("mode")); J.Str(bSimulating ? TEXT("simulation") : TEXT("layout"));
	J.Raw(TEXT(",")); J.Key(TEXT("evalReady")); J.Bool(bEvalReady);
	J.Raw(TEXT(",")); J.Key(TEXT("spec")); J.Raw(TEXT("{"));
	J.Key(TEXT("samplePeriodS")); J.Num(Spec.SamplePeriodS);
	J.Raw(TEXT(",")); J.Key(TEXT("timeConstantS")); J.Num(Spec.TimeConstantS);
	J.Raw(TEXT(",")); J.Key(TEXT("noiseStdK")); J.Num(Spec.NoiseStdK);
	J.Raw(TEXT(",")); J.Key(TEXT("resolutionK")); J.Num(Spec.ResolutionK);
	J.Raw(TEXT(",")); J.Key(TEXT("alarmThresholdK")); J.Num(Spec.AlarmThresholdK);
	J.Raw(TEXT(",")); J.Key(TEXT("alarmHysteresisK")); J.Num(Spec.AlarmHysteresisK);
	J.Raw(TEXT("}"));
	J.Raw(TEXT(",")); J.Key(TEXT("sensors")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Sensors.Num(); ++I)
	{
		const FVrgDashboardSensor& R = Sensors[I];
		if (I) J.Raw(TEXT(","));
		J.Raw(TEXT("{")); J.Key(TEXT("inlet")); J.Int(R.Inlet);
		J.Raw(TEXT(",")); J.Key(TEXT("airNowK")); J.Num(R.AirNowK);
		J.Raw(TEXT(",")); J.Key(TEXT("elementK")); J.Num(R.ElementK);
		J.Raw(TEXT(",")); J.Key(TEXT("readingK")); if (R.bHasReading) J.Num(R.ReadingK); else J.Raw(TEXT("null"));
		J.Raw(TEXT(",")); J.Key(TEXT("readingTimeS")); if (R.bHasReading) J.Num(R.ReadingTimeS); else J.Raw(TEXT("null"));
		J.Raw(TEXT(",")); J.Key(TEXT("readings")); J.Int(R.Readings);
		J.Raw(TEXT(",")); J.Key(TEXT("alarm")); J.Bool(R.bAlarm);
		J.Raw(TEXT(",")); J.Key(TEXT("firstAlarmS")); if (R.FirstAlarmS >= 0.0) J.Num(R.FirstAlarmS); else J.Raw(TEXT("null"));
		J.Raw(TEXT(",")); J.Key(TEXT("alarms")); J.Int(R.Alarms);
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]}"));
	return MoveTemp(J.S);
}

bool FVrgDashboardSensors::FromJson(const FString& Json, FVrgDashboardSensors& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardSensors D;
	FString Mode;
	const TSharedPtr<FJsonObject>* Spec = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Root->TryGetNumberField(TEXT("simTimeS"), D.SimTimeS) || !Root->TryGetStringField(TEXT("mode"), Mode)
		|| !Root->TryGetBoolField(TEXT("evalReady"), D.bEvalReady) || !Root->TryGetObjectField(TEXT("spec"), Spec)
		|| !Root->TryGetArrayField(TEXT("sensors"), Rows)) return false;
	D.bSimulating = Mode == TEXT("simulation");
	const FJsonObject& S = **Spec;
	if (!S.TryGetNumberField(TEXT("samplePeriodS"), D.Spec.SamplePeriodS) || !S.TryGetNumberField(TEXT("timeConstantS"), D.Spec.TimeConstantS)
		|| !S.TryGetNumberField(TEXT("noiseStdK"), D.Spec.NoiseStdK) || !S.TryGetNumberField(TEXT("resolutionK"), D.Spec.ResolutionK)
		|| !S.TryGetNumberField(TEXT("alarmThresholdK"), D.Spec.AlarmThresholdK)
		|| !S.TryGetNumberField(TEXT("alarmHysteresisK"), D.Spec.AlarmHysteresisK)) return false;
	for (const TSharedPtr<FJsonValue>& V : *Rows)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V->TryGetObject(O)) return false;
		FVrgDashboardSensor& R = D.Sensors.AddDefaulted_GetRef();
		double Inlet = 0.0, Readings = 0.0, Alarms = 0.0;
		if (!(*O)->TryGetNumberField(TEXT("inlet"), Inlet) || !(*O)->TryGetNumberField(TEXT("airNowK"), R.AirNowK)
			|| !(*O)->TryGetNumberField(TEXT("elementK"), R.ElementK) || !(*O)->TryGetNumberField(TEXT("readings"), Readings)
			|| !(*O)->TryGetBoolField(TEXT("alarm"), R.bAlarm) || !(*O)->TryGetNumberField(TEXT("alarms"), Alarms)) return false;
		R.Inlet = int32(Inlet); R.Readings = int32(Readings); R.Alarms = int32(Alarms);
		R.bHasReading = (*O)->TryGetNumberField(TEXT("readingK"), R.ReadingK);
		if (R.bHasReading) (*O)->TryGetNumberField(TEXT("readingTimeS"), R.ReadingTimeS); else R.ReadingTimeS = -1.0;
		if (!(*O)->TryGetNumberField(TEXT("firstAlarmS"), R.FirstAlarmS)) R.FirstAlarmS = -1.0;
	}
	Out = MoveTemp(D);
	return true;
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

namespace
{
	template <typename FVisit>
	void VisitCrahNumbers(FVrgDashboardCrah& C, FVisit&& Visit)
	{
		Visit(TEXT("xM"), C.XM); Visit(TEXT("yM"), C.YM); Visit(TEXT("flowM3s"), C.FlowM3s); Visit(TEXT("uaWK"), C.UAWK);
		Visit(TEXT("fanW"), C.FanW); Visit(TEXT("waterSupplyK"), C.WaterSupplyK);
	}
	template <typename FVisit>
	void VisitCduNumbers(FVrgDashboardCdu& C, FVisit&& Visit)
	{
		Visit(TEXT("designDutyW"), C.DesignDutyW); Visit(TEXT("uaWK"), C.UAWK); Visit(TEXT("facilityFlowKgs"), C.FacilityFlowKgs);
		Visit(TEXT("facilityCpJKgK"), C.FacilityCpJKgK); Visit(TEXT("facilitySupplyK"), C.FacilitySupplyK); Visit(TEXT("dutyW"), C.DutyW);
		Visit(TEXT("returnK"), C.ReturnK); Visit(TEXT("supplyK"), C.SupplyK); Visit(TEXT("facilityReturnK"), C.FacilityReturnK);
		Visit(TEXT("hotWK"), C.HotWK); Visit(TEXT("coolantKgs"), C.CoolantKgs);
	}
	template <typename FVisit>
	void VisitPumpNumbers(FVrgDashboardPump& P, FVisit&& Visit)
	{
		Visit(TEXT("flowM3s"), P.FlowM3s); Visit(TEXT("headPa"), P.HeadPa); Visit(TEXT("electricW"), P.ElectricW);
		Visit(TEXT("hydraulicW"), P.HydraulicW); Visit(TEXT("branchMinM3s"), P.BranchMinM3s);
		Visit(TEXT("branchMaxM3s"), P.BranchMaxM3s); Visit(TEXT("branchMeanM3s"), P.BranchMeanM3s);
	}
	template <typename FVisit>
	void VisitCoolingNumbers(FVrgDashboardCooling& C, FVisit&& Visit)
	{
		Visit(TEXT("airRemovedW"), C.AirRemovedW); Visit(TEXT("crahCoilRemovedW"), C.CrahCoilRemovedW);
		Visit(TEXT("coolantRemovedW"), C.CoolantRemovedW); Visit(TEXT("radiatorRemovedW"), C.RadiatorRemovedW);
		Visit(TEXT("hydResidual"), C.HydResidual); Visit(TEXT("itW"), C.ItW); Visit(TEXT("serverFanW"), C.ServerFanW);
		Visit(TEXT("psuLossW"), C.PsuLossW); Visit(TEXT("crahFanW"), C.CrahFanW); Visit(TEXT("pumpW"), C.PumpW);
	}
}

FString FVrgDashboardCooling::ToJson() const
{
	FVrgDashboardCooling& C = const_cast<FVrgDashboardCooling&>(*this);   // the visitors only read
	FJsonText J;
	J.S.Reserve(1024 + Crahs.Num() * 260 + Cdus.Num() * 420 + Pumps.Num() * 300);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("simTimeS")); J.Num(SimTimeS);
	J.Raw(TEXT(",")); J.Key(TEXT("mode")); J.Str(bSimulating ? TEXT("simulation") : TEXT("layout"));
	J.Raw(TEXT(",")); J.Key(TEXT("coupled")); J.Bool(bCoupled);
	J.Raw(TEXT(",")); J.Key(TEXT("crahMode")); J.Str(CrahMode);
	J.Raw(TEXT(",")); J.Key(TEXT("crahPerUnit")); J.Bool(bCrahPerUnit);
	J.Raw(TEXT(",")); J.Key(TEXT("radiatorRacks")); J.Int(RadiatorRacks);
	J.Raw(TEXT(",")); J.Key(TEXT("hydraulics")); J.Bool(bHydraulics);
	J.Raw(TEXT(",")); J.Key(TEXT("hydConverged")); J.Bool(bHydConverged);
	J.Raw(TEXT(",")); J.Key(TEXT("hydSparse")); J.Bool(bHydSparse);
	J.Raw(TEXT(",")); J.Key(TEXT("hydIterations")); J.Int(HydIterations);
	J.Raw(TEXT(",")); J.Key(TEXT("partialPue")); if (bPartialPueValid) J.Num(PartialPue); else J.Raw(TEXT("null"));
	VisitCoolingNumbers(C, J.NumberFields());
	J.Raw(TEXT(",")); J.Key(TEXT("crahs")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Crahs.Num(); ++I)
	{
		FVrgDashboardCrah& R = C.Crahs[I];
		J.Raw(I ? TEXT(",{") : TEXT("{"));
		J.Key(TEXT("kind")); J.Str(R.Kind);
		J.Raw(TEXT(",")); J.Key(TEXT("placed")); J.Bool(R.bPlaced);
		VisitCrahNumbers(R, J.NumberFields());
		// Per-unit air values only where the solver recorded them; null otherwise (never a guessed value).
		J.Raw(TEXT(",")); J.Key(TEXT("removedW")); if (R.bHasAir) J.Num(R.RemovedW); else J.Raw(TEXT("null"));
		J.Raw(TEXT(",")); J.Key(TEXT("intakeMixK")); if (R.bHasAir) J.Num(R.IntakeMixK); else J.Raw(TEXT("null"));
		J.Raw(TEXT(",")); J.Key(TEXT("supplyK")); if (R.bHasAir) J.Num(R.SupplyK); else J.Raw(TEXT("null"));
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]")); J.Raw(TEXT(",")); J.Key(TEXT("cdus")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Cdus.Num(); ++I)
	{
		FVrgDashboardCdu& R = C.Cdus[I];
		J.Raw(I ? TEXT(",{") : TEXT("{"));
		J.Key(TEXT("name")); J.Str(R.Name);
		J.Raw(TEXT(",")); J.Key(TEXT("numRacks")); J.Int(R.NumRacks);
		VisitCduNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]")); J.Raw(TEXT(",")); J.Key(TEXT("pumps")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Pumps.Num(); ++I)
	{
		FVrgDashboardPump& R = C.Pumps[I];
		J.Raw(I ? TEXT(",{") : TEXT("{"));
		J.Key(TEXT("row")); J.Int(R.Row);
		J.Raw(TEXT(",")); J.Key(TEXT("cdu")); J.Str(R.Cdu);
		J.Raw(TEXT(",")); J.Key(TEXT("branches")); J.Int(R.Branches);
		VisitPumpNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]}"));
	return MoveTemp(J.S);
}

bool FVrgDashboardCooling::FromJson(const FString& Json, FVrgDashboardCooling& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardCooling C;
	FString Mode;
	double Radiators = 0.0, Iterations = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* Crahs = nullptr, * Cdus = nullptr, * Pumps = nullptr;
	bool bOk = Root->TryGetNumberField(TEXT("simTimeS"), C.SimTimeS) && Root->TryGetStringField(TEXT("mode"), Mode)
		&& Root->TryGetBoolField(TEXT("coupled"), C.bCoupled) && Root->TryGetStringField(TEXT("crahMode"), C.CrahMode)
		&& Root->TryGetBoolField(TEXT("crahPerUnit"), C.bCrahPerUnit) && Root->TryGetNumberField(TEXT("radiatorRacks"), Radiators)
		&& Root->TryGetBoolField(TEXT("hydraulics"), C.bHydraulics) && Root->TryGetBoolField(TEXT("hydConverged"), C.bHydConverged)
		&& Root->TryGetBoolField(TEXT("hydSparse"), C.bHydSparse) && Root->TryGetNumberField(TEXT("hydIterations"), Iterations)
		&& Root->TryGetArrayField(TEXT("crahs"), Crahs) && Root->TryGetArrayField(TEXT("cdus"), Cdus)
		&& Root->TryGetArrayField(TEXT("pumps"), Pumps);
	if (bOk) VisitCoolingNumbers(C, [&](const TCHAR* Key, double& Value) { bOk = bOk && Root->TryGetNumberField(Key, Value); });
	if (!bOk) return false;
	C.bSimulating = Mode == TEXT("simulation");
	C.RadiatorRacks = int32(Radiators); C.HydIterations = int32(Iterations);
	C.bPartialPueValid = Root->TryGetNumberField(TEXT("partialPue"), C.PartialPue);
	for (const TSharedPtr<FJsonValue>& V : *Crahs)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardCrah R;
		bOk = O.IsValid() && O->TryGetStringField(TEXT("kind"), R.Kind) && O->TryGetBoolField(TEXT("placed"), R.bPlaced);
		if (bOk) VisitCrahNumbers(R, [&](const TCHAR* Key, double& Value) { bOk = bOk && O->TryGetNumberField(Key, Value); });
		if (!bOk) return false;
		R.bHasAir = O->TryGetNumberField(TEXT("removedW"), R.RemovedW);
		if (R.bHasAir && !(O->TryGetNumberField(TEXT("intakeMixK"), R.IntakeMixK) && O->TryGetNumberField(TEXT("supplyK"), R.SupplyK))) return false;
		C.Crahs.Add(R);
	}
	for (const TSharedPtr<FJsonValue>& V : *Cdus)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardCdu R;
		double NumRacks = 0.0;
		bOk = O.IsValid() && O->TryGetStringField(TEXT("name"), R.Name) && O->TryGetNumberField(TEXT("numRacks"), NumRacks);
		if (bOk) VisitCduNumbers(R, [&](const TCHAR* Key, double& Value) { bOk = bOk && O->TryGetNumberField(Key, Value); });
		if (!bOk) return false;
		R.NumRacks = int32(NumRacks);
		C.Cdus.Add(R);
	}
	for (const TSharedPtr<FJsonValue>& V : *Pumps)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FVrgDashboardPump R;
		double Row = 0.0, Branches = 0.0;
		bOk = O.IsValid() && O->TryGetNumberField(TEXT("row"), Row) && O->TryGetStringField(TEXT("cdu"), R.Cdu)
			&& O->TryGetNumberField(TEXT("branches"), Branches);
		if (bOk) VisitPumpNumbers(R, [&](const TCHAR* Key, double& Value) { bOk = bOk && O->TryGetNumberField(Key, Value); });
		if (!bOk) return false;
		R.Row = int32(Row); R.Branches = int32(Branches);
		C.Pumps.Add(R);
	}
	Out = MoveTemp(C);
	return true;
}

// ---- S2b step 6: leak layer ----

namespace
{
	// Field order shared by the writer, the reader and the comparison so the three cannot drift.
	template <typename FVisit>
	void VisitLeakNumbers(FVrgDashboardLeak& L, FVisit&& Visit)
	{
		Visit(TEXT("holeDiameterM"), L.HoleDiameterM); Visit(TEXT("dischargeCoefficient"), L.DischargeCoefficient);
		Visit(TEXT("xM"), L.PositionM.X); Visit(TEXT("yM"), L.PositionM.Y); Visit(TEXT("zM"), L.PositionM.Z);
		Visit(TEXT("elevationM"), L.ElevationM); Visit(TEXT("nodePressurePa"), L.NodePressurePa); Visit(TEXT("flowM3s"), L.FlowM3s);
	}
	template <typename FVisit>
	void VisitLeakInts(FVrgDashboardLeak& L, FVisit&& Visit)
	{
		Visit(TEXT("spec"), L.Spec); Visit(TEXT("networkLeak"), L.NetworkLeak); Visit(TEXT("rack"), L.Rack);
		Visit(TEXT("inlet"), L.Inlet); Visit(TEXT("tank"), L.Tank);
	}
	template <typename FVisit>
	void VisitLeakBools(FVrgDashboardLeak& L, FVisit&& Visit)
	{
		Visit(TEXT("returnBranch"), L.bReturnBranch); Visit(TEXT("nodeIsolated"), L.bNodeIsolated);
	}
	template <typename FVisit>
	void VisitTankNumbers(FVrgDashboardTank& T, FVisit&& Visit)
	{
		Visit(TEXT("xM"), T.PositionM.X); Visit(TEXT("yM"), T.PositionM.Y); Visit(TEXT("zM"), T.PositionM.Z);
		Visit(TEXT("pressurePa"), T.PressurePa); Visit(TEXT("fillPressurePa"), T.FillPressurePa); Visit(TEXT("tankVolumeM3"), T.TankVolumeM3);
		Visit(TEXT("waterM3"), T.WaterM3); Visit(TEXT("initialWaterM3"), T.InitialWaterM3); Visit(TEXT("leakedM3"), T.LeakedM3);
		Visit(TEXT("makeupM3"), T.MakeupM3); Visit(TEXT("leakFlowM3s"), T.LeakFlowM3s); Visit(TEXT("makeupFlowM3s"), T.MakeupFlowM3s);
		Visit(TEXT("pumpFlowM3s"), T.PumpFlowM3s);
	}
	template <typename FVisit>
	void VisitTankInts(FVrgDashboardTank& T, FVisit&& Visit)
	{
		Visit(TEXT("tank"), T.Tank); Visit(TEXT("row"), T.Row); Visit(TEXT("leaks"), T.Leaks);
	}
	template <typename FVisit>
	void VisitTankBools(FVrgDashboardTank& T, FVisit&& Visit)
	{
		Visit(TEXT("makeup"), T.bMakeup); Visit(TEXT("depleted"), T.bDepleted);
	}
	template <typename FVisit>
	void VisitLeakRackNumbers(FVrgDashboardLeakRack& R, FVisit&& Visit)
	{
		Visit(TEXT("designFlowM3s"), R.DesignFlowM3s); Visit(TEXT("flowM3s"), R.FlowM3s); Visit(TEXT("coolantKgs"), R.CoolantKgs);
		Visit(TEXT("designKgs"), R.DesignKgs); Visit(TEXT("maxComponentK"), R.MaxComponentK);
		Visit(TEXT("supplyLeakKgs"), R.SupplyLeakKgs); Visit(TEXT("returnLeakKgs"), R.ReturnLeakKgs);
	}
	template <typename FVisit>
	void VisitLeakRackInts(FVrgDashboardLeakRack& R, FVisit&& Visit)
	{
		Visit(TEXT("rack"), R.Rack); Visit(TEXT("inlet"), R.Inlet); Visit(TEXT("row"), R.Row);
	}
	template <typename FVisit>
	void VisitLeakRackBools(FVrgDashboardLeakRack& R, FVisit&& Visit)
	{
		Visit(TEXT("valveClosed"), R.bValveClosed); Visit(TEXT("isolated"), R.bIsolated); Visit(TEXT("impacted"), R.bImpacted);
		Visit(TEXT("loopDepleted"), R.bLoopDepleted);
	}
	template <typename FVisit>
	void VisitLeaksNumbers(FVrgDashboardLeaks& D, FVisit&& Visit)
	{
		Visit(TEXT("simTimeS"), D.SimTimeS); Visit(TEXT("hydResidual"), D.HydResidual); Visit(TEXT("lowFlowRatio"), D.FaultSettings.LowFlowRatio);
		Visit(TEXT("maxComponentTempK"), D.FaultSettings.MaxComponentTempK); Visit(TEXT("impactThreshold"), D.ImpactThreshold);
		Visit(TEXT("leakHeatW"), D.LeakHeatW); Visit(TEXT("historyLengthS"), D.HistoryLengthS);
	}

	// Writes ints / bools from the visitors after an opening brace (each pair preceded by a comma).
	struct FLeakJsonText : FJsonText
	{
		auto IntFields() { return [this](const TCHAR* K, int32& V) { Raw(TEXT(",")); Key(K); Int(V); }; }
		auto BoolFields() { return [this](const TCHAR* K, bool& V) { Raw(TEXT(",")); Key(K); Bool(V); }; }
		void Ints(const TArray<int32>& A) { Raw(TEXT("[")); for (int32 I = 0; I < A.Num(); ++I) { if (I) Raw(TEXT(",")); Int(A[I]); } Raw(TEXT("]")); }
		void Nums(const TArray<double>& A) { Raw(TEXT("[")); for (int32 I = 0; I < A.Num(); ++I) { if (I) Raw(TEXT(",")); Num(A[I]); } Raw(TEXT("]")); }
		void ValveFields(const FVrgValveRecord& R)
		{
			Key(TEXT("rack")); Int(R.Rack);
			Raw(TEXT(",")); Key(TEXT("closed")); Bool(R.bClosed);
			Raw(TEXT(",")); Key(TEXT("isolatedRacks")); Ints(R.IsolatedRacks);
			Raw(TEXT(",")); Key(TEXT("impactedRacks")); Ints(R.ImpactedRacks);
		}
	};

	bool ReadIntField(const FJsonObject& O, const TCHAR* Key, int32& Out)
	{
		double V = 0.0;
		if (!O.TryGetNumberField(Key, V)) return false;
		Out = int32(V);
		return true;
	}
	bool ReadIntArray(const FJsonObject& O, const TCHAR* Key, TArray<int32>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!O.TryGetArrayField(Key, A)) return false;
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& V : *A) { double D = 0.0; if (!V->TryGetNumber(D)) return false; Out.Add(int32(D)); }
		return true;
	}
	bool ReadNumArray(const FJsonObject& O, const TCHAR* Key, TArray<double>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!O.TryGetArrayField(Key, A)) return false;
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& V : *A) { double D = 0.0; if (!V->TryGetNumber(D)) return false; Out.Add(D); }
		return true;
	}
}

FString FVrgDashboardLeaks::ToJson() const
{
	FVrgDashboardLeaks& D = const_cast<FVrgDashboardLeaks&>(*this);   // the visitors only read
	FLeakJsonText J;
	J.S.Reserve(1024 + Leaks.Num() * 320 + Tanks.Num() * 420 + Racks.Num() * 330 + TankHistory.Num() * 110 + ValveFlowAfterM3s.Num() * 50);
	J.Raw(TEXT("{")); J.Key(TEXT("schema")); J.Int(1);
	J.Raw(TEXT(",")); J.Key(TEXT("mode")); J.Str(bSimulating ? TEXT("simulation") : TEXT("layout"));
	J.Raw(TEXT(",")); J.Key(TEXT("hasRoom")); J.Bool(bHasRoom);
	J.Raw(TEXT(",")); J.Key(TEXT("hydConverged")); J.Bool(bHydConverged);
	J.Raw(TEXT(",")); J.Key(TEXT("hydIterations")); J.Int(HydIterations);
	VisitLeaksNumbers(D, J.NumberFields());
	J.Raw(TEXT(",")); J.Key(TEXT("leaks")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Leaks.Num(); ++I)
	{
		FVrgDashboardLeak& R = D.Leaks[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("branch")); J.Str(R.bReturnBranch ? TEXT("return") : TEXT("supply"));   // label only
		VisitLeakInts(R, J.IntFields()); VisitLeakBools(R, J.BoolFields()); VisitLeakNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("tanks")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Tanks.Num(); ++I)
	{
		FVrgDashboardTank& R = D.Tanks[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("cdu")); J.Str(R.Cdu);
		J.Raw(TEXT(",")); J.Key(TEXT("depletedAtS")); if (R.DepletedAtS >= 0.0) J.Num(R.DepletedAtS); else J.Raw(TEXT("null"));
		VisitTankInts(R, J.IntFields()); VisitTankBools(R, J.BoolFields()); VisitTankNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("racks")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Racks.Num(); ++I)
	{
		FVrgDashboardLeakRack& R = D.Racks[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("name")); J.Str(R.Name);
		VisitLeakRackInts(R, J.IntFields()); VisitLeakRackBools(R, J.BoolFields()); VisitLeakRackNumbers(R, J.NumberFields());
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("valveReport"));
	if (bHasValveReport)
	{
		J.Raw(TEXT("{")); J.ValveFields(ValveReport);
		J.Raw(TEXT(",")); J.Key(TEXT("converged")); J.Bool(bValveConverged);
		J.Raw(TEXT(",")); J.Key(TEXT("flowBeforeM3s")); J.Nums(ValveFlowBeforeM3s);
		J.Raw(TEXT(",")); J.Key(TEXT("flowAfterM3s")); J.Nums(ValveFlowAfterM3s);
		J.Raw(TEXT("}"));
	}
	else J.Raw(TEXT("null"));
	J.Raw(TEXT(",")); J.Key(TEXT("faults")); J.Raw(TEXT("["));
	for (int32 I = 0; I < Faults.Num(); ++I)
	{
		const FVrgFaultRecord& R = Faults[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("timeS")); J.Num(R.TimeS);
		J.Raw(TEXT(",")); J.Key(TEXT("rack")); J.Int(R.Rack);
		J.Raw(TEXT(",")); J.Key(TEXT("kind")); J.Str(FVrgFaultMonitor::KindName(R.Kind));
		J.Raw(TEXT(",")); J.Key(TEXT("value")); J.Num(R.Value);
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("leakEvents")); J.Raw(TEXT("["));
	for (int32 I = 0; I < LeakEvents.Num(); ++I)
	{
		const FVrgLeakRecord& R = LeakEvents[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("timeS")); J.Num(R.TimeS);
		J.Raw(TEXT(",")); J.Key(TEXT("rack")); J.Int(R.Rack);
		J.Raw(TEXT(",")); J.Key(TEXT("returnBranch")); J.Bool(R.bReturnBranch);
		J.Raw(TEXT(",")); J.Key(TEXT("holeDiameterM")); J.Num(R.HoleDiameterM);
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("valveEvents")); J.Raw(TEXT("["));
	for (int32 I = 0; I < ValveEvents.Num(); ++I)
	{
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("timeS")); J.Num(ValveEvents[I].TimeS);
		J.Raw(TEXT(",")); J.ValveFields(ValveEvents[I]);
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("],")); J.Key(TEXT("tankHistory")); J.Raw(TEXT("["));
	for (int32 I = 0; I < TankHistory.Num(); ++I)
	{
		const FVrgTankSample& S = TankHistory[I];
		J.Raw(I ? TEXT(",{") : TEXT("{")); J.Key(TEXT("timeS")); J.Num(S.TimeS);
		J.Raw(TEXT(",")); J.Key(TEXT("tank")); J.Int(S.Row);
		J.Raw(TEXT(",")); J.Key(TEXT("pressurePa")); J.Num(S.PressurePa);
		J.Raw(TEXT(",")); J.Key(TEXT("waterM3")); J.Num(S.WaterM3);
		J.Raw(TEXT(",")); J.Key(TEXT("leakedM3")); J.Num(S.LeakedM3);
		J.Raw(TEXT(",")); J.Key(TEXT("depleted")); J.Bool(S.bDepleted);
		J.Raw(TEXT("}"));
	}
	J.Raw(TEXT("]}"));
	return MoveTemp(J.S);
}

bool FVrgDashboardLeaks::FromJson(const FString& Json, FVrgDashboardLeaks& Out)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
	FVrgDashboardLeaks D;
	FString Mode;
	const TArray<TSharedPtr<FJsonValue>>* Leaks = nullptr, * Tanks = nullptr, * Racks = nullptr, * Faults = nullptr,
		* LeakEvents = nullptr, * ValveEvents = nullptr, * History = nullptr;
	bool bOk = Root->TryGetStringField(TEXT("mode"), Mode) && Root->TryGetBoolField(TEXT("hasRoom"), D.bHasRoom)
		&& Root->TryGetBoolField(TEXT("hydConverged"), D.bHydConverged) && ReadIntField(*Root, TEXT("hydIterations"), D.HydIterations)
		&& Root->TryGetArrayField(TEXT("leaks"), Leaks) && Root->TryGetArrayField(TEXT("tanks"), Tanks)
		&& Root->TryGetArrayField(TEXT("racks"), Racks) && Root->TryGetArrayField(TEXT("faults"), Faults)
		&& Root->TryGetArrayField(TEXT("leakEvents"), LeakEvents) && Root->TryGetArrayField(TEXT("valveEvents"), ValveEvents)
		&& Root->TryGetArrayField(TEXT("tankHistory"), History);
	if (bOk) VisitLeaksNumbers(D, [&](const TCHAR* Key, double& Value) { bOk = bOk && Root->TryGetNumberField(Key, Value); });
	if (!bOk) return false;
	D.bSimulating = Mode == TEXT("simulation");
	auto Ints = [&bOk](const FJsonObject& O) { return [&bOk, &O](const TCHAR* Key, int32& V) { bOk = bOk && ReadIntField(O, Key, V); }; };
	auto Bools = [&bOk](const FJsonObject& O) { return [&bOk, &O](const TCHAR* Key, bool& V) { bOk = bOk && O.TryGetBoolField(Key, V); }; };
	auto Nums = [&bOk](const FJsonObject& O) { return [&bOk, &O](const TCHAR* Key, double& V) { bOk = bOk && O.TryGetNumberField(Key, V); }; };
	for (const TSharedPtr<FJsonValue>& V : *Leaks)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgDashboardLeak& R = D.Leaks.AddDefaulted_GetRef();
		VisitLeakInts(R, Ints(*O)); VisitLeakBools(R, Bools(*O)); VisitLeakNumbers(R, Nums(*O));
	}
	for (const TSharedPtr<FJsonValue>& V : *Tanks)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgDashboardTank& R = D.Tanks.AddDefaulted_GetRef();
		bOk = bOk && O->TryGetStringField(TEXT("cdu"), R.Cdu);
		if (!O->TryGetNumberField(TEXT("depletedAtS"), R.DepletedAtS)) R.DepletedAtS = -1.0;
		VisitTankInts(R, Ints(*O)); VisitTankBools(R, Bools(*O)); VisitTankNumbers(R, Nums(*O));
	}
	for (const TSharedPtr<FJsonValue>& V : *Racks)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgDashboardLeakRack& R = D.Racks.AddDefaulted_GetRef();
		bOk = bOk && O->TryGetStringField(TEXT("name"), R.Name);
		VisitLeakRackInts(R, Ints(*O)); VisitLeakRackBools(R, Bools(*O)); VisitLeakRackNumbers(R, Nums(*O));
	}
	auto ReadValve = [&bOk](const FJsonObject& O, FVrgValveRecord& R)
	{
		bOk = bOk && ReadIntField(O, TEXT("rack"), R.Rack) && O.TryGetBoolField(TEXT("closed"), R.bClosed)
			&& ReadIntArray(O, TEXT("isolatedRacks"), R.IsolatedRacks) && ReadIntArray(O, TEXT("impactedRacks"), R.ImpactedRacks);
	};
	const TSharedPtr<FJsonObject>* Valve = nullptr;
	D.bHasValveReport = Root->TryGetObjectField(TEXT("valveReport"), Valve);
	if (D.bHasValveReport)
	{
		D.ValveReport.TimeS = -1.0;
		ReadValve(**Valve, D.ValveReport);
		bOk = bOk && (*Valve)->TryGetBoolField(TEXT("converged"), D.bValveConverged)
			&& ReadNumArray(**Valve, TEXT("flowBeforeM3s"), D.ValveFlowBeforeM3s) && ReadNumArray(**Valve, TEXT("flowAfterM3s"), D.ValveFlowAfterM3s);
	}
	for (const TSharedPtr<FJsonValue>& V : *Faults)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgFaultRecord& R = D.Faults.AddDefaulted_GetRef();
		FString Kind;
		bOk = bOk && O->TryGetNumberField(TEXT("timeS"), R.TimeS) && ReadIntField(*O, TEXT("rack"), R.Rack)
			&& O->TryGetStringField(TEXT("kind"), Kind) && O->TryGetNumberField(TEXT("value"), R.Value);
		bool bKnown = false;
		for (const EVrgFaultKind K : { EVrgFaultKind::LowCoolantFlow, EVrgFaultKind::OverTemperature, EVrgFaultKind::InventoryDepleted, EVrgFaultKind::HydraulicNotConverged })
			if (Kind == FVrgFaultMonitor::KindName(K)) { R.Kind = K; bKnown = true; }
		bOk = bOk && bKnown;
	}
	for (const TSharedPtr<FJsonValue>& V : *LeakEvents)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgLeakRecord& R = D.LeakEvents.AddDefaulted_GetRef();
		bOk = bOk && O->TryGetNumberField(TEXT("timeS"), R.TimeS) && ReadIntField(*O, TEXT("rack"), R.Rack)
			&& O->TryGetBoolField(TEXT("returnBranch"), R.bReturnBranch) && O->TryGetNumberField(TEXT("holeDiameterM"), R.HoleDiameterM);
	}
	for (const TSharedPtr<FJsonValue>& V : *ValveEvents)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgValveRecord& R = D.ValveEvents.AddDefaulted_GetRef();
		bOk = bOk && O->TryGetNumberField(TEXT("timeS"), R.TimeS);
		ReadValve(*O, R);
	}
	for (const TSharedPtr<FJsonValue>& V : *History)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid()) return false;
		FVrgTankSample& S = D.TankHistory.AddDefaulted_GetRef();
		bOk = bOk && O->TryGetNumberField(TEXT("timeS"), S.TimeS) && ReadIntField(*O, TEXT("tank"), S.Row)
			&& O->TryGetNumberField(TEXT("pressurePa"), S.PressurePa) && O->TryGetNumberField(TEXT("waterM3"), S.WaterM3)
			&& O->TryGetNumberField(TEXT("leakedM3"), S.LeakedM3) && O->TryGetBoolField(TEXT("depleted"), S.bDepleted);
	}
	if (!bOk) return false;
	Out = MoveTemp(D);
	return true;
}

bool FVrgDashboardLeaks::operator==(const FVrgDashboardLeaks& O) const
{
	// Every visited field compared exactly, through the same visitors the JSON uses.
	FVrgDashboardLeaks& A = const_cast<FVrgDashboardLeaks&>(*this);
	FVrgDashboardLeaks& B = const_cast<FVrgDashboardLeaks&>(O);
	TArray<double> NA, NB; TArray<int32> IA, IB; TArray<bool> BA, BB;
	auto N = [](TArray<double>& Out) { return [&Out](const TCHAR*, double& V) { Out.Add(V); }; };
	auto I = [](TArray<int32>& Out) { return [&Out](const TCHAR*, int32& V) { Out.Add(V); }; };
	auto Bo = [](TArray<bool>& Out) { return [&Out](const TCHAR*, bool& V) { Out.Add(V); }; };
	if (A.Leaks.Num() != B.Leaks.Num() || A.Tanks.Num() != B.Tanks.Num() || A.Racks.Num() != B.Racks.Num()) return false;
	VisitLeaksNumbers(A, N(NA)); VisitLeaksNumbers(B, N(NB));
	for (int32 K = 0; K < A.Leaks.Num(); ++K)
	{
		VisitLeakNumbers(A.Leaks[K], N(NA)); VisitLeakInts(A.Leaks[K], I(IA)); VisitLeakBools(A.Leaks[K], Bo(BA));
		VisitLeakNumbers(B.Leaks[K], N(NB)); VisitLeakInts(B.Leaks[K], I(IB)); VisitLeakBools(B.Leaks[K], Bo(BB));
	}
	for (int32 K = 0; K < A.Tanks.Num(); ++K)
	{
		if (A.Tanks[K].Cdu != B.Tanks[K].Cdu || A.Tanks[K].DepletedAtS != B.Tanks[K].DepletedAtS) return false;
		VisitTankNumbers(A.Tanks[K], N(NA)); VisitTankInts(A.Tanks[K], I(IA)); VisitTankBools(A.Tanks[K], Bo(BA));
		VisitTankNumbers(B.Tanks[K], N(NB)); VisitTankInts(B.Tanks[K], I(IB)); VisitTankBools(B.Tanks[K], Bo(BB));
	}
	for (int32 K = 0; K < A.Racks.Num(); ++K)
	{
		if (A.Racks[K].Name != B.Racks[K].Name) return false;
		VisitLeakRackNumbers(A.Racks[K], N(NA)); VisitLeakRackInts(A.Racks[K], I(IA)); VisitLeakRackBools(A.Racks[K], Bo(BA));
		VisitLeakRackNumbers(B.Racks[K], N(NB)); VisitLeakRackInts(B.Racks[K], I(IB)); VisitLeakRackBools(B.Racks[K], Bo(BB));
	}
	auto SameValve = [](const FVrgValveRecord& X, const FVrgValveRecord& Y)
	{
		return X.Rack == Y.Rack && X.bClosed == Y.bClosed && X.IsolatedRacks == Y.IsolatedRacks && X.ImpactedRacks == Y.ImpactedRacks;
	};
	return NA == NB && IA == IB && BA == BB && bSimulating == O.bSimulating && bHasRoom == O.bHasRoom
		&& bHydConverged == O.bHydConverged && HydIterations == O.HydIterations
		&& bHasValveReport == O.bHasValveReport && (!bHasValveReport || (SameValve(ValveReport, O.ValveReport)
			&& bValveConverged == O.bValveConverged && ValveFlowBeforeM3s == O.ValveFlowBeforeM3s && ValveFlowAfterM3s == O.ValveFlowAfterM3s))
		&& Faults == O.Faults && LeakEvents == O.LeakEvents && ValveEvents == O.ValveEvents && TankHistory == O.TankHistory;
}
