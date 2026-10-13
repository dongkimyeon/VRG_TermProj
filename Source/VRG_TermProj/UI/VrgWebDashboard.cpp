// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/VrgWebDashboard.h"
#include "HAL/IConsoleManager.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	void OnWebSettingsChanged(IConsoleVariable*) { FVrgWebDashboard::Get().ApplySettings(); }

	TAutoConsoleVariable<int32> CVarWebEnable(TEXT("vrg.Web.Enable"), 0,
		TEXT("Web dashboard on loopback: 0 off (default), 1 serve GET /dashboard and GET /api/snapshot."),
		FConsoleVariableDelegate::CreateStatic(&OnWebSettingsChanged));
	TAutoConsoleVariable<int32> CVarWebPort(TEXT("vrg.Web.Port"), 18080,
		TEXT("Web dashboard port (127.0.0.1 only)."), FConsoleVariableDelegate::CreateStatic(&OnWebSettingsChanged));

	TUniquePtr<FHttpServerResponse> NoStore(TUniquePtr<FHttpServerResponse> Response)
	{
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		return Response;
	}
}

FVrgWebDashboard& FVrgWebDashboard::Get()
{
	static FVrgWebDashboard Instance;
	return Instance;
}

void FVrgWebDashboard::Publish(const FString& Json)
{
	FScopeLock Guard(&Lock);
	LatestJson = Json;
}

FString FVrgWebDashboard::GetLatestJson() const
{
	FScopeLock Guard(&Lock);
	return LatestJson;
}

void FVrgWebDashboard::PublishSeries(FVrgDashboardSeries&& InSeries)
{
	FScopeLock Guard(&Lock);
	LatestSeries = MoveTemp(InSeries);
	bHasSeries = true;
}

FString FVrgWebDashboard::GetSeriesJson(double SinceS) const
{
	FScopeLock Guard(&Lock);
	return bHasSeries ? LatestSeries.ToJson(SinceS) : FString();
}

void FVrgWebDashboard::Stop()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& Handle : Routes) Router->UnbindRoute(Handle);
		UE_LOG(LogTemp, Display, TEXT("[VrgWeb] 대시보드 중지 (포트 %d)"), BoundPort);
	}
	Routes.Reset();
	Router.Reset();
	BoundPort = 0;
	Publish(FString());
	FScopeLock Guard(&Lock);
	LatestSeries = FVrgDashboardSeries();
	bHasSeries = false;
}

bool FVrgWebDashboard::ApplySettings()
{
	const bool bWant = CVarWebEnable.GetValueOnGameThread() != 0;
	const int32 Port = CVarWebPort.GetValueOnGameThread();
	if (!bWant || Port != BoundPort) Stop();
	if (!bWant || Router.IsValid()) return true;
	if (Port <= 0 || Port > 65535 || !FModuleManager::Get().ModuleExists(TEXT("HTTPServer")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgWeb] 대시보드를 열 수 없다: 포트 %d 또는 HTTPServer 모듈 없음"), Port);
		return false;
	}
	FHttpServerModule& Http = FHttpServerModule::Get();
	Router = Http.GetHttpRouter(uint32(Port), /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[VrgWeb] 포트 %d에 묶지 못했다(사용 중일 수 있다). vrg.Web.Port로 바꿔 다시 켠다."), Port);
		return false;
	}
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/snapshot")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			const FString Json = GetLatestJson();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_snapshot"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/series")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			double SinceS = -1.0;
			if (const FString* Since = Request.QueryParams.Find(TEXT("since"))) LexFromString(SinceS, **Since);
			const FString Json = GetSeriesJson(FMath::IsFinite(SinceS) ? SinceS : -1.0);
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_series"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/dashboard")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			// Read per request so the page can be edited without restarting. Not staged into packages yet.
			FString Html;
			const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Web"), TEXT("index.html"));
			OnComplete(NoStore(FFileHelper::LoadFileToString(Html, *Path)
				? FHttpServerResponse::Create(Html, TEXT("text/html; charset=utf-8"))
				: FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound, TEXT("no_page"), Path)));
			return true;
		})));
	Http.StartAllListeners();
	BoundPort = Port;
	UE_LOG(LogTemp, Display, TEXT("[VrgWeb] 대시보드 시작: http://127.0.0.1:%d/dashboard (읽기 전용)"), Port);
	return true;
}
