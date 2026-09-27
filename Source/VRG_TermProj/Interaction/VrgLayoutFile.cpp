// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/VrgLayoutFile.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Simulation/Core/SimUnits.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	bool LayoutIsEquipment(const AActor* Actor)
	{
		return Cast<ARackThermalActor>(Actor) || Cast<ASpineCabinetActor>(Actor) || Cast<ACoolingDistributionUnitActor>(Actor)
			|| Cast<ACrahUnitActor>(Actor);
	}

	template <typename T> T* LayoutFindFirst(UWorld* World)
	{
		for (TActorIterator<T> It(World); It; ++It) return *It;
		return nullptr;
	}

	UWorld* LayoutGameWorld()
	{
		if (GEngine == nullptr) return nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
			if (Context.World() != nullptr && Context.World()->IsGameWorld()) return Context.World();
		return nullptr;
	}
}

FString VrgLayoutFile::PathFor(const FString& Name)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Layouts"), Name + TEXT(".json"));
}

bool VrgLayoutFile::Save(UWorld* World, const FString& Name, FString& OutSummary)
{
	if (World == nullptr || Name.IsEmpty()) { OutSummary = TEXT("월드나 이름이 없다"); return false; }
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!LayoutIsEquipment(*It)) continue;
		const FVector M = SimUnits::UnrealToMeters(It->GetActorLocation());
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), It->GetName());
		Item->SetStringField(TEXT("class"), It->GetClass()->GetName());
		Item->SetNumberField(TEXT("x_m"), M.X); Item->SetNumberField(TEXT("y_m"), M.Y); Item->SetNumberField(TEXT("z_m"), M.Z);
		Item->SetNumberField(TEXT("yaw_deg"), It->GetActorRotation().Yaw);
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("equipment"), Items);
	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	if (const ADataCenterSimulationActor* Sim = LayoutFindFirst<ADataCenterSimulationActor>(World))
	{
		Settings->SetNumberField(TEXT("containment"), int32(Sim->Containment));
		Settings->SetNumberField(TEXT("crahLayout"), int32(Sim->CrahLayout));
		Settings->SetNumberField(TEXT("coolantRejection"), int32(Sim->CoolantRejection));
	}
	if (const AAirFieldDebugActor* Air = LayoutFindFirst<AAirFieldDebugActor>(World))
		Settings->SetNumberField(TEXT("cellSizeM"), Air->GridParams.CellSizeM);
	Root->SetObjectField(TEXT("settings"), Settings);

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	const FString Path = PathFor(Name);
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutSummary = FString::Printf(TEXT("쓰기 실패: %s"), *Path);
		return false;
	}
	OutSummary = FString::Printf(TEXT("장비 %d대 저장: %s"), Items.Num(), *Path);
	return true;
}

bool VrgLayoutFile::Load(UWorld* World, const FString& Name, FString& OutSummary)
{
	if (World == nullptr || Name.IsEmpty()) { OutSummary = TEXT("월드나 이름이 없다"); return false; }
	FString Text;
	const FString Path = PathFor(Name);
	if (!FFileHelper::LoadFileToString(Text, *Path)) { OutSummary = FString::Printf(TEXT("읽기 실패: %s"), *Path); return false; }
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		OutSummary = FString::Printf(TEXT("JSON 해석 실패: %s"), *Path);
		return false;
	}

	// Layout changes belong to layout mode: stop the simulation first; its next entry rebuilds from the new layout.
	ADataCenterSimulationActor* Sim = LayoutFindFirst<ADataCenterSimulationActor>(World);
	if (Sim != nullptr && Sim->GetSimMode() == ESimMode::Simulation) Sim->EnterLayoutMode();

	TMap<FString, AActor*> ByName;
	for (TActorIterator<AActor> It(World); It; ++It) if (LayoutIsEquipment(*It)) ByName.Add(It->GetName(), *It);
	int32 Moved = 0, Missing = 0;
	TSet<FString> Seen;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("equipment")))
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		if (!Item.IsValid()) continue;
		const FString ActorName = Item->GetStringField(TEXT("name"));
		AActor** Found = ByName.Find(ActorName);
		if (Found == nullptr || (*Found)->GetClass()->GetName() != Item->GetStringField(TEXT("class")))
		{
			++Missing;
			UE_LOG(LogTemp, Warning, TEXT("[VrgLayout] 레벨에 없는 장비: %s"), *ActorName);
			continue;
		}
		const FVector M(Item->GetNumberField(TEXT("x_m")), Item->GetNumberField(TEXT("y_m")), Item->GetNumberField(TEXT("z_m")));
		FRotator R = (*Found)->GetActorRotation();
		R.Yaw = Item->GetNumberField(TEXT("yaw_deg"));
		(*Found)->SetActorLocationAndRotation(SimUnits::MetersToUnreal(M), R);
		Seen.Add(ActorName);
		++Moved;
	}
	const int32 NotInFile = ByName.Num() - Seen.Num();
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	if (Root->TryGetObjectField(TEXT("settings"), Settings))
	{
		double Value = 0.0;
		if (Sim != nullptr)
		{
			if ((*Settings)->TryGetNumberField(TEXT("containment"), Value)) Sim->Containment = static_cast<EAisleContainment>(FMath::Clamp(int32(Value), 0, 2));
			if ((*Settings)->TryGetNumberField(TEXT("crahLayout"), Value)) Sim->CrahLayout = static_cast<ECrahLayout>(FMath::Clamp(int32(Value), 0, 2));
			if ((*Settings)->TryGetNumberField(TEXT("coolantRejection"), Value)) Sim->CoolantRejection = static_cast<ECoolantRejection>(FMath::Clamp(int32(Value), 0, 1));
		}
		if (AAirFieldDebugActor* Air = LayoutFindFirst<AAirFieldDebugActor>(World))
			if ((*Settings)->TryGetNumberField(TEXT("cellSizeM"), Value) && Value >= 0.01) Air->GridParams.CellSizeM = Value;
	}
	OutSummary = FString::Printf(TEXT("%s: 장비 %d대 옮김, 레벨에 없는 장비 %d, 파일에 없는 장비 %d"), *Path, Moved, Missing, NotInFile);
	return Missing == 0;
}

static FAutoConsoleCommand GVrgLayoutSaveCommand(
	TEXT("vrg.Layout.Save"),
	TEXT("장비 배치와 냉각 설정을 Saved/Layouts/<이름>.json에 저장한다. vrg.Layout.Save <이름>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		FString Summary;
		const bool bOk = VrgLayoutFile::Save(LayoutGameWorld(), Args.Num() > 0 ? Args[0] : FString(TEXT("layout")), Summary);
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 저장 %s: %s"), bOk ? TEXT("성공") : TEXT("실패"), *Summary);
	}));

static FAutoConsoleCommand GVrgLayoutLoadCommand(
	TEXT("vrg.Layout.Load"),
	TEXT("Saved/Layouts/<이름>.json의 배치를 같은 이름의 장비에 적용한다(배치 모드로 바꾼다). vrg.Layout.Load <이름>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		FString Summary;
		const bool bOk = VrgLayoutFile::Load(LayoutGameWorld(), Args.Num() > 0 ? Args[0] : FString(TEXT("layout")), Summary);
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 불러오기 %s: %s"), bOk ? TEXT("성공") : TEXT("일부 실패"), *Summary);
	}));
