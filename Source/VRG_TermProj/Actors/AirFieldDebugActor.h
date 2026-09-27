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

// 온도장을 어떻게 그릴지.
UENUM(BlueprintType)
enum class EAirDrawMode : uint8
{
	// 단면 한 장만 그린다. 안쪽까지 들여다보기 좋다.
	SlicePlane,

	// 격자 전체를 그린다. 공간이 셀로 꽉 찬다.
	// 셀이 많으면 무겁고, 찬 셀까지 다 그리면 안개처럼 덮여 아무것도 안 보인다.
	// MinVisibleFraction을 올려 뜨거운 부분만 남기는 게 보통이다.
	Volume,

	// 둘 다.
	Both
};

UCLASS()
class VRG_TERMPROJ_API AAirFieldDebugActor : public AActor
{
	GENERATED_BODY()

public:
	// S5-C opt-in backend; root cells still serve coupling and volume rendering.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree")
	bool bUseOctreeSolver = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree", meta = (ClampMin = "0", ClampMax = "6"))
	int32 OctreeDepth = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree", meta = (ClampMin = "1"))
	int32 OctreeMaxLeaves = 60000;
	// Solver-local metre AABBs. Add/remove boxes then ApplyOctreeRefinement during play:
	// split/merge conserves heat and reprojects velocity without advancing/resetting SimTime.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree")
	TArray<FBox> OctreeRefinementBoxesM;
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air|Octree")
	void ApplyOctreeRefinement();
	// S5-D: refine / merge root cells automatically from temperature gradient and air speed (with hysteresis,
	// hold time and an evaluation interval against chattering). Takes effect at ResetSim.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree")
	bool bAutoAdaptOctree = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air|Octree")
	FOctreeAdaptSettings OctreeAdaptSettings;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Air|Octree")
	FString OctreeStatus;
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

	// 켜면 이 액터가 스스로 적분하지 않는다. 바깥(통합 액터)이 같은 시계로 돌린다.
	// 열원도 바깥이 채우므로 HeatSources는 무시된다 (규약 4·6).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	bool bDrivenExternally = false;

	// 통합 액터가 빌려 가는 내부 솔버. 수명은 이 액터가 쥔다.
	FAirSolver& GetMutableSolver() { return Solver; }

	void SetExternalSimTimeS(double InSimTimeS) { ExternalSimTimeS = InSimTimeS; }

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
	// 이 값이 작으면 실시간보다 느리게 흘러서 기류가 자리잡는 걸 한참 기다려야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxStepsPerFrame = 4;

	// ---- 표시 ----
	// 기본은 볼륨 렌더링이다. 이 박스 경로는 비교 기준으로 남겨 둔 것이라 기본 꺼짐이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawSlice = false;

	// 단면 한 장인지, 격자 전체인지.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	EAirDrawMode DrawMode = EAirDrawMode::SlicePlane;

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
	// 0이면 전부 그린다. 참고로 삼은 상용 도구도 찬 영역을 파랗게 깔아 두므로
	// 기본은 0이다. 뜨거운 기둥만 떼어 보고 싶을 때만 올린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double MinVisibleFraction = 0.0;

	// 찬 셀과 뜨거운 셀의 불투명도. 찬 쪽을 0으로 두면 배경이 되고,
	// 너무 올리면 슬라이스가 불투명한 벽이 되어 안쪽이 안 보인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double ColdOpacity = 0.06;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double HotOpacity = 0.85;

	// 한 프레임에 그릴 셀 수 상한. 디버그 상자 하나가 곧 드로우 호출이라
	// 격자 전체를 1:1로 그리면 D10의 30fps 최저선을 못 지킨다.
	// 넘으면 DrawStride를 자동으로 키우고 실제로 쓴 값을 화면에 적는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization", meta = (ClampMin = "100"))
	int32 MaxDrawnCells = 12000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawDomainBounds = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawHeatSources = true;

	// S7-1: 온도장을 3D 텍스처 + 레이마칭으로 그린다.
	// 디버그 솔리드 박스는 셀 하나가 드로우 호출 하나지만 이쪽은 화면 패스 하나다.
	// 둘 다 남겨 둔 이유는 같은 온도장을 두 방식으로 그려 비교하기 위해서다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume")
	bool bDrawVolume = true;

	// 정규화 온도 1에서의 흡수 계수 [1/cm]. 키우면 진해진다.
	// 도메인을 가로지르는 거리가 수백 cm라 0.001~0.02 정도가 쓸 만하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.0001", ClampMax = "0.1"))
	// 기본값은 도메인 대각선(약 1400 cm)을 가로질렀을 때
	// 찬 공기가 알파 0.3, 뜨거운 공기가 0.99쯤 되도록 역산한 값이다.
	// 0.02로 뒀더니 지수가 9.8이 되어 화면이 통째로 파란 벽이 됐다.
	double VolumeOpacityPerCm = 0.0035;

	// 표시 범위를 장의 실제 최소/최대 온도에 매 갱신 맞춘다.
	// 고정 범위를 쓰면 공기가 24~34 C인데 범위가 24~45면 정규화가 0.5를 못 넘어
	// 화면이 거의 투명해진다. "볼륨이 안 보인다"의 대부분이 이것이었다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume")
	bool bAutoColorRange = true;

	// 자동 범위를 쓸 때 상한을 (최소 + 이 비율 x 폭)으로 좁힌다.
	// 1.0이면 가장 뜨거운 셀 하나만 빨강이 되어 방 전체가 파랗게 보인다.
	// 표시용 대비 조정이며 계산에 쓰이지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.2", ClampMax = "1.0"))
	double AutoColorSpanFraction = 0.6;

	// 찬 공기도 옅게 보이게 하는 최소 밀도 비율. 0이면 뜨거운 곳만 보인다.
	// 참고로 삼은 상용 도구는 찬 영역도 파랗게 채운다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double VolumeFloorDensity = 0.07;

	// 0 = CFD 무지개 (파랑 -> 청록 -> 초록 -> 노랑 -> 빨강), 1 = 열화상 (검정 -> 보라 -> 흰색).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0", ClampMax = "1"))
	int32 VolumeColorMap = 0;

	// 광선 하나가 밟는 표본 수. 흡수를 지수로 넣으므로 이 값을 바꿔도 밝기는 같고
	// 얇은 층의 해상도만 달라진다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "8", ClampMax = "512"))
	int32 VolumeStepCount = 128;

	// 속도장을 화살표로 그린다. 셀이 많으면 화면이 복잡해진다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bDrawVelocityArrows = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSummary = true;

	// S7-2: 속도장을 따라 흐르는 무늬를 볼륨 안에 함께 그린다.
	// Niagara 입자 대신 같은 광선 안에서 처리하므로 드로우 호출이 늘지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume")
	bool bDrawFlow = true;

	// 무늬의 진하기. 0이면 안 그린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	// 0.08로 뒀더니 무늬가 온도 색을 덮어 화면이 통째로 하늘색이 됐다.
	// 주인공은 온도이고 무늬는 "여기 공기가 움직인다"만 알려주면 된다.
	double FlowStrength = 0.025;

	// 무늬가 흐르는 속도 배율. 물리가 아니라 연출값이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double FlowSpeedScale = 0.05;

	// 무늬 촘촘함 [1/cm]. 키우면 줄이 가늘어진다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|Volume", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	double FlowFrequency = 0.08;

	// 직전 프레임에 볼륨 패스가 추가한 드로우 호출 수. 0 또는 1이어야 한다.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Visualization|Volume")
	int32 VolumePassCount = 0;

	// S7-5 성능 측정용. 물리 도메인 크기를 유지한 채 셀 수만 Scale배로 바꾼다.
	// 셀 폭을 같은 비율로 줄이므로 같은 방을 더 잘게 푼다. 즉시 리셋된다.
	void RescaleGrid(double Scale);

	// S7-1 검증용. 솔버를 N스텝 앞당긴다. 실시간으로는 눈에 보이는 상태까지
	// 몇 분이 걸리기 때문이다.
	//
	// **한 프레임에 다 돌리지 않는다.** 처음에는 동기로 돌렸는데, 시작 시점에
	// 400스텝을 통째로 계산하느라 창이 100초 넘게 응답을 멈췄다. 겉보기에
	// "켜자마자 멈춘" 것처럼 보인다. 지금은 프레임마다 시간 예산만큼만 소화하고
	// 남은 수를 VrgPerf::PendingWarmupSteps()에 노출한다. 계측은 그게 0이 될 때까지
	// 수집을 시작하지 않는다.
	void QueueWarmupSteps(int32 NumSteps);

	// 공기 도메인 전체가 보이도록 시점을 옮긴다. 레벨 PlayerStart는 지오메트리 안이다.
	void FrameDomainWithView(double DistanceScale);

	// 레벨에 놓인 랙을 전부 찾아 격자를 그 범위에 맞춘다.
	// 위치와 셀 개수를 직접 맞추는 건 손이 많이 간다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void FitToRacks();

	// 랙마다 배기 열원 상자를 하나씩 놓는다.
	// S4에서 랙 발열을 직접 받게 되면 이 버튼은 필요 없어진다.
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Air")
	void PlaceHeatSourcesFromRacks();

	// 위 두 버튼이 랙 1대에 줄 발열 [W]. GB200 NVL72 컴퓨트 트레이 18장 기준 약 105 kW지만,
	// 그중 액랭으로 빠지는 몫을 뺀 공랭 부하만 공기에 들어간다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double AirHeatPerRackW = 8000.0;

	// 배기 열원을 랙 뒷면에서 얼마나 띄울지 [m].
	// 열원을 랙 몸통 안에 넣으면 안 된다. 물리적으로 더운 공기는 랙 뒤로 나오고,
	// 화면에서도 불투명한 랙 메쉬에 가려 아무것도 안 보인다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double ExhaustStandoffM = 0.35;

	// 배기 열원이 덮는 높이 범위 [m]. 랙 윗부분에서 위로 뻗게 둔다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double ExhaustBottomZM = 0.8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double ExhaustTopZM = 2.6;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FSimClock Clock;
	FAirSolver Solver;
	FAirSolverInput Input;
	double ExternalSimTimeS = 0.0;

	bool bSimRunning = false;
	bool bNeedsReset = true;

	// MaxDrawnCells 때문에 실제로 쓴 간격. 화면 요약에 적는다.
	mutable int32 EffectiveDrawStride = 1;

	// 상자 열원들을 셀별 W 배열로 펼친다. D1: 셀 하나에 겹치면 합산한다.
	void BuildCellHeat();

	void DrawSlice() const;
	void DrawSummaryText() const;

	// S7-1: 온도장을 렌더 스레드로 올린다. 규약 5대로 여기서 복사본을 만들어 넘기고,
	// 렌더 스레드는 액터를 다시 조회하지 않는다.
	void UpdateVolumeRender();

	TSharedPtr<class FVrgVolumeSceneViewExtension, ESPMode::ThreadSafe> VolumeExtension;

	// 마지막으로 올린 시점의 SimTime. 장이 안 바뀌었으면 다시 올리지 않는다.
	// 매 프레임 올리면 큰 격자에서 렌더 스레드가 업로드에 묻힌다
	// (16만 셀에서 Draw 109 ms 중 대부분이 이것이었다).
	double LastUploadedSimTimeS = -1.0;

	// 남은 예열 스텝. 프레임마다 아래 예산만큼만 소화한다.
	int32 PendingWarmupSteps = 0;
	int32 WarmupStepsReported = 0;
	void DrainWarmupSteps();
};
