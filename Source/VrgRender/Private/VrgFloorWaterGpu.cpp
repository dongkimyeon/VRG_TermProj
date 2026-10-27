// Fill out your copyright notice in the Description page of Project Settings.

#include "VrgFloorWaterGpu.h"
#include "VrgAirGpu.h"

#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 FwThreads = 64;
	constexpr int32 FwReduceThreads = 256;

	BEGIN_SHADER_PARAMETER_STRUCT(FVrgFloorParams, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, Consts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, Bed)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, Solid)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, Cells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, Values)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, H)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, HU)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, HV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, NH)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, NHU)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, NHV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FxH)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FxNL)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FxNH)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FxT)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FyH)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FyNL)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FyNH)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, FyT)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, Created)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, Partials)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, Scalars)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, U)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, V)
		SHADER_PARAMETER(uint32, Nx)
		SHADER_PARAMETER(uint32, Ny)
		SHADER_PARAMETER(uint32, Count)
	END_SHADER_PARAMETER_STRUCT()

#define VRG_FW_SHADER(ClassName, Entry) \
	class ClassName : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ClassName); \
		using FParameters = FVrgFloorParams; \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
		{ \
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6); \
		} \
	}; \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Vrg/Private/VrgFloorWater.usf", Entry, SF_Compute);

	VRG_FW_SHADER(FVrgFwSetCellsCS, "SetCellsCS")
	VRG_FW_SHADER(FVrgFwStartCS, "StartCS")
	VRG_FW_SHADER(FVrgFwSpeedPartialCS, "SpeedPartialCS")
	VRG_FW_SHADER(FVrgFwDtCS, "DtCS")
	VRG_FW_SHADER(FVrgFwVelocityCS, "VelocityCS")
	VRG_FW_SHADER(FVrgFwFluxXCS, "FluxXCS")
	VRG_FW_SHADER(FVrgFwFluxYCS, "FluxYCS")
	VRG_FW_SHADER(FVrgFwUpdateCS, "UpdateCS")
	VRG_FW_SHADER(FVrgFwCopyBackCS, "CopyBackCS")
#undef VRG_FW_SHADER

	using FFwPooled = TRefCountPtr<FRDGPooledBuffer>;

	FFwPooled FwMakePooled(FRDGBuilder& GB, const TCHAR* Name, uint32 Stride, int32 N, const void* Data)
	{
		const int32 Count = FMath::Max(N, 1);
		TArray<uint8> Zeros;
		if (Data == nullptr || N == 0) { Zeros.SetNumZeroed(Count * Stride); Data = Zeros.GetData(); }
		return GB.ConvertToExternalBuffer(CreateStructuredBuffer(GB, Name, Stride, Count, Data, Count * Stride));
	}
}

struct FVrgFloorWaterGpu::FImpl
{
	FFwPooled Consts, Bed, Solid, H, HU, HV, NH, NHU, NHV, U, V, FxH, FxNL, FxNH, FxT, FyH, FyNL, FyNH, FyT, Created, Partials, Scalars;
	int32 Nx = 0, Ny = 0, N = 0;
	int32 LastSubsteps = 1;
	bool bConfigured = false;
};

FVrgFloorWaterGpu::FVrgFloorWaterGpu() : Impl(MakeUnique<FImpl>()) {}

FVrgFloorWaterGpu::~FVrgFloorWaterGpu()
{
	// Pooled buffers are render resources: release them on the render thread.
	if (Impl.IsValid() && Impl->bConfigured)
	{
		FImpl* Raw = Impl.Release();
		ENQUEUE_RENDER_COMMAND(VrgFloorRelease)([Raw](FRHICommandListImmediate&) { delete Raw; });
	}
}

bool FVrgFloorWaterGpu::IsAvailable() { return VrgAirGpu::IsAvailable(); }
bool FVrgFloorWaterGpu::IsConfigured() const { return Impl->bConfigured; }

void FVrgFloorWaterGpu::Reset()
{
	if (!Impl->bConfigured) return;
	FImpl* Raw = Impl.Release();
	ENQUEUE_RENDER_COMMAND(VrgFloorReset)([Raw](FRHICommandListImmediate&) { delete Raw; });
	Impl = MakeUnique<FImpl>();
}

bool FVrgFloorWaterGpu::Configure(const FVrgFloorGpuConfig& C)
{
	check(IsInGameThread());
	Reset();
	const int32 N = C.Nx * C.Ny;
	if (!IsAvailable() || C.Nx <= 0 || C.Ny <= 0 || C.BedM.Num() != N || C.Solid.Num() != N) return false;
	FImpl& S = *Impl;
	S.Nx = C.Nx; S.Ny = C.Ny; S.N = N;
	const double Consts[7] = { C.CellM, C.GravityMs2, C.DryDepthM, C.PinDepthM, C.ManningN * C.ManningN, C.Cfl, double(C.MaxSubsteps) };
	const int32 NumX = (C.Nx + 1) * C.Ny, NumY = C.Nx * (C.Ny + 1), NumPartials = FMath::DivideAndRoundUp(N, FwReduceThreads);
	ENQUEUE_RENDER_COMMAND(VrgFloorConfigure)([&](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GB(RHICmdList);
		S.Consts = FwMakePooled(GB, TEXT("VrgFloorConsts"), sizeof(double), 7, Consts);
		S.Bed = FwMakePooled(GB, TEXT("VrgFloorBed"), sizeof(double), N, C.BedM.GetData());
		S.Solid = FwMakePooled(GB, TEXT("VrgFloorSolid"), sizeof(int32), N, C.Solid.GetData());
		for (FFwPooled* P : { &S.H, &S.HU, &S.HV, &S.NH, &S.NHU, &S.NHV, &S.Created, &S.U, &S.V }) *P = FwMakePooled(GB, TEXT("VrgFloorCell"), sizeof(double), N, nullptr);
		for (FFwPooled* P : { &S.FxH, &S.FxNL, &S.FxNH, &S.FxT }) *P = FwMakePooled(GB, TEXT("VrgFloorFx"), sizeof(double), NumX, nullptr);
		for (FFwPooled* P : { &S.FyH, &S.FyNL, &S.FyNH, &S.FyT }) *P = FwMakePooled(GB, TEXT("VrgFloorFy"), sizeof(double), NumY, nullptr);
		S.Partials = FwMakePooled(GB, TEXT("VrgFloorPartials"), sizeof(double), NumPartials, nullptr);
		S.Scalars = FwMakePooled(GB, TEXT("VrgFloorScalars"), sizeof(double), 4, nullptr);
		GB.Execute();
	});
	FlushRenderingCommands();
	S.bConfigured = true;
	return true;
}

bool FVrgFloorWaterGpu::Upload(const TArray<double>& H, const TArray<double>& Hu, const TArray<double>& Hv)
{
	check(IsInGameThread());
	FImpl& S = *Impl;
	if (!S.bConfigured || H.Num() != S.N || Hu.Num() != S.N || Hv.Num() != S.N) return false;
	ENQUEUE_RENDER_COMMAND(VrgFloorUpload)([&](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GB(RHICmdList);
		S.H = FwMakePooled(GB, TEXT("VrgFloorH"), sizeof(double), S.N, H.GetData());
		S.HU = FwMakePooled(GB, TEXT("VrgFloorHU"), sizeof(double), S.N, Hu.GetData());
		S.HV = FwMakePooled(GB, TEXT("VrgFloorHV"), sizeof(double), S.N, Hv.GetData());
		GB.Execute();
	});
	FlushRenderingCommands();
	return true;
}

bool FVrgFloorWaterGpu::Step(double DtS, const TArray<int32>& SetCells, const TArray<double>& SetDepths,
	TArray<double>& OutH, TArray<double>& OutHu, TArray<double>& OutHv, double& OutCreatedDepthSum, int32& OutSubsteps)
{
	check(IsInGameThread());
	FImpl& S = *Impl;
	if (!S.bConfigured || SetCells.Num() != SetDepths.Num() || !(DtS > 0.0)) return false;
	const int32 N = S.N, NumPartials = FMath::DivideAndRoundUp(N, FwReduceThreads);
	OutH.SetNumUninitialized(N); OutHu.SetNumUninitialized(N); OutHv.SetNumUninitialized(N);
	TArray<double> Created, Scalars;
	Created.SetNumUninitialized(N); Scalars.SetNumUninitialized(4);
	bool bFirst = true, bOk = true;
	double Left = DtS;
	int32 Graphs = 0;
	// Substeps queued per submission; gated passes (dt = 0) do nothing, so a generous chunk is cheap.
	int32 Chunk = FMath::Clamp(S.LastSubsteps + 2, 2, 64);
	while (bOk && Left > 0.0 && Graphs < 64)
	{
		ENQUEUE_RENDER_COMMAND(VrgFloorStep)([&](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GB(RHICmdList);
			TMap<FRDGPooledBuffer*, FRDGBufferRef> Registered;
			auto Reg = [&](const FFwPooled& P) -> FRDGBufferRef
			{
				if (FRDGBufferRef* Found = Registered.Find(P.GetReference())) return *Found;
				return Registered.Add(P.GetReference(), GB.RegisterExternalBuffer(P));
			};
			const int32 NumSet = SetCells.Num();
			FRDGBufferRef CellsBuf = CreateStructuredBuffer(GB, TEXT("VrgFloorSetCells"), sizeof(int32), FMath::Max(NumSet, 1),
				NumSet ? (const void*)SetCells.GetData() : (const void*)&N, FMath::Max(NumSet, 1) * sizeof(int32));
			const double Zero = 0.0;
			FRDGBufferRef ValuesBuf = CreateStructuredBuffer(GB, TEXT("VrgFloorSetDepths"), sizeof(double), FMath::Max(NumSet, 1),
				NumSet ? (const void*)SetDepths.GetData() : (const void*)&Zero, FMath::Max(NumSet, 1) * sizeof(double));
			FRDGBufferRef StartBuf = CreateStructuredBuffer(GB, TEXT("VrgFloorStart"), sizeof(double), 1, &DtS, sizeof(double));
			auto Params = [&](uint32 Count, FRDGBufferRef Values) -> FVrgFloorParams*
			{
				FVrgFloorParams* P = GB.AllocParameters<FVrgFloorParams>();
				P->Consts = GB.CreateSRV(Reg(S.Consts)); P->Bed = GB.CreateSRV(Reg(S.Bed)); P->Solid = GB.CreateSRV(Reg(S.Solid));
				P->Cells = GB.CreateSRV(CellsBuf); P->Values = GB.CreateSRV(Values);
				P->H = GB.CreateUAV(Reg(S.H)); P->HU = GB.CreateUAV(Reg(S.HU)); P->HV = GB.CreateUAV(Reg(S.HV));
				P->NH = GB.CreateUAV(Reg(S.NH)); P->NHU = GB.CreateUAV(Reg(S.NHU)); P->NHV = GB.CreateUAV(Reg(S.NHV));
				P->FxH = GB.CreateUAV(Reg(S.FxH)); P->FxNL = GB.CreateUAV(Reg(S.FxNL)); P->FxNH = GB.CreateUAV(Reg(S.FxNH)); P->FxT = GB.CreateUAV(Reg(S.FxT));
				P->FyH = GB.CreateUAV(Reg(S.FyH)); P->FyNL = GB.CreateUAV(Reg(S.FyNL)); P->FyNH = GB.CreateUAV(Reg(S.FyNH)); P->FyT = GB.CreateUAV(Reg(S.FyT));
				P->Created = GB.CreateUAV(Reg(S.Created)); P->Partials = GB.CreateUAV(Reg(S.Partials)); P->Scalars = GB.CreateUAV(Reg(S.Scalars));
				P->U = GB.CreateUAV(Reg(S.U)); P->V = GB.CreateUAV(Reg(S.V));
				P->Nx = uint32(S.Nx); P->Ny = uint32(S.Ny); P->Count = Count;
				return P;
			};
			auto Groups = [](int32 Count, int32 Threads) { return FIntVector(FMath::DivideAndRoundUp(FMath::Max(Count, 1), Threads), 1, 1); };
			const FGlobalShaderMap* Map = GetGlobalShaderMap(GMaxRHIFeatureLevel);
			if (bFirst)
			{
				if (NumSet > 0)
					FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorSetCells"), TShaderMapRef<FVrgFwSetCellsCS>(Map), Params(NumSet, ValuesBuf), Groups(NumSet, FwThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorStart"), TShaderMapRef<FVrgFwStartCS>(Map), Params(N, StartBuf), Groups(N, FwThreads));
			}
			for (int32 K = 0; K < Chunk; ++K)
			{
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorSpeed"), TShaderMapRef<FVrgFwSpeedPartialCS>(Map), Params(N, ValuesBuf), Groups(N, FwReduceThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorDt"), TShaderMapRef<FVrgFwDtCS>(Map), Params(NumPartials, ValuesBuf), FIntVector(1, 1, 1));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorVelocity"), TShaderMapRef<FVrgFwVelocityCS>(Map), Params(N, ValuesBuf), Groups(N, FwThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorFluxX"), TShaderMapRef<FVrgFwFluxXCS>(Map), Params(0, ValuesBuf), Groups((S.Nx + 1) * S.Ny, FwThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorFluxY"), TShaderMapRef<FVrgFwFluxYCS>(Map), Params(0, ValuesBuf), Groups(S.Nx * (S.Ny + 1), FwThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorUpdate"), TShaderMapRef<FVrgFwUpdateCS>(Map), Params(N, ValuesBuf), Groups(N, FwThreads));
				FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgFloorCopyBack"), TShaderMapRef<FVrgFwCopyBackCS>(Map), Params(N, ValuesBuf), Groups(N, FwThreads));
			}
			FRHIGPUBufferReadback RScalars(TEXT("VrgFloorScalarsRb")), RH(TEXT("VrgFloorHRb")), RHU(TEXT("VrgFloorHURb")), RHV(TEXT("VrgFloorHVRb"));
			AddEnqueueCopyPass(GB, &RScalars, Reg(S.Scalars), 4 * sizeof(double));
			AddEnqueueCopyPass(GB, &RH, Reg(S.H), N * sizeof(double));
			AddEnqueueCopyPass(GB, &RHU, Reg(S.HU), N * sizeof(double));
			AddEnqueueCopyPass(GB, &RHV, Reg(S.HV), N * sizeof(double));
			GB.Execute();
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			auto Copy = [&](FRHIGPUBufferReadback& R, TArray<double>& To, int32 Num)
			{
				if (const void* Mapped = R.Lock(Num * sizeof(double))) { FMemory::Memcpy(To.GetData(), Mapped, Num * sizeof(double)); R.Unlock(); }
				else bOk = false;
			};
			Copy(RScalars, Scalars, 4); Copy(RH, OutH, N); Copy(RHU, OutHu, N); Copy(RHV, OutHv, N);
		});
		FlushRenderingCommands();
		bFirst = false;
		++Graphs;
		Left = Scalars[0];
		Chunk = 64;   // more were needed than the estimate: take larger chunks from here
	}
	if (!bOk || Left > 0.0) return false;
	OutSubsteps = FMath::RoundToInt32(Scalars[2]);
	S.LastSubsteps = OutSubsteps;
	OutCreatedDepthSum = 0.0;
	if (Scalars[3] > 0.0)
	{
		// Some depth was clipped (should not happen under the CFL limit): read the per-cell amounts for the ledger.
		ENQUEUE_RENDER_COMMAND(VrgFloorCreated)([&](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GB(RHICmdList);
			FRHIGPUBufferReadback R(TEXT("VrgFloorCreatedRb"));
			AddEnqueueCopyPass(GB, &R, GB.RegisterExternalBuffer(S.Created), N * sizeof(double));
			GB.Execute();
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (const void* Mapped = R.Lock(N * sizeof(double))) { FMemory::Memcpy(Created.GetData(), Mapped, N * sizeof(double)); R.Unlock(); }
			else bOk = false;
		});
		FlushRenderingCommands();
		if (!bOk) return false;
		for (const double V : Created) OutCreatedDepthSum += V;
	}
	for (const double V : OutH) if (!FMath::IsFinite(V)) return false;
	return true;
}
