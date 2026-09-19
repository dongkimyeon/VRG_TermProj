// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "VRG_TermProjGameModeBase.generated.h"

// 디버그 키를 받는 플레이어 컨트롤러를 쓰도록 고정한다.
UCLASS()
class VRG_TERMPROJ_API AVRG_TermProjGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AVRG_TermProjGameModeBase();
};
