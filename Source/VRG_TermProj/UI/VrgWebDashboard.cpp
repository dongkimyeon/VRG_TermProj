// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/VrgWebDashboard.h"
#include "HAL/IConsoleManager.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

void FVrgWebDashboard::PublishLayout(FVrgDashboardLayout&& InLayout)
{
	FScopeLock Guard(&Lock);
	LatestLayout = MoveTemp(InLayout);
	bHasLayout = true;
}

FString FVrgWebDashboard::GetLayoutJson() const
{
	FScopeLock Guard(&Lock);
	return bHasLayout ? LatestLayout.ToJson() : FString();
}

void FVrgWebDashboard::PublishRacks(FVrgDashboardRacks&& InRacks)
{
	FScopeLock Guard(&Lock);
	LatestRacks = MoveTemp(InRacks);
	bHasRacks = true;
}

void FVrgWebDashboard::PublishAirflow(FVrgDashboardAirflow&& InAirflow)
{
	FScopeLock Guard(&Lock);
	LatestAirflow = MoveTemp(InAirflow);
	bHasAirflow = true;
}

FString FVrgWebDashboard::GetAirflowJson() const
{
	FScopeLock Guard(&Lock);
	return bHasAirflow ? LatestAirflow.ToJson() : FString();
}

void FVrgWebDashboard::PublishSensors(FVrgDashboardSensors&& InSensors)
{
	FScopeLock Guard(&Lock);
	LatestSensors = MoveTemp(InSensors);
	bHasSensors = true;
}

FString FVrgWebDashboard::GetSensorsJson() const
{
	FScopeLock Guard(&Lock);
	return bHasSensors ? LatestSensors.ToJson() : FString();
}

FString FVrgWebDashboard::GetRacksJson() const
{
	FScopeLock Guard(&Lock);
	return bHasRacks ? LatestRacks.RowsToJson() : FString();
}

FString FVrgWebDashboard::RequestRackDetailJson(int32 Inlet)
{
	FScopeLock Guard(&Lock);
	RequestedRack = Inlet;
	return bHasRacks && LatestRacks.bHasDetail && LatestRacks.Detail.Inlet == Inlet ? LatestRacks.DetailToJson() : FString();
}

int32 FVrgWebDashboard::GetRequestedRack() const
{
	FScopeLock Guard(&Lock);
	return RequestedRack;
}

bool FVrgWebDashboard::IsValidReportName(const FString& Name)
{
	if (Name.Len() < 1 || Name.Len() > 64) return false;
	for (const TCHAR C : Name)
	{
		const bool bOk = (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9'))
			|| C == TEXT('_') || C == TEXT('-') || (C >= 0xAC00 && C <= 0xD7A3);   // Hangul syllables; no dots, slashes or spaces
		if (!bOk) return false;
	}
	return true;
}

bool FVrgWebDashboard::QueueReport(const FString& Name)
{
	if (!IsValidReportName(Name)) return false;
	FScopeLock Guard(&Lock);
	PendingReport = Name;
	return true;
}

bool FVrgWebDashboard::TakePendingReport(FString& OutName)
{
	FScopeLock Guard(&Lock);
	if (PendingReport.IsEmpty()) return false;
	OutName = MoveTemp(PendingReport);
	PendingReport.Reset();
	return true;
}

void FVrgWebDashboard::SetLastSave(const FString& Name, bool bOk, const FString& Message)
{
	FScopeLock Guard(&Lock);
	LastSaveName = Name; LastSaveMessage = Message; bLastSaveOk = bOk; bHasLastSave = true;
}

namespace
{
	FString ReportDir() { return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports")); }
}

FString FVrgWebDashboard::GetReportListJson() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(ReportDir(), TEXT("*.json")), true, false);
	struct FItem { FString Name; FDateTime Modified; TSharedPtr<FJsonObject> Head; };
	TArray<FItem> Items;
	for (const FString& File : Files)
	{
		const FString Name = FPaths::GetBaseFilename(File);
		if (!IsValidReportName(Name)) continue;
		const FString Path = FPaths::Combine(ReportDir(), File);
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) continue;
		TSharedRef<FJsonObject> Head = MakeShared<FJsonObject>();
		Head->SetStringField(TEXT("name"), Name);
		double V = 0.0;
		if (Root->TryGetNumberField(TEXT("simTimeS"), V)) Head->SetNumberField(TEXT("simTimeS"), V);
		const TSharedPtr<FJsonObject>* Power = nullptr;
		if (Root->TryGetObjectField(TEXT("power"), Power))
		{
			if ((*Power)->TryGetNumberField(TEXT("itW"), V)) Head->SetNumberField(TEXT("itW"), V);
			if ((*Power)->TryGetNumberField(TEXT("partialPue"), V)) Head->SetNumberField(TEXT("partialPue"), V);
		}
		Head->SetBoolField(TEXT("hasLayout"), Root->HasField(TEXT("layout")));
		const FDateTime Modified = IFileManager::Get().GetTimeStamp(*Path);
		Head->SetStringField(TEXT("modified"), Modified.ToIso8601());
		Items.Add({ Name, Modified, Head });
	}
	Items.Sort([](const FItem& A, const FItem& B) { return A.Modified > B.Modified; });
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FItem& I : Items) List.Add(MakeShared<FJsonValueObject>(I.Head));
	Root->SetArrayField(TEXT("reports"), List);
	FScopeLock Guard(&Lock);
	if (PendingReport.IsEmpty()) Root->SetField(TEXT("pending"), MakeShared<FJsonValueNull>());
	else Root->SetStringField(TEXT("pending"), PendingReport);
	if (bHasLastSave)
	{
		TSharedRef<FJsonObject> Last = MakeShared<FJsonObject>();
		Last->SetStringField(TEXT("name"), LastSaveName); Last->SetBoolField(TEXT("ok"), bLastSaveOk); Last->SetStringField(TEXT("message"), LastSaveMessage);
		Root->SetObjectField(TEXT("lastSave"), Last);
	}
	else Root->SetField(TEXT("lastSave"), MakeShared<FJsonValueNull>());
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

FString FVrgWebDashboard::GetReportFileJson(const FString& Name) const
{
	FString Text;
	if (!IsValidReportName(Name) || !FFileHelper::LoadFileToString(Text, *FPaths::Combine(ReportDir(), Name + TEXT(".json")))) return FString();
	return Text;
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
	LatestLayout = FVrgDashboardLayout();
	bHasLayout = false;
	LatestRacks = FVrgDashboardRacks();
	bHasRacks = false;
	LatestAirflow = FVrgDashboardAirflow();
	bHasAirflow = false;
	LatestSensors = FVrgDashboardSensors();
	bHasSensors = false;
	RequestedRack = INDEX_NONE;
	PendingReport.Reset();
	bHasLastSave = false;
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
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/layout")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			const FString Json = GetLayoutJson();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_layout"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/airflow")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			const FString Json = GetAirflowJson();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_airflow"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/sensors")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			const FString Json = GetSensorsJson();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_sensors"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/racks")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			const FString Json = GetRacksJson();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_racks"), TEXT("시뮬레이션 액터가 아직 값을 내지 않았다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/rack")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			int32 Inlet = INDEX_NONE;
			if (const FString* I = Request.QueryParams.Find(TEXT("i"))) LexFromString(Inlet, **I);
			if (Inlet < 0)
			{
				OnComplete(NoStore(FHttpServerResponse::Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"), TEXT("i=<흡기 번호>가 필요하다"))));
				return true;
			}
			const FString Json = RequestRackDetailJson(Inlet);
			// 202: recorded; the game thread builds this rack's detail on its next publish (<= 0.5 s), ask again.
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::Accepted, TEXT("pending"), TEXT("다음 발행 때 만든다"))
				: FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/report")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FString* Name = Request.QueryParams.Find(TEXT("name"));
			const bool bQueued = Name != nullptr && QueueReport(*Name);
			// 202: the game thread writes it on its next publish (<= 0.5 s); /api/reports shows the result.
			OnComplete(NoStore(bQueued
				? FHttpServerResponse::Error(EHttpServerResponseCodes::Accepted, TEXT("queued"), *Name)
				: FHttpServerResponse::Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_name"), TEXT("이름은 영문·숫자·_·-·한글 1~64자"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/reports")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
		{
			OnComplete(NoStore(FHttpServerResponse::Create(GetReportListJson(), TEXT("application/json; charset=utf-8"))));
			return true;
		})));
	Routes.Add(Router->BindRoute(FHttpPath(TEXT("/api/reportfile")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			const FString* Name = Request.QueryParams.Find(TEXT("name"));
			const FString Json = Name != nullptr ? GetReportFileJson(*Name) : FString();
			OnComplete(NoStore(Json.IsEmpty()
				? FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound, TEXT("no_report"), TEXT("그 이름의 리포트가 없다"))
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
