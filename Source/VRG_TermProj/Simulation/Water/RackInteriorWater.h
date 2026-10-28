#pragma once
#include "CoreMinimal.h"

// W3.5 슬롯 저장소. 랙×슬롯 평탄 SoA, 높이순 아래 이웃, 이전/다음 버퍼.
// 모든 물량은 이미 냉각수 장부에 기록된 m³이며 이 클래스는 열을 다시 계산하지 않는다.
class FRackInteriorWater
{
public:
	// 추정: 슬롯 보유 30 mL, 배수 시정수 0.25 s, 뒤쪽 물 중 10%가 슬롯 안에 들어온다.
	double CapacityM3 = 0.00003, DrainTimeS = 0.25, RearCaptureFraction = 0.1;
	void Initialize(const TArray<int32>& RackSlotCounts, const TArray<double>& SlotHeightM);
	void AddWater(int32 Rack, int32 Slot, double VolumeM3, bool bRear, double NowS);
	void Step(double Dt, double NowS);
	double BalanceErrorM3() const;
	TArray<int32> Start, Below;
	TArray<double> HeldM3, TransitM3, PassedM3, FirstWetS, RearExposureM3;
	TArray<double> FloorM3, LastFloorM3, RearFloorM3, LastRearFloorM3;
	double ReceivedM3 = 0.0;
private:
	TArray<double> NextHeldM3, NextTransitM3, RearTransitM3, NextRearTransitM3;
};
