// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Core/SimClock.h"
#include "AirFieldDebugActor.generated.h"

// S3 공기 해석을 레벨에서 눈으로 보기 위한 액터.
//
// --- 열원은 아직 랙에서 읽지 않는다 ---
// 여기서는 상자 모양 열원을 직접 적어 넣는다. 랙 열모델의 발열을 공기 격자로
// 넘기는 것은 도메인 연결이라 S4의 일이다. 지금 이어 붙이면 단계를 건너뛰게 된다.
// D1대로, S4에서 넘길 때도 랙의 부품별 발열을 **합산해 하나의 체적 열원**으로 준다.
//
// --- 연속장은 메쉬로 그리지 않는다 ---
// 장비(랙·배관)는 스태틱 메쉬지만 공기 온도장은 디버그 솔리드 박스로 그린다.
// 32x32 슬라이스를 메쉬 컴포넌트로 만들면 1000개가 넘어 D10의 30fps 최저선을
// 지킬 수 없다. 볼륨 텍스처로 바꾸는 것은 S7(시각화·성능)에서 한다.
//
// 길이는 m로 들고 있다가 그리는 순간에만 cm로 바꾼다 (규약 1).
// 온도는 K로 들고 있다가 표시할 때만 °C로 바꾼다 (규약 2).

// 상자 모양 체적 열원. 상자에 걸리는 셀들에 발열을 고르게 나눈다.
USTRUCT(BlueprintType)
struct FAirHeatSourceBox
{
	GENERATED_BODY()

	// 액터 기준 상자 중심 [m].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	FVector CenterM = FVector(2.0, 2.0, 0.6);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	FVector SizeM = FVector(0.6, 1.2, 1.2);

	// 이 상자 전체가 내는 열 [W]. 걸리는 셀 수로 나눠 고르게 넣는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double HeatW = 3000.0;
};

UENUM(BlueprintType)
enum class EAirSliceAxis : uint8
{
	X,
	Y,
	Z
};

UCLASS()
class VRG_TERMPROJ_API AAirFieldDebugActor : public AActor
{
	GENERATED_BODY()

public:
	AAirFieldDebugActor();

	virtual void Tick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void PauseSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void ResetSim();

	UFUNCTION(BlueprintCallable, Category = "Air")
	FAirSolverSnapshot GetSnapshot() const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	FAirGridParams GridParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	FPoissonSolveSettings PressureSettings;

	// 상자 모양 열원들. S4에서는 랙이 이 자리를 채운다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	TArray<FAirHeatSourceBox> HeatSources;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double InitialTempK = 297.15;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double GravityZMs2 = -9.81;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	bool bEnableBuoyancy = true;

	// 고정 시뮬레이션 스텝 [s]. 모든 도메인이 같은 시계를 쓴다 (규약 4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double FixedStepS = 0.02;

	// 한 프레임에 돌릴 스텝 수 상한. 공기 해석은 압력 풀이 때문에 무겁다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxStepsPerFrame = 2;

	// ---- 표시 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawSlice = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	EAirSliceAxis SliceAxis = EAirSliceAxis::Y;

	// 슬라이스 위치. 0~1이 도메인 양 끝이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double SliceFraction = 0.5;

	// 셀을 몇 개 건너뛰며 그릴지. 1이면 전부 그린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "1", ClampMax = "8"))
	int32 DrawStride = 1;

	// 온도 색상표의 양 끝 [°C]. 표시용이며 계산에 쓰이지 않는다.
	// 아래끝을 초기 공기 온도에 맞춰야 상온 셀이 투명해진다. 어긋나면 화면이
	// 파란 벽으로 덮인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double ColorMinCelsius = 24.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double ColorMaxCelsius = 45.0;

	// 이 값보다 덜 뜨거운 셀은 아예 그리지 않는다 (0~1의 정규화 온도).
	// 상온 셀까지 다 그리면 슬라이스가 불투명한 벽이 되어 안쪽이 안 보인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double MinVisibleFraction = 0.04;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawDomainBounds = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawHeatSources = true;

	// 속도장을 화살표로 그린다. 셀이 많으면 화면이 복잡해진다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawVelocityArrows = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

	// 레벨의 ADataCenterRoomActor를 찾아 격자를 그 방에 맞춘다.
	// 위치와 셀 개수를 직접 맞추는 건 손이 많이 간다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void FitToRoom();

	// 열원 상자를 방의 랙 자리마다 하나씩 놓는다.
	// S4에서 랙 발열을 직접 받게 되면 이 버튼은 필요 없어진다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void PlaceHeatSourcesFromRoom();

	// 위 두 버튼이 랙 1대에 줄 발열 [W]. GB200 NVL72 컴퓨트 트레이 18장 기준 약 105 kW지만,
	// 그중 액랭으로 빠지는 몫을 뺀 공랭 부하만 공기에 들어간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double AirHeatPerRackW = 8000.0;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FSimClock Clock;
	FAirSolver Solver;
	FAirSolverInput Input;

	bool bSimRunning = false;
	bool bNeedsReset = true;

	// 상자 열원들을 셀별 W 배열로 펼친다. D1: 셀 하나에 겹치면 합산한다.
	void BuildCellHeat();

	void DrawSlice() const;
	void DrawSummaryText() const;
};
