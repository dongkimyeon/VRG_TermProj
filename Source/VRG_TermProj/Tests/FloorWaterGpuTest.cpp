// Fill out your copyright notice in the Description page of Project Settings.

// W3 GPU path (2026-10-27): the floor water step on the GPU against the CPU reference, with a tolerance (not bit
// equality: the GPU fuses multiply-adds and refines float sqrt / cbrt by Newton steps in double). VRG.Gpu.* needs a
// real RHI (-RenderOffscreen); under -NullRHI it notes the skip and passes.

#include "Misc/AutomationTest.h"
#include "Simulation/Water/FloorWater.h"
#include "VrgFloorWaterGpu.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr auto FloorGpuFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	bool FloorSkipWithoutGpu(FAutomationTestBase& Test)
	{
		if (FVrgFloorWaterGpu::IsAvailable()) return false;
		Test.AddInfo(TEXT("GPU 없음(NullRHI 등) - 건너뜀. GPU 테스트는 -NullRHI 없이 따로 돌린다."));
		return true;
	}

	struct FFloorCase
	{
		FString Name;
		TFunction<void(FFloorWater&, int32 Gpu)> Setup;   // builds the floor with the given GPU mode
		TFunction<void(FFloorWater&, int32 Step)> Feed;    // per-step deposits (may be empty)
		int32 Steps = 50;
		double DepthScaleM = .05, SpeedScaleMs = .7;
	};

	struct FFloorDiff { double DepthRel = 0.0, MomentumRel = 0.0, ResidualCpu = 0.0, ResidualGpu = 0.0; int32 SubCpu = 0, SubGpu = 0, GpuSteps = 0; };

	FFloorDiff FloorCompare(const FFloorCase& C)
	{
		FFloorWater Cpu, Gpu;
		C.Setup(Cpu, 0); C.Setup(Gpu, 1);
		for (int32 S = 0; S < C.Steps; ++S)
		{
			if (C.Feed) { C.Feed(Cpu, S); C.Feed(Gpu, S); }
			Cpu.Step(.02, S * .02); Gpu.Step(.02, S * .02);
		}
		FFloorDiff D;
		for (int32 I = 0; I < Cpu.GetDepthM().Num(); ++I)
		{
			D.DepthRel = FMath::Max(D.DepthRel, FMath::Abs(Cpu.GetDepthM()[I] - Gpu.GetDepthM()[I]) / C.DepthScaleM);
			D.MomentumRel = FMath::Max(D.MomentumRel, (FMath::Abs(Cpu.GetHu()[I] - Gpu.GetHu()[I]) + FMath::Abs(Cpu.GetHv()[I] - Gpu.GetHv()[I])) / (C.DepthScaleM * C.SpeedScaleMs));
		}
		D.ResidualCpu = Cpu.GetTotals().Residual; D.ResidualGpu = Gpu.GetTotals().Residual;
		D.SubCpu = Cpu.GetTotals().Substeps; D.SubGpu = Gpu.GetTotals().Substeps; D.GpuSteps = Gpu.GetTotals().GpuSteps;
		return D;
	}

	FFloorWaterSettings FloorGpuSettings(double Cell, bool bIdeal, int32 Gpu)
	{
		FFloorWaterSettings S;
		S.CellM = Cell; S.GpuMode = Gpu;
		if (bIdeal) { S.ManningN = 0.0; S.bContactLinePinning = false; }
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterGpuMatchTest, "VRG.Gpu.FloorWater.MatchesCpu", FloorGpuFlags)
bool FFloorWaterGpuMatchTest::RunTest(const FString& Parameters)
{
	if (FloorSkipWithoutGpu(*this)) return true;
	TArray<FFloorCase> Cases;
	// Ritter and Stoker dam breaks (VRG.Water.Floor.DamBreakAnalytic set-up), 1 s.
	for (const double H1 : { 0.0, .01 })
	{
		FFloorCase& C = Cases.AddDefaulted_GetRef();
		C.Name = H1 > 0.0 ? TEXT("Stoker") : TEXT("Ritter");
		C.Setup = [H1](FFloorWater& W, int32 Gpu)
		{
			W.Reset(FVector2D::ZeroVector, 400, 3, 0.0, FloorGpuSettings(.025, true, Gpu));
			for (int32 J = 0; J < 3; ++J) for (int32 I = 0; I < 400; ++I) W.SetState(I, J, (I + .5) * .025 < 5.0 ? .05 : H1);
		};
	}
	// Lake at rest over a bump and a dry island, 10 s.
	{
		FFloorCase& C = Cases.AddDefaulted_GetRef();
		C.Name = TEXT("정지 호수"); C.Steps = 500; C.DepthScaleM = .004; C.SpeedScaleMs = .2;
		C.Setup = [](FFloorWater& W, int32 Gpu)
		{
			W.Reset(FVector2D::ZeroVector, 80, 80, 0.0, FloorGpuSettings(.025, true, Gpu));
			for (int32 J = 0; J < 80; ++J) for (int32 I = 0; I < 80; ++I)
			{
				const double X = (I + .5) * .025, Y = (J + .5) * .025;
				const double Bed = .003 * FMath::Exp(-(FMath::Square(X - .6) + FMath::Square(Y - .6)) / .04) + .006 * FMath::Exp(-(FMath::Square(X - 1.4) + FMath::Square(Y - 1.3)) / .02);
				W.SetBed(I, J, Bed); W.SetState(I, J, FMath::Max(0.0, .004 - Bed));
			}
		};
	}
	// The epoxy puddle: deposits every step (the cells the GPU takes from the CPU copy), friction, pinned contact
	// line, a wall. 0.5 L over 10 s, 20 s in all.
	{
		FFloorCase& C = Cases.AddDefaulted_GetRef();
		C.Name = TEXT("에폭시 웅덩이"); C.Steps = 1000; C.DepthScaleM = .003; C.SpeedScaleMs = .2;
		C.Setup = [](FFloorWater& W, int32 Gpu)
		{
			W.Reset(FVector2D::ZeroVector, 120, 120, 0.0, FloorGpuSettings(.025, false, Gpu));
			for (int32 J = 0; J < 120; ++J) W.SetSolid(66, J, true);
		};
		C.Feed = [](FFloorWater& W, int32 Step) { if (Step < 500) W.AddWater(FVector(1.5, 1.5, 0.0), .5e-3 / 500.0); };
	}
	bool bOk = true;
	for (const FFloorCase& C : Cases)
	{
		const FFloorDiff D = FloorCompare(C);
		AddInfo(FString::Printf(TEXT("%s: 수심 최대 차 %.2e, 운동량 최대 차 %.2e (기준 수심 %.3f m · 속도 %.2f m/s) | 부피 잔차 CPU %.2e / GPU %.2e | 부분 스텝 CPU %d / GPU %d, GPU 스텝 %d"),
			*C.Name, D.DepthRel, D.MomentumRel, C.DepthScaleM, C.SpeedScaleMs, D.ResidualCpu, D.ResidualGpu, D.SubCpu, D.SubGpu, D.GpuSteps));
		bOk &= D.GpuSteps == C.Steps && D.DepthRel < 1e-9 && D.MomentumRel < 1e-9 && D.ResidualGpu < 1e-12 && D.SubCpu == D.SubGpu;
	}
	TestTrue(TEXT("GPU = CPU within 1e-9 of the depth / momentum scale, same substeps, GPU volume conserved (< 1e-12)"), bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFloorWaterGpuDeterminismTest, "VRG.Gpu.FloorWater.DeterministicAndCost", FloorGpuFlags)
bool FFloorWaterGpuDeterminismTest::RunTest(const FString& Parameters)
{
	if (FloorSkipWithoutGpu(*this)) return true;
	// Two GPU runs of a wet whole-room floor (the case the GPU is for): identical bits. And the cost per step against
	// the CPU on the same floor (measurement, D10: not a guarantee).
	auto Run = [](int32 Gpu, int32 Steps, double& OutMs, FFloorWater& W)
	{
		FFloorWaterSettings S; S.GpuMode = Gpu;
		W.Reset(FVector2D::ZeroVector, 540, 412, 0.0, S);
		for (int32 J = 0; J < 412; ++J)
			for (int32 I = 0; I < 540; ++I)
				W.SetState(I, J, .005 + .015 * FMath::Exp(-(FMath::Square((I - 270) * .025) + FMath::Square((J - 206) * .025)) / .25));
		W.Step(.02, 0.0);   // warm-up (configure, upload)
		const double T0 = FPlatformTime::Seconds();
		for (int32 Step = 1; Step <= Steps; ++Step) W.Step(.02, Step * .02);
		OutMs = (FPlatformTime::Seconds() - T0) * 1e3 / Steps;
	};
	FFloorWater A, B, C;
	double MsA = 0.0, MsB = 0.0, MsC = 0.0;
	Run(1, 50, MsA, A);
	Run(1, 50, MsB, B);
	Run(0, 50, MsC, C);
	const bool bSame = A.GetDepthM() == B.GetDepthM() && A.GetHu() == B.GetHu() && A.GetHv() == B.GetHv();
	double Diff = 0.0;
	for (int32 I = 0; I < A.GetDepthM().Num(); ++I) Diff = FMath::Max(Diff, FMath::Abs(A.GetDepthM()[I] - C.GetDepthM()[I]) / .02);
	AddInfo(FString::Printf(TEXT("방 전체 젖음 %d칸, 1 s: GPU 스텝당 %.2f ms(두 번째 %.2f), CPU %.2f ms | GPU 두 실행 비트 동일 %d | GPU-CPU 수심 최대 차 %.2e (2 cm 기준) | 부피 잔차 GPU %.2e | GPU 스텝 %d (측정값, 보장 아님)"),
		540 * 412, MsA, MsB, MsC, bSame ? 1 : 0, Diff, A.GetTotals().Residual, A.GetTotals().GpuSteps));
	TestTrue(TEXT("deterministic: two GPU runs identical"), bSame);
	TestTrue(TEXT("whole room: GPU = CPU within 1e-9 of 2 cm, volume conserved"), A.GetTotals().GpuSteps == 51 && Diff < 1e-9 && A.GetTotals().Residual < 1e-12);
	return true;
}

#endif
