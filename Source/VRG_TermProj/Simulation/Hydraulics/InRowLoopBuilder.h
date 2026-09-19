// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "InRowLoopBuilder.generated.h"

// 인로우 냉각 루프 한 줄을 네트워크에 얹는다.
//
//   CDU -> 펌프 -> 공급 헤더(행을 따라) -> 랙 분기 -> 회수 헤더 -> CDU
//
// 같은 위상을 방 액터, 배관 디버그 액터, 테스트가 각각 따로 세우고 있었다.
// 위상은 물리이므로 한 곳에서만 만든다. 좌표는 여기 없다 - 솔버는 위상만 안다.
//
// 이 파일은 UObject를 만들지 않는다. 워커 스레드에서 불러도 된다.

USTRUCT(BlueprintType)
struct FInRowLoopSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "1", ClampMax = "64"))
	int32 NumRacks = 6;

	// 랙 한 대의 슬롯 수. 분기 저항을 병렬 합성할 때 기준이 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SledsPerRack = 12;

	// 랙별로 지금 꽂혀 있는 유닛 수. 길이가 NumRacks와 다르면 전부 만재로 본다.
	// 0이면 그 랙의 노드와 분기 배관을 아예 만들지 않는다. 큰 저항으로 흉내 내면
	// 미세 유량이 남아 거짓 결과가 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	TArray<int32> DockedSledsPerRack;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double HeaderSegmentResistancePaS2M6 = 2.7e8;

	// 슬롯이 전부 찼을 때의 분기 저항. 2차 손실 병렬 합성이라 꽂힌 수가 n이면
	// 유효 저항은 이 값 * (SledsPerRack / n)^2 이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double RackBranchResistancePaS2M6 = 1.4e11;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double ReturnToCduResistancePaS2M6 = 5.4e8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double CduReferencePressurePa = 2.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpShutoffHeadPa = 4.0e5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics")
	double PumpCurveCoeffPaS2M6 = 8.0e9;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	double PumpEfficiency = 0.7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hydraulics", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	double PumpSpeedRatio = 1.0;
};

// 얹은 뒤 돌려주는 인덱스들. 부르는 쪽이 여기에 좌표를 붙이거나 유량을 읽는다.
struct FInRowLoopHandles
{
	int32 CduNodeIndex = INDEX_NONE;
	int32 PumpIndex = INDEX_NONE;

	// 길이 NumRacks.
	TArray<int32> SupplyNodeIndices;
	TArray<int32> ReturnNodeIndices;

	// 유닛이 0대인 랙은 INDEX_NONE이다.
	TArray<int32> RackNodeIndices;

	// 공급 헤더 -> 랙. 유닛이 0대인 랙은 INDEX_NONE이다.
	TArray<int32> RackBranchPipeIndices;

	// 랙 -> 회수 헤더. 유닛이 0대인 랙은 INDEX_NONE이다.
	TArray<int32> RackReturnPipeIndices;

	// 헤더 구간. 길이는 NumRacks - 1.
	TArray<int32> SupplyHeaderPipeIndices;
	TArray<int32> ReturnHeaderPipeIndices;

	// 회수 헤더 끝 -> CDU.
	int32 ReturnToCduPipeIndex = INDEX_NONE;

	// 표시용 역할 구분. 관 색을 공급/회수로 나눌 때 쓴다.
	// 계산에는 쓰이지 않는다 - 솔버는 어느 쪽이 공급인지 모른다.
	bool IsSupplySidePipe(int32 PipeIndex) const
	{
		return SupplyHeaderPipeIndices.Contains(PipeIndex) || RackBranchPipeIndices.Contains(PipeIndex);
	}
};

namespace InRowLoopBuilder
{
	// 꽂힌 유닛 수 n에서의 분기 저항. n이 0 이하면 0을 돌려준다 (분기를 만들지 않는다).
	VRG_TERMPROJ_API double BranchResistancePaS2M6(const FInRowLoopSpec& Spec, int32 DockedSleds);

	// Spec의 DockedSledsPerRack에서 RackIndex의 값을 꺼낸다. 길이가 안 맞으면 만재다.
	VRG_TERMPROJ_API int32 DockedSledsAt(const FInRowLoopSpec& Spec, int32 RackIndex);

	// 네트워크에 루프 한 줄을 얹는다. 기존 내용은 지우지 않으므로 여러 줄을 얹을 수 있다.
	VRG_TERMPROJ_API FInRowLoopHandles AddLoop(FHydraulicNetwork& Network, const FInRowLoopSpec& Spec);
}
