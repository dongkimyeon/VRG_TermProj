// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/LayoutBarrierActor.h"
#include "EngineUtils.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/RackThermalActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Simulation/Air/AirSolver.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/Engine.h"
#include "Visualization/GjRackLayout.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UI/VrgWebDashboard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Every numeric field of a rack row, for comparing two parsed copies.
	void VisitFieldsForTest(const FVrgDashboardRackRow& R, TArray<double>& Out)
	{
		Out = { R.XM, R.YM, R.Utilization, R.HeatW, R.ToCoolantW, R.ToRoomAirW, R.InletNowK, R.CoolantInK, R.CoolantOutK,
			R.CoolantFlowM3s, R.FanSpeedMean, R.FanPowerW, R.HottestPartK, double(R.NumSlots) };
	}
}

// Web dashboard stage 1 (2026-10-13): the summary card JSON round-trips, its window values are exactly the report's
// (same FVrgEvaluation calls), its "now" values are the coupling snapshot, and the server publishes only while enabled.
// The HTTP transport itself is checked by hand in -game (Tools/웹대시보드_계획.md).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardSummaryTest, "VRG.Web.DashboardSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardSummaryTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	for (const double X : { -150.0, 150.0 })
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(X, 0, 0), FRotator(0, X < 0 ? 0 : 180, 0));
		Rack->Params = FRackThermalModel::MakeGb200Nvl72Params();
		Rack->Input = FRackThermalInput();
		Rack->RackSizeM = FVector(1.068, .6, 2.236);
		Rack->bShowReadout = false;
	}
	AAirFieldDebugActor* Air = World->SpawnActor<AAirFieldDebugActor>();
	Air->bShowSummary = false;
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->EvalSettings.WindowS = 2.0;
	ADataCenterSimulationActor::FEvalEvent Marker;   // stage 2: shows up as a vertical line on the series
	Marker.TimeS = 1.0; Marker.Kind = TEXT("util"); Marker.Value = .9;
	Sim->EvalEvents.Add(Marker);

	Sim->EnterLayoutMode();
	const FVrgDashboardSummary Layout = Sim->BuildDashboardSummary();
	TestFalse(TEXT("layout mode is reported as layout"), Layout.bSimulating);

	Sim->EnterSimulationMode();
	for (int32 F = 0; F < 10; ++F) Sim->Tick(.32f);   // 3.2 s
	const FVrgDashboardSummary S = Sim->BuildDashboardSummary();
	TestTrue(TEXT("simulation mode with evaluation samples"), S.bSimulating && S.bEvalReady && S.InletCount > 0);

	FVrgDashboardSummary Back;
	TestTrue(TEXT("JSON round-trip parses"), FVrgDashboardSummary::FromJson(S.ToJson(), Back));
	TestTrue(TEXT("JSON round-trip is exact"), Back.SimTimeS == S.SimTimeS && Back.ItWindowW == S.ItWindowW && Back.ItNowW == S.ItNowW
		&& Back.CoolantRemovedNowW == S.CoolantRemovedNowW && Back.AirRemovedNowW == S.AirRemovedNowW
		&& Back.bPartialPueValid == S.bPartialPueValid && Back.PartialPueWindow == S.PartialPueWindow
		&& Back.InletWindowMeanMaxK == S.InletWindowMeanMaxK && Back.InletWindowMeanAvgK == S.InletWindowMeanAvgK
		&& Back.InletCount == S.InletCount && Back.WindowS == S.WindowS && Back.bSimulating == S.bSimulating);

	// Same SimTime: the dashboard's window values must be the report's numbers (K stays K; the report writes C = K - 273.15).
	FString Summary;
	TestTrue(TEXT("report written"), Sim->WriteEvaluationReport(TEXT("AutomationWebDashboard"), Summary));
	FString Json;
	TSharedPtr<FJsonObject> Report;
	TestTrue(TEXT("report parsed"), FFileHelper::LoadFileToString(Json, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Reports"), TEXT("AutomationWebDashboard.json")))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report) && Report.IsValid());
	if (Report.IsValid())
	{
		const TSharedPtr<FJsonObject> Power = Report->GetObjectField(TEXT("power")), Sum = Report->GetObjectField(TEXT("summary"));
		TestEqual(TEXT("IT window = report"), S.ItWindowW, Power->GetNumberField(TEXT("itW")));
		TestEqual(TEXT("partial PUE = report"), S.PartialPueWindow, Power->GetNumberField(TEXT("partialPue")));
		TestEqual(TEXT("inlet max = report"), S.InletWindowMeanMaxK - 273.15, Sum->GetNumberField(TEXT("inletWindowMeanMaxC")));
		TestEqual(TEXT("inlet mean = report"), S.InletWindowMeanAvgK - 273.15, Sum->GetNumberField(TEXT("inletWindowMeanAvgC")));
		TestEqual(TEXT("window = report"), S.WindowS, Report->GetNumberField(TEXT("windowS")));
	}
	const FCouplingSnapshot Snap = Sim->GetSnapshot();
	TestEqual(TEXT("IT now = snapshot"), S.ItNowW, Snap.TotalItHeatW);
	TestEqual(TEXT("coolant removed = snapshot"), S.CoolantRemovedNowW, Snap.TotalRadiatorRejectedW);
	TestEqual(TEXT("air removed = snapshot"), S.AirRemovedNowW, Snap.TotalAirCoolingRemovedW);
	TestTrue(TEXT("plausible: positive IT, removal and PUE >= 1"), S.ItWindowW > 0.0 && S.CoolantRemovedNowW + S.AirRemovedNowW > 0.0
		&& S.bPartialPueValid && S.PartialPueWindow >= 1.0);

	// Server: nothing published while off; enabled -> the actor's Tick publishes a copy equal to its summary; off again -> cleared.
	IConsoleVariable* Enable = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Enable"));
	IConsoleVariable* Port = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Web.Port"));
	FVrgWebDashboard& Web = FVrgWebDashboard::Get();
	TestTrue(TEXT("console variables exist"), Enable != nullptr && Port != nullptr);
	if (Enable != nullptr && Port != nullptr)
	{
		const int32 OldPort = Port->GetInt();
		Port->Set(18093, ECVF_SetByCode);
		Enable->Set(0, ECVF_SetByCode);
		Sim->Tick(.02f);
		TestTrue(TEXT("off: no server, nothing published"), !Web.IsRunning() && Web.GetLatestJson().IsEmpty());
		Enable->Set(1, ECVF_SetByCode);
		TestTrue(TEXT("on: listening on the set port"), Web.IsRunning() && Web.GetPort() == 18093);
		Sim->Tick(.02f);
		FVrgDashboardSummary Published;
		TestTrue(TEXT("published copy parses"), FVrgDashboardSummary::FromJson(Web.GetLatestJson(), Published));
		const FVrgDashboardSummary Now = Sim->BuildDashboardSummary();
		TestTrue(TEXT("published copy = summary at that SimTime"), Published.SimTimeS == Now.SimTimeS && Published.ItWindowW == Now.ItWindowW
			&& Published.InletWindowMeanMaxK == Now.InletWindowMeanMaxK);

		// Stage 2 series: one point per closed second without gaps, the last WindowS points average to the report window,
		// "since" returns only newer points, and the event marker is carried with its applied flag.
		FVrgDashboardSeries Series, Newer;
		TestTrue(TEXT("series parses"), FVrgDashboardSeries::FromJson(Web.GetSeriesJson(-1.0), Series));
		const FVrgEvaluation& Eval = Sim->GetEvaluation();
		TestTrue(TEXT("series generation and length"), Series.Generation == Eval.GetGeneration() && Series.LengthS == 600.0);
		TestEqual(TEXT("one point per closed second (3.22 s -> 3)"), Series.Points.Num(), 3);
		bool bGapless = Series.Points.Num() > 0 && Series.Points[0].TimeS == 1.0;
		for (int32 I = 1; I < Series.Points.Num(); ++I) bGapless &= Series.Points[I].TimeS - Series.Points[I - 1].TimeS == 1.0;
		TestTrue(TEXT("1 s spacing, no gaps"), bGapless);
		if (Series.Points.Num() >= 2)
		{
			const int32 N = Series.Points.Num();
			const double SeriesItW = (Series.Points[N - 2].ItW + Series.Points[N - 1].ItW) / 2.0;
			const double WindowItW = Eval.GetWindowPower().ItW;
			AddInfo(FString::Printf(TEXT("series last-2 IT %.6f W vs window %.6f W, relative %.3e"), SeriesItW, WindowItW,
				FMath::Abs(SeriesItW - WindowItW) / WindowItW));
			TestTrue(TEXT("last WindowS points = report window IT (relative < 1e-12)"), FMath::Abs(SeriesItW - WindowItW) <= 1e-12 * WindowItW);
			bool bPlausible = true;
			for (const FVrgSeriesPoint& P : Series.Points)
				bPlausible &= P.ItW > 0.0 && P.CoolantRemovedW + P.AirRemovedW > 0.0 && P.InletMaxK >= P.InletMeanK
					&& P.InletMeanK > 273.15 + 15.0 && P.InletMaxK < 273.15 + 35.0 && P.bPartialPueValid && P.PartialPue >= 1.0;
			TestTrue(TEXT("points plausible (IT > 0, removal > 0, max >= mean, 15..35 C, PUE >= 1)"), bPlausible);
		}
		TestTrue(TEXT("since filter"), FVrgDashboardSeries::FromJson(Web.GetSeriesJson(2.0), Newer) && Newer.Points.Num() == 1
			&& Newer.Points[0].TimeS == 3.0);
		TestTrue(TEXT("event marker carried and applied"), Series.Events.Num() == 1 && Series.Events[0].TimeS == 1.0
			&& Series.Events[0].Kind == TEXT("util") && Series.Events[0].bApplied);

		// Stage 3 plan view: footprints in metres at the actor transforms, inlet colours = the evaluation's window means.
		ALayoutBarrierActor* Wall = World->SpawnActor<ALayoutBarrierActor>(FVector(0, 300, 0), FRotator::ZeroRotator);
		Wall->SetActorScale3D(FVector(.2, 2.0, 2.4));   // barrier size [m] is its scale
		Sim->Tick(.02f);   // the 0.5 s real-time throttle may skip this publish; build directly as well
		FVrgDashboardLayout Plan, Direct = Sim->BuildDashboardLayout();
		TestTrue(TEXT("layout JSON round-trip"), FVrgDashboardLayout::FromJson(Direct.ToJson(), Plan));
		TestEqual(TEXT("inlet means: one per inlet"), Plan.InletWindowMeanK.Num(), Eval.GetNumInlets());
		bool bMeansExact = Plan.InletWindowMeanK.Num() == Eval.GetNumInlets();
		for (int32 I = 0; bMeansExact && I < Eval.GetNumInlets(); ++I) bMeansExact = Plan.InletWindowMeanK[I] == Eval.GetInlet(I).WindowMeanK;
		TestTrue(TEXT("inlet means = evaluation window means (exact)"), bMeansExact);
		TestTrue(TEXT("domain present"), Plan.bHasDomain && Plan.DomainMaxXM > Plan.DomainMinXM && Plan.DomainMaxYM > Plan.DomainMinYM && Plan.DomainMaxZM >= 2.236 && Plan.DomainMinZM <= 0.0);
		int32 PlanRackCount = 0, AutoCrahs = 0, Walls = 0;
		TSet<int32> SeenInlets;
		for (const FVrgDashboardEquipment& E : Plan.Equipment)
		{
			if (E.Kind == TEXT("rack"))
			{
				++PlanRackCount;
				SeenInlets.Add(E.InletIndex);
				const bool bLeft = E.CenterXM < 0.0;
				TestTrue(TEXT("rack footprint at the actor (m) with its size and yaw"), FMath::Abs(E.CenterXM - (bLeft ? -1.5 : 1.5)) < 1e-12
					&& FMath::Abs(E.CenterYM) < 1e-12 && E.DepthM == 1.068 && E.WidthM == .6 && E.HeightM == 2.236 && FMath::Abs(E.BaseZM) < 1e-12
					&& FMath::Abs(FRotator::NormalizeAxis(E.YawDeg - (bLeft ? 0.0 : 180.0))) < 1e-9);
				TestTrue(TEXT("rack centre inside the air domain"), E.CenterXM > Plan.DomainMinXM && E.CenterXM < Plan.DomainMaxXM
					&& E.CenterYM > Plan.DomainMinYM && E.CenterYM < Plan.DomainMaxYM);
			}
			if (E.bAutoPlaced) { ++AutoCrahs; TestEqual(TEXT("automatic entries are CRAH units"), E.Kind, FString(TEXT("crah"))); }
			if (E.Kind == TEXT("barrier"))
			{
				++Walls;
				TestTrue(TEXT("barrier footprint from its scale"), FMath::Abs(E.DepthM - .2) < 1e-12 && FMath::Abs(E.WidthM - 2.0) < 1e-12
					&& FMath::Abs(E.CenterYM - 3.0) < 1e-12 && FMath::Abs(E.HeightM - 2.4) < 1e-12);
			}
		}
		TestEqual(TEXT("both racks listed"), PlanRackCount, 2);
		// Every equipment actor (PlaceableBoxM) appears once; the rest of the entries are automatic CRAH units.
		int32 FullWalk = 0;
		FVector BoxM;
		for (TActorIterator<AActor> It(World); It; ++It) FullWalk += ADataCenterDebugController::PlaceableBoxM(*It, BoxM) ? 1 : 0;
		TestEqual(TEXT("listed equipment = every PlaceableBoxM actor"), Plan.Equipment.Num() - AutoCrahs, FullWalk);
		TestTrue(TEXT("rack inlet indices 0 and 1"), SeenInlets.Num() == 2 && SeenInlets.Contains(0) && SeenInlets.Contains(1));
		TestEqual(TEXT("one barrier listed"), Walls, 1);
		AddInfo(FString::Printf(TEXT("plan view: %d entries, automatic CRAH units %d"), Plan.Equipment.Num(), AutoCrahs));
		TestTrue(TEXT("published layout parses"), Web.GetLayoutJson().IsEmpty() || FVrgDashboardLayout::FromJson(Web.GetLayoutJson(), Plan));

		// Stage 4 rack tab: rows are the models' last-step values; the detail of one rack sums back to its totals.
		const FCouplingSnapshot RackSnap = Sim->GetSnapshot();
		FVrgDashboardRacks Rows;
		TestTrue(TEXT("rack rows JSON round-trip"), FVrgDashboardRacks::RowsFromJson(Sim->BuildDashboardRacks(INDEX_NONE).RowsToJson(), Rows));
		TestEqual(TEXT("one row per inlet"), Rows.Rows.Num(), Eval.GetNumInlets());
		for (const FVrgDashboardRackRow& R : Rows.Rows)
		{
			ARackThermalActor* RackActor = nullptr;
			for (TActorIterator<ARackThermalActor> It(World); It; ++It) if (It->GetName() == R.Name) RackActor = *It;
			if (!TestNotNull(TEXT("row names a rack actor"), RackActor)) continue;
			const FRackThermalModel& M = RackActor->GetMutableModel();
			TestTrue(TEXT("row = model last-step values"), R.HeatW == M.GetLastTotalHeatW() && R.ToCoolantW == M.GetLastHeatRemovedByCoolantW()
				&& R.ToRoomAirW == M.GetLastTotalHeatToRoomAirW() && R.CoolantOutK == M.GetState().CoolantTempK && R.FanPowerW == M.GetLastTotalFanPowerW());
			TestTrue(TEXT("row inlet = coupling inlet, coolant flow = coupling flow / density"),
				RackSnap.RackAirInletTempK.IsValidIndex(R.Inlet) && R.InletNowK == RackSnap.RackAirInletTempK[R.Inlet]
				&& FMath::Abs(R.CoolantFlowM3s - RackSnap.RackCoolantMassFlowKgs[R.Inlet] / Sim->CoolantDensityKgM3) <= 1e-15 * R.CoolantFlowM3s);
			TestTrue(TEXT("row plausible (heat > 0, coolant out > in, docked = slots, hottest part named)"), R.HeatW > 0.0 && R.CoolantOutK > R.CoolantInK
				&& R.DockedSlots == R.NumSlots && R.NumSlots > 0 && !R.HottestPart.IsEmpty() && R.HottestPartK > R.InletNowK);
		}
		const FVrgDashboardRacks WithDetail = Sim->BuildDashboardRacks(1);
		FVrgDashboardRackDetail Detail;
		TestTrue(TEXT("detail JSON round-trip"), WithDetail.bHasDetail && FVrgDashboardRacks::DetailFromJson(WithDetail.DetailToJson(), Detail));
		if (WithDetail.bHasDetail && Rows.Rows.IsValidIndex(1))
		{
			int32 Parts = 0;
			double PartHeatW = 0.0, PartCoolantW = 0.0, SlotHeatW = 0.0;
			for (const FVrgDashboardRackSlot& DetailSlot : Detail.Slots)
			{
				Parts += DetailSlot.Parts.Num(); SlotHeatW += DetailSlot.HeatW;
				for (const FVrgDashboardRackPart& Q : DetailSlot.Parts) { PartHeatW += Q.HeatW; PartCoolantW += Q.ToCoolantW; }
			}
			const FVrgDashboardRackRow& Row1 = Rows.Rows[1];
			ARackThermalActor* RackActor = nullptr;
			for (TActorIterator<ARackThermalActor> It(World); It; ++It) if (It->GetName() == Row1.Name) RackActor = *It;
			const FRackThermalModel& M = RackActor->GetMutableModel();
			TestTrue(TEXT("detail: one slot per sled, one part per node"), Detail.Slots.Num() == M.GetNumSleds() && Parts == M.GetNumNodes());
			const double HeatRel = FMath::Abs(PartHeatW - Detail.TotalHeatW) / Detail.TotalHeatW;
			const double CoolantRel = FMath::Abs(PartCoolantW - Detail.ToCoolantW) / Detail.ToCoolantW;
			AddInfo(FString::Printf(TEXT("rack detail: parts %d, sum part heat vs total %.3e, sum part to-coolant vs total %.3e, total %.1f W vs row %.1f W"),
				Parts, HeatRel, CoolantRel, Detail.TotalHeatW, Row1.HeatW));
			TestTrue(TEXT("detail: part heat sums to the rack total (relative < 1e-12)"), HeatRel < 1e-12 && FMath::Abs(SlotHeatW - PartHeatW) <= 1e-12 * PartHeatW);
			TestTrue(TEXT("detail: part to-coolant sums to the rack total (relative < 1e-12)"), CoolantRel < 1e-12);
			TestTrue(TEXT("detail total = row heat (same last step)"), FMath::Abs(Detail.TotalHeatW - Row1.HeatW) <= 1e-12 * Row1.HeatW);
		}
		// The direct writer (now served) and the FJsonObject reference read back to the same values, bit for bit.
		if (WithDetail.bHasDetail)
		{
			FVrgDashboardRacks FastRows, DomRows;
			FVrgDashboardRackDetail FastDetail, DomDetail;
			const bool bParsed = FVrgDashboardRacks::RowsFromJson(WithDetail.RowsToJson(), FastRows) && FVrgDashboardRacks::RowsFromJson(WithDetail.RowsToJsonDom(), DomRows)
				&& FVrgDashboardRacks::DetailFromJson(WithDetail.DetailToJson(), FastDetail) && FVrgDashboardRacks::DetailFromJson(WithDetail.DetailToJsonDom(), DomDetail);
			bool bSame = bParsed && FastRows.Rows.Num() == DomRows.Rows.Num() && FastDetail.Slots.Num() == DomDetail.Slots.Num() && FastRows.SimTimeS == DomRows.SimTimeS;
			for (int32 I = 0; bSame && I < FastRows.Rows.Num(); ++I)
			{
				FVrgDashboardRackRow &F = FastRows.Rows[I], &D = DomRows.Rows[I];
				bSame = F.Inlet == D.Inlet && F.Name == D.Name && F.HottestPart == D.HottestPart && F.DockedSlots == D.DockedSlots;
				TArray<double> FV, DV;
				VisitFieldsForTest(F, FV); VisitFieldsForTest(D, DV);
				bSame &= FV == DV;
			}
			bSame &= FastDetail.TotalHeatW == DomDetail.TotalHeatW && FastDetail.ToCoolantW == DomDetail.ToCoolantW && FastDetail.MixedAirOutletK == DomDetail.MixedAirOutletK;
			for (int32 SlotI = 0; bSame && SlotI < FastDetail.Slots.Num(); ++SlotI)
			{
				const FVrgDashboardRackSlot &F = FastDetail.Slots[SlotI], &D = DomDetail.Slots[SlotI];
				bSame = F.Index == D.Index && F.Device == D.Device && F.bDocked == D.bDocked && F.HeatW == D.HeatW && F.OutletAirK == D.OutletAirK
					&& F.Parts.Num() == D.Parts.Num();
				for (int32 K = 0; bSame && K < F.Parts.Num(); ++K)
					bSame = F.Parts[K].Name == D.Parts[K].Name && F.Parts[K].Kind == D.Parts[K].Kind && F.Parts[K].TempK == D.Parts[K].TempK
						&& F.Parts[K].HeatW == D.Parts[K].HeatW && F.Parts[K].ToCoolantW == D.Parts[K].ToCoolantW && F.Parts[K].ToAirW == D.Parts[K].ToAirW;
			}
			TestTrue(TEXT("direct writer = FJsonObject reference (every row, slot and part, bit-exact)"), bSame);
			auto Time = [](auto&& Body) { const uint64 T0 = FPlatformTime::Cycles64(); for (int32 R = 0; R < 50; ++R) Body(); return FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - T0) / 50; };
			FString Sink;
			const double RowsDom = Time([&] { Sink = WithDetail.RowsToJsonDom(); }), DetailDom = Time([&] { Sink = WithDetail.DetailToJsonDom(); });
			const double RowsFast = Time([&] { Sink = WithDetail.RowsToJson(); }), DetailFast = Time([&] { Sink = WithDetail.DetailToJson(); });
			AddInfo(FString::Printf(TEXT("rack JSON (2 racks, 613 parts, 50 runs): rows %.4f -> %.4f ms, detail %.4f -> %.4f ms"), RowsDom, RowsFast, DetailDom, DetailFast));
		}
		// Strings are escaped (quote, backslash, control characters) and read back unchanged.
		{
			FVrgDashboardRacks Odd;
			FVrgDashboardRackRow& R = Odd.Rows.AddDefaulted_GetRef();
			R.Name = TEXT("a\"b\\c\nd\te"); R.Kind = TEXT("rack"); R.HottestPart = TEXT("슬롯 1 \"x\"");
			FVrgDashboardRacks OddBack;
			TestTrue(TEXT("escaped strings round-trip"), FVrgDashboardRacks::RowsFromJson(Odd.RowsToJson(), OddBack) && OddBack.Rows.Num() == 1
				&& OddBack.Rows[0].Name == R.Name && OddBack.Rows[0].HottestPart == R.HottestPart);
		}

		// Server path: the first ask records the rack (202 pending), the next publish builds it, then the detail is served.
		TestTrue(TEXT("first ask is pending and recorded"), Web.RequestRackDetailJson(1).IsEmpty() && Web.GetRequestedRack() == 1);
		Web.PublishRacks(Sim->BuildDashboardRacks(Web.GetRequestedRack()));
		TestFalse(TEXT("after a publish the detail is served"), Web.RequestRackDetailJson(1).IsEmpty());
		TestFalse(TEXT("rows served"), Web.GetRacksJson().IsEmpty());

		// Stage 5 report save: names are checked, the game thread writes the queued report with the same writer as
		// vrg.Eval.Report, and the file carries the plan view and window heat removal at that SimTime.
		TestTrue(TEXT("valid report names"), FVrgWebDashboard::IsValidReportName(TEXT("AB_1")) && FVrgWebDashboard::IsValidReportName(TEXT("a-b"))
			&& FVrgWebDashboard::IsValidReportName(TEXT("컨테인먼트비교")));
		TestTrue(TEXT("rejected report names"), !FVrgWebDashboard::IsValidReportName(TEXT("")) && !FVrgWebDashboard::IsValidReportName(TEXT("../x"))
			&& !FVrgWebDashboard::IsValidReportName(TEXT("a.b")) && !FVrgWebDashboard::IsValidReportName(TEXT("a b"))
			&& !FVrgWebDashboard::IsValidReportName(FString::ChrN(65, TEXT('a'))));
		TestFalse(TEXT("bad name not queued"), Web.QueueReport(TEXT("../evil")));
		TestTrue(TEXT("bad name file not served"), Web.GetReportFileJson(TEXT("../evil")).IsEmpty());
		TestTrue(TEXT("report queued"), Web.QueueReport(TEXT("AutoWebReport")));
		TestTrue(TEXT("queued report handled on the game thread"), Sim->ProcessDashboardRequests());
		TestFalse(TEXT("nothing left to handle"), Sim->ProcessDashboardRequests());
		TSharedPtr<FJsonObject> Saved;
		const FString SavedText = Web.GetReportFileJson(TEXT("AutoWebReport"));
		if (TestTrue(TEXT("saved report parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SavedText), Saved) && Saved.IsValid()))
		{
			const FVrgEvaluationSample W = Eval.GetWindowPower();
			const TSharedPtr<FJsonObject> Power = Saved->GetObjectField(TEXT("power"));
			TestTrue(TEXT("report window heat removal = evaluation window (exact)"), Power->GetNumberField(TEXT("coolantRemovedW")) == W.CoolantRemovedW
				&& Power->GetNumberField(TEXT("airRemovedW")) == W.AirRemovedW && W.CoolantRemovedW > 0.0);
			const TSharedPtr<FJsonObject>* LayoutObj = nullptr;
			FVrgDashboardLayout SavedPlan;
			FString LayoutText;
			if (TestTrue(TEXT("report carries the plan view"), Saved->TryGetObjectField(TEXT("layout"), LayoutObj)))
				FJsonSerializer::Serialize(LayoutObj->ToSharedRef(), TJsonWriterFactory<>::Create(&LayoutText));
			TestTrue(TEXT("plan view in the report = plan view now"), FVrgDashboardLayout::FromJson(LayoutText, SavedPlan)
				&& SavedPlan.Equipment.Num() == Sim->BuildDashboardLayout().Equipment.Num() && SavedPlan.InletWindowMeanK.Num() == Eval.GetNumInlets());
		}
		TSharedPtr<FJsonObject> List;
		bool bListed = false, bLastOk = false;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Web.GetReportListJson()), List) && List.IsValid())
		{
			for (const TSharedPtr<FJsonValue>& V : List->GetArrayField(TEXT("reports"))) bListed |= V->AsObject()->GetStringField(TEXT("name")) == TEXT("AutoWebReport");
			const TSharedPtr<FJsonObject>* Last = nullptr;
			bLastOk = List->TryGetObjectField(TEXT("lastSave"), Last) && (*Last)->GetBoolField(TEXT("ok")) && (*Last)->GetStringField(TEXT("name")) == TEXT("AutoWebReport");
		}
		TestTrue(TEXT("report list shows it and the last save succeeded"), bListed && bLastOk);

		Enable->Set(0, ECVF_SetByCode);
		TestTrue(TEXT("off again: stopped and cleared"), !Web.IsRunning() && Web.GetLatestJson().IsEmpty() && Web.GetSeriesJson(-1.0).IsEmpty()
			&& Web.GetLayoutJson().IsEmpty() && Web.GetRacksJson().IsEmpty() && Web.GetRequestedRack() == INDEX_NONE);
		Port->Set(OldPort, ECVF_SetByCode);
	}
	World->DestroyWorld(false);
	return true;
}

// Stage 2: the 1 s series points are the averages of the step values over each second on the integrated clock.
// Synthetic inputs (not equipment data): IT ramps linearly, one inlet ramps, the other is constant. Steps hand in the value
// at the step end (right sum), so second k averages the ramp at k + 0.51 s for dt = 0.02 s.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardSeriesAnalyticTest, "VRG.Web.SeriesAnalytic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardSeriesAnalyticTest::RunTest(const FString& Parameters)
{
	FVrgEvaluationSettings Settings;
	Settings.WindowS = 3.0;
	Settings.SeriesLengthS = 5.0;   // small ring so 8 s overflows it
	FVrgEvaluation Eval;
	Eval.Reset(Settings, 2, { 300.0, 290.0 });
	const int32 FirstGeneration = Eval.GetGeneration();
	const double Dt = .02;
	for (int32 J = 1; J <= 400; ++J)   // 8 s
	{
		const double T = J * Dt;
		FVrgEvaluationSample P;
		P.ItW = 1000.0 + 100.0 * T; P.CrahFanW = 10.0; P.PumpW = 5.0; P.CoolantRemovedW = 700.0; P.AirRemovedW = 200.0;
		Eval.AfterStep(T, Dt, { 300.0, 290.0 + T }, P);
	}
	TArray<FVrgSeriesPoint> Points, Newer;
	Eval.GetSeries(-1.0, Points);
	TestEqual(TEXT("ring keeps the last SeriesLengthS points"), Points.Num(), 5);
	double MaxError = 0.0;
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		const FVrgSeriesPoint& P = Points[I];
		const double K = 3.0 + I;                       // second [K, K+1) -> TimeS = K + 1
		const double ItW = 1000.0 + 100.0 * (K + .51);
		const double Ramp = 290.0 + K + .51;
		TestEqual(TEXT("oldest-first, 1 s spacing"), P.TimeS, K + 1.0);
		MaxError = FMath::Max(MaxError, FMath::Abs(P.ItW - ItW) / ItW);
		MaxError = FMath::Max(MaxError, FMath::Abs(P.InletMaxK - FMath::Max(300.0, Ramp)) / 300.0);
		MaxError = FMath::Max(MaxError, FMath::Abs(P.InletMeanK - (300.0 + Ramp) / 2.0) / 300.0);
		MaxError = FMath::Max(MaxError, FMath::Abs(P.PartialPue - (ItW + 15.0) / ItW));
		MaxError = FMath::Max(MaxError, FMath::Abs(P.CoolantRemovedW - 700.0) / 700.0 + FMath::Abs(P.AirRemovedW - 200.0) / 200.0);
		TestTrue(TEXT("PUE valid"), P.bPartialPueValid);
	}
	AddInfo(FString::Printf(TEXT("max relative error vs analytic 1 s averages: %.3e"), MaxError));
	TestTrue(TEXT("1 s averages match the analytic values (relative < 1e-12)"), MaxError < 1e-12);
	Eval.GetSeries(6.0, Newer);
	TestTrue(TEXT("since 6 s -> points at 7 and 8 s"), Newer.Num() == 2 && Newer[0].TimeS == 7.0 && Newer[1].TimeS == 8.0);

	// JSON round-trip keeps every value bit-exact.
	FVrgDashboardSeries Payload, Back;
	Payload.Generation = Eval.GetGeneration(); Payload.SimTimeS = 8.0; Payload.LengthS = Settings.SeriesLengthS; Payload.Points = Points;
	FVrgDashboardEvent E; E.TimeS = 4.0; E.Kind = TEXT("rackutil"); E.Index = 2; E.Value = .5; E.bApplied = true;
	Payload.Events.Add(E);
	TestTrue(TEXT("series JSON parses"), FVrgDashboardSeries::FromJson(Payload.ToJson(-1.0), Back));
	bool bExact = Back.Points.Num() == Points.Num() && Back.Events.Num() == 1 && Back.Events[0].Index == 2 && Back.Events[0].bApplied;
	for (int32 I = 0; bExact && I < Points.Num(); ++I)
		bExact = Back.Points[I].TimeS == Points[I].TimeS && Back.Points[I].ItW == Points[I].ItW && Back.Points[I].InletMaxK == Points[I].InletMaxK
			&& Back.Points[I].InletMeanK == Points[I].InletMeanK && Back.Points[I].PartialPue == Points[I].PartialPue
			&& Back.Points[I].CoolantRemovedW == Points[I].CoolantRemovedW && Back.Points[I].AirRemovedW == Points[I].AirRemovedW;
	TestTrue(TEXT("series JSON round-trip is exact"), bExact);

	Eval.Reset(Settings, 2, { 300.0, 290.0 });
	Eval.GetSeries(-1.0, Points);
	TestTrue(TEXT("reset clears the series and bumps the generation"), Points.Num() == 0 && Eval.GetGeneration() == FirstGeneration + 1);
	return true;
}

// 3D view stage 2 (2026-10-14): the dashboard pipes are the room actor's drawn paths for the solved network pipes,
// with this step's solved flow and only the model's own temperatures (rack branches), in both mesh styles.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardPipesTest, "VRG.Web.PipesMatchNetwork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardPipesTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
	TArray<ARackThermalActor*> Racks;
	for (const double Y : { 0.0, 66.0 })   // one row of two racks (synthetic placement)
	{
		ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
		Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
		Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
		Racks.Add(Rack);
	}
	World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);

	for (const bool bGj : { false, true })
	{
		const TCHAR* Style = bGj ? TEXT("GJ") : TEXT("plain");
		Room->bUseGjConnections = bGj;
		Room->RebuildRoom();      // a style switch alone does not change the layout hash: ask for the rebuild
		Room->RebuildRoomNow();
		Sim->EnterSimulationMode(); Sim->Tick(.021f);
		for (int32 I = 0; I < 10; ++I) Sim->Tick(.04f);
		const FHydraulicNetwork& Net = Room->GetNetwork();
		const FVrgDashboardLayout Direct = Sim->BuildDashboardLayout();
		FVrgDashboardLayout Plan;
		TestTrue(FString::Printf(TEXT("%s: layout with pipes round-trips"), Style), FVrgDashboardLayout::FromJson(Direct.ToJson(), Plan));
		TestTrue(FString::Printf(TEXT("%s: hydraulics converged"), Style), Plan.bHydraulicsConverged);

		// Every solved pipe is drawn exactly once; the pump links are the only extra paths.
		TArray<int32> Drawn; Drawn.Init(0, Net.GetNumPipes());
		bool bFlowExact = Plan.Pipes.Num() == Direct.Pipes.Num(), bPointsExact = bFlowExact, bTempRule = true;
		int32 PumpLinks = 0, WithTemp = 0;
		for (int32 I = 0; I < Plan.Pipes.Num(); ++I)
		{
			const FVrgDashboardPipe& P = Plan.Pipes[I];
			bPointsExact &= P.PointsM.Num() >= 2 && I < Direct.Pipes.Num() && P.PointsM == Direct.Pipes[I].PointsM;
			if (P.Edge < 0) { ++PumpLinks; bFlowExact &= P.FlowM3s == Net.GetPumpFlowM3s(-1 - P.Edge); bTempRule &= !P.bTempValid; continue; }
			if (!Drawn.IsValidIndex(P.Edge)) { bFlowExact = false; continue; }
			++Drawn[P.Edge];
			bFlowExact &= P.FlowM3s == Net.GetPipeFlowM3s(P.Edge);
			double T = 0.0;
			const bool bModel = Room->TryGetPipeTemperatureK(P.Edge, T);
			bTempRule &= P.bTempValid == bModel && (!bModel || P.TempK == T);
			WithTemp += P.bTempValid ? 1 : 0;
		}
		int32 Missing = 0, Twice = 0;
		for (const int32 C : Drawn) { Missing += C == 0; Twice += C > 1; }
		TestEqual(FString::Printf(TEXT("%s: every network pipe drawn"), Style), Missing, 0);
		TestEqual(FString::Printf(TEXT("%s: no pipe drawn twice"), Style), Twice, 0);
		TestTrue(FString::Printf(TEXT("%s: flows = solved flows (exact)"), Style), bFlowExact);
		TestTrue(FString::Printf(TEXT("%s: points survive JSON exactly"), Style), bPointsExact);
		TestTrue(FString::Printf(TEXT("%s: temperature only where the model has one, equal to it"), Style), bTempRule);
		TestEqual(FString::Printf(TEXT("%s: rack branch temperatures = 2 racks x supply/return"), Style), WithTemp, 4);
		TestEqual(FString::Printf(TEXT("%s: pump links"), Style), PumpLinks, bGj ? Net.GetNumPumps() : 0);

		// Supply branches carry the rack inlet, return branches the rack outlet (the model's values, K).
		const TArray<ARackThermalActor*> Routed = Room->GetRoutedRacks();
		const TArray<int32>& Branches = Room->GetRackBranchPipeIndices();
		bool bBranchTemps = Routed.Num() == 2 && Branches.Num() == 2;
		for (int32 R = 0; bBranchTemps && R < 2; ++R)
			for (const FVrgDashboardPipe& P : Plan.Pipes)
				if (P.Edge == Branches[R]) bBranchTemps &= P.bSupply && P.TempK == Routed[R]->GetCoolantDisplaySnapshot().InletTempK;
		TestTrue(FString::Printf(TEXT("%s: supply branch = rack coolant inlet"), Style), bBranchTemps);
		AddInfo(FString::Printf(TEXT("%s: %d drawn paths for %d pipes + %d pump links, %d with temperature"),
			Style, Plan.Pipes.Num(), Net.GetNumPipes(), PumpLinks, WithTemp));
		Sim->EnterLayoutMode();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

// 3D view stage 3 (2026-10-14): airflow samples are the cell-centre mean of the two MAC faces per axis, on the cell
// layers holding the requested heights, every StrideCells cells, solid cells skipped, in world metres (synthetic field).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebDashboardAirflowTest, "VRG.Web.AirflowSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FWebDashboardAirflowTest::RunTest(const FString& Parameters)
{
	FAirGridParams Params;
	Params.NumCellsX = 8; Params.NumCellsY = 6; Params.NumCellsZ = 4; Params.CellSizeM = .25;
	FAirSolver Solver;
	Solver.Initialize(Params, 297.15);
	FUniformAirGrid& G = Solver.GetMutableGrid();
	// Face values chosen so the cell-centre mean is known in closed form: u = X, v = 2Y, w = -3Z on the faces.
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X <= 8; ++X) G.VelocityXMs[G.FaceXIndex(X, Y, Z)] = X;
	for (int32 Z = 0; Z < 4; ++Z) for (int32 Y = 0; Y <= 6; ++Y) for (int32 X = 0; X < 8; ++X) G.VelocityYMs[G.FaceYIndex(X, Y, Z)] = 2.0 * Y;
	for (int32 Z = 0; Z <= 4; ++Z) for (int32 Y = 0; Y < 6; ++Y) for (int32 X = 0; X < 8; ++X) G.VelocityZMs[G.FaceZIndex(X, Y, Z)] = -3.0 * Z;
	G.bSolidCell[G.CellIndex(3, 1, 1)] = true;   // one solid cell on a sampled position (skipped)
	const FVector Origin(10.0, -2.0, 0.0);
	// 0.30 m -> layer 1, 0.90 m -> layer 3, 0.95 m duplicate of layer 3, 5 m and -1 m outside the grid (dropped).
	const FVrgDashboardAirflow A = FVrgDashboardAirflow::Sample(Solver, Origin, 2, { .30, .90, .95, 5.0, -1.0 });
	TestFalse(TEXT("uniform path"), A.bOctree);
	TestTrue(TEXT("two layers at the cell-centre heights 0.375 / 0.875 m"), A.LayerHeightsM.Num() == 2
		&& A.LayerHeightsM[0] == .375 && A.LayerHeightsM[1] == .875);
	// Stride 2 starts one cell in: X = 1,3,5,7 and Y = 1,3,5 -> 12 per layer, minus the solid one.
	TestEqual(TEXT("sample count"), A.PositionsM.Num(), 12 * 2 - 1);
	bool bExact = A.PositionsM.Num() == A.VelocitiesMs.Num();
	double MaxSpeed = 0.0;
	for (int32 I = 0; bExact && I < A.PositionsM.Num(); ++I)
	{
		const FVector L = (A.PositionsM[I] - Origin) / .25 - FVector(.5);   // back to cell indices
		const int32 X = FMath::RoundToInt32(L.X), Y = FMath::RoundToInt32(L.Y), Z = FMath::RoundToInt32(L.Z);
		bExact = X % 2 == 1 && Y % 2 == 1 && (Z == 1 || Z == 3) && !(X == 3 && Y == 1 && Z == 1)
			&& A.VelocitiesMs[I] == FVector(X + .5, 2.0 * Y + 1.0, -3.0 * Z - 1.5);
		MaxSpeed = FMath::Max(MaxSpeed, A.VelocitiesMs[I].Size());
	}
	TestTrue(TEXT("every sample = face mean at its cell (exact), solid skipped"), bExact);
	TestEqual(TEXT("max speed over samples"), A.MaxSpeedMs, MaxSpeed);
	FVrgDashboardAirflow Back;
	TestTrue(TEXT("JSON round-trip parses"), FVrgDashboardAirflow::FromJson(A.ToJson(), Back));
	TestTrue(TEXT("JSON round-trip exact"), Back.PositionsM == A.PositionsM && Back.VelocitiesMs == A.VelocitiesMs
		&& Back.LayerHeightsM == A.LayerHeightsM && Back.StrideCells == 2 && Back.CellM == .25 && Back.MaxSpeedMs == A.MaxSpeedMs);
	const FVrgDashboardAirflow None = FVrgDashboardAirflow::Sample(Solver, Origin, 2, { 7.0 });
	TestTrue(TEXT("no layer inside -> no samples"), None.PositionsM.Num() == 0 && None.LayerHeightsM.Num() == 0);
	return true;
}

#endif
