// Fill out your copyright notice in the Description page of Project Settings.


#include "VRG_TermProjGameModeBase.h"
#include "Interaction/DataCenterDebugController.h"

AVRG_TermProjGameModeBase::AVRG_TermProjGameModeBase()
{
	PlayerControllerClass = ADataCenterDebugController::StaticClass();
}
