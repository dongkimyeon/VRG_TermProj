// Fill out your copyright notice in the Description page of Project Settings.

#include "VrgPressureGpuSolver.h"

#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 VrgJacobiThreadsPerGroup = 64;

	class FVrgPressureJacobiCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVrgPressureJacobiCS);
		SHADER_USE_PARAMETER_STRUCT(FVrgPressureJacobiCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, RhsBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FluidBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, XIn)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, XOut)
			SHADER_PARAMETER(FIntVector, GridSize)
			SHADER_PARAMETER(int32, NumCells)
			SHADER_PARAMETER(float, CellSizeSquared)
			SHADER_PARAMETER(float, Omega)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
			FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("THREADS_PER_GROUP"), VrgJacobiThreadsPerGroup);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FVrgPressureJacobiCS, "/Vrg/Private/VrgPressureJacobi.usf", "MainJacobiCS", SF_Compute);
}

bool VrgPressureGpu::IsAvailable()
{
	return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5 && !IsRunningCommandlet() && GIsRHIInitialized;
}

bool VrgPressureGpu::RunJacobiSweeps(const FVrgPressureGpuRequest& Request,
	const TArray<float>& RightHandSide, const TArray<uint32>& FluidMask, TArray<float>& InOutX)
{
	check(IsInGameThread());

	const int64 NumCells64 = int64(Request.GridSize.X) * Request.GridSize.Y * Request.GridSize.Z;
	if (!IsAvailable() || NumCells64 <= 0 || NumCells64 > MAX_int32
		|| RightHandSide.Num() != NumCells64 || FluidMask.Num() != NumCells64
		|| InOutX.Num() != NumCells64 || Request.Iterations <= 0)
	{
		return false;
	}
	const int32 NumCells = int32(NumCells64);

	// 결과를 받을 자리. 성공했을 때만 InOutX에 덮는다.
	TArray<float> Result;
	Result.SetNumUninitialized(NumCells);
	bool bSucceeded = false;

	ENQUEUE_RENDER_COMMAND(VrgPressureJacobi)(
		[Request, NumCells, &RightHandSide, &FluidMask, &InOutX, &Result, &bSucceeded]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGBufferRef RhsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("VrgPressureRhs"),
				sizeof(float), NumCells, RightHandSide.GetData(), NumCells * sizeof(float));
			FRDGBufferRef FluidBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("VrgPressureFluid"),
				sizeof(uint32), NumCells, FluidMask.GetData(), NumCells * sizeof(uint32));

			// 핑퐁 두 장. 규약 7대로 in-place 갱신을 하지 않는다.
			FRDGBufferRef BufferA = CreateStructuredBuffer(GraphBuilder, TEXT("VrgPressureX0"),
				sizeof(float), NumCells, InOutX.GetData(), NumCells * sizeof(float));
			FRDGBufferRef BufferB = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumCells), TEXT("VrgPressureX1"));

			const TShaderMapRef<FVrgPressureJacobiCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount(
				FMath::DivideAndRoundUp(NumCells, VrgJacobiThreadsPerGroup), 1, 1);

			for (int32 Iteration = 0; Iteration < Request.Iterations; ++Iteration)
			{
				FVrgPressureJacobiCS::FParameters* Parameters =
					GraphBuilder.AllocParameters<FVrgPressureJacobiCS::FParameters>();
				Parameters->RhsBuffer = GraphBuilder.CreateSRV(RhsBuffer);
				Parameters->FluidBuffer = GraphBuilder.CreateSRV(FluidBuffer);
				Parameters->XIn = GraphBuilder.CreateSRV(BufferA);
				Parameters->XOut = GraphBuilder.CreateUAV(BufferB);
				Parameters->GridSize = Request.GridSize;
				Parameters->NumCells = NumCells;
				Parameters->CellSizeSquared = Request.CellSizeM * Request.CellSizeM;
				Parameters->Omega = Request.Omega;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VrgPressureJacobi"),
					ComputeShader, Parameters, GroupCount);

				Swap(BufferA, BufferB);
			}

			// 투영이 CPU에 있으므로 해를 어차피 내려받아야 한다.
			// 그래서 평균 빼기와 잔차도 내려받은 뒤 CPU에서 한다.
			FRHIGPUBufferReadback Readback(TEXT("VrgPressureReadback"));
			AddEnqueueCopyPass(GraphBuilder, &Readback, BufferA, NumCells * sizeof(float));
			GraphBuilder.Execute();

			// 이 호출 자체가 게임 스레드를 막고 있으므로 여기서 완료를 기다려도 된다.
			RHICmdList.BlockUntilGPUIdle();
			if (const void* Mapped = Readback.Lock(NumCells * sizeof(float)))
			{
				FMemory::Memcpy(Result.GetData(), Mapped, NumCells * sizeof(float));
				Readback.Unlock();
				bSucceeded = true;
			}
		});

	// 결과를 참조로 받았으므로 렌더 스레드가 끝날 때까지 기다려야 한다.
	FlushRenderingCommands();

	if (!bSucceeded)
	{
		return false;
	}
	for (const float Value : Result)
	{
		// NaN/Inf가 섞이면 조용히 쓰지 않는다. 호출자가 CPU 경로로 돌아가야 한다.
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
	}
	InOutX = MoveTemp(Result);
	return true;
}
