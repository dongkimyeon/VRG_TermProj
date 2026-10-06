// Fill out your copyright notice in the Description page of Project Settings.

#include "Interaction/VrgLayoutFile.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgRackLifecycle.h"
#include "Interaction/VrgSpineLifecycle.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgCrahLifecycle.h"
#include "Interaction/VrgCrahLayoutState.h"
#include "Interaction/VrgCduLayoutState.h"
#include "Interaction/VrgSpineLayoutState.h"
#include "Actors/DataCenterRoomActor.h"
#include "JsonObjectConverter.h"
#include "Interaction/VrgLayoutValidation.h"

#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/CrahUnitActor.h"
#include "Actors/LayoutBarrierActor.h"
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
 bool WriteCrahRecord(const FCrahLayoutRecord& R,const TSharedRef<FJsonObject>& Item)
 {
  Item->SetStringField(TEXT("crahId"),R.Id); Item->SetBoolField(TEXT("runtimeCrah"),R.bRuntime);
  TSharedRef<FJsonObject> Config=MakeShared<FJsonObject>();
  if(!FJsonObjectConverter::UStructToJsonObject(FCrahLayoutSettings::StaticStruct(),&R.Settings,Config)) return false;
  Item->SetObjectField(TEXT("crahSettings"),Config);
  // Original actor transform is UE cm (scale dimensionless); original size is metres.
  Item->SetBoolField(TEXT("convertedAutomaticCrah"),R.bConvertedAutomatic);
  if(R.bConvertedAutomatic) {
   TSharedRef<FJsonObject> T=MakeShared<FJsonObject>(),Size=MakeShared<FJsonObject>();
   if(!FJsonObjectConverter::UStructToJsonObject(TBaseStructure<FTransform>::Get(),&R.AutomaticTransform,T)
    || !FJsonObjectConverter::UStructToJsonObject(TBaseStructure<FVector>::Get(),&R.AutomaticSizeM,Size)) return false;
   Item->SetObjectField(TEXT("automaticCrahTransformCm"),T); Item->SetObjectField(TEXT("automaticCrahSizeM"),Size);
  }
  return true;
 }

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
	Root->SetBoolField(TEXT("crahInventory"), true); // Optional v2; omitted runtime units restore automatic layout.
	Root->SetBoolField(TEXT("cduInventory"), true); // Additive v2; absent flag preserves legacy CDU transforms/placed inventory.
	Root->SetBoolField(TEXT("spineInventory"), true); // Additive v2 capability; old v2 remains transform-only for spines.
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
		if (const auto* Spine = Cast<ASpineCabinetActor>(*It))
		{
			Item->SetStringField(TEXT("spineId"), VrgSpineLifecycle::Identity(Spine));
			Item->SetBoolField(TEXT("runtimeSpine"), Spine->bRuntimeLayoutSpine);
			TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
			const auto Snapshot = Spine->CaptureLayoutSettings();
			if (!FJsonObjectConverter::UStructToJsonObject(FSpineLayoutSettings::StaticStruct(), &Snapshot, Config)) return false;
			Item->SetObjectField(TEXT("spineSettings"), Config);
		}
		if (const auto* Cdu = Cast<ACoolingDistributionUnitActor>(*It))
		{
			Item->SetStringField(TEXT("cduId"), VrgCduLifecycle::Identity(Cdu));
			Item->SetBoolField(TEXT("runtimeCdu"), Cdu->bRuntimeLayoutCdu);
			TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
			const auto Snapshot = Cdu->CaptureLayoutSettings();
			if (!FJsonObjectConverter::UStructToJsonObject(FCduLayoutSettings::StaticStruct(), &Snapshot, Config)) return false;
			Item->SetObjectField(TEXT("cduSettings"), Config);
		}
		if (const auto* Crah = Cast<ACrahUnitActor>(*It))
		{
   if(!WriteCrahRecord({VrgCrahLifecycle::Identity(Crah),Crah->GetFName(),Crah->GetActorTransform(),Crah->CaptureLayoutSettings(),
    Crah->bRuntimeLayoutCrah,Crah->bConvertedAutomaticCrah,Crah->AutomaticLayoutTransform,Crah->AutomaticLayoutSizeM},Item)) return false;
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Root->SetArrayField(TEXT("equipment"), Items);
	// Explicit intent: older files with absent spine entries never imply placed-spine deletion.
	TArray<TSharedPtr<FJsonValue>> Deleted;
	if (auto* State = World->GetSubsystem<UVrgSpineLayoutState>()) for (const auto& Pair : State->DeletedPlaced)
	{
		const auto& R = Pair.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), R.Name.ToString()); O->SetStringField(TEXT("class"), ASpineCabinetActor::StaticClass()->GetName());
		O->SetStringField(TEXT("spineId"), R.Id); O->SetBoolField(TEXT("runtimeSpine"), false);
		const FVector M = SimUnits::UnrealToMeters(R.Transform.GetLocation());
		const FRotator Rotation = R.Transform.Rotator(); const FVector Scale = R.Transform.GetScale3D();
		O->SetNumberField(TEXT("x_m"), M.X); O->SetNumberField(TEXT("y_m"), M.Y); O->SetNumberField(TEXT("z_m"), M.Z);
		O->SetNumberField(TEXT("yaw_deg"), Rotation.Yaw); O->SetNumberField(TEXT("pitch_deg"), Rotation.Pitch); O->SetNumberField(TEXT("roll_deg"), Rotation.Roll);
		O->SetNumberField(TEXT("scale_x"), Scale.X); O->SetNumberField(TEXT("scale_y"), Scale.Y); O->SetNumberField(TEXT("scale_z"), Scale.Z);
		TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(FSpineLayoutSettings::StaticStruct(), &R.Settings, Config)) return false;
		O->SetObjectField(TEXT("spineSettings"), Config); Deleted.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("deletedSpines"), Deleted); // Presence opts into native placed-spine restoration as well.
	TArray<TSharedPtr<FJsonValue>> DeletedCdus;
	if (auto* State = World->GetSubsystem<UVrgCduLayoutState>()) for (const auto& Pair : State->DeletedPlaced)
	{
		const auto& R = Pair.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), R.Name.ToString()); O->SetStringField(TEXT("class"), ACoolingDistributionUnitActor::StaticClass()->GetName());
		O->SetStringField(TEXT("cduId"), R.Id); O->SetBoolField(TEXT("runtimeCdu"), false);
		const FVector M = SimUnits::UnrealToMeters(R.Transform.GetLocation());
		const FRotator Rotation = R.Transform.Rotator(); const FVector Scale = R.Transform.GetScale3D();
		O->SetNumberField(TEXT("x_m"), M.X); O->SetNumberField(TEXT("y_m"), M.Y); O->SetNumberField(TEXT("z_m"), M.Z);
		O->SetNumberField(TEXT("yaw_deg"), Rotation.Yaw); O->SetNumberField(TEXT("pitch_deg"), Rotation.Pitch); O->SetNumberField(TEXT("roll_deg"), Rotation.Roll);
		O->SetNumberField(TEXT("scale_x"), Scale.X); O->SetNumberField(TEXT("scale_y"), Scale.Y); O->SetNumberField(TEXT("scale_z"), Scale.Z);
		TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(FCduLayoutSettings::StaticStruct(), &R.Settings, Config)) return false;
		O->SetObjectField(TEXT("cduSettings"), Config); DeletedCdus.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("deletedCdus"), DeletedCdus); // Explicit placed-CDU intent, independent of Undo history.
 TArray<TSharedPtr<FJsonValue>> DeletedCrahs;
 if(auto* State=World->GetSubsystem<UVrgCrahLayoutState>()) for(const auto& Pair:State->DeletedPlaced) {
  const auto& R=Pair.Value; TSharedRef<FJsonObject> O=MakeShared<FJsonObject>();
  O->SetStringField(TEXT("name"),R.Name.ToString()); O->SetStringField(TEXT("class"),ACrahUnitActor::StaticClass()->GetName());
  const FVector M=SimUnits::UnrealToMeters(R.Transform.GetLocation()),Scale=R.Transform.GetScale3D(); const auto Rot=R.Transform.Rotator();
  O->SetNumberField(TEXT("x_m"),M.X); O->SetNumberField(TEXT("y_m"),M.Y); O->SetNumberField(TEXT("z_m"),M.Z);
  O->SetNumberField(TEXT("yaw_deg"),Rot.Yaw); O->SetNumberField(TEXT("pitch_deg"),Rot.Pitch); O->SetNumberField(TEXT("roll_deg"),Rot.Roll);
  O->SetNumberField(TEXT("scale_x"),Scale.X); O->SetNumberField(TEXT("scale_y"),Scale.Y); O->SetNumberField(TEXT("scale_z"),Scale.Z);
  if(!WriteCrahRecord(R,O)) return false; DeletedCrahs.Add(MakeShared<FJsonValueObject>(O));
 }
 Root->SetArrayField(TEXT("deletedCrahs"),DeletedCrahs); // Optional v2 placed deletion capability; omissions never imply deletion.
	// Walls / curtains (optional v2): the list is the complete barrier inventory of the file. Size = scale [m].
	Root->SetBoolField(TEXT("barrierInventory"), true);
	TArray<TSharedPtr<FJsonValue>> Barriers;
	for (TActorIterator<ALayoutBarrierActor> It(World); It; ++It)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		const FVector M = SimUnits::UnrealToMeters(It->GetActorLocation());
		const FRotator Rotation = It->GetActorRotation(); const FVector Scale = It->GetActorScale3D();
		O->SetStringField(TEXT("name"), It->GetName()); O->SetStringField(TEXT("barrierId"), VrgBarrierLifecycle::Identity(*It));
		O->SetNumberField(TEXT("kind"), int32(It->Kind));
		O->SetNumberField(TEXT("x_m"), M.X); O->SetNumberField(TEXT("y_m"), M.Y); O->SetNumberField(TEXT("z_m"), M.Z);
		O->SetNumberField(TEXT("yaw_deg"), Rotation.Yaw); O->SetNumberField(TEXT("pitch_deg"), Rotation.Pitch); O->SetNumberField(TEXT("roll_deg"), Rotation.Roll);
		O->SetNumberField(TEXT("size_x_m"), Scale.X); O->SetNumberField(TEXT("size_y_m"), Scale.Y); O->SetNumberField(TEXT("size_z_m"), Scale.Z);
		Barriers.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("barriers"), Barriers);


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
	OutSummary = FString::Printf(TEXT("장비 %d대, 벽/차단막 %d개 저장: %s"), Items.Num(), Barriers.Num(), *Path);
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
		TOptional<FSpineLayoutSettings> Spine;
		TOptional<FCduLayoutSettings> Cdu;
		TOptional<FCrahLayoutSettings> Crah;
		bool bRuntimeCrah = false;
  bool bConvertedAutomaticCrah = false; FTransform AutomaticCrahTransform=FTransform::Identity; FVector AutomaticCrahSizeM=FVector::ZeroVector;
		bool bRuntimeCdu = false;
		bool bRuntimeSpine = false;
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
	bool bCrahInventory = false;
	if (Version == 2 && Root->HasField(TEXT("crahInventory")) && !Root->TryGetBoolField(TEXT("crahInventory"), bCrahInventory))
		return Fail(TEXT("CRAH 목록 기능 표식 오류"));
	bool bCduInventory = false;
	if (Version == 2 && Root->HasField(TEXT("cduInventory")) && !Root->TryGetBoolField(TEXT("cduInventory"), bCduInventory))
		return Fail(TEXT("CDU 목록 기능 표식 오류"));
	bool bSpineInventory = false;
	if (Version == 2 && Root->HasField(TEXT("spineInventory")) && !Root->TryGetBoolField(TEXT("spineInventory"), bSpineInventory))
		return Fail(TEXT("스파인 목록 기능 표식 오류"));
	const bool bSpineDeletions = Version == 2 && Root->HasField(TEXT("deletedSpines"));
	const TArray<TSharedPtr<FJsonValue>>* DeletedValues = nullptr;
	if (bSpineDeletions && (!bSpineInventory || !Root->TryGetArrayField(TEXT("deletedSpines"), DeletedValues)))
		return Fail(TEXT("명시 스파인 삭제 목록 오류"));
	const bool bCduDeletions = Version == 2 && Root->HasField(TEXT("deletedCdus"));
	const TArray<TSharedPtr<FJsonValue>>* DeletedCduValues = nullptr;
	if (bCduDeletions && (!bCduInventory || !Root->TryGetArrayField(TEXT("deletedCdus"), DeletedCduValues)))
		return Fail(TEXT("명시 CDU 삭제 목록 오류"));
 const bool bCrahDeletions=Version==2 && Root->HasField(TEXT("deletedCrahs"));
 const TArray<TSharedPtr<FJsonValue>>* DeletedCrahValues=nullptr;
 if(bCrahDeletions && (!bCrahInventory || !Root->TryGetArrayField(TEXT("deletedCrahs"),DeletedCrahValues))) return Fail(TEXT("명시 CRAH 삭제 목록 오류"));
 auto* CrahState=World->GetSubsystem<UVrgCrahLayoutState>();
	auto* CduState = World->GetSubsystem<UVrgCduLayoutState>();
	auto* SpineState = World->GetSubsystem<UVrgSpineLayoutState>();
	TMap<FString,ACrahUnitActor*> ByCrahId;
	TMap<FString,ACoolingDistributionUnitActor*> ByCduId;
	TMap<FString,ASpineCabinetActor*> BySpineId;
	TMap<FString,AActor*> ByName;
	TMap<FString,ARackThermalActor*> ById;
	TArray<AActor*> Current;
	for (TActorIterator<AActor> It(World);It;++It) if (LayoutIsEquipment(*It))
	{
		ByName.Add(It->GetName(),*It); Current.Add(*It);
		if (auto* Crah = Cast<ACrahUnitActor>(*It))
		{
			const FString Id = VrgCrahLifecycle::Identity(Crah);
			if (ByCrahId.Contains(Id)) return Fail(TEXT("월드 CRAH 식별자 중복"));
			ByCrahId.Add(Id, Crah);
		}
		if (auto* Cdu = Cast<ACoolingDistributionUnitActor>(*It))
		{
			const FString Id = VrgCduLifecycle::Identity(Cdu);
			if (ByCduId.Contains(Id)) return Fail(TEXT("월드 CDU 식별자 중복"));
			ByCduId.Add(Id, Cdu);
		}
		if (auto* Spine = Cast<ASpineCabinetActor>(*It))
		{
			const FString Id = VrgSpineLifecycle::Identity(Spine);
			if (BySpineId.Contains(Id)) return Fail(TEXT("월드 스파인 식별자 중복"));
			BySpineId.Add(Id, Spine);
		}
		if (auto* Rack=Cast<ARackThermalActor>(*It))
		{
			const FString Id=VrgRackLifecycle::Identity(Rack);
			if (ById.Contains(Id)) return Fail(TEXT("월드 랙 식별자 중복"));
			ById.Add(Id,Rack);
		}
	}
	TArray<FLoadedEquipment> Items, DeletedItems, DeletedCduItems, DeletedCrahItems;
	TSet<FString> SeenNames,SeenIds,SeenSpineIds,SeenCduIds,SeenCrahIds;
	int32 Missing=0;
	TArray<TSharedPtr<FJsonValue>> AllValues = *Values;
	if (DeletedValues) AllValues.Append(*DeletedValues);
	const int32 DeletedCduStart = AllValues.Num();
	if (DeletedCduValues) AllValues.Append(*DeletedCduValues);
	const int32 DeletedCrahStart=AllValues.Num();
	if(DeletedCrahValues) AllValues.Append(*DeletedCrahValues);
	for (int32 ValueIndex = 0; ValueIndex < AllValues.Num(); ++ValueIndex)
	{
		const auto& Value = AllValues[ValueIndex];
		const bool bDeleteSpine = ValueIndex >= Values->Num() && ValueIndex < DeletedCduStart;
		const bool bDeleteCdu = ValueIndex >= DeletedCduStart && ValueIndex < DeletedCrahStart;
  const bool bDeleteCrah=ValueIndex>=DeletedCrahStart;
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
			if (O->HasField(TEXT("spineId")))
			{
				if (!bSpineInventory || Item.Rack.IsSet() || !O->TryGetStringField(TEXT("spineId"),Item.Id) || Item.Id.IsEmpty()
					|| SeenIds.Contains(Item.Id) || !O->TryGetBoolField(TEXT("runtimeSpine"),Item.bRuntimeSpine)
					|| !O->TryGetObjectField(TEXT("spineSettings"),Config)) return Fail(TEXT("스파인 ID/설정 오류 또는 중복"));
				SeenIds.Add(Item.Id); if (!bDeleteSpine) SeenSpineIds.Add(Item.Id); Item.Actor = BySpineId.FindRef(Item.Id);
				FSpineLayoutSettings SpineSettings;
				double SwitchCount, FirstSlot;
				if (!ReadLayoutNumber(*Config,TEXT("numSwitches"),SwitchCount) || !ReadLayoutNumber(*Config,TEXT("firstSwitchSlot"),FirstSlot)
					|| SwitchCount != FMath::FloorToDouble(SwitchCount) || FirstSlot != FMath::FloorToDouble(FirstSlot)
					|| SwitchCount < 1 || SwitchCount > 8 || FirstSlot < 0 || FirstSlot > 36
					|| !FJsonObjectConverter::JsonObjectToUStruct(Config->ToSharedRef(),&SpineSettings,0,0,true)
					|| !SpineSettings.IsValid()) return Fail(TEXT("스파인 설정 구조/범위 오류")); // Existing actor authoring bounds, no new hardware values.
				Item.Spine = SpineSettings;
				auto* ExistingSpine = Cast<ASpineCabinetActor>(Item.Actor);
				if ((!Item.Actor && (Item.Class != ASpineCabinetActor::StaticClass()->GetName()
					|| (!Item.bRuntimeSpine && !bSpineDeletions && !SpineState->DeletedPlaced.Contains(Item.Id))))
					|| (Item.Actor && (!ExistingSpine || ExistingSpine->bRuntimeLayoutSpine != Item.bRuntimeSpine))) return Fail(TEXT("스파인 종류/출처 불일치"));
			}
			else if (bSpineInventory && (Item.Class == ASpineCabinetActor::StaticClass()->GetName() || Cast<ASpineCabinetActor>(Item.Actor)))
				return Fail(TEXT("스파인 ID/설정 누락"));
			if (O->HasField(TEXT("cduId")))
			{
				if (!bCduInventory || bDeleteSpine || Item.Rack.IsSet() || Item.Spine.IsSet()
					|| !O->TryGetStringField(TEXT("cduId"),Item.Id) || Item.Id.IsEmpty() || SeenIds.Contains(Item.Id)
					|| !O->TryGetBoolField(TEXT("runtimeCdu"),Item.bRuntimeCdu)
					|| !O->TryGetObjectField(TEXT("cduSettings"),Config)) return Fail(TEXT("CDU ID/설정 오류 또는 중복"));
				SeenIds.Add(Item.Id); if (!bDeleteCdu) SeenCduIds.Add(Item.Id); Item.Actor = ByCduId.FindRef(Item.Id);
				FCduLayoutSettings CduSettings;
				if (!FJsonObjectConverter::JsonObjectToUStruct(Config->ToSharedRef(),&CduSettings,0,0,true)
					|| !CduSettings.IsValid()) return Fail(TEXT("CDU 설정 구조/범위 오류"));
				Item.Cdu = CduSettings;
				const auto* ExistingCdu = Cast<ACoolingDistributionUnitActor>(Item.Actor);
				// Native placed restoration requires deletion capability or known intent; omissions never delete placed CDUs.
				if ((!Item.Actor && (Item.Class != ACoolingDistributionUnitActor::StaticClass()->GetName()
					|| (!Item.bRuntimeCdu && !bCduDeletions && !CduState->DeletedPlaced.Contains(Item.Id))))
					|| (Item.Actor && (!ExistingCdu || ExistingCdu->bRuntimeLayoutCdu != Item.bRuntimeCdu))) return Fail(TEXT("CDU 종류/출처 불일치"));
			}
			else if (bCduInventory && (Item.Class == ACoolingDistributionUnitActor::StaticClass()->GetName() || Cast<ACoolingDistributionUnitActor>(Item.Actor)))
				return Fail(TEXT("CDU ID/설정 누락"));
			if (O->HasField(TEXT("crahId")))
			{
				if (!bCrahInventory || bDeleteSpine || bDeleteCdu || Item.Rack.IsSet() || Item.Spine.IsSet() || Item.Cdu.IsSet()
					|| !O->TryGetStringField(TEXT("crahId"),Item.Id) || Item.Id.IsEmpty() || SeenIds.Contains(Item.Id)
					|| !O->TryGetBoolField(TEXT("runtimeCrah"),Item.bRuntimeCrah)
					|| !O->TryGetObjectField(TEXT("crahSettings"),Config)) return Fail(TEXT("CRAH ID/설정 오류 또는 중복"));
				SeenIds.Add(Item.Id); if(!bDeleteCrah) SeenCrahIds.Add(Item.Id); Item.Actor = ByCrahId.FindRef(Item.Id);
				FCrahLayoutSettings CrahSettings;
				if (!FJsonObjectConverter::JsonObjectToUStruct(Config->ToSharedRef(),&CrahSettings,0,0,true)
					|| !CrahSettings.IsValid()) return Fail(TEXT("CRAH 설정 구조/범위 오류"));
				Item.Crah = CrahSettings;
    if(O->HasField(TEXT("convertedAutomaticCrah")) && !O->TryGetBoolField(TEXT("convertedAutomaticCrah"),Item.bConvertedAutomaticCrah)) return Fail(TEXT("CRAH 자동 전환 표식 오류"));
    if(Item.bConvertedAutomaticCrah) {
     const TSharedPtr<FJsonObject>* AutoTransform=nullptr; const TSharedPtr<FJsonObject>* AutoSize=nullptr;
     if(!O->TryGetObjectField(TEXT("automaticCrahTransformCm"),AutoTransform) || !O->TryGetObjectField(TEXT("automaticCrahSizeM"),AutoSize)
      || !FJsonObjectConverter::JsonObjectToUStruct(AutoTransform->ToSharedRef(),TBaseStructure<FTransform>::Get(),&Item.AutomaticCrahTransform,0,0,true)
      || !FJsonObjectConverter::JsonObjectToUStruct(AutoSize->ToSharedRef(),TBaseStructure<FVector>::Get(),&Item.AutomaticCrahSizeM,0,0,true)
      || Item.AutomaticCrahTransform.ContainsNaN() || !Item.AutomaticCrahTransform.GetRotation().IsNormalized()
      || Item.AutomaticCrahTransform.GetScale3D().GetAbsMin()==0 || Item.AutomaticCrahSizeM.ContainsNaN() || Item.AutomaticCrahSizeM.GetMin()<=0)
      return Fail(TEXT("CRAH 자동 전환 원래 경계 오류"));
    }
				const auto* ExistingCrah = Cast<ACrahUnitActor>(Item.Actor);
				if ((!Item.Actor && (Item.Class != ACrahUnitActor::StaticClass()->GetName()
     || (!Item.bRuntimeCrah && !bCrahDeletions && !CrahState->DeletedPlaced.Contains(Item.Id))))
					|| (Item.Actor && (!ExistingCrah || ExistingCrah->bRuntimeLayoutCrah != Item.bRuntimeCrah))) return Fail(TEXT("CRAH 종류/출처 불일치"));
			}
			else if (bCrahInventory && (Item.Class == ACrahUnitActor::StaticClass()->GetName() || Cast<ACrahUnitActor>(Item.Actor)))
				return Fail(TEXT("CRAH ID/설정 누락"));
		}
		if (bDeleteSpine && (!Item.Spine.IsSet() || Item.bRuntimeSpine
			|| Item.Class != ASpineCabinetActor::StaticClass()->GetName())) return Fail(TEXT("명시 삭제는 기존 네이티브 스파인만 지원"));
		if (bDeleteCdu && (!Item.Cdu.IsSet() || Item.bRuntimeCdu || Item.Class != ACoolingDistributionUnitActor::StaticClass()->GetName()))
			return Fail(TEXT("명시 삭제는 기존 네이티브 CDU만 지원"));
  if(bDeleteCrah && (!Item.Crah.IsSet() || Item.bRuntimeCrah || Item.Class!=ACrahUnitActor::StaticClass()->GetName()))
   return Fail(TEXT("명시 삭제는 기존 네이티브 CRAH만 지원"));
		if (Item.Actor && (Item.Actor->GetClass()->GetName()!=Item.Class || !Item.Actor->GetRootComponent()
			|| Item.Actor->GetRootComponent()->Mobility!=EComponentMobility::Movable)) return Fail(TEXT("장비 종류 또는 이동 가능 상태 불일치"));
		if (!bDeleteSpine && !bDeleteCdu && !bDeleteCrah && !Item.Actor && (Version==1 || (!Item.Rack.IsSet() && !Item.Spine.IsSet() && !Item.Cdu.IsSet() && !Item.Crah.IsSet())))
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
		if (bDeleteSpine) DeletedItems.Add(MoveTemp(Item)); else if (bDeleteCdu) DeletedCduItems.Add(MoveTemp(Item)); else if(bDeleteCrah) DeletedCrahItems.Add(MoveTemp(Item)); else Items.Add(MoveTemp(Item));
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
	// Walls / curtains: validate the whole list before anything changes. Absent flag = barriers stay as they are.
	bool bBarrierInventory = false;
	if (Version == 2 && Root->HasField(TEXT("barrierInventory")) && !Root->TryGetBoolField(TEXT("barrierInventory"), bBarrierInventory))
		return Fail(TEXT("벽/차단막 목록 기능 표식 오류"));
	TArray<FLayoutBarrierRecord> LoadedBarriers;
	if (bBarrierInventory)
	{
		const TArray<TSharedPtr<FJsonValue>>* BarrierValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("barriers"), BarrierValues)) return Fail(TEXT("벽/차단막 목록 없음"));
		TSet<FString> BarrierIds;
		for (const auto& Value : *BarrierValues)
		{
			if (!FiniteLayoutJson(Value) || Value->Type != EJson::Object) return Fail(TEXT("벽/차단막 형식/유한값 오류"));
			const auto O = Value->AsObject();
			FLayoutBarrierRecord R; FString Name; double Kind, X, Y, Z, Yaw, Pitch, Roll, SX, SY, SZ;
			if (!O->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() || SeenNames.Contains(Name)
				|| Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")) || Name.Contains(TEXT(":"))
				|| !O->TryGetStringField(TEXT("barrierId"), R.Id) || R.Id.IsEmpty() || BarrierIds.Contains(R.Id)
				|| !ReadLayoutNumber(O, TEXT("kind"), Kind) || (Kind != 0 && Kind != 1)
				|| !ReadLayoutNumber(O, TEXT("x_m"), X) || !ReadLayoutNumber(O, TEXT("y_m"), Y) || !ReadLayoutNumber(O, TEXT("z_m"), Z)
				|| !ReadLayoutNumber(O, TEXT("yaw_deg"), Yaw) || !ReadLayoutNumber(O, TEXT("pitch_deg"), Pitch) || !ReadLayoutNumber(O, TEXT("roll_deg"), Roll)
				|| !ReadLayoutNumber(O, TEXT("size_x_m"), SX) || !ReadLayoutNumber(O, TEXT("size_y_m"), SY) || !ReadLayoutNumber(O, TEXT("size_z_m"), SZ))
				return Fail(TEXT("벽/차단막 이름/ID/종류/변환 오류 또는 중복"));
			SeenNames.Add(Name); BarrierIds.Add(R.Id);
			R.Name = FName(*Name); R.Kind = static_cast<EBarrierKind>(int32(Kind));
			R.Transform = FTransform(FRotator(Pitch, Yaw, Roll), SimUnits::MetersToUnreal(FVector(X, Y, Z)), FVector(SX, SY, SZ));
			if (!VrgBarrierLifecycle::IsValidTransform(R.Transform)) return Fail(TEXT("벽/차단막 크기는 양수여야 합니다"));
			LoadedBarriers.Add(R);
		}
	}
	TArray<ARackThermalActor*> Remove;
	if (Version==2) for (const auto& Pair:ById) if (!SeenIds.Contains(Pair.Key))
	{
		if (Pair.Value->GetClass()!=ARackThermalActor::StaticClass() || Pair.Value->GetLocalRole()!=ROLE_Authority) return Fail(TEXT("제거할 수 없는 랙 종류/권한"));
		Remove.Add(Pair.Value);
	}
	TArray<ASpineCabinetActor*> RemoveSpines;
	for (const auto& Item : DeletedItems) if (auto* Spine = Cast<ASpineCabinetActor>(Item.Actor))
	{
		if (Spine->GetLocalRole() != ROLE_Authority) return Fail(TEXT("스파인 삭제 권한 없음"));
		RemoveSpines.Add(Spine);
	}
	if (bSpineInventory) for (const auto& Pair:BySpineId)
		if (Pair.Value->bRuntimeLayoutSpine && !SeenSpineIds.Contains(Pair.Key))
		{
			if (Pair.Value->GetClass()!=ASpineCabinetActor::StaticClass() || Pair.Value->GetLocalRole()!=ROLE_Authority)
				return Fail(TEXT("제거할 수 없는 런타임 스파인"));
			RemoveSpines.Add(Pair.Value);
		}
	TArray<ACoolingDistributionUnitActor*> RemoveCdus;
	for (const auto& Item : DeletedCduItems) if (auto* Cdu = Cast<ACoolingDistributionUnitActor>(Item.Actor))
	{
		if (Cdu->GetLocalRole()!=ROLE_Authority) return Fail(TEXT("CDU 삭제 권한 없음"));
		RemoveCdus.Add(Cdu);
	}
	if (bCduInventory) for (const auto& Pair : ByCduId) if (Pair.Value->bRuntimeLayoutCdu && !SeenCduIds.Contains(Pair.Key))
	{
		if (Pair.Value->GetClass()!=ACoolingDistributionUnitActor::StaticClass() || Pair.Value->GetLocalRole()!=ROLE_Authority)
			return Fail(TEXT("제거할 수 없는 런타임 CDU"));
		RemoveCdus.Add(Pair.Value);
	}
	TArray<ACrahUnitActor*> RemoveCrahs;
 for(const auto& Item:DeletedCrahItems) if(auto* Crah=Cast<ACrahUnitActor>(Item.Actor)) {
  if(Crah->GetLocalRole()!=ROLE_Authority) return Fail(TEXT("CRAH 삭제 권한 없음"));
  RemoveCrahs.Add(Crah);
 }
	if (bCrahInventory) for (const auto& Pair : ByCrahId) if (Pair.Value->bRuntimeLayoutCrah && !SeenCrahIds.Contains(Pair.Key))
	{
		if (Pair.Value->GetClass()!=ACrahUnitActor::StaticClass() || Pair.Value->GetLocalRole()!=ROLE_Authority)
			return Fail(TEXT("제거할 수 없는 런타임 CRAH"));
		RemoveCrahs.Add(Pair.Value);
	}
	// Verify target names before spawning or touching current equipment. Rack names may be released by reconciliation.
	for (const auto& Item:Items) if (Version==2 && (Item.Rack.IsSet() || Item.Spine.IsSet() || Item.Cdu.IsSet() || Item.Crah.IsSet()))
	{
		UObject* Occupant=FindObjectFast<UObject>(World->PersistentLevel,FName(*Item.Name));
		if (Occupant && Occupant!=Item.Actor && !Remove.Contains(Cast<ARackThermalActor>(Occupant)) && !RemoveSpines.Contains(Cast<ASpineCabinetActor>(Occupant)) && !RemoveCdus.Contains(Cast<ACoolingDistributionUnitActor>(Occupant)) && !RemoveCrahs.Contains(Cast<ACrahUnitActor>(Occupant))) return Fail(TEXT("복원 이름이 다른 객체와 충돌"));
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
	struct FSpineRemovalBackup {
		FName Name; FString Id; FTransform Transform; FSpineLayoutSettings Settings; FRackThermalModel Model; FRackThermalInput Input;
		bool bRuntime;
	};
	TArray<FSpineRemovalBackup> SpineBackups;
	for (auto* Spine:RemoveSpines) SpineBackups.Add({Spine->GetFName(),VrgSpineLifecycle::Identity(Spine),Spine->GetActorTransform(),
		Spine->CaptureLayoutSettings(),Spine->GetMutableModel(),Spine->GetMutableInput(),Spine->bRuntimeLayoutSpine});
	struct FCduRemovalBackup { FName Name; FString Id; FTransform Transform; FCduLayoutSettings Settings; bool bRuntime; };
	TArray<FCduRemovalBackup> CduBackups;
	for (auto* Cdu : RemoveCdus) CduBackups.Add({Cdu->GetFName(),VrgCduLifecycle::Identity(Cdu),Cdu->GetActorTransform(),Cdu->CaptureLayoutSettings(),Cdu->bRuntimeLayoutCdu});
	TArray<FCrahLayoutRecord> CrahBackups;
	for (auto* Crah : RemoveCrahs) CrahBackups.Add({VrgCrahLifecycle::Identity(Crah),Crah->GetFName(),Crah->GetActorTransform(),Crah->CaptureLayoutSettings(),Crah->bRuntimeLayoutCrah,Crah->bConvertedAutomaticCrah,Crah->AutomaticLayoutTransform,Crah->AutomaticLayoutSizeM});
	const auto OldDeletedPlaced = SpineState->DeletedPlaced;
	const auto OldDeletedCdus = CduState->DeletedPlaced;
 const auto OldDeletedCrahs=CrahState->DeletedPlaced;
	// Barrier replacement happens first (below); the rollback puts the old barriers back.
	TArray<FLayoutBarrierRecord> OldBarriers;
	TArray<ALayoutBarrierActor*> OldBarrierActors, NewBarriers;
	bool bBarriersReplaced = false;
	if (bBarrierInventory) for (TActorIterator<ALayoutBarrierActor> It(World); It; ++It)
	{
		OldBarrierActors.Add(*It);
		OldBarriers.Add({VrgBarrierLifecycle::Identity(*It), It->GetFName(), It->GetActorTransform(), It->Kind});
	}
	auto RollbackBarriers=[&]() {
		if (!bBarriersReplaced) return;
		for (auto* B : NewBarriers) if (IsValid(B)) { B->Rename(*MakeUniqueObjectName(B->GetOuter(),B->GetClass(),TEXT("RetiredLayoutBarrier")).ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional); B->Destroy(false,false); }
		for (const auto& R : OldBarriers) verify(VrgBarrierLifecycle::Spawn(World, R));
		bBarriersReplaced = false;
	};
	auto Rollback=[&]() {
		RollbackBarriers();
		for (auto& Item:Items) if (Item.bSpawned && IsValid(Item.Actor))
		{
			if (Item.Crah.IsSet()) VrgCrahLifecycle::Remove(CastChecked<ACrahUnitActor>(Item.Actor));
			else if (Item.Cdu.IsSet()) VrgCduLifecycle::Remove(CastChecked<ACoolingDistributionUnitActor>(Item.Actor));
			else if (Item.Spine.IsSet()) VrgSpineLifecycle::Remove(CastChecked<ASpineCabinetActor>(Item.Actor));
			else VrgRackLifecycle::Remove(CastChecked<ARackThermalActor>(Item.Actor));
		}
		for (int32 I=0;I<RemoveCrahs.Num();++I)
		{
			const auto& B=CrahBackups[I];
			if (IsValid(RemoveCrahs[I])) RemoveCrahs[I]->Rename(*B.Name.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
			else if (auto* Restored=VrgCrahLifecycle::Spawn(World,B))
				for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->NotifyLayoutCrahRecreated(Restored);
		}
		for (int32 I=0;I<RemoveCdus.Num();++I)
		{
			const auto& B=CduBackups[I];
			if (IsValid(RemoveCdus[I])) RemoveCdus[I]->Rename(*B.Name.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
			else if (auto* Restored=VrgCduLifecycle::Spawn(World,B.Name,B.Id,B.Transform,B.Settings,B.bRuntime))
				for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->NotifyLayoutCduRecreated(Restored);
		}
		for (int32 I=0;I<RemoveSpines.Num();++I)
		{
			const auto& B=SpineBackups[I];
			if (IsValid(RemoveSpines[I])) RemoveSpines[I]->Rename(*B.Name.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional);
			else if (auto* Restored=VrgSpineLifecycle::Spawn(World,B.Name,B.Id,B.Transform,B.Settings,B.bRuntime))
			{
				Restored->GetMutableModel()=B.Model; Restored->GetMutableInput()=B.Input;
				for (TActorIterator<ADataCenterDebugController> It(World); It; ++It) It->NotifyLayoutSpineRecreated(Restored);
			}
		}
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
		SpineState->DeletedPlaced = OldDeletedPlaced;
		CduState->DeletedPlaced = OldDeletedCdus;
  CrahState->DeletedPlaced=OldDeletedCrahs;
	};
	if (bBarrierInventory)
	{
		if (!OldBarrierActors.IsEmpty() && !VrgBarrierLifecycle::RemoveBatch(OldBarrierActors)) return Fail(TEXT("기존 벽/차단막 제거 실패: 이전 배치 유지"));
		bBarriersReplaced = true;
		for (const auto& R : LoadedBarriers)
		{
			auto* B = VrgBarrierLifecycle::Spawn(World, R);
			if (!B) { Rollback(); return Fail(TEXT("벽/차단막 생성 실패: 이전 배치 유지")); }
			NewBarriers.Add(B);
		}
	}
	for (auto& Item:Items) if (!Item.Actor)
	{
		UClass* Kind=Item.Crah.IsSet()?ACrahUnitActor::StaticClass():Item.Cdu.IsSet()?ACoolingDistributionUnitActor::StaticClass():Item.Spine.IsSet()?ASpineCabinetActor::StaticClass():ARackThermalActor::StaticClass();
		const FName Temp=MakeUniqueObjectName(World->PersistentLevel,Kind,TEXT("PendingLayoutEquipment"));
		Item.Actor=Item.Crah.IsSet()?static_cast<AActor*>(VrgCrahLifecycle::Spawn(World,{Item.Id,Temp,Item.Target,Item.Crah.GetValue(),Item.bRuntimeCrah,Item.bConvertedAutomaticCrah,Item.AutomaticCrahTransform,Item.AutomaticCrahSizeM}))
			:Item.Cdu.IsSet()?static_cast<AActor*>(VrgCduLifecycle::Spawn(World,Temp,Item.Id,Item.Target,Item.Cdu.GetValue(),Item.bRuntimeCdu))
			:Item.Spine.IsSet()?static_cast<AActor*>(VrgSpineLifecycle::Spawn(World,Temp,Item.Id,Item.Target,Item.Spine.GetValue(),Item.bRuntimeSpine))
			:VrgRackLifecycle::Spawn(World,Temp,Item.Id,Kind,Item.Target,Item.Rack.GetValue());
		Item.bSpawned=Item.Actor!=nullptr;
		if (!Item.Actor) { Rollback(); return Fail(TEXT("랙 생성 실패: 이전 배치 유지")); }
	}
	for (auto* Rack:Remove)
	{
		const FName Temp=MakeUniqueObjectName(Rack->GetOuter(),Rack->GetClass(),TEXT("PendingLayoutRemoval"));
		if (!Rack->Rename(*Temp.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("랙 이름 준비 실패")); }
	}
	for (auto* Spine:RemoveSpines)
	{
		const FName Temp=MakeUniqueObjectName(Spine->GetOuter(),Spine->GetClass(),TEXT("PendingSpineRemoval"));
		if (!Spine->Rename(*Temp.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("스파인 이름 준비 실패")); }
	}
	for (auto* Cdu:RemoveCdus)
	{
		const FName Temp=MakeUniqueObjectName(Cdu->GetOuter(),Cdu->GetClass(),TEXT("PendingCduRemoval"));
		if (!Cdu->Rename(*Temp.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("CDU 이름 준비 실패")); }
	}
	for (auto* Crah:RemoveCrahs)
	{
		const FName Temp=MakeUniqueObjectName(Crah->GetOuter(),Crah->GetClass(),TEXT("PendingCrahRemoval"));
		if (!Crah->Rename(*Temp.ToString(),nullptr,REN_DontCreateRedirectors|REN_NonTransactional)) { Rollback(); return Fail(TEXT("CRAH 이름 준비 실패")); }
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
	for (auto* Spine:RemoveSpines) if (!Spine->Destroy())
	{
		Rollback(); VrgRackLifecycle::Rebuild(World);
		return Fail(TEXT("스파인 제거 실패: 이전 배치 복원"));
	}
	for (auto* Cdu:RemoveCdus) if (!Cdu->Destroy())
	{
		Rollback(); VrgRackLifecycle::Rebuild(World);
		return Fail(TEXT("CDU 제거 실패: 이전 배치 복원"));
	}
	for (auto* Crah:RemoveCrahs) if (!Crah->Destroy(false,false))
	{
		Rollback(); VrgRackLifecycle::Rebuild(World);
		return Fail(TEXT("CRAH 제거 실패: 이전 배치 복원"));
	}
 if(bCrahDeletions) {
  CrahState->DeletedPlaced.Reset();
  for(const auto& I:DeletedCrahItems) CrahState->DeletedPlaced.Add(I.Id,{I.Id,FName(*I.Name),I.Target,I.Crah.GetValue(),false,
   I.bConvertedAutomaticCrah,I.AutomaticCrahTransform,I.AutomaticCrahSizeM});
 }
	// Commit only after structural validation, all spawns/names/transforms and removals have succeeded.
	if (bSpineDeletions)
	{
		SpineState->DeletedPlaced.Reset();
		for (const auto& Item : DeletedItems) SpineState->DeletedPlaced.Add(Item.Id,
			{Item.Id, FName(*Item.Name), Item.Target, Item.Spine.GetValue(), false});
	}
	if (bCduDeletions)
	{
		CduState->DeletedPlaced.Reset();
		for (const auto& Item : DeletedCduItems) CduState->DeletedPlaced.Add(Item.Id,
			{Item.Id,FName(*Item.Name),Item.Target,Item.Cdu.GetValue(),false});
	}
	ADataCenterSimulationActor* Sim=LayoutFindFirst<ADataCenterSimulationActor>(World);
	if (Sim && Sim->GetSimMode()==ESimMode::Simulation) Sim->EnterLayoutMode();
	for (TActorIterator<ADataCenterDebugController> It(World);It;++It) It->ResetLayoutEditsForLoad();
	for (auto& Item:Items) if (Item.Rack.IsSet())
	{
		auto* Rack=CastChecked<ARackThermalActor>(Item.Actor); Rack->LayoutId=Item.Id; Rack->bDrivenExternally=true;
		if (!Item.bSpawned) { Rack->ApplyLayoutSettings(Item.Rack.GetValue()); Rack->PauseSim(); Rack->RefreshVisuals(); }
	}
	for (auto& Item:Items) if (Item.Spine.IsSet())
	{
		auto* Spine=CastChecked<ASpineCabinetActor>(Item.Actor);
		Spine->LayoutId=Item.Id;
		SpineState->DeletedPlaced.Remove(Item.Id);
		if (!Item.bSpawned) { Spine->ApplyLayoutSettings(Item.Spine.GetValue()); Spine->RefreshVisuals(); }
	}
	for (auto& Item:Items) if (Item.Cdu.IsSet())
	{
		auto* Cdu=CastChecked<ACoolingDistributionUnitActor>(Item.Actor); Cdu->LayoutId=Item.Id;
		CduState->DeletedPlaced.Remove(Item.Id);
		if (!Item.bSpawned) { Cdu->ApplyLayoutSettings(Item.Cdu.GetValue()); Cdu->RefreshVisuals(); }
	}
	for (auto& Item:Items) if (Item.Crah.IsSet())
	{
		auto* Crah=CastChecked<ACrahUnitActor>(Item.Actor); Crah->LayoutId=Item.Id;
  CrahState->DeletedPlaced.Remove(Item.Id);
  Crah->bConvertedAutomaticCrah=Item.bConvertedAutomaticCrah;
  Crah->AutomaticLayoutTransform=Item.AutomaticCrahTransform; Crah->AutomaticLayoutSizeM=Item.AutomaticCrahSizeM;
		if (!Item.bSpawned) { Crah->ApplyLayoutSettings(Item.Crah.GetValue()); Crah->RefreshVisuals(); }
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
	OutSummary=FString::Printf(TEXT("v%.0f: 장비%d 적용, 랙%d 생성/%d 제거 (상태·기록 저장 없음)"),Version,Items.Num(),Items.FilterByPredicate([](const auto& I){return I.bSpawned && I.Rack.IsSet();}).Num(),Remove.Num());
	OutSummary += FString::Printf(TEXT(" | 스파인%d 생성/%d 제거"), Items.FilterByPredicate([](const auto& I){return I.bSpawned && I.Spine.IsSet();}).Num(), RemoveSpines.Num());
	OutSummary += FString::Printf(TEXT(" | CDU%d 생성/%d 제거"), Items.FilterByPredicate([](const auto& I){return I.bSpawned && I.Cdu.IsSet();}).Num(), RemoveCdus.Num());
	OutSummary += FString::Printf(TEXT(" | CRAH%d 생성/%d 제거"), Items.FilterByPredicate([](const auto& I){return I.bSpawned && I.Crah.IsSet();}).Num(), RemoveCrahs.Num());
	if (bBarrierInventory) OutSummary += FString::Printf(TEXT(" | 벽/차단막 %d개 (이전 %d개 교체)"), NewBarriers.Num(), OldBarriers.Num());
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
