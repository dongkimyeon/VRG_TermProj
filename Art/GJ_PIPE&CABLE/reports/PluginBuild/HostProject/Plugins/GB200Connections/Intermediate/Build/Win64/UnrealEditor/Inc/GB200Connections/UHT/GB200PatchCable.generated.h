// Copyright Epic Games, Inc. All Rights Reserved.
/*===========================================================================
	Generated code exported from UnrealHeaderTool.
	DO NOT modify this manually! Edit the corresponding .h files instead!
===========================================================================*/

// IWYU pragma: private, include "GB200PatchCable.h"

#ifdef GB200CONNECTIONS_GB200PatchCable_generated_h
#error "GB200PatchCable.generated.h already included, missing '#pragma once' in GB200PatchCable.h"
#endif
#define GB200CONNECTIONS_GB200PatchCable_generated_h

#include "UObject/ObjectMacros.h"
#include "UObject/ReflectedTypeAccessors.h"
#include "UObject/ScriptMacros.h"

PRAGMA_DISABLE_DEPRECATION_WARNINGS

// ********** Begin Class AGB200PatchCable *********************************************************
#define FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_RPC_WRAPPERS_NO_PURE_DECLS \
	DECLARE_FUNCTION(execGetVisualSegmentCount); \
	DECLARE_FUNCTION(execRebuildConnection);


struct Z_Construct_UClass_AGB200PatchCable_Statics;
GB200CONNECTIONS_API UClass* Z_Construct_UClass_AGB200PatchCable(ETypeConstructPhase);

#define FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_INCLASS_NO_PURE_DECLS \
private: \
	friend struct ::Z_Construct_UClass_AGB200PatchCable_Statics; \
	friend GB200CONNECTIONS_API UClass* ::Z_Construct_UClass_AGB200PatchCable(ETypeConstructPhase); \
public: \
	DECLARE_CLASS2(AGB200PatchCable, AActor, COMPILED_IN_FLAGS(0 | CLASS_Config), CASTCLASS_None, TEXT("/Script/GB200Connections"), Z_Construct_UClass_AGB200PatchCable) \
	DECLARE_SERIALIZER(AGB200PatchCable)


#define FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_ENHANCED_CONSTRUCTORS \
	/** Deleted move- and copy-constructors, should never be used */ \
	AGB200PatchCable(AGB200PatchCable&&) = delete; \
	AGB200PatchCable(const AGB200PatchCable&) = delete; \
	DECLARE_VTABLE_PTR_HELPER_CTOR(NO_API, AGB200PatchCable); \
	DEFINE_VTABLE_PTR_HELPER_CTOR_CALLER(AGB200PatchCable); \
	DEFINE_DEFAULT_CONSTRUCTOR_CALL(AGB200PatchCable) \
	NO_API virtual ~AGB200PatchCable();


#define FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_10_PROLOG
#define FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_GENERATED_BODY \
PRAGMA_DISABLE_DEPRECATION_WARNINGS \
public: \
	FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_RPC_WRAPPERS_NO_PURE_DECLS \
	FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_INCLASS_NO_PURE_DECLS \
	FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h_13_ENHANCED_CONSTRUCTORS \
private: \
PRAGMA_ENABLE_DEPRECATION_WARNINGS


class AGB200PatchCable;

// ********** End Class AGB200PatchCable ***********************************************************

#undef CURRENT_FILE_ID
#define CURRENT_FILE_ID FID_HostProject_Plugins_GB200Connections_Source_GB200Connections_Public_GB200PatchCable_h

PRAGMA_ENABLE_DEPRECATION_WARNINGS
