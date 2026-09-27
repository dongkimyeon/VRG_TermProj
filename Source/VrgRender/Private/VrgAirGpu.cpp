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
	VRG_MG_SHADER(FVrgMgScatterCS, "ScatterCS")
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

namespace
{
	using FReads = TArray<TPair<FRDGBufferRef, int32>>;
}

// Shared CG driver: the standalone Solve() and the resident air step differ only in what runs before the setup (uploads /
// the velocity stages) and after the final mean removal (read X back / scatter and the velocity correction).
namespace
{
	FVrgGpuPoissonResult MgSolve(FVrgAirGpuPressure::FImpl& S, const FVrgGpuPoissonSettings& Settings,
		TFunctionRef<void(FMgGraph&)> Before, TFunctionRef<void(FMgGraph&, FReads&)> After, TArray<TArray<uint8>*> AfterOut)
	{
		FVrgGpuPoissonResult Result;
		TArray<double> ScalarsInit;
		ScalarsInit.SetNumZeroed(NumScalars);
		ScalarsInit[SlotTarget] = Settings.NormalizedResidualTarget;
		ScalarsInit[SlotOmega] = FMath::Clamp(Settings.Omega, .05, 1.0);
		const int32 MaxIt = FMath::Max(1, Settings.MaxIterations);
		const int32 Chunk = FMath::Max(1, Settings.ChunkIterations);

		TArray<uint8> ScalarBytes;
		auto ScalarsOut = [&]() { return reinterpret_cast<const double*>(ScalarBytes.GetData()); };
		int32 Queued = 0;
		auto QueueIterations = [&](FMgGraph& G, FReads& Reads)
		{
			const int32 Last = FMath::Min(Queued + Chunk, MaxIt);
			for (int32 It = Queued + 1; It <= Last; ++It) G.Iteration(It);
			Queued = Last;
			Reads.Add({ G.Reg(S.Scalars), NumScalars * int32(sizeof(double)) });
		};

		// Submission 1: the caller's stages, the setup (as Solve() up to the first residual and preconditioner), first chunk.
		bool bOk = RunGraph([&](FRDGBuilder& GB, FReads& Reads)
		{
			FMgGraph G{ GB, S };
			Before(G);
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
			bOk = RunGraph([&](FRDGBuilder& GB, FReads& Reads)
			{
				FMgGraph G{ GB, S };
				QueueIterations(G, Reads);
			}, { &ScalarBytes });
			++Result.Submissions;
		}

		// Final: remove the mean of X when the loop ran, then the caller's passes; the scalars are read last.
		if (bOk)
		{
			TArray<TArray<uint8>*> Out = AfterOut;
			Out.Add(&ScalarBytes);
			bOk = RunGraph([&](FRDGBuilder& GB, FReads& Reads)
			{
				FMgGraph G{ GB, S };
				G.Reduce(1, S.X, nullptr, S.N0, SlotReduce, false);
				FVrgMgParams* P = G.Params(S.N0, 0, SlotReduce);
				P->OutA = G.Uav(S.X);
				G.Run<FVrgMgFinishCS>(P, FMgGraph::Groups(S.N0));
				After(G, Reads);
				Reads.Add({ G.Reg(S.Scalars), NumScalars * int32(sizeof(double)) });
			}, Out);
			++Result.Submissions;
		}
		if (!bOk || ScalarBytes.Num() != NumScalars * int32(sizeof(double))) return Result;
		const double* Sc = ScalarsOut();
		Result.bRan = true;
		Result.bConverged = Sc[SlotConverged] == 1.0;
		Result.bIncompatibleRightHandSide = Sc[SlotStatus] == 2.0;
		Result.IterationsUsed = int32(Sc[SlotIterations]);
		Result.NormalizedResidual = Sc[SlotResidual];
		Result.ReferenceScale = Sc[SlotRef];
		Result.MeanRightHandSide = Sc[SlotMeanRhs];
		return Result;
	}
}

FVrgGpuPoissonResult FVrgAirGpuPressure::Solve(const TArray<double>& RhsGrid, TArray<double>& InOutFieldGrid,
	const FVrgGpuPoissonSettings& Settings)
{
	check(IsInGameThread());
	FImpl& S = *Impl;
	if (!S.bConfigured || RhsGrid.Num() != S.NumGrid || InOutFieldGrid.Num() != S.NumGrid) return FVrgGpuPoissonResult();
	TArray<uint8> XBytes;
	FVrgGpuPoissonResult Result = MgSolve(S, Settings,
		[&](FMgGraph& G)
		{
			G.GB.QueueBufferUpload(G.Reg(S.RhsGrid), RhsGrid.GetData(), S.NumGrid * sizeof(double));
			G.GB.QueueBufferUpload(G.Reg(S.FieldGrid), InOutFieldGrid.GetData(), S.NumGrid * sizeof(double));
		},
		[&](FMgGraph& G, FReads& Reads) { Reads.Add({ G.Reg(S.X), S.N0 * int32(sizeof(double)) }); },
		{ &XBytes });
	if (!Result.bRan || XBytes.Num() != S.N0 * int32(sizeof(double))) return FVrgGpuPoissonResult();
	const double* X = reinterpret_cast<const double*>(XBytes.GetData());
	for (int32 I = 0; I < S.N0; ++I)
	{
		if (!FMath::IsFinite(X[I])) return FVrgGpuPoissonResult();  // never hand NaN to the projection; the caller falls back
	}
	for (int32 I = 0; I < S.N0; ++I) InOutFieldGrid[S.GridOfCpu[I]] = X[I];
	return Result;
}

// ---- 4b/4c: resident air step ----

namespace
{
	enum EStepConst { CDtStep = 0, CDtMom = 1, CDtTemp = 2, CH = 3, CRho = 4, CCp = 5, CGravityZ = 6, CRefT = 7, CNuMol = 8,
		CAlphaMol = 9, CInvPrT = 10, CChenXu = 11, CCoolSupplyT = 12, CCellHeatCap = 13, CNumFluid = 14, NumConsts = 16 };
	enum EStepScalar { SSMaxU0 = 0, SSMaxU1 = 1, SSMaxU2 = 2, SSMaxNu = 3, SSSumNu = 4, SSSumDiv = 5, SSCoolW = 6, SSDuctJ = 7,
		NumStepScalars = 16 };

	BEGIN_SHADER_PARAMETER_STRUCT(FVrgStepParams, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, Consts)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InA)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InB)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InC)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<double>, InD)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxA)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxB)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxC)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxD)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, IdxE)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, OutA)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, OutB)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<double>, OutC)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, Axis)
		SHADER_PARAMETER(uint32, NX)
		SHADER_PARAMETER(uint32, NY)
		SHADER_PARAMETER(uint32, NZ)
		SHADER_PARAMETER(uint32, Mode)
		SHADER_PARAMETER(uint32, DtSlot)
	END_SHADER_PARAMETER_STRUCT()

#define VRG_STEP_SHADER(ClassName, Entry) \
	class ClassName : public FGlobalShader \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ClassName); \
		using FParameters = FVrgStepParams; \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
		{ \
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
		} \
	}; \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Vrg/Private/VrgAirStep.usf", Entry, SF_Compute);

	VRG_STEP_SHADER(FVrgStepNuTCS, "NuTCS")
	VRG_STEP_SHADER(FVrgStepAdvectCS, "AdvectCS")
	VRG_STEP_SHADER(FVrgStepBuoyancyCS, "BuoyancyCS")
	VRG_STEP_SHADER(FVrgStepWallCS, "WallCS")
	VRG_STEP_SHADER(FVrgStepDuctVelocityCS, "DuctVelocityCS")
	VRG_STEP_SHADER(FVrgStepDiffuseCS, "DiffuseCS")
	VRG_STEP_SHADER(FVrgStepDivergenceCS, "DivergenceCS")
	VRG_STEP_SHADER(FVrgStepSubtractFluidMeanCS, "SubtractFluidMeanCS")
	VRG_STEP_SHADER(FVrgStepGradientCS, "GradientCS")
	VRG_STEP_SHADER(FVrgStepTemperatureFluxCS, "TemperatureFluxCS")
	VRG_STEP_SHADER(FVrgStepDuctMixCS, "DuctMixCS")
	VRG_STEP_SHADER(FVrgStepDuctFluxCS, "DuctFluxCS")
	VRG_STEP_SHADER(FVrgStepTemperatureUpdateCS, "TemperatureUpdateCS")
	VRG_STEP_SHADER(FVrgStepInjectHeatCS, "InjectHeatCS")
	VRG_STEP_SHADER(FVrgStepCoolingCS, "CoolingCS")
#undef VRG_STEP_SHADER
}

struct FVrgAirGpuSolver::FImpl
{
	FVrgAirGpuPressure Pressure;
	FIntVector Dims = FIntVector::ZeroValue;
	int32 NumCells = 0, NumFluid = 0;
	int32 NumFaces[3] = { 0, 0, 0 };
	double H = 0.25, Rho = 1.184, Cp = 1005.0, RefT = 297.15;
	FPooled Solid, WallDist, NuT, T, TNext, Consts, Scalars, Partials;
	FPooled V[3], VNext[3], Flux[3];
	bool bConfigured = false;
};

namespace
{
	// Graph helpers for the step kernels (same registration as the pressure graph).
	struct FStepGraph
	{
		FMgGraph& G;
		FVrgAirGpuSolver::FImpl& A;

		FVrgStepParams* Params(uint32 Count, uint32 Axis = 0, uint32 Mode = 0, uint32 DtSlot = CDtStep)
		{
			FVrgAirGpuPressure::FImpl& S = G.S;
			FVrgStepParams* P = G.GB.AllocParameters<FVrgStepParams>();
			P->Consts = G.Srv(A.Consts);
			P->InA = P->InB = P->InC = P->InD = G.Srv(S.DummyD);
			P->IdxA = G.Srv(A.Solid);
			P->IdxB = P->IdxC = P->IdxD = P->IdxE = G.Srv(S.DummyI);
			P->OutA = P->OutB = P->OutC = G.Uav(S.DummyOut);
			P->Count = Count; P->Axis = Axis; P->Mode = Mode; P->DtSlot = DtSlot;
			P->NX = uint32(A.Dims.X); P->NY = uint32(A.Dims.Y); P->NZ = uint32(A.Dims.Z);
			return P;
		}
		template <typename TShader> void Dispatch(FVrgStepParams* P)
		{
			const TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(G.GB, RDG_EVENT_NAME("VrgAirStep"), Shader, P,
				FIntVector(FMgGraph::Groups(int32(P->Count)), 1, 1));
		}

		// Reduction over any double buffer into the step scalars (mode as FMgGraph::Reduce; never gated).
		void Reduce(uint32 Mode, const FPooled& Buf, int32 N, int32 Slot) { Reduce(Mode, G.Reg(Buf), N, Slot); }
		void Reduce(uint32 Mode, FRDGBufferRef Buf, int32 N, int32 Slot)
		{
			FVrgAirGpuPressure::FImpl& S = G.S;
			FVrgMgParams* P1 = G.GB.AllocParameters<FVrgMgParams>();
			P1->InA = P1->InB = P1->InC = P1->InD = G.Srv(S.DummyD);
			P1->IdxA = P1->IdxB = P1->IdxC = G.Srv(S.DummyI);
			P1->OutA = P1->OutB = G.Uav(S.DummyOut);
			P1->InA = G.GB.CreateSRV(Buf);
			P1->Scalars = G.Uav(A.Scalars);
			P1->Partials = G.Uav(A.Partials);
			P1->Count = uint32(N); P1->Mode = Mode; P1->Slot = 0; P1->Aux = 0;
			const int32 NumPartials = FMgGraph::Groups(N, MgReduceThreads);
			G.Run<FVrgMgReducePartialCS>(P1, NumPartials);
			FVrgMgParams* P2 = G.GB.AllocParameters<FVrgMgParams>();
			*P2 = *P1;
			P2->Count = uint32(NumPartials); P2->Slot = uint32(Slot);
			G.Run<FVrgMgReduceFinalCS>(P2, 1);
		}

		void UploadConsts(const TArray<double>& Values) { G.GB.QueueBufferUpload(G.Reg(A.Consts), Values.GetData(), NumConsts * sizeof(double)); }

		FRDGBufferRef Transient(const TCHAR* Name, uint32 Stride, int32 N, const void* Data)
		{
			const int32 Count = FMath::Max(N, 1);
			if (Data == nullptr || N == 0)
			{
				FRDGBufferRef B = G.GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, Count), Name);
				TArray<uint8> Zero; Zero.SetNumZeroed(Count * Stride);
				G.GB.QueueBufferUpload(B, Zero.GetData(), Zero.Num());
				return B;
			}
			return CreateStructuredBuffer(G.GB, Name, Stride, Count, Data, Count * Stride);
		}

		void Wall(int32 Axis)
		{
			FVrgStepParams* P = Params(uint32(A.NumFaces[Axis]), uint32(Axis));
			P->OutA = G.Uav(A.V[Axis]);
			Dispatch<FVrgStepWallCS>(P);
		}
		void DuctVelocity(const FVrgAirGpuDucts* Ducts)
		{
			if (Ducts == nullptr || !Ducts->HasFaces()) return;
			const int32 N = Ducts->Face.Num();
			FVrgStepParams* P = Params(uint32(N));
			P->IdxB = G.GB.CreateSRV(Transient(TEXT("VrgDuctPacked"), 4, N, Ducts->Packed.GetData()));
			P->IdxC = G.GB.CreateSRV(Transient(TEXT("VrgDuctFace"), 4, N, Ducts->Face.GetData()));
			P->InA = G.GB.CreateSRV(Transient(TEXT("VrgDuctVel"), 8, N, Ducts->Velocity.GetData()));
			P->OutA = G.Uav(A.V[0]); P->OutB = G.Uav(A.V[1]); P->OutC = G.Uav(A.V[2]);
			Dispatch<FVrgStepDuctVelocityCS>(P);
		}
		void WallAndDucts(const FVrgAirGpuDucts* Ducts)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis) Wall(Axis);
			DuctVelocity(Ducts);
		}
	};

	TArray<double> MakeConsts(const FVrgAirGpuSolver::FImpl& A)
	{
		TArray<double> C;
		C.SetNumZeroed(NumConsts);
		C[CH] = A.H; C[CRho] = A.Rho; C[CCp] = A.Cp; C[CRefT] = A.RefT;
		C[CCellHeatCap] = A.Rho * (A.H * A.H * A.H) * A.Cp;  // as CellHeatCapacityJK on the CPU
		C[CNumFluid] = double(A.NumFluid);
		return C;
	}
}

FVrgAirGpuSolver::FVrgAirGpuSolver() : Impl(MakeUnique<FImpl>()) {}
FVrgAirGpuSolver::~FVrgAirGpuSolver()
{
	if (Impl.IsValid() && Impl->bConfigured)
	{
		FImpl* Raw = Impl.Release();
		ENQUEUE_RENDER_COMMAND(VrgAirStepRelease)([Raw](FRHICommandListImmediate&) { delete Raw; });
	}
}

bool FVrgAirGpuSolver::IsConfigured() const { return Impl->bConfigured; }

void FVrgAirGpuSolver::Reset()
{
	if (!Impl->bConfigured) return;
	FImpl* Raw = Impl.Release();
	Impl = MakeUnique<FImpl>();
	ENQUEUE_RENDER_COMMAND(VrgAirStepRelease)([Raw](FRHICommandListImmediate&) { delete Raw; });
}

bool FVrgAirGpuSolver::Configure(const FVrgAirGpuGrid& Grid, const FVrgMgHierarchy& Hierarchy)
{
	check(IsInGameThread());
	Reset();
	FImpl& A = *Impl;
	const int32 N = Grid.Dims.X * Grid.Dims.Y * Grid.Dims.Z;
	if (!VrgAirGpu::IsAvailable() || N <= 0 || Grid.Solid.Num() != N || (Grid.WallDistanceM.Num() != 0 && Grid.WallDistanceM.Num() != N)
		|| Hierarchy.NumGridCells != N)
	{
		return false;
	}
	if (!A.Pressure.Configure(Hierarchy)) return false;
	A.Dims = Grid.Dims; A.NumCells = N; A.NumFluid = Grid.NumFluidCells;
	A.NumFaces[0] = (Grid.Dims.X + 1) * Grid.Dims.Y * Grid.Dims.Z;
	A.NumFaces[1] = Grid.Dims.X * (Grid.Dims.Y + 1) * Grid.Dims.Z;
	A.NumFaces[2] = Grid.Dims.X * Grid.Dims.Y * (Grid.Dims.Z + 1);
	A.H = Grid.CellSizeM; A.Rho = Grid.DensityKgM3; A.Cp = Grid.SpecificHeatJKgK; A.RefT = Grid.ReferenceTempK;
	const int32 MaxFaces = FMath::Max3(A.NumFaces[0], A.NumFaces[1], A.NumFaces[2]);
	TArray<TArray<uint8>*> NoReads;
	const bool bOk = RunGraph([&](FRDGBuilder& GB, FReads&)
	{
		A.Solid = MakePooled(GB, TEXT("VrgAirSolid"), 4, N, Grid.Solid.GetData());
		A.WallDist = MakePooled(GB, TEXT("VrgAirWallDist"), 8, N, Grid.WallDistanceM.Num() == N ? Grid.WallDistanceM.GetData() : nullptr);
		A.NuT = MakePooled(GB, TEXT("VrgAirNuT"), 8, N, nullptr);
		A.T = MakePooled(GB, TEXT("VrgAirT"), 8, N, nullptr);
		A.TNext = MakePooled(GB, TEXT("VrgAirTNext"), 8, N, nullptr);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			A.V[Axis] = MakePooled(GB, TEXT("VrgAirV"), 8, A.NumFaces[Axis], nullptr);
			A.VNext[Axis] = MakePooled(GB, TEXT("VrgAirVNext"), 8, A.NumFaces[Axis], nullptr);
			A.Flux[Axis] = MakePooled(GB, TEXT("VrgAirFlux"), 8, A.NumFaces[Axis], nullptr);
		}
		A.Consts = MakePooled(GB, TEXT("VrgAirConsts"), 8, NumConsts, nullptr);
		A.Scalars = MakePooled(GB, TEXT("VrgAirScalars"), 8, NumStepScalars, nullptr);
		A.Partials = MakePooled(GB, TEXT("VrgAirPartials"), 8, FMath::DivideAndRoundUp(FMath::Max(MaxFaces, N), MgReduceThreads), nullptr);
	}, NoReads);
	A.bConfigured = bOk;
	return bOk;
}

bool FVrgAirGpuSolver::UploadState(const TArray<double>& T, const TArray<double>& Vx, const TArray<double>& Vy,
	const TArray<double>& Vz, const TArray<double>& P)
{
	check(IsInGameThread());
	FImpl& A = *Impl;
	if (!A.bConfigured || T.Num() != A.NumCells || P.Num() != A.NumCells || Vx.Num() != A.NumFaces[0]
		|| Vy.Num() != A.NumFaces[1] || Vz.Num() != A.NumFaces[2])
	{
		return false;
	}
	FVrgAirGpuPressure::FImpl& S = A.Pressure.GetImpl();
	TArray<TArray<uint8>*> NoReads;
	return RunGraph([&](FRDGBuilder& GB, FReads&)
	{
		FMgGraph G{ GB, S };
		GB.QueueBufferUpload(G.Reg(A.T), T.GetData(), T.Num() * sizeof(double));
		GB.QueueBufferUpload(G.Reg(A.V[0]), Vx.GetData(), Vx.Num() * sizeof(double));
		GB.QueueBufferUpload(G.Reg(A.V[1]), Vy.GetData(), Vy.Num() * sizeof(double));
		GB.QueueBufferUpload(G.Reg(A.V[2]), Vz.GetData(), Vz.Num() * sizeof(double));
		GB.QueueBufferUpload(G.Reg(S.FieldGrid), P.GetData(), P.Num() * sizeof(double));
	}, NoReads);
}

bool FVrgAirGpuSolver::PreStep(bool bTurbulence, double ChenXuCoefficient, double& OutMaxAbsVelocityMs, double& OutMaxNuTM2s,
	double& OutSumNuTM2s)
{
	check(IsInGameThread());
	FImpl& A = *Impl;
	if (!A.bConfigured) return false;
	FVrgAirGpuPressure::FImpl& S = A.Pressure.GetImpl();
	TArray<double> Consts = MakeConsts(A);
	Consts[CChenXu] = ChenXuCoefficient;
	TArray<uint8> Bytes;
	const bool bOk = RunGraph([&](FRDGBuilder& GB, FReads& Reads)
	{
		FMgGraph G{ GB, S };
		FStepGraph W{ G, A };
		W.UploadConsts(Consts);
		for (int32 Axis = 0; Axis < 3; ++Axis) W.Reduce(2, A.V[Axis], A.NumFaces[Axis], SSMaxU0 + Axis);
		if (bTurbulence)
		{
			FVrgStepParams* P = W.Params(uint32(A.NumCells));
			P->InA = G.Srv(A.V[0]); P->InB = G.Srv(A.V[1]); P->InC = G.Srv(A.V[2]); P->InD = G.Srv(A.WallDist);
			P->OutA = G.Uav(A.NuT);
			W.Dispatch<FVrgStepNuTCS>(P);
			W.Reduce(2, A.NuT, A.NumCells, SSMaxNu);
			W.Reduce(1, A.NuT, A.NumCells, SSSumNu);
		}
		Reads.Add({ G.Reg(A.Scalars), NumStepScalars * int32(sizeof(double)) });
	}, { &Bytes });
	if (!bOk || Bytes.Num() != NumStepScalars * int32(sizeof(double))) return false;
	const double* Sc = reinterpret_cast<const double*>(Bytes.GetData());
	OutMaxAbsVelocityMs = FMath::Max3(Sc[SSMaxU0], Sc[SSMaxU1], Sc[SSMaxU2]);
	OutMaxNuTM2s = bTurbulence ? Sc[SSMaxNu] : 0.0;
	OutSumNuTM2s = bTurbulence ? Sc[SSSumNu] : 0.0;
	return true;
}

bool FVrgAirGpuSolver::Velocity(const FVrgAirGpuVelocityInput& In, FVrgGpuPoissonResult& OutPressure, double& OutMaxAbsVelocityMs)
{
	check(IsInGameThread());
	FImpl& A = *Impl;
	if (!A.bConfigured) return false;
	FVrgAirGpuPressure::FImpl& S = A.Pressure.GetImpl();
	TArray<double> Consts = MakeConsts(A);
	Consts[CDtStep] = In.DtS;
	Consts[CGravityZ] = In.GravityZMs2;
	Consts[CNuMol] = In.MolecularViscosityM2s;
	const int32 MomSub = FMath::Max(1, In.MomentumSubsteps);
	Consts[CDtMom] = In.DtS / MomSub;
	TArray<uint8> Bytes;
	OutPressure = MgSolve(S, In.Pressure,
		[&](FMgGraph& G)
		{
			FStepGraph W{ G, A };
			W.UploadConsts(Consts);
			// AdvectVelocity: read V, write VNext, then swap (rule 7).
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				FVrgStepParams* P = W.Params(uint32(A.NumFaces[Axis]), uint32(Axis));
				P->InA = G.Srv(A.V[0]); P->InB = G.Srv(A.V[1]); P->InC = G.Srv(A.V[2]); P->OutA = G.Uav(A.VNext[Axis]);
				W.Dispatch<FVrgStepAdvectCS>(P);
			}
			for (int32 Axis = 0; Axis < 3; ++Axis) Swap(A.V[Axis], A.VNext[Axis]);
			if (In.bBuoyancy && A.RefT > UE_DOUBLE_SMALL_NUMBER)
			{
				FVrgStepParams* P = W.Params(uint32(A.NumFaces[2]));
				P->InA = G.Srv(A.T); P->OutA = G.Uav(A.V[2]);
				W.Dispatch<FVrgStepBuoyancyCS>(P);
			}
			if (In.bTurbulence)
			{
				W.WallAndDucts(In.Ducts);
				for (int32 Sub = 0; Sub < MomSub; ++Sub)
				{
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						FVrgStepParams* P = W.Params(uint32(A.NumFaces[Axis]), uint32(Axis), 0, CDtMom);
						P->InA = G.Srv(A.V[Axis]); P->InB = G.Srv(A.NuT); P->OutA = G.Uav(A.VNext[Axis]);
						W.Dispatch<FVrgStepDiffuseCS>(P);
					}
					for (int32 Axis = 0; Axis < 3; ++Axis) Swap(A.V[Axis], A.VNext[Axis]);
				}
			}
			W.WallAndDucts(In.Ducts);
			// Projection right-hand side into the pressure solver's grid buffer, fluid mean removed.
			{
				FVrgStepParams* P = W.Params(uint32(A.NumCells));
				P->InA = G.Srv(A.V[0]); P->InB = G.Srv(A.V[1]); P->InC = G.Srv(A.V[2]); P->OutA = G.Uav(S.RhsGrid);
				W.Dispatch<FVrgStepDivergenceCS>(P);
			}
			W.Reduce(1, S.RhsGrid, A.NumCells, SSSumDiv);
			if (A.NumFluid > 0)
			{
				FVrgStepParams* P = W.Params(uint32(A.NumCells), 0, SSSumDiv);
				P->InA = G.Srv(A.Scalars); P->OutA = G.Uav(S.RhsGrid);
				W.Dispatch<FVrgStepSubtractFluidMeanCS>(P);
			}
		},
		[&](FMgGraph& G, FReads& Reads)
		{
			FStepGraph W{ G, A };
			// The solution goes back into the resident pressure field, then u -= dt/rho grad p and the walls again.
			{
				FVrgMgParams* P = G.Params(S.N0);
				P->InA = G.Srv(S.X); P->IdxA = G.Srv(S.GridOf); P->OutA = G.Uav(S.FieldGrid);
				G.Run<FVrgMgScatterCS>(P, FMgGraph::Groups(S.N0));
			}
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				FVrgStepParams* P = W.Params(uint32(A.NumFaces[Axis]), uint32(Axis));
				P->InA = G.Srv(S.FieldGrid); P->OutA = G.Uav(A.V[Axis]);
				W.Dispatch<FVrgStepGradientCS>(P);
			}
			W.WallAndDucts(In.Ducts);
			for (int32 Axis = 0; Axis < 3; ++Axis) W.Reduce(2, A.V[Axis], A.NumFaces[Axis], SSMaxU0 + Axis);
			Reads.Add({ G.Reg(A.Scalars), NumStepScalars * int32(sizeof(double)) });
		},
		{ &Bytes });
	if (!OutPressure.bRan || Bytes.Num() != NumStepScalars * int32(sizeof(double))) return false;
	const double* Sc = reinterpret_cast<const double*>(Bytes.GetData());
	OutMaxAbsVelocityMs = FMath::Max3(Sc[SSMaxU0], Sc[SSMaxU1], Sc[SSMaxU2]);
	return true;
}

bool FVrgAirGpuSolver::Transport(const FVrgAirGpuTransportInput& In, FVrgAirGpuTransportOutput& Out, TArray<double>& OutT,
	TArray<double>& OutVx, TArray<double>& OutVy, TArray<double>& OutVz)
{
	check(IsInGameThread());
	FImpl& A = *Impl;
	if (!A.bConfigured || In.HeatCell.Num() != In.HeatW.Num() || In.CoolCell.Num() != In.CoolUAWK.Num()) return false;
	FVrgAirGpuPressure::FImpl& S = A.Pressure.GetImpl();
	const int32 Sub = FMath::Max(1, In.TemperatureSubsteps);
	TArray<double> Consts = MakeConsts(A);
	Consts[CDtStep] = In.DtS;
	Consts[CDtTemp] = In.DtS / Sub;
	Consts[CAlphaMol] = In.MolecularDiffusivityM2s;
	Consts[CInvPrT] = In.InverseTurbulentPrandtl;
	Consts[CCoolSupplyT] = In.CoolingSupplyTempK;
	const FVrgAirGpuDucts* Ducts = In.Ducts && In.Ducts->HasFaces() ? In.Ducts : nullptr;
	const int32 NumDucts = Ducts ? Ducts->NumDucts : 0;
	TArray<uint8> TBytes, VBytes[3], ScalarBytes, DuctBytes;
	TArray<TArray<uint8>*> Outs = { &TBytes, &VBytes[0], &VBytes[1], &VBytes[2], &ScalarBytes, &DuctBytes };
	const bool bOk = RunGraph([&](FRDGBuilder& GB, FReads& Reads)
	{
		FMgGraph G{ GB, S };
		FStepGraph W{ G, A };
		W.UploadConsts(Consts);
		TArray<double> ZeroScalars; ZeroScalars.SetNumZeroed(NumStepScalars);
		GB.QueueBufferUpload(G.Reg(A.Scalars), ZeroScalars.GetData(), NumStepScalars * sizeof(double));
		FRDGBufferRef DuctPacked = nullptr, DuctFace = nullptr, DuctFluid = nullptr, DuctIndex = nullptr, DuctVel = nullptr,
			IntakeStart = nullptr, IntakeFaces = nullptr, DuctParams = nullptr, ExhaustK = nullptr, DuctRemoved = nullptr;
		if (Ducts)
		{
			const int32 NF = Ducts->Face.Num();
			DuctPacked = W.Transient(TEXT("VrgDuctPacked"), 4, NF, Ducts->Packed.GetData());
			DuctFace = W.Transient(TEXT("VrgDuctFace"), 4, NF, Ducts->Face.GetData());
			DuctFluid = W.Transient(TEXT("VrgDuctFluid"), 4, NF, Ducts->FluidCell.GetData());
			DuctIndex = W.Transient(TEXT("VrgDuctIndex"), 4, NF, Ducts->Duct.GetData());
			DuctVel = W.Transient(TEXT("VrgDuctVel"), 8, NF, Ducts->Velocity.GetData());
			IntakeStart = W.Transient(TEXT("VrgDuctIntakeStart"), 4, Ducts->IntakeStart.Num(), Ducts->IntakeStart.GetData());
			IntakeFaces = W.Transient(TEXT("VrgDuctIntakeFaces"), 4, Ducts->IntakeFaces.Num(), Ducts->IntakeFaces.GetData());
			DuctParams = W.Transient(TEXT("VrgDuctParams"), 8, Ducts->Params.Num(), Ducts->Params.GetData());
			ExhaustK = W.Transient(TEXT("VrgDuctExhaustK"), 8, NumDucts, nullptr);
			DuctRemoved = W.Transient(TEXT("VrgDuctRemoved"), 8, NumDucts, nullptr);
		}
		for (int32 Step = 0; Step < Sub; ++Step)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				FVrgStepParams* P = W.Params(uint32(A.NumFaces[Axis]), uint32(Axis), In.bDiffusion ? 1 : 0, CDtTemp);
				P->InA = G.Srv(A.T); P->InB = G.Srv(A.V[Axis]); P->InC = G.Srv(A.NuT); P->OutA = G.Uav(A.Flux[Axis]);
				W.Dispatch<FVrgStepTemperatureFluxCS>(P);
			}
			if (Ducts)
			{
				{
					FVrgStepParams* P = W.Params(uint32(NumDucts), 0, 0, CDtTemp);
					P->IdxB = GB.CreateSRV(IntakeStart); P->IdxC = GB.CreateSRV(IntakeFaces); P->IdxD = GB.CreateSRV(DuctFluid);
					P->InA = GB.CreateSRV(DuctVel); P->InB = G.Srv(A.T); P->InC = GB.CreateSRV(DuctParams);
					P->OutA = GB.CreateUAV(ExhaustK); P->OutB = GB.CreateUAV(DuctRemoved);
					W.Dispatch<FVrgStepDuctMixCS>(P);
				}
				{
					FVrgStepParams* P = W.Params(uint32(Ducts->Face.Num()));
					P->IdxB = GB.CreateSRV(DuctPacked); P->IdxC = GB.CreateSRV(DuctFace); P->IdxD = GB.CreateSRV(DuctFluid);
					P->IdxE = GB.CreateSRV(DuctIndex);
					P->InA = GB.CreateSRV(DuctVel); P->InB = G.Srv(A.T); P->InC = GB.CreateSRV(ExhaustK);
					P->OutA = G.Uav(A.Flux[0]); P->OutB = G.Uav(A.Flux[1]); P->OutC = G.Uav(A.Flux[2]);
					W.Dispatch<FVrgStepDuctFluxCS>(P);
				}
			}
			{
				FVrgStepParams* P = W.Params(uint32(A.NumCells), 0, 0, CDtTemp);
				P->InA = G.Srv(A.T); P->InB = G.Srv(A.Flux[0]); P->InC = G.Srv(A.Flux[1]); P->InD = G.Srv(A.Flux[2]);
				P->OutA = G.Uav(A.TNext);
				W.Dispatch<FVrgStepTemperatureUpdateCS>(P);
			}
			Swap(A.T, A.TNext);
		}
		if (In.HeatCell.Num() > 0)
		{
			FVrgStepParams* P = W.Params(uint32(In.HeatCell.Num()));
			P->IdxB = GB.CreateSRV(W.Transient(TEXT("VrgHeatCell"), 4, In.HeatCell.Num(), In.HeatCell.GetData()));
			P->InA = GB.CreateSRV(W.Transient(TEXT("VrgHeatW"), 8, In.HeatW.Num(), In.HeatW.GetData()));
			P->OutA = G.Uav(A.T);
			W.Dispatch<FVrgStepInjectHeatCS>(P);
		}
		if (In.CoolCell.Num() > 0)
		{
			FRDGBufferRef Applied = GB.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(8, In.CoolCell.Num()), TEXT("VrgCoolApplied"));
			FVrgStepParams* P = W.Params(uint32(In.CoolCell.Num()));
			P->IdxB = GB.CreateSRV(W.Transient(TEXT("VrgCoolCell"), 4, In.CoolCell.Num(), In.CoolCell.GetData()));
			P->InA = GB.CreateSRV(W.Transient(TEXT("VrgCoolUA"), 8, In.CoolUAWK.Num(), In.CoolUAWK.GetData()));
			P->OutA = G.Uav(A.T); P->OutB = GB.CreateUAV(Applied);
			W.Dispatch<FVrgStepCoolingCS>(P);
			W.Reduce(1, Applied, In.CoolCell.Num(), SSCoolW);
		}
		Reads.Add({ G.Reg(A.T), A.NumCells * int32(sizeof(double)) });
		for (int32 Axis = 0; Axis < 3; ++Axis) Reads.Add({ G.Reg(A.V[Axis]), A.NumFaces[Axis] * int32(sizeof(double)) });
		Reads.Add({ G.Reg(A.Scalars), NumStepScalars * int32(sizeof(double)) });
		Reads.Add({ DuctRemoved ? DuctRemoved : G.Reg(A.Scalars), (DuctRemoved ? FMath::Max(NumDucts, 1) : 1) * int32(sizeof(double)) });
	}, Outs);
	if (!bOk || TBytes.Num() != A.NumCells * int32(sizeof(double))) return false;
	OutT.SetNumUninitialized(A.NumCells);
	FMemory::Memcpy(OutT.GetData(), TBytes.GetData(), TBytes.Num());
	TArray<double>* OutV[3] = { &OutVx, &OutVy, &OutVz };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		OutV[Axis]->SetNumUninitialized(A.NumFaces[Axis]);
		FMemory::Memcpy(OutV[Axis]->GetData(), VBytes[Axis].GetData(), VBytes[Axis].Num());
	}
	const double* Sc = reinterpret_cast<const double*>(ScalarBytes.GetData());
	Out.CellCoolingRemovedW = In.CoolCell.Num() > 0 ? Sc[SSCoolW] : 0.0;
	Out.DuctCoilRemovedJ = 0.0;
	if (Ducts)
	{
		const double* Removed = reinterpret_cast<const double*>(DuctBytes.GetData());
		for (int32 D = 0; D < NumDucts; ++D) Out.DuctCoilRemovedJ += Removed[D];
	}
	return true;
}
