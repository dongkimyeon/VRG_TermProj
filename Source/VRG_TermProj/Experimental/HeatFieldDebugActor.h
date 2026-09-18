// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HeatFieldDebugActor.generated.h"

// 이 클래스는 S0 단계의 일회성 스파이크다. S3(균일 격자 공기 해석) 착수 시
// Experimental/ 폴더째 삭제하고 여기서 검증한 인덱싱/핑퐁 버퍼 구조만
// Simulation/Air로 옮겨 다시 짠다. 속도장·압력·이류·부력이 없어 S3 솔버와
// 코드를 공유하지 않는다 (겹치는 건 격자 자료구조뿐).

UENUM(BlueprintType)
enum class EHeatFieldSliceAxis : uint8
{
	X,
	Y,
	Z
};

UCLASS()
class VRG_TERMPROJ_API AHeatFieldDebugActor : public AActor
{
	GENERATED_BODY()

public:
	AHeatFieldDebugActor();

	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "HeatField")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "HeatField")
	void PauseSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "HeatField")
	void ResetField();

	// 데모용 가짜 값. 공기의 실제 열확산계수(약 2.2e-5 m^2/s)가 아니라
	// 격자 규모(32^3, 0.1m)에서 육안으로 확산이 보이도록 과장했다.
	// S3에서 부력 기반 모델로 대체된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HeatField")
	float ThermalDiffusivityM2s = 5.0e-3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HeatField")
	float TimeStepS = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HeatField")
	FIntVector SourceCellIndex = FIntVector(16, 16, 16);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HeatField")
	float SourceHeatW = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HeatField")
	float InitialTemperatureK = 293.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	EHeatFieldSliceAxis SliceAxis = EHeatFieldSliceAxis::Z;

protected:
	virtual void BeginPlay() override;

private:
	static constexpr int32 GridSizeX = 32;
	static constexpr int32 GridSizeY = 32;
	static constexpr int32 GridSizeZ = 32;
	static constexpr float CellSizeM = 0.1f;

	// 해수면 표준 공기 물성치 (열확산계수와 달리 과장하지 않은 실제값).
	// 소스 셀의 HeatW를 온도 상승으로 환산하는 데만 쓰인다.
	static constexpr float AirDensityKgM3 = 1.2041f;
	static constexpr float AirSpecificHeatJKgK = 1005.0f;

	TArray<float> TemperatureBufferA;
	TArray<float> TemperatureBufferB;
	bool bBufferAIsCurrent = true;
	bool bSimRunning = false;
	float AccumulatedTimeS = 0.0f;

	static FORCEINLINE int32 LinearIndex(int32 i, int32 j, int32 k)
	{
		return i + j * GridSizeX + k * GridSizeX * GridSizeY;
	}

	void InitializeField();
	void StepSimulation(float DeltaTimeS);
	bool CheckStability() const;
	void DrawSlice() const;
	static FLinearColor TemperatureToColor(float NormalizedT);
};
