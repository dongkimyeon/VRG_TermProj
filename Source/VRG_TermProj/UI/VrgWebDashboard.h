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
//   GET /api/sensors   -> virtual inlet sensors: reading, true air, lag/error spec, alarm (3D stage 4)
//   GET /api/cooling   -> CRAH units, CDUs, pumps and the power breakdown of the last step (tab 4)
//   GET /api/leaks     -> leaks, tanks, valve isolation / hydraulic impact per rack, fault judgements (S2b step 6)
//   POST /api/report?name= -> queue a report save (202); GET /api/reports -> saved reports; GET /api/reportfile?name= (stage 5)
//   GET /api/roomair   -> room air statistics and horizontal temperature slices (2026-11-11, 202 until built)
//   GET /api/status    -> plant live state, the report's plant / dew point / checks / water / instrument sections, step
//                         costs (2026-11-11, 202 until built)
//   GET /vendor/three.min.js, /vendor/OrbitControls.js -> <Project>/Web/vendor (offline 3D; the page falls back to the CDN)
// Console: vrg.Web.Enable 0|1 (default 0), vrg.Web.Port (default 18080), vrg.Web.AutoOpen 0|1 (default 1: the first entry
// into simulation mode turns the server on and opens the default browser; not in tests, commandlets or -unattended runs).
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
	void PublishSensors(FVrgDashboardSensors&& InSensors);
	FString GetSensorsJson() const;
	void PublishCooling(FVrgDashboardCooling&& InCooling);
	FString GetCoolingJson() const;
	void PublishLeaks(FVrgDashboardLeaks&& InLeaks);
	FString GetLeaksJson() const;
	FString GetRacksJson() const;
	// Records Inlet as the requested rack; returns its detail JSON, or empty while the next publish has not built it.
	FString RequestRackDetailJson(int32 Inlet);
	int32 GetRequestedRack() const;
	// 2026-11-11 tabs (room air, facility/validation): built on the game thread only while the page asked within the last
	// AskWindowS seconds. The request marks the time and returns the latest copy (empty before the first build -> 202).
	static constexpr double AskWindowS = 3.0;
	void PublishRoomAir(FVrgDashboardRoomAir&& InRoomAir);
	FString RequestRoomAirJson();
	bool WantsRoomAir() const;
	// The status JSON is built by the actor (the report's own section builders), so it is published as text.
	void PublishStatus(FString&& InJson);
	FString RequestStatusJson();
	bool WantsStatus() const;

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
	// vrg.Web.AutoOpen (default 1) and not an automation test, commandlet or -unattended run.
	static bool ShouldAutoOpen();
	// Called when simulation mode starts: the first time in this process (and only when ShouldAutoOpen), turns the server
	// on and opens http://127.0.0.1:<port>/dashboard in the default browser. Returns true when it opened the browser.
	bool AutoOpenOnSimulationStart();
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
	FVrgDashboardSensors LatestSensors;
	bool bHasSensors = false;
	FVrgDashboardCooling LatestCooling;
	bool bHasCooling = false;
	FVrgDashboardLeaks LatestLeaks;
	bool bHasLeaks = false;
	int32 RequestedRack = INDEX_NONE;
	FVrgDashboardRoomAir LatestRoomAir;
	bool bHasRoomAir = false;
	double RoomAirAskedS = -1.0e9;
	FString LatestStatus;
	double StatusAskedS = -1.0e9;
	FString PendingReport;
	FString LastSaveName, LastSaveMessage;
	bool bHasLastSave = false, bLastSaveOk = false;
};
