// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UniformAirGrid.generated.h"

// S3: 균일 격자 공기 해석의 자료구조.
//
// --- MAC 엇갈림 격자 ---
// 스칼라(압력, 온도)는 셀 중심에, 속도 성분은 각 면 중심에 둔다.
// 엇갈려 두면 압력 기울기와 발산이 한 칸 차분으로 떨어져 체커보드가 생기지 않는다.
//
//        v(i,j+1,k)
//            ^
//   u(i,j,k) |  [p,T](i,j,k)  u(i+1,j,k)
//     ------>+------------------>
//            |
//        v(i,j,k)
//
// --- 규약 7 ---
// 전부 성분별 평탄 배열(SoA) + 선형 인덱스다. 포인터 그래프를 만들지 않는다.
// 읽기/쓰기 버퍼를 분리한 핑퐁 방식이며 in-place 갱신을 하지 않는다.
//
// --- 규약 1·2 ---
// 길이 m, 속도 m/s, 압력 Pa, 온도 K. 셀 크기도 m다.
// UE 좌표(cm) 변환은 액터 경계에서만 한다.
//
// 이 클래스는 UObject가 아니다. 액터를 조회하지 않으므로 워커 스레드에서 돌릴 수 있다.

USTRUCT(BlueprintType)
struct FAirGridParams
{
	GENERATED_BODY()

	// 셀 개수. 셀 중심 값의 개수이기도 하다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1", ClampMax = "256"))
	int32 NumCellsX = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1", ClampMax = "256"))
	int32 NumCellsY = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "1", ClampMax = "256"))
	int32 NumCellsZ = 32;

	// 셀 한 변의 길이 [m]. 균일 격자라 세 축이 같다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air", meta = (ClampMin = "0.001"))
	double CellSizeM = 0.25;

	// 공기 밀도 [kg/m^3]. Boussinesq 근사라 기준값 하나만 쓴다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double DensityKgM3 = 1.184;

	// 공기 정압비열 [J/(kg*K)].
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double SpecificHeatJKgK = 1005.0;

	// 부력 기준 온도 [K]. 이 온도보다 뜨거우면 뜬다.
	// 절대온도로 들고 있어야 한다 (규약 2). 이상기체 근사에서 팽창계수가 1/T_ref다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	double ReferenceTempK = 297.15;
};

// 균일 MAC 격자의 인덱싱과 장(field) 저장.
class VRG_TERMPROJ_API FUniformAirGrid
{
public:
	void Initialize(const FAirGridParams& InParams);

	const FAirGridParams& GetParams() const { return Params; }

	int32 GetNumCellsX() const { return Params.NumCellsX; }
	int32 GetNumCellsY() const { return Params.NumCellsY; }
	int32 GetNumCellsZ() const { return Params.NumCellsZ; }
	double GetCellSizeM() const { return Params.CellSizeM; }

	// 셀 하나의 부피 [m^3]. 열원을 체적 열원으로 넣을 때 쓴다 (D1).
	double GetCellVolumeM3() const
	{
		return Params.CellSizeM * Params.CellSizeM * Params.CellSizeM;
	}

	// 도메인 크기 [m].
	FVector GetDomainSizeM() const
	{
		return FVector(Params.NumCellsX, Params.NumCellsY, Params.NumCellsZ) * Params.CellSizeM;
	}

	int32 GetNumCells() const { return Params.NumCellsX * Params.NumCellsY * Params.NumCellsZ; }
	int32 GetNumFacesX() const { return (Params.NumCellsX + 1) * Params.NumCellsY * Params.NumCellsZ; }
	int32 GetNumFacesY() const { return Params.NumCellsX * (Params.NumCellsY + 1) * Params.NumCellsZ; }
	int32 GetNumFacesZ() const { return Params.NumCellsX * Params.NumCellsY * (Params.NumCellsZ + 1); }

	// ---- 선형 인덱스 ----
	// 곱셈 순서를 여기서만 정한다. 밖에서 직접 계산하지 않는다 (규약 7).
	FORCEINLINE int32 CellIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Params.NumCellsX * (Y + Params.NumCellsY * Z);
	}

	FORCEINLINE int32 FaceXIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + (Params.NumCellsX + 1) * (Y + Params.NumCellsY * Z);
	}

	FORCEINLINE int32 FaceYIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Params.NumCellsX * (Y + (Params.NumCellsY + 1) * Z);
	}

	FORCEINLINE int32 FaceZIndex(int32 X, int32 Y, int32 Z) const
	{
		return X + Params.NumCellsX * (Y + Params.NumCellsY * Z);
	}

	FORCEINLINE bool IsValidCell(int32 X, int32 Y, int32 Z) const
	{
		return X >= 0 && X < Params.NumCellsX
			&& Y >= 0 && Y < Params.NumCellsY
			&& Z >= 0 && Z < Params.NumCellsZ;
	}

	// 셀 중심의 좌표 [m]. 도메인 원점은 (0,0,0)이다.
	FORCEINLINE FVector CellCenterM(int32 X, int32 Y, int32 Z) const
	{
		return (FVector(X, Y, Z) + FVector(0.5)) * Params.CellSizeM;
	}

	// ---- 장(field) ----
	// 속도는 면에, 온도와 압력은 셀 중심에 있다.
	TArray<double> VelocityXMs;
	TArray<double> VelocityYMs;
	TArray<double> VelocityZMs;
	TArray<double> TemperatureK;
	TArray<double> PressurePa;

	// 핑퐁용 쓰기 버퍼. 한 패스가 끝나면 Swap으로 바꾼다. in-place 갱신을 하지 않는다.
	TArray<double> NextVelocityXMs;
	TArray<double> NextVelocityYMs;
	TArray<double> NextVelocityZMs;
	TArray<double> NextTemperatureK;

	void SwapVelocityBuffers();
	void SwapTemperatureBuffers();

	// 모든 셀 온도를 같은 값으로 채우고 속도를 0으로 둔다.
	void ResetToUniform(double InTemperatureK);

	// ---- 보간 ----
	// 임의 위치의 속도 [m/s]. 반라그랑주 이류에서 쓴다.
	// 도메인 밖은 경계로 잘라 낸다 (벽을 통과하지 않는다).
	FVector SampleVelocityMs(const FVector& PositionM) const;

	// 임의 위치의 셀 중심 스칼라. 삼선형 보간이다.
	double SampleCellScalar(const TArray<double>& CellField, const FVector& PositionM) const;

	// ---- 검사용 ----
	// 셀의 속도 발산 [1/s]. 비압축 조건에서 0에 가까워야 한다.
	double CellDivergencePerS(int32 X, int32 Y, int32 Z) const;

	// 모든 셀 중 가장 큰 발산 크기 [1/s].
	double MaxAbsDivergencePerS() const;

	// 셀 중심 속도 크기의 최댓값 [m/s]. 발산을 정규화할 기준으로 쓴다.
	double MaxAbsVelocityMs() const;

	// 공기가 담고 있는 총 열에너지 [J]. 기준 온도에서 잰다.
	double TotalThermalEnergyJ(double DatumTempK) const;

private:
	FAirGridParams Params;

	// 면 속도를 한 축에 대해 선형 보간한다. 도메인 밖은 경계값을 쓴다.
	double SampleFaceComponent(const TArray<double>& FaceField, const FVector& PositionM,
		int32 Axis) const;
};
