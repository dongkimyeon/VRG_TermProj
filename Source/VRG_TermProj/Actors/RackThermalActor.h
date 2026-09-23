// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/Core/SimClock.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Thermal/RadiatorModel.h"
#include "RackThermalActor.generated.h"

class UStaticMeshComponent;

// 랙(캐비닛) 1대를 스태틱 메쉬로 세우고 열모델을 돌리는 액터.
//
// 캐비닛 = 서버 유닛 N대, 유닛 1대 = CPU/GPU/RAM/VRM 부품 4개.
// 유닛은 레일에서 앞으로 뽑았다가 다시 꽂을 수 있다. 뽑으면 콜드플레이트와
// 강제 공랭이 끊기고, 빈 슬롯은 유동저항이 낮아 공기를 더 가져간다.
//
// 액터는 입력 전달과 결과 표현만 한다. 물리 계산은 전부 FRackThermalModel 안에 있다.
// 길이는 m로 들고 있다가 메쉬를 배치하는 순간에만 cm로 바꾼다 (규약 1).
UCLASS()
class VRG_TERMPROJ_API ARackThermalActor : public AActor
{
	GENERATED_BODY()

public:
	ARackThermalActor();

	virtual void Tick(float DeltaTime) override;

	// 레벨 뷰포트에서도 틱이 돌게 해서 Play 없이 배치를 확인할 수 있게 한다.
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void StartSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void PauseSim();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Rack")
	void ResetSim();

	UFUNCTION(BlueprintCallable, Category = "Rack")
	FRackThermalSnapshot GetSnapshot() const;

	// 유닛을 뽑거나 꽂는다. 물리 상태는 즉시 바뀌고(퀵디스커넥트가 바로 끊긴다),
	// 메쉬는 SledSlideSpeed로 따라간다.
	UFUNCTION(BlueprintCallable, Category = "Rack")
	void SetSledDocked(int32 SledIndex, bool bDocked);

	UFUNCTION(BlueprintCallable, Category = "Rack")
	void ToggleSledDocked(int32 SledIndex);

	UFUNCTION(BlueprintCallable, Category = "Rack")
	bool IsSledDocked(int32 SledIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Rack")
	int32 GetNumDockedSleds() const;

	// 켜면 이 액터가 스스로 적분하지 않는다. 바깥(통합 액터)이 같은 시계로 돌린다.
	// 규약 4: 도메인이 여럿이면 시계가 하나여야 한다. 액터마다 따로 돌리면
	// 같은 시간 구간을 두 번 적분하거나 서로 어긋난 시각을 보게 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	bool bDrivenExternally = false;

	// 통합 액터가 빌려 가는 내부 모델과 입력. 수명은 이 액터가 쥔다.
	FRackThermalModel& GetMutableModel() { return Model; }
	FRackThermalInput& GetMutableInput() { return Input; }
	const FRadiatorParams& GetRadiatorParams() const { return Radiator; }

	// 바깥이 적분한 뒤 시각을 맞춰 준다. 표시에만 쓴다.
	void SetExternalSimTimeS(double InSimTimeS) { ExternalSimTimeS = InSimTimeS; }

	// 입력 배열 길이를 Params에 맞춘다. 통합 액터가 붙기 전에 불러야 한다.
	UFUNCTION(BlueprintCallable, Category = "Rack")
	void ConformInputs() { ConformInputArrays(); }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalParams Params;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalInput Input;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	FRackThermalState InitialState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator")
	FRadiatorParams Radiator;

	// 켜면 랙에서 나온 냉각수를 라디에이터에 통과시켜 다시 랙 입구로 돌린다.
	// 열 도메인 안에서만 닫는 루프다. 유량은 여전히 이 액터의 입력이며
	// 수력 솔버에서 받아오지 않는다 (그 연결은 S4에서 한다).
	// 끄면 CoolantInletTempK를 고정 경계로 쓰는 S1 개루프 구성이 된다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radiator")
	bool bCloseCoolantLoopThroughRadiator = true;

	// 고정 시뮬레이션 스텝 [s]. 모든 도메인이 같은 시계를 쓴다 (규약 4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rack")
	double FixedStepS = 0.01;

	// ---- 외형 (m) ----
	// X = 깊이(유닛이 +X 방향으로 뽑힌다, 공기도 +X에서 들어온다), Y = 폭, Z = 높이.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	FVector RackSizeM = FVector(1.2, 0.6, 2.0);

	// Opt-in visual kit. Thermal parameters and the original primitive path are retained.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization|GJ")
	bool bUseGjModels = false;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Visualization|GJ")
	void RefreshVisuals();

	// Authored local metres (+X front), converted at this actor boundary, including rotation/scale.
	FVector GjPointWorldCm(const FVector& LocalM) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double PanelThicknessM = 0.03;

	// 유닛을 완전히 뽑았을 때 앞으로 나오는 거리.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double SledPullOutM = 0.9;

	// 슬라이드 속도 [1/s]. 물리와 무관한 연출 값이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	float SledSlideSpeed = 3.0f;

	// 부품 색상표의 양 끝 [°C]. 표시용이라 계산에 쓰이지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double ColorMinCelsius = 20.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	double ColorMaxCelsius = 90.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowReadout = true;

	// 유닛마다 부품 온도를 글자로 적는다. 유닛이 많으면 화면이 복잡해진다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visualization")
	bool bShowSledLabels = true;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	FSimClock Clock;
	FRackThermalModel Model;
	bool bSimRunning = false;
	double ExternalSimTimeS = 0.0;
	bool bMeshesDirty = true;

	// 캐비닛 껍데기. 유닛과 달리 움직이지 않는다.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> CabinetMeshes;

	// 유닛 섀시. 길이 NumSleds.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SledMeshes;

	// 부품 박스. 평탄 배열 [SledIndex * NumRackComponents + Component].
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> ComponentMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> SledHarnessMeshes;

	bool bUsingGjMeshes = false;
	bool BuildGjMeshes();

	// 부품이 꽂힌 상태일 때의 X [cm]. 슬라이드 오프셋을 여기에 더해 쓴다.
	TArray<double> ComponentBaseXCm;

	// 유닛이 뽑혀 나온 정도. 0 = 완전히 꽂힘, 1 = 완전히 뽑힘.
	// 물리 상태(Input.SledDocked)와 달리 연출용이라 서서히 따라간다.
	TArray<float> SledSlideAlpha;

	// Input의 TArray 길이를 Params.NumSleds에 맞춘다. 비어 있으면 기본값으로 채운다.
	void ConformInputArrays();

	void RebuildMeshes();
	void UpdateSledSlide(float DeltaTime);
	void UpdateMeshColors() const;

	// 유닛 하나가 차지하는 칸의 바닥 Z와 칸 높이 [m].
	// 섬시 트레이와 부품은 이 칸 안에 들어가야 한다 (위 칸을 침범하면 안 된다).
	void GetSledRowM(int32 SledIndex, double& OutBottomZM, double& OutRowHeightM) const;

	void DrawReadout() const;
};
