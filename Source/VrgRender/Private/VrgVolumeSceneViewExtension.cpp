// Fill out your copyright notice in the Description page of Project Settings.

#include "VrgVolumeSceneViewExtension.h"

#include "GlobalShader.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

static TAutoConsoleVariable<int32> CVarVrgVolumeDebug(
	TEXT("vrg.Volume.Debug"),
	0,
	TEXT("볼륨 레이마칭 진단. 0=정상, 1=상자 교차 구간, 2=최대 온도, 3=장면 깊이"),
	ECVF_RenderThreadSafe);

namespace
{
	// 셰이더의 레이마칭 파라미터. 전부 렌더 스레드에서 채운다.
	class FVrgVolumeRaymarchPS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVrgVolumeRaymarchPS);
		SHADER_USE_PARAMETER_STRUCT(FVrgVolumeRaymarchPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
			SHADER_PARAMETER_TEXTURE(Texture3D, VolumeTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, VolumeSampler)
			SHADER_PARAMETER_TEXTURE(Texture3D, VelocityTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, VelocitySampler)
			SHADER_PARAMETER(FVector3f, VolumeBoundsMin)
			SHADER_PARAMETER(FVector3f, VolumeBoundsMax)
			SHADER_PARAMETER(FVector2f, TempRangeK)
			SHADER_PARAMETER(float, OpacityPerCm)
			SHADER_PARAMETER(int32, StepCount)
			SHADER_PARAMETER(int32, DebugMode)
			SHADER_PARAMETER(float, FlowStrength)
			SHADER_PARAMETER(float, FlowSpeedScale)
			SHADER_PARAMETER(float, FlowFrequency)
			SHADER_PARAMETER(float, MaxSpeedMs)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FVrgVolumeRaymarchPS, "/Vrg/Private/VrgVolumeRaymarch.usf", "MainPS", SF_Pixel);
}

FVrgVolumeSceneViewExtension::FVrgVolumeSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FVrgVolumeSceneViewExtension::SetEnabled_GameThread(bool bInEnabled)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(VrgVolumeSetEnabled)(
		[this, bInEnabled](FRHICommandListImmediate&)
		{
			bRenderEnabled = bInEnabled;
		});
}

void FVrgVolumeSceneViewExtension::UpdateVolume_GameThread(const FIntVector& InSize,
	TArray<float>&& InVoxelsK, const FBox& InWorldBoundsCm, float InMinTempK, float InMaxTempK,
	float InOpacityPerCm, int32 InStepCount)
{
	check(IsInGameThread());
	const int64 Expected = int64(InSize.X) * InSize.Y * InSize.Z;
	if (InSize.X <= 0 || InSize.Y <= 0 || InSize.Z <= 0 || InVoxelsK.Num() != Expected)
	{
		// 조용히 맞춰 주지 않는다. 크기가 어긋나면 표시가 통째로 거짓말이 된다.
		UE_LOG(LogTemp, Warning, TEXT("[VrgVolume] 복셀 수(%d)가 격자 %dx%dx%d와 맞지 않는다."),
			InVoxelsK.Num(), InSize.X, InSize.Y, InSize.Z);
		return;
	}

	// 규약 5: 게임 스레드가 만든 복사본만 넘긴다. 렌더 스레드는 액터를 보지 않는다.
	ENQUEUE_RENDER_COMMAND(VrgVolumeUpload)(
		[this, InSize, Voxels = MoveTemp(InVoxelsK), InWorldBoundsCm, InMinTempK, InMaxTempK,
			InOpacityPerCm, InStepCount](FRHICommandListImmediate& RHICmdList)
		{
			if (!VolumeTexture.IsValid() || VolumeSize != InSize)
			{
				// 절대온도를 그대로 담는다. 8비트로 정규화하면 표시 범위를 바꿀 때마다
				// 다시 올려야 하고, 범위가 좁을 때 계단이 생긴다.
				const FRHITextureCreateDesc Desc =
					FRHITextureCreateDesc::Create3D(TEXT("VrgVolumeTemperature"),
						InSize.X, InSize.Y, InSize.Z, PF_R32_FLOAT)
					.SetFlags(ETextureCreateFlags::ShaderResource);
				VolumeTexture = RHICmdList.CreateTexture(Desc);
				VolumeSize = InSize;
			}

			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, InSize.X, InSize.Y, InSize.Z);
			RHICmdList.UpdateTexture3D(VolumeTexture, 0, Region,
				InSize.X * sizeof(float), InSize.X * InSize.Y * sizeof(float),
				reinterpret_cast<const uint8*>(Voxels.GetData()));

			BoundsMinCm = FVector3f(InWorldBoundsCm.Min);
			BoundsMaxCm = FVector3f(InWorldBoundsCm.Max);
			MinTempK = InMinTempK;
			MaxTempK = InMaxTempK;
			OpacityPerCm = InOpacityPerCm;
			StepCount = InStepCount;
		});
}

void FVrgVolumeSceneViewExtension::UpdateVelocity_GameThread(const FIntVector& InSize,
	TArray<FVector4f>&& InVelocityMs, float InMaxSpeedMs, float InFlowStrength,
	float InFlowSpeedScale, float InFlowFrequency)
{
	check(IsInGameThread());
	const int64 Expected = int64(InSize.X) * InSize.Y * InSize.Z;
	if (InSize.X <= 0 || InSize.Y <= 0 || InSize.Z <= 0 || InVelocityMs.Num() != Expected)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgVolume] 속도 복셀 수(%d)가 격자 %dx%dx%d와 맞지 않는다."),
			InVelocityMs.Num(), InSize.X, InSize.Y, InSize.Z);
		return;
	}

	ENQUEUE_RENDER_COMMAND(VrgVelocityUpload)(
		[this, InSize, Velocity = MoveTemp(InVelocityMs), InMaxSpeedMs, InFlowStrength,
			InFlowSpeedScale, InFlowFrequency](FRHICommandListImmediate& RHICmdList)
		{
			if (!VelocityTexture.IsValid() || VelocityTexture->GetSizeXYZ() != InSize)
			{
				const FRHITextureCreateDesc Desc =
					FRHITextureCreateDesc::Create3D(TEXT("VrgVolumeVelocity"),
						InSize.X, InSize.Y, InSize.Z, PF_A32B32G32R32F)
					.SetFlags(ETextureCreateFlags::ShaderResource);
				VelocityTexture = RHICmdList.CreateTexture(Desc);
			}

			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, InSize.X, InSize.Y, InSize.Z);
			RHICmdList.UpdateTexture3D(VelocityTexture, 0, Region,
				InSize.X * sizeof(FVector4f), InSize.X * InSize.Y * sizeof(FVector4f),
				reinterpret_cast<const uint8*>(Velocity.GetData()));

			MaxSpeedMs = InMaxSpeedMs;
			FlowStrength = InFlowStrength;
			FlowSpeedScale = InFlowSpeedScale;
			FlowFrequency = InFlowFrequency;
		});
}

void FVrgVolumeSceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass,
	const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// 톤매핑 뒤에 얹는다. 앞에 얹으면 색상표가 톤매퍼를 거쳐 씻겨 나간다.
	if (Pass != EPostProcessingPass::Tonemap)
	{
		return;
	}
	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
		this, &FVrgVolumeSceneViewExtension::AddVolumePass_RenderThread));
}

FScreenPassTexture FVrgVolumeSceneViewExtension::AddVolumePass_RenderThread(FRDGBuilder& GraphBuilder,
	const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor =
		FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!bRenderEnabled || !VolumeTexture.IsValid() || !SceneColor.IsValid())
	{
		LastPassCount.store(0, std::memory_order_relaxed);
		return SceneColor;
	}

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		// 입력과 같은 크기/형식이되 렌더 타깃 플래그를 확실히 켠다.
		// 입력 Desc를 그대로 쓰면 SceneColor의 플래그 조합에 따라 렌더 타깃이 아닐 수 있다.
		FRDGTextureDesc Desc = SceneColor.Texture->Desc;
		Desc.Reset();
		Desc.Flags = ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource;
		Output = FScreenPassRenderTarget(
			GraphBuilder.CreateTexture(Desc, TEXT("VrgVolumeOutput")),
			SceneColor.ViewRect, ERenderTargetLoadAction::ENoAction);
	}

	FVrgVolumeRaymarchPS::FParameters* Parameters = GraphBuilder.AllocParameters<FVrgVolumeRaymarchPS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->SceneTextures = Inputs.SceneTextures;
	Parameters->InputTexture = SceneColor.Texture;
	Parameters->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->VolumeTexture = VolumeTexture;
	Parameters->VolumeSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// 셰이더는 translated world에서 계산한다. 월드 좌표에 뷰 이동을 더해 맞춘다.
	const FVector3f PreViewTranslation = FVector3f(View.ViewMatrices.GetPreViewTranslation());
	Parameters->VolumeBoundsMin = BoundsMinCm + PreViewTranslation;
	Parameters->VolumeBoundsMax = BoundsMaxCm + PreViewTranslation;
	Parameters->TempRangeK = FVector2f(MinTempK, MaxTempK);
	Parameters->OpacityPerCm = OpacityPerCm;
	Parameters->StepCount = StepCount;
	Parameters->DebugMode = CVarVrgVolumeDebug.GetValueOnRenderThread();

	// 속도장이 아직 없으면 온도 텍스처를 꽂고 세기를 0으로 둔다.
	// 바인딩이 비면 셰이더가 죽으므로 더미라도 넣어야 한다.
	Parameters->VelocityTexture = VelocityTexture.IsValid() ? VelocityTexture : VolumeTexture;
	Parameters->VelocitySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->FlowStrength = VelocityTexture.IsValid() ? FlowStrength : 0.0f;
	Parameters->FlowSpeedScale = FlowSpeedScale;
	Parameters->FlowFrequency = FlowFrequency;
	Parameters->MaxSpeedMs = MaxSpeedMs;
	Parameters->RenderTargets[0] = Output.GetRenderTargetBinding();

	const TShaderMapRef<FVrgVolumeRaymarchPS> PixelShader(GetGlobalShaderMap(View.GetFeatureLevel()));
	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("VrgVolumeRaymarch"),
		FScreenPassViewInfo(View), FScreenPassTextureViewport(Output),
		FScreenPassTextureViewport(SceneColor), PixelShader, Parameters);

	// 셀 수와 무관하게 전체 화면 패스 하나다.
	LastPassCount.store(1, std::memory_order_relaxed);
	return MoveTemp(Output);
}
