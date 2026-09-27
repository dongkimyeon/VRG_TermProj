// Fill out your copyright notice in the Description page of Project Settings.

#include "VrgAirGpu.h"

#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
	constexpr int32 VrgAirThreads = 64;

	class FVrgAirProbeCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVrgAirProbeCS);
		SHADER_USE_PARAMETER_STRUCT(FVrgAirProbeCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, ProbeA)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, ProbeB)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, ProbeOut)
			SHADER_PARAMETER(uint32, ProbeCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, ProbeScalars)  // C, D (shader parameter structs have no double scalars)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FVrgAirProbeCS, "/Vrg/Private/VrgAirGpu.usf", "ProbeCS", SF_Compute);
}

bool VrgAirGpu::IsAvailable()
{
	return GIsRHIInitialized && !GUsingNullRHI && GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
}

bool VrgAirGpu::ProbeDoubleArithmetic(const TArray<double>& A, const TArray<double>& B, double C, double D,
	TArray<double>& Out, FString& OutError)
{
	check(IsInGameThread());
	const int32 N = A.Num();
	if (!IsAvailable()) { OutError = TEXT("GPU 없음"); return false; }
	if (N == 0 || B.Num() != N) { OutError = TEXT("입력 길이"); return false; }
	Out.SetNumZeroed(N);
	bool bOk = false;
	ENQUEUE_RENDER_COMMAND(VrgAirProbe)([&, N, C, D](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef BufA = CreateStructuredBuffer(GraphBuilder, TEXT("VrgAirProbeA"), sizeof(double), N, A.GetData(), N * sizeof(double));
		FRDGBufferRef BufB = CreateStructuredBuffer(GraphBuilder, TEXT("VrgAirProbeB"), sizeof(double), N, B.GetData(), N * sizeof(double));
		FRDGBufferRef BufOut = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(double), N), TEXT("VrgAirProbeOut"));
		FVrgAirProbeCS::FParameters* P = GraphBuilder.AllocParameters<FVrgAirProbeCS::FParameters>();
		P->ProbeA = GraphBuilder.CreateSRV(BufA);
		P->ProbeB = GraphBuilder.CreateSRV(BufB);
		P->ProbeOut = GraphBuilder.CreateUAV(BufOut);
		P->ProbeCount = uint32(N);
		const double Scalars[2] = { C, D };
		P->ProbeScalars = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("VrgAirProbeScalars"), sizeof(double), 2, Scalars, sizeof(Scalars)));
		const TShaderMapRef<FVrgAirProbeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VrgAirProbe"), Shader, P,
			FIntVector(FMath::DivideAndRoundUp(N, VrgAirThreads), 1, 1));
		FRHIGPUBufferReadback Readback(TEXT("VrgAirProbeReadback"));
		AddEnqueueCopyPass(GraphBuilder, &Readback, BufOut, N * sizeof(double));
		GraphBuilder.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		if (const void* Mapped = Readback.Lock(N * sizeof(double)))
		{
			FMemory::Memcpy(Out.GetData(), Mapped, N * sizeof(double));
			Readback.Unlock();
			bOk = true;
		}
	});
	FlushRenderingCommands();
	if (!bOk) OutError = TEXT("내려받기 실패");
	return bOk;
}

// ---- 4d: MG-PCG on the GPU ----

namespace
{
	constexpr int32 MgThreads = 64;
	constexpr int32 MgReduceThreads = 256;
	constexpr int32 NumScalars = 16;
	enum EMgSlot { SlotRef = 0, SlotSumVB = 1, SlotSumV = 2, SlotSumF = 3, SlotRZ = 4, SlotPQ = 5, SlotAlpha = 6, SlotBeta = 7,
		SlotResidual = 8, SlotIterations = 9, SlotStatus = 10, SlotConverged = 11, SlotMeanRhs = 12, SlotReduce = 13,
		SlotTarget = 14, SlotOmega = 15 };

	BEGIN_SHADER_PARAMETER_STRUCT(FVrgMgParams, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InA)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InB)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InC)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InD)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxA)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxB)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxC)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, OutA)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, OutB)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, Scalars)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, Partials)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER(uint32, Slot)
		SHADER_PARAMETER(uint32, Aux)
	END_SHADER_PARAMETER_STRUCT()

#define VRG_MG_SHADER(ClassName, Entry) \
	class ClassName : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ClassName); \
		using FParameters = FVrgMgParams; \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
		{ \
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
		} \
	}; \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Vrg/Private/VrgAirMg.usf", Entry, SF_Compute);

	VRG_MG_SHADER(FVrgMgGatherCS, "GatherCS")
	VRG_MG_SHADER(FVrgMgReducePartialCS, "ReducePartialCS")
	VRG_MG_SHADER(FVrgMgReduceFinalCS, "ReduceFinalCS")
	VRG_MG_SHADER(FVrgMgScalarCS, "ScalarCS")
	VRG_MG_SHADER(FVrgMgBuildFCS, "BuildFCS")
	VRG_MG_SHADER(FVrgMgApplyACS, "ApplyACS")
	VRG_MG_SHADER(FVrgMgSmoothCS, "SmoothCS")
	VRG_MG_SHADER(FVrgMgRestrictCS, "RestrictCS")
	VRG_MG_SHADER(FVrgMgProlongCS, "ProlongCS")
	VRG_MG_SHADER(FVrgMgCoarsestCS, "CoarsestCS")
	VRG_MG_SHADER(FVrgMgVectorCS, "VectorCS")
	VRG_MG_SHADER(FVrgMgFinishCS, "FinishCS")
#undef VRG_MG_SHADER

	using FPooled = TRefCountPtr<FRDGPooledBuffer>;
}

struct FVrgAirGpuPressure::FImpl
{
	struct FLevel
	{
		int32 N = 0;
		FPooled RowStart, Col, Trans, Diag, Parent, ChildStart, Child, X, F, R, Tmp;
	};
	TArray<FLevel> Levels;
	FPooled Volume0, GridOf, RhsGrid, FieldGrid, B, X, F, R, Z, P, Q, Scalars, Partials;
	// Every parameter a shader references must be bound even when a branch never reads it: one-element stand-ins.
	FPooled DummyD, DummyI, DummyOut;
	int32 N0 = 0, NumGrid = 0, Pre = 2, Post = 2, Coarsest = 60;
	TArray<int32> GridOfCpu;
	bool bConfigured = false;
};

namespace
{
	// Registered view of every persistent buffer for one graph.
	struct FMgGraph
	{
		FRDGBuilder& GB;
		FVrgAirGpuPressure::FImpl& S;
		TMap<FRDGPooledBuffer*, FRDGBufferRef> Registered;

		FRDGBufferRef Reg(const FPooled& P)
		{
			if (FRDGBufferRef* Found = Registered.Find(P.GetReference())) return *Found;
			return Registered.Add(P.GetReference(), GB.RegisterExternalBuffer(P));
		}
		FRDGBufferSRVRef Srv(const FPooled& P) { return GB.CreateSRV(Reg(P)); }
		FRDGBufferUAVRef Uav(const FPooled& P) { return GB.CreateUAV(Reg(P)); }

		FVrgMgParams* Params(uint32 Count, uint32 Mode = 0, uint32 Slot = 0, uint32 Aux = 0)
		{
			FVrgMgParams* Out = GB.AllocParameters<FVrgMgParams>();
			Out->InA = Out->InB = Out->InC = Out->InD = Srv(S.DummyD);
			Out->IdxA = Out->IdxB = Out->IdxC = Srv(S.DummyI);
			Out->OutA = Out->OutB = Out->Partials = Uav(S.DummyOut);
			Out->Scalars = Uav(S.Scalars);
			Out->Count = Count; Out->Mode = Mode; Out->Slot = Slot; Out->Aux = Aux;
			return Out;
		}

		template <typename TShader>
		void Run(FVrgMgParams* Params, int32 Groups)
		{
			const TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GB, RDG_EVENT_NAME("VrgMg"), Shader, Params, FIntVector(FMath::Max(Groups, 1), 1, 1));
		}
		static int32 Groups(int32 N, int32 Threads = MgThreads) { return FMath::DivideAndRoundUp(FMath::Max(N, 1), Threads); }

		// Mode 0: sum A*B, 1: sum A, 2: max |A|, 3: max |A / B|. Gated: only while the solve is running.
		void Reduce(uint32 Mode, const FPooled& A, const FPooled* B, int32 N, int32 Slot, bool bGated)
		{
			FVrgMgParams* P1 = Params(N, Mode, 0, bGated ? 1 : 0);
			P1->InA = Srv(A);
			if (B) P1->InB = Srv(*B);
			P1->Partials = Uav(S.Partials);
			const int32 NumPartials = Groups(N, MgReduceThreads);
			Run<FVrgMgReducePartialCS>(P1, NumPartials);
			FVrgMgParams* P2 = Params(NumPartials, Mode, Slot, bGated ? 1 : 0);
			P2->Partials = Uav(S.Partials);
			Run<FVrgMgReduceFinalCS>(P2, 1);
		}
		void Scalar(uint32 Mode, uint32 Aux = 0) { Run<FVrgMgScalarCS>(Params(1, Mode, 0, Aux), 1); }

		void Smooth(FVrgAirGpuPressure::FImpl::FLevel& L, const FPooled& From, const FPooled& To)
		{
			FVrgMgParams* P = Params(L.N);
			P->InA = Srv(From); P->InB = Srv(L.Trans); P->InC = Srv(L.F); P->InD = Srv(L.Diag);
			P->IdxA = Srv(L.RowStart); P->IdxB = Srv(L.Col); P->OutA = Uav(To);
			Run<FVrgMgSmoothCS>(P, Groups(L.N));
		}
		void Sweeps(FVrgAirGpuPressure::FImpl::FLevel& L, int32 Count)
		{
			for (int32 K = 0; K < Count; ++K) Smooth(L, K % 2 == 0 ? L.X : L.Tmp, K % 2 == 0 ? L.Tmp : L.X);
			if (Count % 2 == 1) Copy(L.X, L.Tmp, L.N, false);
		}
		void Copy(const FPooled& To, const FPooled& From, int32 N, bool bAlways)
		{
			FVrgMgParams* P = Params(N, 4, 0, bAlways ? 2 : 0);
			P->OutA = Uav(To); P->InA = Srv(From);
			Run<FVrgMgVectorCS>(P, Groups(N));
		}

		void VCycle(int32 Li)
		{
			FVrgAirGpuPressure::FImpl::FLevel& L = S.Levels[Li];
			if (Li == S.Levels.Num() - 1)
			{
				FVrgMgParams* P = Params(L.N, 0, 0, uint32(S.Coarsest));
				P->OutA = Uav(L.X); P->OutB = Uav(L.Tmp); P->InB = Srv(L.Trans); P->InC = Srv(L.F); P->InD = Srv(L.Diag);
				P->IdxA = Srv(L.RowStart); P->IdxB = Srv(L.Col);
				Run<FVrgMgCoarsestCS>(P, 1);
				return;
			}
			Sweeps(L, S.Pre);
			{
				FVrgMgParams* P = Params(L.N, 0, 0, 1);
				P->InA = Srv(L.X); P->InB = Srv(L.Trans); P->InD = Srv(L.Diag); P->IdxA = Srv(L.RowStart); P->IdxB = Srv(L.Col);
				P->OutA = Uav(L.R);
				Run<FVrgMgApplyACS>(P, Groups(L.N));
			}
			FVrgAirGpuPressure::FImpl::FLevel& C = S.Levels[Li + 1];
			{
				FVrgMgParams* P = Params(C.N);
				P->InA = Srv(L.F); P->InB = Srv(L.R); P->IdxA = Srv(C.ChildStart); P->IdxB = Srv(C.Child);
				P->OutA = Uav(C.F); P->OutB = Uav(C.X);
				Run<FVrgMgRestrictCS>(P, Groups(C.N));
			}
			VCycle(Li + 1);
			{
				FVrgMgParams* P = Params(L.N);
				P->InA = Srv(C.X); P->IdxA = Srv(L.Parent); P->OutA = Uav(L.X);
				Run<FVrgMgProlongCS>(P, Groups(L.N));
			}
			Sweeps(L, S.Post);
		}

		// Z = M^-1 R: one V-cycle from zero, then the mean removed (as Precondition() in FMultigridPoisson::Solve).
		void Precondition()
		{
			FVrgAirGpuPressure::FImpl::FLevel& L0 = S.Levels[0];
			{
				FVrgMgParams* P = Params(S.N0, 0);
				P->InA = Srv(S.R); P->OutA = Uav(L0.F); P->OutB = Uav(L0.X);
				Run<FVrgMgVectorCS>(P, Groups(S.N0));
			}
			VCycle(0);
			Copy(S.Z, L0.X, S.N0, false);
			Reduce(1, S.Z, nullptr, S.N0, SlotReduce, true);
			FVrgMgParams* P = Params(S.N0, 1, SlotReduce);
			P->OutA = Uav(S.Z);
			Run<FVrgMgVectorCS>(P, Groups(S.N0));
		}

		void ApplyA0(const FPooled& X, const FPooled& Out, const FPooled* F)
		{
			FVrgAirGpuPressure::FImpl::FLevel& L0 = S.Levels[0];
			FVrgMgParams* P = Params(S.N0, F ? 1 : 0, 0, 1);
			P->InA = Srv(X); P->InB = Srv(L0.Trans); P->InD = Srv(L0.Diag); P->IdxA = Srv(L0.RowStart); P->IdxB = Srv(L0.Col);
			if (F) P->InC = Srv(*F);
			P->OutA = Uav(Out);
			Run<FVrgMgApplyACS>(P, Groups(S.N0));
		}

		void Iteration(int32 It)
		{
			ApplyA0(S.P, S.Q, nullptr);
			Reduce(0, S.P, &S.Q, S.N0, SlotPQ, true);
			Scalar(2);
			{
				FVrgMgParams* P = Params(S.N0, 2);
				P->OutA = Uav(S.X); P->InA = Srv(S.P); P->OutB = Uav(S.R); P->InB = Srv(S.Q);
				Run<FVrgMgVectorCS>(P, Groups(S.N0));
			}
			Reduce(3, S.R, &S.Volume0, S.N0, SlotReduce, true);
			Scalar(3, uint32(It));
			Precondition();
			Reduce(0, S.R, &S.Z, S.N0, SlotReduce, true);
			Scalar(4);
			FVrgMgParams* P = Params(S.N0, 3, 0, 0);
			P->OutA = Uav(S.P); P->InA = Srv(S.Z);
			Run<FVrgMgVectorCS>(P, Groups(S.N0));
		}
	};

	FPooled MakePooled(FRDGBuilder& GB, const TCHAR* Name, uint32 Stride, int32 N, const void* Data)
	{
		const int32 Count = FMath::Max(N, 1);
		TArray<uint8> Zeros;
		if (Data == nullptr || N == 0)
		{
			Zeros.SetNumZeroed(Count * Stride);
			Data = Zeros.GetData();
		}
		FRDGBufferRef Buffer = CreateStructuredBuffer(GB, Name, Stride, Count, Data, Count * Stride);
		return GB.ConvertToExternalBuffer(Buffer);
	}

	// Runs Build(GraphBuilder) on the render thread, reads back up to two buffers, and waits (game thread).
	bool RunGraph(TFunction<void(FRDGBuilder&, TArray<TPair<FRDGBufferRef, int32>>&)> Build, TArray<TArray<uint8>*> Out)
	{
		bool bOk = false;
		ENQUEUE_RENDER_COMMAND(VrgMgGraph)([&](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GB(RHICmdList);
			TArray<TPair<FRDGBufferRef, int32>> Reads;
			Build(GB, Reads);
			TArray<TUniquePtr<FRHIGPUBufferReadback>> Readbacks;
			for (const TPair<FRDGBufferRef, int32>& R : Reads)
			{
				Readbacks.Add(MakeUnique<FRHIGPUBufferReadback>(TEXT("VrgMgReadback")));
				AddEnqueueCopyPass(GB, Readbacks.Last().Get(), R.Key, R.Value);
			}
			GB.Execute();
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			bOk = true;
			for (int32 I = 0; I < Readbacks.Num(); ++I)
			{
				const int32 Bytes = Reads[I].Value;
				if (const void* Mapped = Readbacks[I]->Lock(Bytes))
				{
					Out[I]->SetNumUninitialized(Bytes);
					FMemory::Memcpy(Out[I]->GetData(), Mapped, Bytes);
					Readbacks[I]->Unlock();
				}
				else
				{
					bOk = false;
				}
			}
		});
		FlushRenderingCommands();
		return bOk;
	}
}

FVrgAirGpuPressure::FVrgAirGpuPressure() : Impl(MakeUnique<FImpl>()) {}
FVrgAirGpuPressure::~FVrgAirGpuPressure()
{
	// Pooled buffers are render resources: release them on the render thread.
	if (Impl.IsValid() && Impl->bConfigured)
	{
		FImpl* Raw = Impl.Release();
		ENQUEUE_RENDER_COMMAND(VrgMgRelease)([Raw](FRHICommandListImmediate&) { delete Raw; });
	}
}

bool FVrgAirGpuPressure::IsConfigured() const { return Impl->bConfigured; }

void FVrgAirGpuPressure::Reset()
{
	if (!Impl->bConfigured) return;
	FImpl* Raw = Impl.Release();
	Impl = MakeUnique<FImpl>();
	ENQUEUE_RENDER_COMMAND(VrgMgRelease)([Raw](FRHICommandListImmediate&) { delete Raw; });
}

bool FVrgAirGpuPressure::Configure(const FVrgMgHierarchy& H)
{
	check(IsInGameThread());
	Reset();
	if (!VrgAirGpu::IsAvailable() || H.Levels.Num() == 0) return false;
	const int32 N0 = H.Levels[0].NumCells;
	if (N0 <= 0 || H.Volume0.Num() != N0 || H.GridOf.Num() != N0 || H.NumGridCells <= 0) return false;
	for (int32 Li = 0; Li < H.Levels.Num(); ++Li)
	{
		const FVrgMgLevelData& L = H.Levels[Li];
		const bool bLast = Li == H.Levels.Num() - 1;
		if (L.RowStart.Num() != L.NumCells + 1 || L.Col.Num() != L.RowStart.Last() || L.Trans.Num() != L.Col.Num()
			|| L.Diag.Num() != L.NumCells || (!bLast && L.Parent.Num() != L.NumCells)
			|| (Li > 0 && (L.ChildStart.Num() != L.NumCells + 1 || L.Child.Num() != H.Levels[Li - 1].NumCells)))
		{
			return false;
		}
	}
	FImpl& S = *Impl;
	S.N0 = N0; S.NumGrid = H.NumGridCells; S.Pre = H.PreSweeps; S.Post = H.PostSweeps; S.Coarsest = H.CoarsestSweeps;
	S.GridOfCpu = H.GridOf;
	TArray<TArray<uint8>*> NoReads;
	const bool bOk = RunGraph([&](FRDGBuilder& GB, TArray<TPair<FRDGBufferRef, int32>>&)
	{
		for (const FVrgMgLevelData& L : H.Levels)
		{
			FImpl::FLevel& G = S.Levels.AddDefaulted_GetRef();
			G.N = L.NumCells;
			G.RowStart = MakePooled(GB, TEXT("VrgMgRowStart"), 4, L.RowStart.Num(), L.RowStart.GetData());
			G.Col = MakePooled(GB, TEXT("VrgMgCol"), 4, L.Col.Num(), L.Col.GetData());
			G.Trans = MakePooled(GB, TEXT("VrgMgTrans"), 8, L.Trans.Num(), L.Trans.GetData());
			G.Diag = MakePooled(GB, TEXT("VrgMgDiag"), 8, L.Diag.Num(), L.Diag.GetData());
			G.Parent = MakePooled(GB, TEXT("VrgMgParent"), 4, L.Parent.Num(), L.Parent.GetData());
			G.ChildStart = MakePooled(GB, TEXT("VrgMgChildStart"), 4, L.ChildStart.Num(), L.ChildStart.GetData());
			G.Child = MakePooled(GB, TEXT("VrgMgChild"), 4, L.Child.Num(), L.Child.GetData());
			G.X = MakePooled(GB, TEXT("VrgMgX"), 8, L.NumCells, nullptr);
			G.F = MakePooled(GB, TEXT("VrgMgF"), 8, L.NumCells, nullptr);
			G.R = MakePooled(GB, TEXT("VrgMgR"), 8, L.NumCells, nullptr);
			G.Tmp = MakePooled(GB, TEXT("VrgMgTmp"), 8, L.NumCells, nullptr);
		}
		S.Volume0 = MakePooled(GB, TEXT("VrgMgVolume0"), 8, N0, H.Volume0.GetData());
		S.GridOf = MakePooled(GB, TEXT("VrgMgGridOf"), 4, N0, H.GridOf.GetData());
		S.RhsGrid = MakePooled(GB, TEXT("VrgMgRhsGrid"), 8, H.NumGridCells, nullptr);
		S.FieldGrid = MakePooled(GB, TEXT("VrgMgFieldGrid"), 8, H.NumGridCells, nullptr);
		for (FPooled* V : { &S.B, &S.X, &S.F, &S.R, &S.Z, &S.P, &S.Q }) *V = MakePooled(GB, TEXT("VrgMgVector"), 8, N0, nullptr);
		S.Scalars = MakePooled(GB, TEXT("VrgMgScalars"), 8, NumScalars, nullptr);
		S.DummyD = MakePooled(GB, TEXT("VrgMgDummyD"), 8, 1, nullptr);
		S.DummyI = MakePooled(GB, TEXT("VrgMgDummyI"), 4, 1, nullptr);
		S.DummyOut = MakePooled(GB, TEXT("VrgMgDummyOut"), 8, 1, nullptr);
		S.Partials = MakePooled(GB, TEXT("VrgMgPartials"), 8, FMath::DivideAndRoundUp(N0, MgReduceThreads), nullptr);
	}, NoReads);
	S.bConfigured = bOk;
	return bOk;
}

FVrgGpuPoissonResult FVrgAirGpuPressure::Solve(const TArray<double>& RhsGrid, TArray<double>& InOutFieldGrid,
	const FVrgGpuPoissonSettings& Settings)
{
	check(IsInGameThread());
	FVrgGpuPoissonResult Result;
	FImpl& S = *Impl;
	if (!S.bConfigured || RhsGrid.Num() != S.NumGrid || InOutFieldGrid.Num() != S.NumGrid) return Result;

	TArray<double> ScalarsInit;
	ScalarsInit.SetNumZeroed(NumScalars);
	ScalarsInit[SlotTarget] = Settings.NormalizedResidualTarget;
	ScalarsInit[SlotOmega] = FMath::Clamp(Settings.Omega, .05, 1.0);
	const int32 MaxIt = FMath::Max(1, Settings.MaxIterations);
	const int32 Chunk = FMath::Max(1, Settings.ChunkIterations);

	TArray<uint8> ScalarBytes, XBytes;
	auto ScalarsOut = [&]() { return reinterpret_cast<const double*>(ScalarBytes.GetData()); };
	int32 Queued = 0;
	auto QueueIterations = [&](FMgGraph& G, TArray<TPair<FRDGBufferRef, int32>>& Reads)
	{
		const int32 Last = FMath::Min(Queued + Chunk, MaxIt);
		for (int32 It = Queued + 1; It <= Last; ++It) G.Iteration(It);
		Queued = Last;
		Reads.Add({ G.Reg(S.Scalars), NumScalars * int32(sizeof(double)) });
	};

	// Submission 1: setup (as Solve() up to the first residual and preconditioner) and the first chunk.
	bool bOk = RunGraph([&](FRDGBuilder& GB, TArray<TPair<FRDGBufferRef, int32>>& Reads)
	{
		FMgGraph G{ GB, S };
		GB.QueueBufferUpload(G.Reg(S.RhsGrid), RhsGrid.GetData(), S.NumGrid * sizeof(double));
		GB.QueueBufferUpload(G.Reg(S.FieldGrid), InOutFieldGrid.GetData(), S.NumGrid * sizeof(double));
		GB.QueueBufferUpload(G.Reg(S.Scalars), ScalarsInit.GetData(), NumScalars * sizeof(double));
		{
			FVrgMgParams* P = G.Params(S.N0);
			P->InA = G.Srv(S.RhsGrid); P->InB = G.Srv(S.FieldGrid); P->IdxA = G.Srv(S.GridOf);
			P->OutA = G.Uav(S.B); P->OutB = G.Uav(S.X);
			G.Run<FVrgMgGatherCS>(P, FMgGraph::Groups(S.N0));
		}
		G.Reduce(2, S.B, nullptr, S.N0, SlotRef, false);
		G.Reduce(0, S.Volume0, &S.B, S.N0, SlotSumVB, false);
		G.Reduce(1, S.Volume0, nullptr, S.N0, SlotSumV, false);
		G.Scalar(0);
		{
			FVrgMgParams* P = G.Params(S.N0, 0);
			P->InA = G.Srv(S.Volume0); P->InB = G.Srv(S.B); P->OutA = G.Uav(S.F);
			G.Run<FVrgMgBuildFCS>(P, FMgGraph::Groups(S.N0));
		}
		G.Reduce(1, S.F, nullptr, S.N0, SlotSumF, true);
		{
			FVrgMgParams* P = G.Params(S.N0, 1);
			P->InA = G.Srv(S.Volume0); P->OutA = G.Uav(S.F);
			G.Run<FVrgMgBuildFCS>(P, FMgGraph::Groups(S.N0));
		}
		G.ApplyA0(S.X, S.R, &S.F);
		G.Reduce(3, S.R, &S.Volume0, S.N0, SlotReduce, true);
		G.Scalar(1);
		G.Precondition();
		{
			FVrgMgParams* P = G.Params(S.N0, 3, 0, 1);
			P->OutA = G.Uav(S.P); P->InA = G.Srv(S.Z);
			G.Run<FVrgMgVectorCS>(P, FMgGraph::Groups(S.N0));
		}
		G.Reduce(0, S.R, &S.Z, S.N0, SlotReduce, true);
		G.Scalar(5);
		QueueIterations(G, Reads);
	}, { &ScalarBytes });
	++Result.Submissions;

	// Further chunks while the GPU says it is still running (D5: the host sees every chunk's status).
	while (bOk && ScalarsOut()[SlotStatus] == 0.0 && Queued < MaxIt)
	{
		bOk = RunGraph([&](FRDGBuilder& GB, TArray<TPair<FRDGBufferRef, int32>>& Reads)
		{
			FMgGraph G{ GB, S };
			QueueIterations(G, Reads);
		}, { &ScalarBytes });
		++Result.Submissions;
	}

	// Final: remove the mean of X when the loop ran, read X back.
	if (bOk)
	{
		bOk = RunGraph([&](FRDGBuilder& GB, TArray<TPair<FRDGBufferRef, int32>>& Reads)
		{
			FMgGraph G{ GB, S };
			G.Reduce(1, S.X, nullptr, S.N0, SlotReduce, false);
			FVrgMgParams* P = G.Params(S.N0, 0, SlotReduce);
			P->OutA = G.Uav(S.X);
			G.Run<FVrgMgFinishCS>(P, FMgGraph::Groups(S.N0));
			Reads.Add({ G.Reg(S.X), S.N0 * int32(sizeof(double)) });
			Reads.Add({ G.Reg(S.Scalars), NumScalars * int32(sizeof(double)) });
		}, { &XBytes, &ScalarBytes });
		++Result.Submissions;
	}
	if (!bOk || XBytes.Num() != S.N0 * int32(sizeof(double))) return Result;

	const double* Sc = ScalarsOut();
	const double* X = reinterpret_cast<const double*>(XBytes.GetData());
	for (int32 I = 0; I < S.N0; ++I)
	{
		if (!FMath::IsFinite(X[I])) return Result;  // never hand NaN to the projection; the caller falls back
	}
	for (int32 I = 0; I < S.N0; ++I) InOutFieldGrid[S.GridOfCpu[I]] = X[I];
	Result.bRan = true;
	Result.bConverged = Sc[SlotConverged] == 1.0;
	Result.bIncompatibleRightHandSide = Sc[SlotStatus] == 2.0;
	Result.IterationsUsed = int32(Sc[SlotIterations]);
	Result.NormalizedResidual = Sc[SlotResidual];
	Result.ReferenceScale = Sc[SlotRef];
	Result.MeanRightHandSide = Sc[SlotMeanRhs];
	return Result;
}
