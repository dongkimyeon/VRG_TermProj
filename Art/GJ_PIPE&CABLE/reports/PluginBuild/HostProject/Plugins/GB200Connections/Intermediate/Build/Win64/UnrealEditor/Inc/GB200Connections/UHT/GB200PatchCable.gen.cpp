// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

#include "UObject/GeneratedCppIncludes.h"
#include "GB200PatchCable.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS
static_assert(!UE_WITH_CONSTINIT_UOBJECT, "This generated code can only be compiled with !UE_WITH_CONSTINIT_UOBJECT");
void EmptyLinkFunctionForGeneratedCodeGB200PatchCable() {}

// ********** Begin Cross Module References ********************************************************
COREUOBJECT_API UScriptStruct* Z_Construct_UScriptStruct_FVector(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_AActor(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_AActor(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_UMaterialInterface(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_USplineComponent(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_USplineMeshComponent(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_UStaticMesh(ETypeConstructPhase);
ENGINE_API UClass* Z_Construct_UClass_UStaticMeshComponent(ETypeConstructPhase);
// ********** End Cross Module References **********************************************************

// ********** Begin Same Module References *********************************************************
UPackage* Z_Construct_UPackage__Script_GB200Connections(ETypeConstructPhase);
GB200CONNECTIONS_API UClass* Z_Construct_UClass_AGB200PatchCable(ETypeConstructPhase);
GB200CONNECTIONS_API UClass* Z_Construct_UClass_AGB200PatchCable(ETypeConstructPhase);
// ********** End Same Module References ***********************************************************
#define UHT_STRUCT_BASE(INIT) UE::CodeGen::ConstInit::TCompiledInObjectPtr<const FStructBaseChain>(UE::Private::AsStructBaseChain(INIT))

// ********** Begin Class AGB200PatchCable Function GetVisualSegmentCount **************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_Construct_UFunction_AGB200PatchCable_GetVisualSegmentCount_Statics
struct UHT_STATICS
{
	struct GB200PatchCable_eventGetVisualSegmentCount_Parms
	{
		int32 ReturnValue;
	};
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Type_MetaData[] = {
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
#endif // WITH_METADATA

// ********** Begin Function GetVisualSegmentCount constinit property declarations *****************
	static const UECodeGen_Private::FIntPropertyParams NewProp_ReturnValue;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
// ********** End Function GetVisualSegmentCount constinit property declarations *******************
	static const UECodeGen_Private::FFunctionParams FuncParams;
};

// ********** Begin Function GetVisualSegmentCount Property Definitions ****************************
const UECodeGen_Private::FIntPropertyParams UHT_STATICS::NewProp_ReturnValue = { "ReturnValue", nullptr, (EPropertyFlags)0x0010000000000580, UECodeGen_Private::EPropertyGenFlags::Int, nullptr, nullptr, 1, STRUCT_OFFSET(GB200PatchCable_eventGetVisualSegmentCount_Parms, ReturnValue), METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FPropertyParamsBase* const UHT_STATICS::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_ReturnValue,
};
static_assert(UE_ARRAY_COUNT(UHT_STATICS::PropPointers) < 2048);
// ********** End Function GetVisualSegmentCount Property Definitions ******************************
const UECodeGen_Private::FFunctionParams UHT_STATICS::FuncParams = { { (FTypeConstructFunc*)Z_Construct_UClass_AGB200PatchCable, nullptr, "GetVisualSegmentCount", UHT_STATICS::PropPointers, UE_ARRAY_COUNT(UHT_STATICS::PropPointers), DataSizeOf<UHT_STATICS::GB200PatchCable_eventGetVisualSegmentCount_Parms>(), RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x54020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(UHT_STATICS::Type_MetaData), UHT_STATICS::Type_MetaData)},  };
static_assert(sizeof(UHT_STATICS::GB200PatchCable_eventGetVisualSegmentCount_Parms) < MAX_uint16);
UFunction* Z_Construct_UFunction_AGB200PatchCable_GetVisualSegmentCount(ETypeConstructPhase Phase)
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, UHT_STATICS::FuncParams);
	}
	return ReturnFunction;
}
#undef UHT_STATICS
DEFINE_FUNCTION(AGB200PatchCable::execGetVisualSegmentCount)
{
	P_FINISH;
	P_NATIVE_BEGIN;
	*(int32*)Z_Param__Result=P_THIS->GetVisualSegmentCount();
	P_NATIVE_END;
}
// ********** End Class AGB200PatchCable Function GetVisualSegmentCount ****************************

// ********** Begin Class AGB200PatchCable Function RebuildConnection ******************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_Construct_UFunction_AGB200PatchCable_RebuildConnection_Statics
struct UHT_STATICS
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Type_MetaData[] = {
		{ "CallInEditor", "true" },
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
#endif // WITH_METADATA

// ********** Begin Function RebuildConnection constinit property declarations *********************
// ********** End Function RebuildConnection constinit property declarations ***********************
	static const UECodeGen_Private::FFunctionParams FuncParams;
};
const UECodeGen_Private::FFunctionParams UHT_STATICS::FuncParams = { { (FTypeConstructFunc*)Z_Construct_UClass_AGB200PatchCable, nullptr, "RebuildConnection", nullptr, 0, 0, RF_Public|RF_Transient|RF_MarkAsNative, (EFunctionFlags)0x04020401, 0, 0, METADATA_PARAMS(UE_ARRAY_COUNT(UHT_STATICS::Type_MetaData), UHT_STATICS::Type_MetaData)},  };
UFunction* Z_Construct_UFunction_AGB200PatchCable_RebuildConnection(ETypeConstructPhase Phase)
{
	static UFunction* ReturnFunction = nullptr;
	if (!ReturnFunction)
	{
		UECodeGen_Private::ConstructUFunction(&ReturnFunction, UHT_STATICS::FuncParams);
	}
	return ReturnFunction;
}
#undef UHT_STATICS
DEFINE_FUNCTION(AGB200PatchCable::execRebuildConnection)
{
	P_FINISH;
	P_NATIVE_BEGIN;
	P_THIS->RebuildConnection();
	P_NATIVE_END;
}
// ********** End Class AGB200PatchCable Function RebuildConnection ********************************

// ********** Begin Class AGB200PatchCable *********************************************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_Construct_UClass_AGB200PatchCable_Statics
struct UHT_STATICS
{
#if WITH_METADATA
	static constexpr UECodeGen_Private::FMetaDataPairParam Type_MetaData[] = {
		{ "BlueprintType", "true" },
		{ "Comment", "// Visual authoring only. Coordinates exposed in cm at the Unreal boundary.\n" },
		{ "IncludePath", "GB200PatchCable.h" },
		{ "IsBlueprintBase", "true" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
		{ "ToolTip", "Visual authoring only. Coordinates exposed in cm at the Unreal boundary." },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Path_MetaData[] = {
		{ "Category", "Connection" },
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_BodyMesh_MetaData[] = {
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_BodyMaterial_MetaData[] = {
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartPlugMesh_MetaData[] = {
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndPlugMesh_MetaData[] = {
		{ "Category", "Connection" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartAnchor_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndAnchor_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartPortId_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndPortId_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartOffsetCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndOffsetCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartOutward_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndOutward_MetaData[] = {
		{ "Category", "Patch" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartPlugLengthCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ClampMin", "0" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndPlugLengthCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ClampMin", "0" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_bAutoPath_MetaData[] = {
		{ "Category", "Patch" },
		{ "Comment", "// Auto mode follows explicit anchor pairs; it does not infer network topology or avoid obstacles.\n" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
		{ "ToolTip", "Auto mode follows explicit anchor pairs; it does not infer network topology or avoid obstacles." },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_SagCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ClampMin", "0" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_LeadCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ClampMin", "1" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_MaxSegmentCm_MetaData[] = {
		{ "Category", "Patch" },
		{ "ClampMin", "1" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_Segments_MetaData[] = {
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_StartPlug_MetaData[] = {
		{ "Category", "Connection" },
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
	static constexpr UECodeGen_Private::FMetaDataPairParam NewProp_EndPlug_MetaData[] = {
		{ "Category", "Connection" },
		{ "EditInline", "true" },
		{ "ModuleRelativePath", "Public/GB200PatchCable.h" },
	};
#endif // WITH_METADATA

// ********** Begin Class AGB200PatchCable constinit property declarations *************************
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Path;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_BodyMesh;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_BodyMaterial;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_StartPlugMesh;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_EndPlugMesh;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_StartAnchor;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_EndAnchor;
	static const UECodeGen_Private::FStrPropertyParams NewProp_StartPortId;
	static const UECodeGen_Private::FStrPropertyParams NewProp_EndPortId;
	static const UECodeGen_Private::FStructPropertyParams NewProp_StartOffsetCm;
	static const UECodeGen_Private::FStructPropertyParams NewProp_EndOffsetCm;
	static const UECodeGen_Private::FStructPropertyParams NewProp_StartOutward;
	static const UECodeGen_Private::FStructPropertyParams NewProp_EndOutward;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_StartPlugLengthCm;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_EndPlugLengthCm;
	static void NewProp_bAutoPath_SetBit(void* Obj)
	{
		((AGB200PatchCable*)Obj)->bAutoPath = 1;
	}
	static const UECodeGen_Private::FBoolPropertyParams NewProp_bAutoPath;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_SagCm;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_LeadCm;
	static const UECodeGen_Private::FFloatPropertyParams NewProp_MaxSegmentCm;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_Segments_Inner;
	static const UECodeGen_Private::FArrayPropertyParams NewProp_Segments;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_StartPlug;
	static const UECodeGen_Private::FObjectPropertyParams NewProp_EndPlug;
	static const UECodeGen_Private::FPropertyParamsBase* const PropPointers[];
// ********** End Class AGB200PatchCable constinit property declarations ***************************
	static constexpr UE::CodeGen::FClassNativeFunction Funcs[] = {
		{ .NameUTF8 = UTF8TEXT("GetVisualSegmentCount"), .Pointer = &AGB200PatchCable::execGetVisualSegmentCount },
		{ .NameUTF8 = UTF8TEXT("RebuildConnection"), .Pointer = &AGB200PatchCable::execRebuildConnection },
	};
	static FTypeConstructFunc* DependentSingletons[];
	static constexpr FClassFunctionLinkInfo FuncInfo[] = {
		{ &Z_Construct_UFunction_AGB200PatchCable_GetVisualSegmentCount, "GetVisualSegmentCount" }, // 219953d2085e478e5351d42e1810c56284e6c573
		{ &Z_Construct_UFunction_AGB200PatchCable_RebuildConnection, "RebuildConnection" }, // 406149ca9337aff3ac06ba339dd3f9f59ea830f6
	};
	static_assert(UE_ARRAY_COUNT(FuncInfo) < 2048);
	static constexpr FCppClassTypeInfoStatic StaticCppClassTypeInfo = {
		TCppClassTypeTraits<AGB200PatchCable>::IsAbstract,
	};
	static const UECodeGen_Private::FClassParams ClassParams;
}; // struct UHT_STATICS

// ********** Begin Class AGB200PatchCable Property Definitions ************************************
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_Path = { "Path", nullptr, (EPropertyFlags)0x01140000000a001d, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, Path), Z_Construct_UClass_USplineComponent, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Path_MetaData), NewProp_Path_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_BodyMesh = { "BodyMesh", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, BodyMesh), Z_Construct_UClass_UStaticMesh, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_BodyMesh_MetaData), NewProp_BodyMesh_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_BodyMaterial = { "BodyMaterial", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, BodyMaterial), Z_Construct_UClass_UMaterialInterface, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_BodyMaterial_MetaData), NewProp_BodyMaterial_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_StartPlugMesh = { "StartPlugMesh", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartPlugMesh), Z_Construct_UClass_UStaticMesh, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartPlugMesh_MetaData), NewProp_StartPlugMesh_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_EndPlugMesh = { "EndPlugMesh", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndPlugMesh), Z_Construct_UClass_UStaticMesh, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndPlugMesh_MetaData), NewProp_EndPlugMesh_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_StartAnchor = { "StartAnchor", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartAnchor), Z_Construct_UClass_AActor, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartAnchor_MetaData), NewProp_StartAnchor_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_EndAnchor = { "EndAnchor", nullptr, (EPropertyFlags)0x0114000000000005, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndAnchor), Z_Construct_UClass_AActor, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndAnchor_MetaData), NewProp_EndAnchor_MetaData) };
const UECodeGen_Private::FStrPropertyParams UHT_STATICS::NewProp_StartPortId = { "StartPortId", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Str, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartPortId), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartPortId_MetaData), NewProp_StartPortId_MetaData) };
const UECodeGen_Private::FStrPropertyParams UHT_STATICS::NewProp_EndPortId = { "EndPortId", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Str, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndPortId), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndPortId_MetaData), NewProp_EndPortId_MetaData) };
const UECodeGen_Private::FStructPropertyParams UHT_STATICS::NewProp_StartOffsetCm = { "StartOffsetCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartOffsetCm), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartOffsetCm_MetaData), NewProp_StartOffsetCm_MetaData) };
const UECodeGen_Private::FStructPropertyParams UHT_STATICS::NewProp_EndOffsetCm = { "EndOffsetCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndOffsetCm), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndOffsetCm_MetaData), NewProp_EndOffsetCm_MetaData) };
const UECodeGen_Private::FStructPropertyParams UHT_STATICS::NewProp_StartOutward = { "StartOutward", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartOutward), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartOutward_MetaData), NewProp_StartOutward_MetaData) };
const UECodeGen_Private::FStructPropertyParams UHT_STATICS::NewProp_EndOutward = { "EndOutward", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Struct, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndOutward), Z_Construct_UScriptStruct_FVector, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndOutward_MetaData), NewProp_EndOutward_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_StartPlugLengthCm = { "StartPlugLengthCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartPlugLengthCm), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartPlugLengthCm_MetaData), NewProp_StartPlugLengthCm_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_EndPlugLengthCm = { "EndPlugLengthCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndPlugLengthCm), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndPlugLengthCm_MetaData), NewProp_EndPlugLengthCm_MetaData) };
const UECodeGen_Private::FBoolPropertyParams UHT_STATICS::NewProp_bAutoPath = { "bAutoPath", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Bool | UECodeGen_Private::EPropertyGenFlags::NativeBool, nullptr, nullptr, 1, sizeof(bool), sizeof(AGB200PatchCable), &UHT_STATICS::NewProp_bAutoPath_SetBit, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_bAutoPath_MetaData), NewProp_bAutoPath_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_SagCm = { "SagCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, SagCm), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_SagCm_MetaData), NewProp_SagCm_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_LeadCm = { "LeadCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, LeadCm), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_LeadCm_MetaData), NewProp_LeadCm_MetaData) };
const UECodeGen_Private::FFloatPropertyParams UHT_STATICS::NewProp_MaxSegmentCm = { "MaxSegmentCm", nullptr, (EPropertyFlags)0x0010000000000005, UECodeGen_Private::EPropertyGenFlags::Float, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, MaxSegmentCm), METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_MaxSegmentCm_MetaData), NewProp_MaxSegmentCm_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_Segments_Inner = { "Segments", nullptr, (EPropertyFlags)0x0104000000080008, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, 0, Z_Construct_UClass_USplineMeshComponent, METADATA_PARAMS(0, nullptr) };
const UECodeGen_Private::FArrayPropertyParams UHT_STATICS::NewProp_Segments = { "Segments", nullptr, (EPropertyFlags)0x0144008000002008, UECodeGen_Private::EPropertyGenFlags::Array, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, Segments), EArrayPropertyFlags::None, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_Segments_MetaData), NewProp_Segments_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_StartPlug = { "StartPlug", nullptr, (EPropertyFlags)0x01440000000a0009, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, StartPlug), Z_Construct_UClass_UStaticMeshComponent, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_StartPlug_MetaData), NewProp_StartPlug_MetaData) };
const UECodeGen_Private::FObjectPropertyParams UHT_STATICS::NewProp_EndPlug = { "EndPlug", nullptr, (EPropertyFlags)0x01440000000a0009, UECodeGen_Private::EPropertyGenFlags::Object | UECodeGen_Private::EPropertyGenFlags::ObjectPtr, nullptr, nullptr, 1, STRUCT_OFFSET(AGB200PatchCable, EndPlug), Z_Construct_UClass_UStaticMeshComponent, METADATA_PARAMS(UE_ARRAY_COUNT(NewProp_EndPlug_MetaData), NewProp_EndPlug_MetaData) };
const UECodeGen_Private::FPropertyParamsBase* const UHT_STATICS::PropPointers[] = {
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_Path,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_BodyMesh,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_BodyMaterial,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartPlugMesh,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndPlugMesh,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartAnchor,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndAnchor,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartPortId,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndPortId,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartOffsetCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndOffsetCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartOutward,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndOutward,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartPlugLengthCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndPlugLengthCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_bAutoPath,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_SagCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_LeadCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_MaxSegmentCm,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_Segments_Inner,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_Segments,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_StartPlug,
	(const UECodeGen_Private::FPropertyParamsBase*)&UHT_STATICS::NewProp_EndPlug,
};
static_assert(UE_ARRAY_COUNT(UHT_STATICS::PropPointers) < 2048);
// ********** End Class AGB200PatchCable Property Definitions **************************************
FTypeConstructFunc* UHT_STATICS::DependentSingletons[] = {
	(FTypeConstructFunc*)Z_Construct_UClass_AActor,
	(FTypeConstructFunc*)Z_Construct_UPackage__Script_GB200Connections,
};
static_assert(UE_ARRAY_COUNT(UHT_STATICS::DependentSingletons) < 16);
const UECodeGen_Private::FClassParams UHT_STATICS::ClassParams = {
	&Z_Construct_UClass_AGB200PatchCable,
	"Engine",
	&StaticCppClassTypeInfo,
	DependentSingletons,
	FuncInfo,
	UHT_STATICS::PropPointers,
	nullptr,
	UE_ARRAY_COUNT(DependentSingletons),
	UE_ARRAY_COUNT(FuncInfo),
	UE_ARRAY_COUNT(UHT_STATICS::PropPointers),
	0,
	0x009000A4u,
	METADATA_PARAMS(UE_ARRAY_COUNT(UHT_STATICS::Type_MetaData), UHT_STATICS::Type_MetaData)
};
static void AGB200PatchCable_StaticRegisterNativesAGB200PatchCable()
{
	UClass* Class = AGB200PatchCable::StaticClass();
	FNativeFunctionRegistrar::RegisterFunctions(Class, 		MakeConstArrayView(UHT_STATICS::Funcs));
}
FClassRegistrationInfo Z_Registration_Info_UClass_AGB200PatchCable;
UClass* Z_Construct_UClass_AGB200PatchCable(ETypeConstructPhase Phase)
{
	if (Phase == ETypeConstructPhase::Inner)
	{
		using TClass = AGB200PatchCable;
		if (!Z_Registration_Info_UClass_AGB200PatchCable.InnerSingleton)
		{
			GetPrivateStaticClassBody(
				TClass::StaticPackage(),
				TEXT("GB200PatchCable"),
				Z_Registration_Info_UClass_AGB200PatchCable.InnerSingleton,
				AGB200PatchCable_StaticRegisterNativesAGB200PatchCable,
				DataSizeOf<TClass>(),
				alignof(TClass),
				TClass::StaticClassFlags,
				TClass::StaticClassCastFlags(),
				TClass::StaticConfigName(),
				(UClass::ClassConstructorType)InternalConstructor<TClass>,
				(UClass::ClassVTableHelperCtorCallerType)InternalVTableHelperCtorCaller<TClass>,
				UOBJECT_CPPCLASS_STATICFUNCTIONS_FORCLASS(TClass),
				&TClass::Super::StaticClass,
				&TClass::WithinClass::StaticClass
			);
		}
		return Z_Registration_Info_UClass_AGB200PatchCable.InnerSingleton;
	}
	if (!Z_Registration_Info_UClass_AGB200PatchCable.OuterSingleton)
	{
		UECodeGen_Private::ConstructUClass(Z_Registration_Info_UClass_AGB200PatchCable.OuterSingleton, UHT_STATICS::ClassParams);
	}
	return Z_Registration_Info_UClass_AGB200PatchCable.OuterSingleton;
}
#undef UHT_STATICS
DEFINE_VTABLE_PTR_HELPER_CTOR_NS(, AGB200PatchCable);
AGB200PatchCable::~AGB200PatchCable() {}
// ********** End Class AGB200PatchCable ***********************************************************

// ********** Begin Registration *******************************************************************
#ifdef UHT_STATICS
#error UHT_STATICS already defined
#endif
#define UHT_STATICS Z_CompiledInDeferFile_FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h__Script_GB200Connections_Statics
struct UHT_STATICS
{
	static constexpr FClassRegisterCompiledInInfo ClassInfo[] = {
		{ Z_Construct_UClass_AGB200PatchCable, TEXT("AGB200PatchCable"), &Z_Registration_Info_UClass_AGB200PatchCable, CONSTRUCT_RELOAD_VERSION_INFO(FClassReloadVersionInfo, sizeof(AGB200PatchCable), 1111217307U) },
	};
}; // UHT_STATICS 
static FRegisterCompiledInInfo Z_CompiledInDeferFile_FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h__Script_GB200Connections_24c34eb81226ce26d822b158af860d88c35c36fb{
	TEXT("/Script/GB200Connections"),
	UHT_STATICS::ClassInfo, UE_ARRAY_COUNT(UHT_STATICS::ClassInfo),
	nullptr, 0,
	nullptr, 0,
	nullptr, 0,
};
#undef UHT_STATICS
// ********** End Registration *********************************************************************
#undef UHT_STRUCT_BASE

PRAGMA_ENABLE_DEPRECATION_WARNINGS
