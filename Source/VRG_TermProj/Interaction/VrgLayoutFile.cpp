// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/VrgLayoutFile.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Actors/DataCenterRoomActor.h"
#include "JsonObjectConverter.h"
#include "Interaction/VrgLayoutValidation.h"

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
#include "HAL/FileManager.h"
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
	Root->SetNumberField(TEXT("version"), 2);
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
		Item->SetNumberField(TEXT("pitch_deg"), It->GetActorRotation().Pitch);
		Item->SetNumberField(TEXT("roll_deg"), It->GetActorRotation().Roll);
		const FVector Scale = It->GetActorScale3D();
		Item->SetNumberField(TEXT("scale_x"), Scale.X); Item->SetNumberField(TEXT("scale_y"), Scale.Y); Item->SetNumberField(TEXT("scale_z"), Scale.Z);
		if (const auto* Rack = Cast<ARackThermalActor>(*It))
		{
			Item->SetStringField(TEXT("rackId"), VrgRackLifecycle::Identity(Rack));
			TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
			const auto Snapshot = Rack->CaptureLayoutSettings();
			if (!FJsonObjectConverter::UStructToJsonObject(FRackLayoutSettings::StaticStruct(), &Snapshot, Config)) return false;
			Item->SetObjectField(TEXT("rackSettings"), Config);
		}
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
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutSummary = FString::Printf(TEXT("쓰기 실패: %s"), *Path);
		return false;
	}
	OutSummary = FString::Printf(TEXT("장비 %d대 저장: %s"), Items.Num(), *Path);
	return true;
}

namespace
{
	bool FiniteLayoutJson(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid() || V->Type==EJson::Null) return false;
		if (V->Type==EJson::Number) return FMath::IsFinite(V->AsNumber());
		if (V->Type==EJson::Array) { for (const auto& X:V->AsArray()) if (!FiniteLayoutJson(X)) return false; }
		if (V->Type==EJson::Object) { for (const auto& X:V->AsObject()->Values) if (!FiniteLayoutJson(X.Value)) return false; }
		return true;
	}
	bool ReadLayoutNumber(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Out)
	{
		return O->TryGetNumberField(Key,Out) && FMath::IsFinite(Out);
	}
	struct FLoadedEquipment
	{
		FString Name, Class, Id;
		AActor* Actor=nullptr;
		FTransform Target;
		TOptional<FRackLayoutSettings> Rack;
		bool bSpawned=false;
	};
}

bool VrgLayoutFile::Load(UWorld* World, const FString& Name, FString& OutSummary)
{
	auto Fail=[&](const FString& Reason) { OutSummary=Reason; return false; };
	if (!World || !World->IsGameWorld() || Name.IsEmpty()) return Fail(TEXT("실행 월드나 이름이 없다"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text,*PathFor(Name))) return Fail(TEXT("배치 파일 읽기 실패"));
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Root) || !Root.IsValid()) return Fail(TEXT("JSON 해석 실패"));
	double Version=1;
	if (Root->HasField(TEXT("version")) && !ReadLayoutNumber(Root,TEXT("version"),Version)) return Fail(TEXT("잘못된 버전"));
	if (Version!=1 && Version!=2) return Fail(TEXT("지원하지 않는 버전"));
	const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
	if (!Root->TryGetArrayField(TEXT("equipment"),Values)) return Fail(TEXT("장비 목록 없음"));
	TMap<FString,AActor*> ByName;
	TMap<FString,ARackThermalActor*> ById;
	TArray<AActor*> Current;
	for (TActorIterator<AActor> It(World);It;++It) if (LayoutIsEquipment(*It))
	{
		ByName.Add(It->GetName(),*It); Current.Add(*It);
		if (auto* Rack=Cast<ARackThermalActor>(*It))
		{
			const FString Id=VrgRackLifecycle::Identity(Rack);
			if (ById.Contains(Id)) return Fail(TEXT("월드 랙 식별자 중복"));
			ById.Add(Id,Rack);
		}
	}
	TArray<FLoadedEquipment> Items;
	TSet<FString> SeenNames,SeenIds;
	int32 Missing=0;
	for (const auto& Value:*Values)
	{
		if (!FiniteLayoutJson(Value) || Value->Type!=EJson::Object) return Fail(TEXT("장비 데이터 형식/유한값 오류"));
		const auto O=Value->AsObject(); FLoadedEquipment Item;
		if (!O->TryGetStringField(TEXT("name"),Item.Name) || Item.Name.IsEmpty()
			|| !O->TryGetStringField(TEXT("class"),Item.Class) || SeenNames.Contains(Item.Name)
			|| Item.Name.Contains(TEXT("/")) || Item.Name.Contains(TEXT(".")) || Item.Name.Contains(TEXT(":"))) return Fail(TEXT("장비 이름/종류 오류 또는 중복"));
		SeenNames.Add(Item.Name);
		double X,Y,Z,Yaw,Pitch=0,Roll=0,SX=1,SY=1,SZ=1;
		if (!ReadLayoutNumber(O,TEXT("x_m"),X) || !ReadLayoutNumber(O,TEXT("y_m"),Y) || !ReadLayoutNumber(O,TEXT("z_m"),Z)
			|| !ReadLayoutNumber(O,TEXT("yaw_deg"),Yaw)) return Fail(TEXT("장비 위치/방향 오류"));
		Item.Actor=ByName.FindRef(Item.Name);
		if (Version==2)
		{
			if (!ReadLayoutNumber(O,TEXT("pitch_deg"),Pitch) || !ReadLayoutNumber(O,TEXT("roll_deg"),Roll)
				|| !ReadLayoutNumber(O,TEXT("scale_x"),SX) || !ReadLayoutNumber(O,TEXT("scale_y"),SY)
				|| !ReadLayoutNumber(O,TEXT("scale_z"),SZ) || SX==0 || SY==0 || SZ==0) return Fail(TEXT("장비 전체 변환 오류"));
			const TSharedPtr<FJsonObject>* Config=nullptr;
			if (O->HasField(TEXT("rackId")))
			{
				if (!O->TryGetStringField(TEXT("rackId"),Item.Id) || Item.Id.IsEmpty() || SeenIds.Contains(Item.Id)
					|| !O->TryGetObjectField(TEXT("rackSettings"),Config)) return Fail(TEXT("랙 식별자/설정 오류 또는 중복"));
				SeenIds.Add(Item.Id); Item.Actor=ById.FindRef(Item.Id);
				FRackLayoutSettings Settings;
				if (!FJsonObjectConverter::JsonObjectToUStruct(Config->ToSharedRef(),&Settings,0,0,true)
					|| !VrgValidRackSettings(Settings)) return Fail(TEXT("랙 설정 범위/구조 오류"));
				// These are live coupling inputs, not user layout configuration; do not import sampled inlet/flow state.
				const FRackThermalInput Defaults;
				Settings.Input.CoolantInletTempK=Defaults.CoolantInletTempK;
				Settings.Input.CoolantMassFlowKgs=Defaults.CoolantMassFlowKgs;
				Settings.Input.AirInletTempK=Defaults.AirInletTempK; Settings.Input.SlotAirInletTempK.Reset();
				Item.Rack=Settings;
				if ((!Item.Actor && Item.Class!=ARackThermalActor::StaticClass()->GetName())
					|| (Item.Actor && !Cast<ARackThermalActor>(Item.Actor))) return Fail(TEXT("생성할 수 없는 랙 종류"));
			}
			else if (Item.Class==ARackThermalActor::StaticClass()->GetName() || Cast<ARackThermalActor>(Item.Actor)) return Fail(TEXT("랙 ID/설정 누락"));
		}
		if (Item.Actor && (Item.Actor->GetClass()->GetName()!=Item.Class || !Item.Actor->GetRootComponent()
			|| Item.Actor->GetRootComponent()->Mobility!=EComponentMobility::Movable)) return Fail(TEXT("장비 종류 또는 이동 가능 상태 불일치"));
		if (!Item.Actor && (Version==1 || !Item.Rack.IsSet()))
		{
			++Missing; UE_LOG(LogTemp,Warning,TEXT("[VrgLayout] 레벨에 없는 장비: %s"),*Item.Name);
		}
		if (Version==1 && Item.Actor)
		{
			Pitch=Item.Actor->GetActorRotation().Pitch; Roll=Item.Actor->GetActorRotation().Roll;
			const FVector Scale=Item.Actor->GetActorScale3D(); SX=Scale.X; SY=Scale.Y; SZ=Scale.Z;
		}
		Item.Target=FTransform(FRotator(Pitch,Yaw,Roll),SimUnits::MetersToUnreal(FVector(X,Y,Z)),FVector(SX,SY,SZ));
		if (Item.Target.ContainsNaN()) return Fail(TEXT("좌표 변환 범위 오류"));
		Items.Add(MoveTemp(Item));
	}
	if (Missing) return Fail(FString::Printf(TEXT("레벨에 없는 장비 %d: 적용하지 않음"),Missing));
	const TSharedPtr<FJsonObject>* SettingsObject=nullptr;
	if (!Root->TryGetObjectField(TEXT("settings"),SettingsObject)) return Fail(TEXT("냉각 설정 객체 없음"));
	for (const auto& Pair:(*SettingsObject)->Values) if (!FiniteLayoutJson(Pair.Value)) return Fail(TEXT("냉각 설정 유한값 오류"));
	for (const TCHAR* Key:{TEXT("containment"),TEXT("crahLayout"),TEXT("coolantRejection"),TEXT("cellSizeM")})
	{
		if (!(*SettingsObject)->HasField(Key)) continue;
		double V; if (!ReadLayoutNumber(*SettingsObject,Key,V)) return Fail(TEXT("냉각 설정 형식 오류"));
		if (Version==2 && (FString(Key)==TEXT("cellSizeM") ? V<0.01 : (V!=FMath::FloorToDouble(V) || V<0 || V>(FString(Key)==TEXT("coolantRejection")?1:2))))
			return Fail(TEXT("냉각 설정 범위 오류")); // Existing enum and air-cell admissibility bounds, not fitted physics values.
	}
	TArray<ARackThermalActor*> Remove;
	if (Version==2) for (const auto& Pair:ById) if (!SeenIds.Contains(Pair.Key))
	{
		if (Pair.Value->GetClass()!=ARackThermalActor::StaticClass() || Pair.Value->GetLocalRole()!=ROLE_Authority) return Fail(TEXT("제거할 수 없는 랙 종류/권한"));
		Remove.Add(Pair.Value);
	}
	// Verify target names before spawning or touching current equipment. Rack names may be released by reconciliation.
	for (const auto& Item:Items) if (Version==2 && Item.Rack.IsSet())
	{
		UObject* Occupant=FindObjectFast<UObject>(World->PersistentLevel,FName(*Item.Name));
		if (Occupant && Occupant!=Item.Actor && !Remove.Contains(Cast<ARackThermalActor>(Occupant))) return Fail(TEXT("복원 이름이 다른 객체와 충돌"));
	}
	TArray<FTransform> OldTransforms;
	for (AActor* Actor:Current) OldTransforms.Add(Actor->GetActorTransform());
	struct FRemovalBackup {
		FName Name; FString Id; FString PreviousId; FTransform Transform; FRackLayoutSettings Settings;
		FRackThermalModel Model; FRackThermalInput Input; FRackThermalState Initial; double Time; bool bExternal;
	};
	TArray<FRemovalBackup> Backups;
	TArray<FName> RemovedNames;
	for (auto* Rack:Remove) {
		RemovedNames.Add(Rack->GetFName());
		Backups.Add({Rack->GetFName(),VrgRackLifecycle::Identity(Rack),Rack->LayoutId,Rack->GetActorTransform(),Rack->CaptureLayoutSettings(),
			Rack->GetMutableModel(),Rack->Input,Rack->InitialState,Rack->GetSnapshot().SimTimeS,Rack->bDrivenExternally});
	}
	auto Rollback=[&]() {
		for (auto& Item:Items) if (Item.bSpawned && IsValid(Item.Actor)) VrgRackLifecycle::Remove(CastChecked<ARackThermalActor>(Item.Actor));
		for (int32 I=0;I<Remove.Num();++I) {
			if (IsValid(Remove[I])) Remove[I]->Rename(*RemovedNames[I].ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
			else {
				const auto& B=Backups[I];
				auto* Restored=VrgRackLifecycle::Spawn(World,B.Name,B.Id,ARackThermalActor::StaticClass(),B.Transform,B.Settings);
				if (Restored) {
					// Rollback only: preserve live state in memory, never serialize it into a layout file.
					Restored->GetMutableModel()=B.Model; Restored->Input=B.Input; Restored->InitialState=B.Initial;
					Restored->bDrivenExternally=B.bExternal; Restored->SetExternalSimTimeS(B.Time);
					for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->NotifyLayoutRackRecreated(Restored);
					Restored->LayoutId=B.PreviousId;
				}
			}
		}
		for (int32 I=0;I<Current.Num();++I) if (IsValid(Current[I])) Current[I]->SetActorTransform(OldTransforms[I],false);
	};
	for (auto& Item:Items) if (!Item.Actor)
	{
		const FName Temp=MakeUniqueObjectName(World->PersistentLevel,ARackThermalActor::StaticClass(),TEXT("PendingLayoutRack"));
		Item.Actor=VrgRackLifecycle::Spawn(World,Temp,Item.Id,ARackThermalActor::StaticClass(),Item.Target,Item.Rack.GetValue());
		Item.bSpawned=Item.Actor!=nullptr;
		if (!Item.Actor) { Rollback(); return Fail(TEXT("랙 생성 실패: 이전 배치 유지")); }
	}
	for (auto* Rack:Remove)
	{
		const FName Temp=MakeUniqueObjectName(Rack->GetOuter(),Rack->GetClass(),TEXT("PendingLayoutRemoval"));
		if (!Rack->Rename(*Temp.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("랙 이름 준비 실패")); }
	}
	for (auto& Item:Items)
	{
		if (Item.bSpawned && !Item.Actor->Rename(*Item.Name,nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("랙 이름 복원 실패")); }
		if (!Item.Actor->SetActorTransform(Item.Target,false)) { Rollback(); return Fail(TEXT("변환 적용 실패: 이전 배치 유지")); }
	}
	for (auto* Rack:Remove) if (!Rack->Destroy())
	{
		Rollback(); VrgRackLifecycle::Rebuild(World);
		return Fail(TEXT("랙 제거 실패: 이전 배치 복원"));
	}
	// Commit only after structural validation, all spawns/names/transforms and removals have succeeded.
	ADataCenterSimulationActor* Sim=LayoutFindFirst<ADataCenterSimulationActor>(World);
	if (Sim && Sim->GetSimMode()==ESimMode::Simulation) Sim->EnterLayoutMode();
	for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->ResetLayoutEditsForLoad();
	for (auto& Item:Items) if (Item.Rack.IsSet())
	{
		auto* Rack=CastChecked<ARackThermalActor>(Item.Actor); Rack->LayoutId=Item.Id; Rack->bDrivenExternally=true;
		if (!Item.bSpawned) { Rack->ApplyLayoutSettings(Item.Rack.GetValue()); Rack->PauseSim(); Rack->RefreshVisuals(); }
	}
	double V;
	if (Sim)
	{
		if ((*SettingsObject)->TryGetNumberField(TEXT("containment"),V)) Sim->Containment=static_cast<EAisleContainment>(FMath::Clamp(int32(V),0,2));
		if ((*SettingsObject)->TryGetNumberField(TEXT("crahLayout"),V)) Sim->CrahLayout=static_cast<ECrahLayout>(FMath::Clamp(int32(V),0,2));
		if ((*SettingsObject)->TryGetNumberField(TEXT("coolantRejection"),V)) Sim->CoolantRejection=static_cast<ECoolantRejection>(FMath::Clamp(int32(V),0,1));
	}
	if (auto* Air=LayoutFindFirst<AAirFieldDebugActor>(World))
		if ((*SettingsObject)->TryGetNumberField(TEXT("cellSizeM"),V) && V>=0.01) Air->GridParams.CellSizeM=V;
	VrgRackLifecycle::Rebuild(World);
	OutSummary=FString::Printf(TEXT("v%.0f: 장비%d 적용, 랙%d 생성/%d 제거 (상태·기록 저장 없음)"),Version,Items.Num(),Items.FilterByPredicate([](const auto& I){return I.bSpawned;}).Num(),Remove.Num());
	return true;
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
	TEXT("Saved/Layouts/<이름>.json의 배치를 랙 목록/설정과 기존 장비 변환을 복원한다(배치 모드로 바꾼다). vrg.Layout.Load <이름>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		FString Summary;
		const bool bOk = VrgLayoutFile::Load(LayoutGameWorld(), Args.Num() > 0 ? Args[0] : FString(TEXT("layout")), Summary);
		UE_LOG(LogTemp, Display, TEXT("[VrgLayout] 불러오기 %s: %s"), bOk ? TEXT("성공") : TEXT("일부 실패"), *Summary);
	}));
