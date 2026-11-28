#include "Simulation/Acoustics/NoiseLayout.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace VrgNoise
{
namespace
{
	bool Number(const FJsonObject& Object, const TCHAR* Key, double& Out)
	{
		const auto* Field = Object.Values.Find(Key);
		return Field && Field->IsValid() && (*Field)->Type == EJson::Number
			&& (*Field)->TryGetNumber(Out) && FMath::IsFinite(Out);
	}

	TSharedRef<FJsonObject> WriteXY(const FVector2D& Point)
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("xM"), Point.X);
		Object->SetNumberField(TEXT("yM"), Point.Y);
		return Object;
	}

	bool ReadXY(const FJsonObject& Object, FVector2D& Point)
	{
		return Number(Object, TEXT("xM"), Point.X) && Number(Object, TEXT("yM"), Point.Y);
	}

	TArray<TSharedPtr<FJsonValue>> WriteCoefficients(const double (&Values)[IndoorOctaves])
	{
		TArray<TSharedPtr<FJsonValue>> Output;
		for (double Value : Values) Output.Add(MakeShared<FJsonValueNumber>(Value));
		return Output;
	}

	bool ReadCoefficients(const FJsonObject& Object, const TCHAR* Key, double (&Output)[IndoorOctaves])
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Object.TryGetArrayField(Key, Array) || Array->Num() != IndoorOctaves) return false;
		for (int32 I = 0; I < IndoorOctaves; ++I)
			if (!(*Array)[I].IsValid() || (*Array)[I]->Type != EJson::Number || !(*Array)[I]->TryGetNumber(Output[I])) return false;
		return true;
	}
}

bool FNoiseLayout::IsValid(const FBox& FinalRoomM) const
{
	if (!Environment.IsValid() || !Absorption.IsValid() || (Probe.IsSet() && !Probe->IsValid()) ||
		(CduLwA.IsSet() && !FMath::IsFinite(*CduLwA)) || !(IndoorHeightM > 0.0 && FMath::IsFinite(IndoorHeightM)))
		return false;
	if (Boundary.IsSet())
	{
		TArray<FVector2D> Points;
		if (!(Boundary->HeightM > 0.0 && FMath::IsFinite(Environment.GroundZM + Boundary->HeightM))
			|| !SampleOutdoorBoundary(Boundary->VerticesM, Boundary->SpacingM, Points)) return false;
	}
	if (IndoorPositionM.IsSet() && (IndoorPositionM->ContainsNaN()
		|| (FinalRoomM.IsValid && IndoorHeightM >= FinalRoomM.GetSize().Z))) return false;
	if (IndoorSpacingM.IsSet())
	{
		if (!IndoorPositionM.IsSet() || !FMath::IsFinite(*IndoorSpacingM) || *IndoorSpacingM <= 0.0) return false;
		TArray<FVector> Points;
		if (FinalRoomM.IsValid && !SampleIndoorMap(FinalRoomM, *IndoorSpacingM, IndoorHeightM, Points)) return false;
	}
	return true;
}

TSharedRef<FJsonObject> WriteNoiseLayout(const FNoiseLayout& Layout)
{
	auto Json = MakeShared<FJsonObject>();
	if (Layout.Probe.IsSet())
	{
		const auto& Position = Layout.Probe->PositionM;
		auto Probe = WriteXY(FVector2D(Position.X, Position.Y));
		Probe->SetNumberField(TEXT("zM"), Position.Z);
		Probe->SetBoolField(TEXT("hemisphere"), Layout.Probe->Radiation == EOutdoorRadiation::Hemisphere);
		Json->SetObjectField(TEXT("probe"), Probe);
	}
	if (Layout.Boundary.IsSet())
	{
		auto Boundary = MakeShared<FJsonObject>();
		Boundary->SetNumberField(TEXT("spacingM"), Layout.Boundary->SpacingM);
		Boundary->SetNumberField(TEXT("heightM"), Layout.Boundary->HeightM);
		TArray<TSharedPtr<FJsonValue>> Vertices;
		for (const auto& Point : Layout.Boundary->VerticesM) Vertices.Add(MakeShared<FJsonValueObject>(WriteXY(Point)));
		Boundary->SetArrayField(TEXT("vertices"), Vertices);
		Json->SetObjectField(TEXT("boundary"), Boundary);
	}
	const auto& Environment = Layout.Environment;
	auto Outside = MakeShared<FJsonObject>();
	Outside->SetNumberField(TEXT("groundZM"), Environment.GroundZM);
	Outside->SetNumberField(TEXT("groundFactor"), Environment.GroundFactor);
	Outside->SetNumberField(TEXT("relativeHumidity"), Environment.RelativeHumidity);
	Outside->SetNumberField(TEXT("pressurePa"), Environment.PressurePa);
	TArray<TSharedPtr<FJsonValue>> Screens;
	for (const auto& Screen : Environment.Screens)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("start"), WriteXY(Screen.StartM));
		Obj->SetObjectField(TEXT("end"), WriteXY(Screen.EndM));
		Obj->SetNumberField(TEXT("heightM"), Screen.HeightM);
		Obj->SetNumberField(TEXT("reflectedEnergy"), Screen.ReflectedEnergy);
		Screens.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Outside->SetArrayField(TEXT("screens"), Screens);
	Json->SetObjectField(TEXT("environment"), Outside);

	auto Inside = MakeShared<FJsonObject>();
	if (Layout.IndoorPositionM.IsSet()) Inside->SetObjectField(TEXT("position"), WriteXY(*Layout.IndoorPositionM));
	if (Layout.IndoorSpacingM.IsSet()) Inside->SetNumberField(TEXT("spacingM"), *Layout.IndoorSpacingM);
	if (Layout.CduLwA.IsSet()) Inside->SetNumberField(TEXT("cduLwA"), *Layout.CduLwA);
	Inside->SetNumberField(TEXT("heightM"), Layout.IndoorHeightM);
	Inside->SetArrayField(TEXT("shellAbsorption"), WriteCoefficients(Layout.Absorption.Shell));
	Inside->SetArrayField(TEXT("equipmentAbsorption"), WriteCoefficients(Layout.Absorption.Equipment));
	Json->SetObjectField(TEXT("indoor"), Inside);
	return Json;
}

bool ReadNoiseLayout(const FJsonObject& Json, FNoiseLayout& Out)
{
	FNoiseLayout Layout;
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (Json.HasField(TEXT("probe")))
	{
		FVector2D Point;
		double Z;
		bool bHemisphere;
		if (!Json.TryGetObjectField(TEXT("probe"), Obj) || !ReadXY(**Obj, Point) || !Number(**Obj, TEXT("zM"), Z)
			|| !(*Obj)->TryGetBoolField(TEXT("hemisphere"), bHemisphere)) return false;
		Layout.Probe = FOutdoorReceiver{FVector(Point, Z), bHemisphere ? EOutdoorRadiation::Hemisphere : EOutdoorRadiation::Sphere};
	}
	if (Json.HasField(TEXT("boundary")))
	{
		auto& Boundary = Layout.Boundary.Emplace();
		const TArray<TSharedPtr<FJsonValue>>* Vertices = nullptr;
		if (!Json.TryGetObjectField(TEXT("boundary"), Obj) || !Number(**Obj, TEXT("spacingM"), Boundary.SpacingM)
			|| !Number(**Obj, TEXT("heightM"), Boundary.HeightM) || !(*Obj)->TryGetArrayField(TEXT("vertices"), Vertices)
			|| Vertices->Num() > MaxBoundaryReceivers + 1) return false;
		for (const auto& Value : *Vertices)
		{
			FVector2D Point;
			const TSharedPtr<FJsonObject>* Vertex = nullptr;
			if (!Value || !Value->TryGetObject(Vertex) || !ReadXY(**Vertex, Point)) return false;
			Boundary.VerticesM.Add(Point);
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Screens = nullptr;
	auto& Env = Layout.Environment;
	if (!Json.TryGetObjectField(TEXT("environment"), Obj) || !Number(**Obj, TEXT("groundZM"), Env.GroundZM)
		|| !Number(**Obj, TEXT("groundFactor"), Env.GroundFactor) || !Number(**Obj, TEXT("relativeHumidity"), Env.RelativeHumidity)
		|| !Number(**Obj, TEXT("pressurePa"), Env.PressurePa) || !(*Obj)->TryGetArrayField(TEXT("screens"), Screens)
		|| Screens->Num() > MaxOutdoorScreens) return false;
	for (const auto& Value : *Screens)
	{
		const TSharedPtr<FJsonObject> *Screen = nullptr, *A = nullptr, *B = nullptr;
		auto& Dest = Env.Screens.AddDefaulted_GetRef();
		if (!Value || !Value->TryGetObject(Screen) || !(*Screen)->TryGetObjectField(TEXT("start"), A)
			|| !(*Screen)->TryGetObjectField(TEXT("end"), B) || !ReadXY(**A, Dest.StartM) || !ReadXY(**B, Dest.EndM)
			|| !Number(**Screen, TEXT("heightM"), Dest.HeightM) || !Number(**Screen, TEXT("reflectedEnergy"), Dest.ReflectedEnergy)) return false;
	}
	if (!Json.TryGetObjectField(TEXT("indoor"), Obj) || !Number(**Obj, TEXT("heightM"), Layout.IndoorHeightM)
		|| !ReadCoefficients(**Obj, TEXT("shellAbsorption"), Layout.Absorption.Shell)
		|| !ReadCoefficients(**Obj, TEXT("equipmentAbsorption"), Layout.Absorption.Equipment)) return false;
	if ((*Obj)->HasField(TEXT("position")))
	{
		const TSharedPtr<FJsonObject>* Point = nullptr;
		FVector2D XY;
		if (!(*Obj)->TryGetObjectField(TEXT("position"), Point) || !ReadXY(**Point, XY)) return false;
		Layout.IndoorPositionM = XY;
	}
	if ((*Obj)->HasField(TEXT("spacingM")))
	{
		double SpacingM;
		if (!Number(**Obj, TEXT("spacingM"), SpacingM)) return false;
		Layout.IndoorSpacingM = SpacingM;
	}
	if ((*Obj)->HasField(TEXT("cduLwA")))
	{
		double Rating;
		if (!Number(**Obj, TEXT("cduLwA"), Rating)) return false;
		Layout.CduLwA = Rating;
	}
	if (!Layout.IsValid()) return false;
	Out = MoveTemp(Layout);
	return true;
}
}
