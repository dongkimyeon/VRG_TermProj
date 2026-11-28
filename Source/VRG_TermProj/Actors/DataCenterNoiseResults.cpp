#include "Actors/DataCenterSimulationActor.h"
#include "Algo/MaxElement.h"
#include "Simulation/Acoustics/NoiseReporting.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

using namespace VrgNoise;

FNoiseLayout ADataCenterSimulationActor::CaptureNoiseLayout() const
{
	FNoiseLayout Layout;
	Layout.Probe = OutdoorNoiseReceiver;
	Layout.Boundary = OutdoorBoundaryRequest;
	Layout.Environment = OutdoorNoiseEnvironment;
	Layout.IndoorPositionM = IndoorNoisePositionM;
	Layout.IndoorSpacingM = IndoorNoiseMapSpacingM;
	Layout.IndoorHeightM = IndoorNoiseHeightM;
	Layout.Absorption = IndoorNoiseAbsorption;
	Layout.CduLwA = IndoorCduRating;
	return Layout;
}

void ADataCenterSimulationActor::ApplyNoiseLayout(const FNoiseLayout& Settings)
{
	// Callers validate the authored settings against the projected room before changing live layout state.
	check(Settings.IsValid());
	OutdoorNoiseReceiver = Settings.Probe;
	OutdoorNoiseEnvironment = Settings.Environment;
	OutdoorBoundaryRequest = Settings.Boundary;
	OutdoorBoundaryPoints.Reset();
	OutdoorBoundaryHeightM = 0.0;
	if (Settings.Boundary.IsSet())
	{
		SampleOutdoorBoundary(Settings.Boundary->VerticesM, Settings.Boundary->SpacingM, OutdoorBoundaryPoints);
		OutdoorBoundaryHeightM = Settings.Boundary->HeightM;
	}
	IndoorNoisePositionM = Settings.IndoorPositionM;
	IndoorNoiseMapSpacingM = Settings.IndoorSpacingM;
	IndoorNoiseHeightM = Settings.IndoorHeightM;
	IndoorNoiseAbsorption = Settings.Absorption;
	IndoorCduRating = Settings.CduLwA;
	ResetOutdoorNoiseRun();
	ResetIndoorNoiseRun();
	ResetNoiseReceiverGroup(EReceiverSet::Probe);
	ResetNoiseReceiverGroup(EReceiverSet::Boundary);
}

void ADataCenterSimulationActor::ResetNoiseReceiverGroup(EReceiverSet Set)
{
	NoiseReceiverGroups[int32(Set)] = FNoiseReceiverGroup();
}

void ADataCenterSimulationActor::ObserveNoiseAfterStep(const FVrgEvaluationSample& Evaluation, const TArray<double>& Inlets)
{
	if (!OutdoorNoiseReceiver.IsSet() && OutdoorBoundaryPoints.IsEmpty() && !IndoorNoisePositionM.IsSet()) return;
	const double End = Clock.GetSimTimeS();
	const double Start = FMath::Max(0.0, End - Clock.GetFixedStepS());
	FNoiseOperatingPoint Operating{Evaluation.ItW, Evaluation.CrahFanW + Evaluation.PumpW + Evaluation.PlantW, {}};
	if (!Inlets.IsEmpty()) Operating.MaxInletK = *Algo::MaxElement(Inlets);

	if (OutdoorNoiseReceiver.IsSet() && !OutdoorNoiseHistory.IsEmpty())
	{
		const auto& Latest = OutdoorNoiseHistory[(OutdoorNoiseNextSample + OutdoorNoiseHistory.Num() - 1) % OutdoorNoiseHistory.Num()];
		const FNoiseReading One{OutdoorNoiseReceiver->PositionM, Latest.Pressure, Latest.NearSources};
		NoiseReceiverGroups[int32(EReceiverSet::Probe)].Observe(MakeArrayView(&One, 1), Start, End, Latest.SimTimeS, Operating);
	}
	TArray<FNoiseReading, TInlineAllocator<MaxIndoorReceivers>> Readings;
	const auto Observe = [&](EReceiverSet Set, const auto& Snapshot)
	{
		if (!Snapshot.IsSet()) return;
		Readings.Reset();
		for (const auto& Receiver : Snapshot->Receivers)
			Readings.Add({Receiver.PositionM, Receiver.Pressure, Receiver.NearSources});
		NoiseReceiverGroups[int32(Set)].Observe(Readings, Start, End, Snapshot->SimTimeS, Operating);
	};
	Observe(EReceiverSet::Boundary, OutdoorBoundarySnapshot);
	Observe(EReceiverSet::Indoor, IndoorNoiseSnapshot);
}

namespace
{
	void SetFiniteOptional(FJsonObject& Obj, const TCHAR* Key, TOptional<double> Value)
	{
		if (Value.IsSet() && FMath::IsFinite(*Value)) Obj.SetNumberField(Key, *Value);
		else Obj.SetField(Key, MakeShared<FJsonValueNull>());
	}

	FString ReceiverId(EReceiverSet Group, int32 Index)
	{
		return FString::Printf(TEXT("%s:%d"), ReceiverSetName(Group), Index);
	}

	TArray<TSharedPtr<FJsonValue>> TraceJson(const FNoiseTrack& Track)
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		const int32 Count = Track.Trace.Num();
		Results.Reserve(Count);
		for (int32 Offset = 0; Offset < Count; ++Offset)
		{
			const auto& Sample = Track.Trace[(Track.NextTrace + Offset) % Count];
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("timeS"), Sample.TimeS);
			SetFiniteOptional(*Obj, TEXT("lpA"), Sample.Db);
			FLevel Level;
			Level.State = Sample.State;
			Level.MissingSources = Sample.bPartial ? 1 : 0;
			Obj->SetStringField(TEXT("state"), NoiseLevelState(Level));
			Results.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Results;
	}
}

TSharedRef<FJsonObject> ADataCenterSimulationActor::MakeNoiseReportJson(bool bAllTraces, const FString& TraceId) const
{
	auto Noise = MakeShared<FJsonObject>();
	Noise->SetNumberField(TEXT("simTimeS"), Clock.GetSimTimeS());
	Noise->SetBoolField(TEXT("simulating"), GetSimMode() == ESimMode::Simulation);
	Noise->SetNumberField(TEXT("legendMinDb"), NoiseLegendMinDb);
	Noise->SetNumberField(TEXT("legendMaxDb"), NoiseLegendMaxDb);
	Noise->SetStringField(TEXT("traceId"), TraceId);
	TArray<TSharedPtr<FJsonValue>> Groups;
	TArray<TSharedPtr<FJsonValue>> Receivers;
	for (int32 G = 0; G < int32(EReceiverSet::Count); ++G)
	{
		const auto Set = static_cast<EReceiverSet>(G);
		const auto& Group = NoiseReceiverGroups[G];
		if (Group.Tracks.IsEmpty()) continue;
		auto Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("group"), ReceiverSetName(Set));
		Info->SetNumberField(TEXT("startS"), Group.StartS);
		Info->SetNumberField(TEXT("endS"), Group.EndS);
		Info->SetNumberField(TEXT("sourceTimeS"), Group.LatestSourceS);
		Info->SetNumberField(TEXT("meanItW"), Group.MeanItW);
		Info->SetNumberField(TEXT("meanCoolingW"), Group.MeanCoolingW);
		SetFiniteOptional(*Info, TEXT("meanMaxInletK"), Group.MeanMaxInletK);
		Groups.Add(MakeShared<FJsonValueObject>(Info));
		for (int32 I = 0; I < Group.Tracks.Num(); ++I)
		{
			const auto& Track = Group.Tracks[I];
			const auto& Pos = Track.Latest.PositionM;
			const auto& Pressure = Track.Latest.Pressure;
			const FString Id = ReceiverId(Set, I);
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("id"), Id);
			Row->SetStringField(TEXT("group"), ReceiverSetName(Set));
			Row->SetNumberField(TEXT("xM"), Pos.X);
			Row->SetNumberField(TEXT("yM"), Pos.Y);
			Row->SetNumberField(TEXT("zM"), Pos.Z);
			Row->SetStringField(TEXT("comparisonKey"), FString::Printf(TEXT("%s|%.17g|%.17g|%.17g|%.17g|%.17g"),
				ReceiverSetName(Set), Pos.X == 0.0 ? 0.0 : Pos.X, Pos.Y == 0.0 ? 0.0 : Pos.Y, Pos.Z == 0.0 ? 0.0 : Pos.Z, Group.StartS, Group.EndS));
			SetFiniteOptional(*Row, TEXT("lpA"), Pressure.Db);
			Row->SetStringField(TEXT("state"), NoiseLevelState(Pressure));
			Row->SetStringField(TEXT("color"), TEXT("#") + NoiseLevelColor(Pressure).ToHex().Left(6));
			Row->SetBoolField(TEXT("estimated"), Pressure.bEstimated || Pressure.Db.IsSet()); // all current propagation models are estimates
			Row->SetNumberField(TEXT("known"), Pressure.KnownSources);
			Row->SetNumberField(TEXT("stopped"), Pressure.SilentSources);
			Row->SetNumberField(TEXT("missing"), Pressure.MissingSources);
			Row->SetNumberField(TEXT("near"), Track.Latest.NearSources);
			SetFiniteOptional(*Row, TEXT("laeq"), Track.Average.GetDb());
			Row->SetStringField(TEXT("laeqState"), Track.Average.GetState());
			Row->SetNumberField(TEXT("durationS"), Track.Average.DurationS);
			Row->SetNumberField(TEXT("incompleteS"), Track.Average.IncompleteS);
			if (bAllTraces || Id == TraceId) Row->SetArrayField(TEXT("trace"), TraceJson(Track));
			Receivers.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Noise->SetArrayField(TEXT("groups"), Groups);
	Noise->SetArrayField(TEXT("receivers"), Receivers);
	if (bAllTraces)
	{
		Noise->SetObjectField(TEXT("settings"), WriteNoiseLayout(CaptureNoiseLayout()));
		Noise->SetStringField(TEXT("model"), TEXT("indoor: direct+diffuse; probe: free field; boundary: octave propagation with DR95 proxy"));
	}
	return Noise;
}

FString ADataCenterSimulationActor::BuildNoiseDashboardJson(const FString& TraceId) const
{
	FString Text;
	FJsonSerializer::Serialize(MakeNoiseReportJson(false, TraceId), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
	return Text;
}

namespace VrgNoise
{
namespace
{
	FString DbLabel(const FJsonObject& Row, const TCHAR* ValueKey, const TCHAR* StateKey)
	{
		double Db;
		const FString State = Row.GetStringField(StateKey);
		return Row.TryGetNumberField(ValueKey, Db) && FMath::IsFinite(Db)
			? FString::Printf(TEXT("%.2f%s"), Db, State == TEXT("partial") ? TEXT(" (부분)") : TEXT("")) : State;
	}

	const FJsonObject* FindNoiseGroup(const FJsonObject& Report, const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!Report.TryGetArrayField(TEXT("groups"), Groups)) return nullptr;
		for (const auto& Value : *Groups)
		{
			const TSharedPtr<FJsonObject>* Group = nullptr;
			FString GroupName;
			if (Value && Value->TryGetObject(Group) && (*Group)->TryGetStringField(TEXT("group"), GroupName) && GroupName == Name)
				return Group->Get();
		}
		return nullptr;
	}

	FString MetricLabel(const FJsonObject& Group, const TCHAR* Field, double Scale, double Offset = 0.0)
	{
		double Value;
		return Group.TryGetNumberField(Field, Value) && FMath::IsFinite(Value)
			? FString::Printf(TEXT("%.2f"), Value * Scale + Offset) : FString(TEXT("—"));
	}

	FString PositionLabel(const FJsonObject& Row)
	{
		return FString::Printf(TEXT("(%.3f, %.3f, %.3f)"), Row.GetNumberField(TEXT("xM")), Row.GetNumberField(TEXT("yM")), Row.GetNumberField(TEXT("zM")));
	}
}

void AppendNoiseMarkdown(const FJsonObject& Noise, FString& Markdown)
{
	const auto& Rows = Noise.GetArrayField(TEXT("receivers"));
	if (Rows.IsEmpty()) return;
	Markdown += TEXT("\n## 소음 추정\n\n| 수신점 | 세계 좌표 m | 순간 LpA dBA | LAeq dBA | 누적 s | 불완전 s |\n|---|---|---|---|---|---|\n");
	for (const auto& Value : Rows)
	{
		const auto& Row = *Value->AsObject();
		Markdown += FString::Printf(TEXT("| %s | %s | %s | %s | %.3f | %.3f |\n"), *Row.GetStringField(TEXT("id")), *PositionLabel(Row),
			*DbLabel(Row, TEXT("lpA"), TEXT("state")), *DbLabel(Row, TEXT("laeq"), TEXT("laeqState")),
			Row.GetNumberField(TEXT("durationS")), Row.GetNumberField(TEXT("incompleteS")));
	}
	Markdown += TEXT("\n부분값은 확보된 에너지의 하한값이다. JSON에 수신점 시계열과 같은 시간 구간의 IT·냉각 전력·흡기 온도를 저장한다.\n");
}

void AppendNoiseComparison(const FJsonObject& ReportA, const FJsonObject& ReportB, FString& Markdown)
{
	const TSharedPtr<FJsonObject> *A = nullptr, *B = nullptr;
	const TArray<TSharedPtr<FJsonValue>> *RowsA = nullptr, *RowsB = nullptr;
	if (!ReportA.TryGetObjectField(TEXT("noise"), A) || !ReportB.TryGetObjectField(TEXT("noise"), B)
		|| !(*A)->TryGetArrayField(TEXT("receivers"), RowsA) || !(*B)->TryGetArrayField(TEXT("receivers"), RowsB)
		|| (RowsA->IsEmpty() && RowsB->IsEmpty())) return;
	TMap<FString, TSharedPtr<FJsonObject>> IndexB;
	for (const auto& Value : *RowsB)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		FString Key;
		if (Value && Value->TryGetObject(Row) && (*Row)->TryGetStringField(TEXT("comparisonKey"), Key)) IndexB.Add(Key, *Row);
	}
	Markdown += TEXT("\n## 소음 A/B\n\n동일한 수신점 유형·세계 좌표·시작과 끝 시각을 사용한다. 수치 차이는 양쪽 완전한 LAeq에 대해 표시한다.\n\n"
		"| 수신점 | 세계 좌표 m | A LAeq dBA | B LAeq dBA | 차이 dB |\n|---|---|---|---|---|\n");
	TSet<FString> MatchedGroups;
	int32 Count = 0;
	for (const auto& Value : *RowsA)
	{
		const TSharedPtr<FJsonObject>* RowA = nullptr;
		FString Key;
		if (!Value || !Value->TryGetObject(RowA) || !(*RowA)->TryGetStringField(TEXT("comparisonKey"), Key)) continue;
		const auto* RowB = IndexB.Find(Key);
		if (!RowB) continue;
		double DbA = 0.0, DbB = 0.0;
		const bool bComparable = (*RowA)->GetStringField(TEXT("laeqState")) == TEXT("known") && (*RowB)->GetStringField(TEXT("laeqState")) == TEXT("known")
			&& (*RowA)->TryGetNumberField(TEXT("laeq"), DbA) && (*RowB)->TryGetNumberField(TEXT("laeq"), DbB) && FMath::IsFinite(DbB - DbA);
		Markdown += FString::Printf(TEXT("| %s | %s | %s | %s | %s |\n"), *(*RowA)->GetStringField(TEXT("id")), *PositionLabel(**RowA),
			*DbLabel(**RowA, TEXT("laeq"), TEXT("laeqState")), *DbLabel(**RowB, TEXT("laeq"), TEXT("laeqState")),
			bComparable ? *FString::Printf(TEXT("%+.2f"), DbB - DbA) : TEXT("—"));
		FString GroupName;
		if ((*RowA)->TryGetStringField(TEXT("group"), GroupName)) MatchedGroups.Add(GroupName);
		++Count;
	}
	if (Count == 0)
	{
		Markdown += TEXT("\n일치하는 위치와 시간 구간이 없다.\n");
		return;
	}
	if (MatchedGroups.IsEmpty()) return;
	Markdown += TEXT("\n| 유형 | 시간 s | 평균 IT kW A/B | 평균 냉각 kW A/B | 최고 흡기 평균 °C A/B |\n|---|---|---|---|---|\n");
	for (int32 G = 0; G < int32(EReceiverSet::Count); ++G)
	{
		const FString Name = ReceiverSetName(static_cast<EReceiverSet>(G));
		if (!MatchedGroups.Contains(Name)) continue;
		const auto* GroupA = FindNoiseGroup(**A, Name);
		const auto* GroupB = FindNoiseGroup(**B, Name);
		if (!GroupA || !GroupB) continue;
		Markdown += FString::Printf(TEXT("| %s | %.3f–%.3f | %s / %s | %s / %s | %s / %s |\n"),
			*Name, GroupA->GetNumberField(TEXT("startS")), GroupA->GetNumberField(TEXT("endS")),
			*MetricLabel(*GroupA, TEXT("meanItW"), .001), *MetricLabel(*GroupB, TEXT("meanItW"), .001),
			*MetricLabel(*GroupA, TEXT("meanCoolingW"), .001), *MetricLabel(*GroupB, TEXT("meanCoolingW"), .001),
			*MetricLabel(*GroupA, TEXT("meanMaxInletK"), 1.0, -273.15), *MetricLabel(*GroupB, TEXT("meanMaxInletK"), 1.0, -273.15));
	}
}
}
