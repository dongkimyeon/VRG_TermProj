// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "Simulation/Evaluation/VrgDashboardSummary.h"

class IHttpRouter;

// Web dashboard stage 1 (2026-10-13, Tools/웹대시보드_계획.md): engine HTTPServer module on loopback only
// (Config DefaultEngine.ini [HTTPServer.Listeners] DefaultBindAddress=127.0.0.1).
//   GET /dashboard     -> <Project>/Web/index.html (the engine router cannot bind "/") (static summary card, polls /api/snapshot)
//   GET /api/snapshot  -> latest published FVrgDashboardSummary JSON (503 before the first publish)
//   GET /api/series?since=<SimTime s> -> 1 s points after since (last 600 s) and event markers (stage 2)
//   GET /api/layout    -> plan view: equipment footprints (m) and inlet window means (K) (stage 3)
//   GET /api/racks     -> rack tab rows; GET /api/rack?i=<inlet> -> that rack's slots and parts (202 until built) (stage 4)
//   GET /api/airflow   -> sampled cell-centre air velocities for the 3D airflow layer (3D stage 3)
//   POST /api/report?name= -> queue a report save (202); GET /api/reports -> saved reports; GET /api/reportfile?name= (stage 5)
// Console: vrg.Web.Enable 0|1 (default 0), vrg.Web.Port (default 18080).
// Rule 5: the integrated actor publishes a JSON copy on the game thread; request handlers only read that copy under a
// lock and never touch actors, UObjects or solvers. The site only displays; it changes no physics input.
class VRG_TERMPROJ_API FVrgWebDashboard
{
public:
	static FVrgWebDashboard& Get();

	bool IsRunning() const { return Router.IsValid(); }
	int32 GetPort() const { return BoundPort; }
	void Publish(const FString& Json);
	FString GetLatestJson() const;
	void PublishSeries(FVrgDashboardSeries&& InSeries);
	// Empty before the first PublishSeries.
	FString GetSeriesJson(double SinceS) const;
	// Struct copy from the game thread; the JSON is built when a request asks for it (empty before the first publish).
	void PublishLayout(FVrgDashboardLayout&& InLayout);
	FString GetLayoutJson() const;
	// Rack tab: the actor publishes all rows plus the detail of the rack the page asked for last (RequestedRack).
	void PublishRacks(FVrgDashboardRacks&& InRacks);
	// 3D airflow layer: struct copy, JSON built per request (empty before the first publish).
	void PublishAirflow(FVrgDashboardAirflow&& InAirflow);
	FString GetAirflowJson() const;
	FString GetRacksJson() const;
	// Records Inlet as the requested rack; returns its detail JSON, or empty while the next publish has not built it.
	FString RequestRackDetailJson(int32 Inlet);
	int32 GetRequestedRack() const;

	// Stage 5: report save requests from the page (the only write the site makes). The game thread takes the request on
	// its next publish and writes it with the same function as vrg.Eval.Report; the result is shown in the report list.
	static bool IsValidReportName(const FString& Name);
	bool QueueReport(const FString& Name);
	bool TakePendingReport(FString& OutName);
	void SetLastSave(const FString& Name, bool bOk, const FString& Message);
	// Saved/Reports/*.json, newest first, with a few headline fields (file reads only; no actor access).
	FString GetReportListJson() const;
	// The file's text, or empty when the name is invalid or the file is missing.
	FString GetReportFileJson(const FString& Name) const;
	// Re-reads the console variables and starts/stops/moves the server; returns false and logs on failure.
	bool ApplySettings();
	void Stop();

private:
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> Routes;
	int32 BoundPort = 0;
	mutable FCriticalSection Lock;
	FString LatestJson;
	FVrgDashboardSeries LatestSeries;
	bool bHasSeries = false;
	FVrgDashboardLayout LatestLayout;
	bool bHasLayout = false;
	FVrgDashboardRacks LatestRacks;
	bool bHasRacks = false;
	FVrgDashboardAirflow LatestAirflow;
	bool bHasAirflow = false;
	int32 RequestedRack = INDEX_NONE;
	FString PendingReport;
	FString LastSaveName, LastSaveMessage;
	bool bHasLastSave = false, bLastSaveOk = false;
};
