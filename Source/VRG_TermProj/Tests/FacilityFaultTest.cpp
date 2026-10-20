// Fill out your copyright notice in the Description page of Project Settings.

// S8 F6b (Tools/설비냉각_계획.md, D4): facility leaks and equipment faults, judged by re-solving the plant networks.
// Orifice flow and mass continuity in closed form; the plant ledger with the leak heat; the reach of the same CDU-branch
// leak under each topology comes from the solve (a separate chilled water loop does not move at all, the others are
// reported, never ranked); pump trip with standby switchover down to no pump (D2: no division by a zero flow); chiller
// and dry cooler failures; a coupled room whose CDU loses facility water.

#include "Misc/AutomationTest.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgFaultFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double FaultRho = 997.0, FaultCp = 4180.0;

	// The topology tests' plant: two CDU branches (0.4 MW each at 10 K) and four CRAH coils.
	FFacilityPlant MakeFaultPlant(EPlantTopology T, double AmbientC)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.4e6 / (FaultCp * 10.0) / FaultRho, 0.4e6 / (FaultCp * 10.0) / FaultRho };
		FFacilityThermalSpec Th;
		Th.AmbientK = AmbientC + 273.15;
		Th.Topology = T;
		TArray<FPlantCrahCoil> Coils;
		for (int32 U = 0; U < 4; ++U)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		FFacilityPlant Plant;
		Plant.Build(Loop, Th, 0.8e6, FaultRho, FaultCp, FHydraulicSolveSettings(), Coils, 291.15);
		return Plant;
	}

	void RunFault(FFacilityPlant& P, double Seconds)
	{
		// The CDU duty follows the branch flow the plant hands each CDU (the coupled exchanger would), here a fixed 10 K
		// rise so a branch that loses water also loses duty; the CRAH heat is the design load.
		for (int32 I = 0; I < FMath::RoundToInt32(Seconds / 0.5); ++I)
		{
			const TArray<double> Duty = { P.GetBranchMassFlowKgs(0) * FaultCp * 10.0, P.GetBranchMassFlowKgs(1) * FaultCp * 10.0 };
			P.Step(0.5, Duty, P.GetDesignChwLoadW());
		}
	}

	double BranchSum(const FFacilityWaterLoop& L, int32 From, int32 To)
	{
		double Sum = 0.0;
		for (int32 B = From; B < To; ++B) Sum += L.GetBranchFlowM3s(B);
		return Sum;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultZeroLeakTest, "VRG.Plant.Fault.ZeroLeakUnchanged", VrgFaultFlags)
bool FPlantFaultZeroLeakTest::RunTest(const FString& Parameters)
{
	FFacilityPlant A = MakeFaultPlant(EPlantTopology::Separate, 30.0), B = MakeFaultPlant(EPlantTopology::Separate, 30.0);
	const bool bAdded = B.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::BranchSupply, 0, 0.0) != INDEX_NONE
		&& B.AddLeak(EPlantLoopId::Chw, EPlantLeakSite::SupplyHeader, INDEX_NONE, 0.0) != INDEX_NONE;
	RunFault(A, 1500.0); RunFault(B, 1500.0);
	const double DFws = FMath::Abs(A.GetState().SupplyK - B.GetState().SupplyK), DChw = FMath::Abs(A.GetState().ChwSupplyK - B.GetState().ChwSupplyK);
	const double DFlow = FMath::Abs(A.GetBranchMassFlowKgs(0) - B.GetBranchMassFlowKgs(0)) / A.GetBranchMassFlowKgs(0);
	AddInfo(FString::Printf(TEXT("구멍 0 mm 두 개: FWS 공급 차 %.3e K, CHW 공급 차 %.3e K, CDU 0 유량 상대 차 %.3e, 누수 열 %.3e J, 장부 %.3e / %.3e"),
		DFws, DChw, DFlow, B.GetState().LeakOutJ, A.GetLedgerResidual(), B.GetLedgerResidual()));
	TestTrue(TEXT("zero holes added on two loops"), bAdded && B.GetFaultLog().Num() == 2);
	TestTrue(TEXT("a zero hole changes nothing (< 1e-9 K, flow rel < 1e-12, no leak heat)"), DFws < 1e-9 && DChw < 1e-9 && DFlow < 1e-12 && B.GetState().LeakOutJ == 0.0);
	TestTrue(TEXT("no branch counted as affected by a zero hole"), B.GetFaultLog()[0].AffectedBranches == 0 && B.GetFaultLog()[1].AffectedBranches == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultOrificeTest, "VRG.Plant.Fault.LeakOrificeMassLedger", VrgFaultFlags)
bool FPlantFaultOrificeTest::RunTest(const FString& Parameters)
{
	// Supply-side leak on CDU branch 0 and a return-side leak on the FWS return header, 10 mm each.
	FFacilityPlant P = MakeFaultPlant(EPlantTopology::Separate, 30.0);
	RunFault(P, 600.0);
	const double Branch0Before = P.GetBranchMassFlowKgs(0), Branch1Before = P.GetBranchMassFlowKgs(1);
	const int32 K1 = P.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::BranchSupply, 0, 0.010);
	const int32 K2 = P.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::ReturnHeader, INDEX_NONE, 0.010);
	const FFacilityWaterLoop& L = P.GetLoop();
	const FHydraulicNetwork& N = L.GetNetwork();
	// Orifice closed form at the solved node gauge pressure.
	double OrificeRel = 0.0;
	for (const int32 K : { K1, K2 })
	{
		const FHydraulicLeak& Hole = N.GetLeak(L.GetLeak(K).NetworkLeak);
		const double Area = 0.25 * UE_DOUBLE_PI * Hole.HoleDiameterM * Hole.HoleDiameterM;
		const double Expect = Hole.DischargeCoefficient * Area * FMath::Sqrt(2.0 * N.GetNodePressurePa(Hole.NodeIndex) / FaultRho);
		OrificeRel = FMath::Max(OrificeRel, FMath::Abs(L.GetLeakFlowM3s(K) - Expect) / Expect);
	}
	// Mass: pumps = branches + supply-side leak; dry coolers = branches - return-side leak.
	double Dry = 0.0;
	for (const int32 Pipe : L.GetHandles().ReturnCoilPipes) Dry += N.GetPipeFlowM3s(Pipe);
	const double Branches = BranchSum(L, 0, L.GetNumBranches());
	const double PumpRel = FMath::Abs(L.GetTotalPumpFlowM3s() - Branches - L.GetSupplySideLeakM3s()) / L.GetTotalPumpFlowM3s();
	const double DryRel = FMath::Abs(Dry - (Branches - L.GetReturnSideLeakM3s())) / Dry;
	const double SinkRel = FMath::Abs(P.GetTotalMassFlowKgs() - Dry * FaultRho) / (Dry * FaultRho);
	const FPlantFaultReport& R = P.GetFaultLog()[0];
	RunFault(P, 1200.0);
	const FFacilityPlantState& S = P.GetState();
	AddInfo(FString::Printf(TEXT("10 mm 두 개: 누수 %.4f + %.4f L/s(오리피스 상대 %.3e), 펌프 = 분기 + 공급측 누수 %.3e, 드라이쿨러 = 분기 - 회수측 누수 %.3e(설비 사용 %.3e), CDU 0 %.3f -> %.3f kg/s, CDU 1 %.3f -> %.3f kg/s, 영향 분기 %d/2, 펌프 %.4f -> %.4f | 1200 s 뒤 누수 열 %.1f kW(누적 %.3e J, 물 %.0f kg), FWS 공급 %.3f C, 장부 %.3e"),
		L.GetLeakFlowM3s(K1) * 1e3, L.GetLeakFlowM3s(K2) * 1e3, OrificeRel, PumpRel, DryRel, SinkRel, Branch0Before, P.GetFaultLog()[0].BranchAfterM3s[0] * FaultRho,
		Branch1Before, P.GetFaultLog()[0].BranchAfterM3s[1] * FaultRho, R.AffectedBranches, R.PumpSpeedBefore, R.PumpSpeedAfter,
		S.LeakHeatW * 1e-3, S.LeakOutJ, S.LeakMassKg, S.SupplyK - 273.15, P.GetLedgerResidual()));
	TestTrue(TEXT("both leaks placed"), K1 != INDEX_NONE && K2 != INDEX_NONE && P.GetFaultLog().Num() == 2);
	TestTrue(TEXT("orifice flow = Cd A sqrt(2 p / rho) at the solved node (rel < 1e-12)"), OrificeRel < 1e-12);
	TestTrue(TEXT("pumps = branches + supply-side leak (rel < 1e-12)"), PumpRel < 1e-12);
	TestTrue(TEXT("dry coolers = branches - return-side leak, and the plant uses that flow (rel < 1e-12)"), DryRel < 1e-12 && SinkRel < 1e-12);
	TestTrue(TEXT("the leaking branch lost flow (it is in the reported reach)"), P.GetFaultLog()[0].BranchAfterM3s[0] * FaultRho < Branch0Before && R.AffectedBranches >= 1);
	TestTrue(TEXT("plant ledger with the leak heat as an outflow (rel < 1e-12), leak heat > 0"), FMath::Abs(P.GetLedgerResidual()) < 1e-12 && S.LeakOutJ > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultReachTest, "VRG.Plant.Fault.ReachByTopology", VrgFaultFlags)
bool FPlantFaultReachTest::RunTest(const FString& Parameters)
{
	// The same 30 mm hole on CDU branch 0's supply under each topology. Where that branch lives depends on the topology
	// (FWS branch 0; CHW branch 4 next to the four CRAH coils; CHW second-stage branch 4). The reach is the solve's.
	FString Lines;
	for (const EPlantTopology T : { EPlantTopology::Separate, EPlantTopology::SharedChw, EPlantTopology::Series })
	{
		FFacilityPlant P = MakeFaultPlant(T, 25.0);
		RunFault(P, 300.0);
		const FFacilityWaterLoop& Chw = P.GetChwLoop();
		TArray<double> CrahBefore;
		for (int32 U = 0; U < 4; ++U) CrahBefore.Add(Chw.GetBranchFlowM3s(U));
		const bool bSeparate = T == EPlantTopology::Separate;
		const EPlantLoopId Id = bSeparate ? EPlantLoopId::Fws : EPlantLoopId::Chw;
		const int32 Branch = bSeparate ? 0 : 4;
		const double CduBefore = P.GetBranchMassFlowKgs(0);
		const bool bOk = P.AddLeak(Id, EPlantLeakSite::BranchSupply, Branch, 0.030) != INDEX_NONE;
		double CrahMaxRel = 0.0;
		for (int32 U = 0; U < 4; ++U) CrahMaxRel = FMath::Max(CrahMaxRel, FMath::Abs(Chw.GetBranchFlowM3s(U) - CrahBefore[U]) / CrahBefore[U]);
		const FPlantFaultReport& R = P.GetFaultLog().Last();
		Lines += FString::Printf(TEXT(" [%s: %s 루프, 누수 %.3f L/s, CDU 0 %.3f -> %.3f kg/s, CRAH 냉수 최대 상대 변화 %.3e, 그 루프에서 유량이 바뀐 분기 %d/%d, 펌프 %.4f -> %.4f%s]"),
			FFacilityPlant::TopologyName(T), FFacilityPlant::LoopName(Id), R.LeakM3s * 1e3, CduBefore, P.GetBranchMassFlowKgs(0), CrahMaxRel,
			R.AffectedBranches, R.BranchAfterM3s.Num(), R.PumpSpeedBefore, R.PumpSpeedAfter, R.bPumpsSaturated ? TEXT(", 포화") : TEXT(""));
		TestTrue(FString::Printf(TEXT("%s: leak placed on the CDU branch of its loop, that CDU lost flow"), FFacilityPlant::TopologyName(T)),
			bOk && P.GetBranchMassFlowKgs(0) < CduBefore && R.AffectedBranches >= 1);
		if (bSeparate) TestTrue(TEXT("separate: the chilled water loop is another network, its CRAH flows do not move at all"), CrahMaxRel == 0.0);
	}
	// A hole large enough that the pumps cannot hold the differential: every branch of the loop is in the reach.
	FFacilityPlant Big = MakeFaultPlant(EPlantTopology::Separate, 25.0);
	Big.AddLeak(EPlantLoopId::Fws, EPlantLeakSite::SupplyHeader, INDEX_NONE, 0.150);
	const FPlantFaultReport& RB = Big.GetFaultLog().Last();
	Lines += FString::Printf(TEXT(" [분리 공급 헤더 150 mm: 누수 %.1f L/s, 분기 %.3f/%.3f -> %.3f/%.3f L/s, 영향 %d/%d, 포화 %d]"), RB.LeakM3s * 1e3,
		RB.BranchBeforeM3s[0] * 1e3, RB.BranchBeforeM3s[1] * 1e3, RB.BranchAfterM3s[0] * 1e3, RB.BranchAfterM3s[1] * 1e3, RB.AffectedBranches, RB.BranchAfterM3s.Num(), RB.bPumpsSaturated ? 1 : 0);
	TestTrue(TEXT("pumps saturated by a header leak: all branches lose flow"), RB.bPumpsSaturated && RB.AffectedBranches == 2
		&& RB.BranchAfterM3s[0] < RB.BranchBeforeM3s[0] && RB.BranchAfterM3s[1] < RB.BranchBeforeM3s[1]);
	AddInfo(FString::Printf(TEXT("같은 CDU 분기 30 mm 누수, 외기 25 C:%s (구성끼리 순서는 판정하지 않음)"), *Lines));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultPumpTest, "VRG.Plant.Fault.PumpTripToNoPump", VrgFaultFlags)
bool FPlantFaultPumpTest::RunTest(const FString& Parameters)
{
	FFacilityPlant P = MakeFaultPlant(EPlantTopology::Separate, 30.0);
	RunFault(P, 300.0);
	const double Before = P.GetBranchMassFlowKgs(0);
	P.TripPump(EPlantLoopId::Fws, 0);
	const FPlantFaultReport R1 = P.GetFaultLog().Last();
	const double AfterSwitch = P.GetBranchMassFlowKgs(0);
	P.TripPump(EPlantLoopId::Fws, 1);
	const FPlantFaultReport R2 = P.GetFaultLog().Last();
	const double OnePump = P.GetBranchMassFlowKgs(0);
	P.TripPump(EPlantLoopId::Fws, 2);
	const FPlantFaultReport R3 = P.GetFaultLog().Last();
	RunFault(P, 600.0);
	const FFacilityPlantState& S = P.GetState();
	const bool bFinite = FMath::IsFinite(S.SupplyK) && FMath::IsFinite(S.ReturnK) && FMath::IsFinite(P.GetLedgerResidual());
	AddInfo(FString::Printf(TEXT("FWS 펌프 2+1: 0번 고장 -> %s(%d대, CDU 0 %.4f -> %.4f kg/s, 상대 %.3e) | 1번 고장 -> %d대 속도 %.4f%s, CDU 0 %.4f kg/s | 2번 고장 -> %d대, CDU 0 %.3e kg/s | 600 s 뒤 공급 %.3f 회수 %.3f C, 장부 %.3e"),
		*R1.What, R1.PumpsRunningAfter, Before, AfterSwitch, FMath::Abs(AfterSwitch - Before) / Before, R2.PumpsRunningAfter, R2.PumpSpeedAfter,
		R2.bPumpsSaturated ? TEXT(" 포화") : TEXT(""), OnePump, R3.PumpsRunningAfter, P.GetBranchMassFlowKgs(0), S.SupplyK - 273.15, S.ReturnK - 273.15, P.GetLedgerResidual()));
	TestTrue(TEXT("first trip: the standby starts, two pumps still run, flows held (rel < 1e-9)"), R1.PumpsRunningAfter == 2 && FMath::Abs(AfterSwitch - Before) / Before < 1e-9);
	TestTrue(TEXT("second trip: one pump left"), R2.PumpsRunningAfter == 1 && OnePump > 0.0);
	TestTrue(TEXT("third trip: no pump, no flow, no NaN (D2), ledger still closes (rel < 1e-12)"), R3.PumpsRunningAfter == 0 && P.GetBranchMassFlowKgs(0) < 1e-9
		&& bFinite && FMath::Abs(P.GetLedgerResidual()) < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultEquipmentTest, "VRG.Plant.Fault.ChillerAndDryCooler", VrgFaultFlags)
bool FPlantFaultEquipmentTest::RunTest(const FString& Parameters)
{
	// CHW chillers 2 duty + 1 standby: one out of service is covered (N+1), two leave one chiller -> capacity limit,
	// the chilled water rises above its setpoint.
	FFacilityPlant P = MakeFaultPlant(EPlantTopology::Separate, 30.0);
	RunFault(P, 1200.0);
	P.SetTrippedChillers(1, false);
	RunFault(P, 1200.0);
	const double OneOutK = P.GetState().ChwSupplyK;
	const int32 OneOutRunning = P.GetState().ChillersRunning;
	P.SetTrippedChillers(2, false);
	RunFault(P, 600.0);
	const FFacilityPlantState& S = P.GetState();
	// FWS dry cooler fans all failed at 20 C outdoor air: the trim chiller takes what they no longer remove.
	FFacilityPlant D = MakeFaultPlant(EPlantTopology::Separate, 20.0);
	RunFault(D, 1200.0);
	const int32 TrimBefore = D.GetState().TrimRunning;
	D.SetTrippedDryCoolers(EPlantLoopId::Fws, D.GetNumDryCoolers());
	RunFault(D, 1200.0);
	const FFacilityPlantState& SD = D.GetState();
	AddInfo(FString::Printf(TEXT("칠러 1대 정지: %d대 운전, 냉수 %.6f C | 2대 정지: %d대, 용량 부족 %d, 냉수 %.3f C(600 s), 장부 %.3e | FWS 드라이쿨러 팬 %d대 모두 고장(외기 20 C): 드라이쿨러 %.1f kW, 트림 %d -> %d대 %.1f kW, FWS 공급 %.3f C, 장부 %.3e"),
		OneOutRunning, OneOutK - 273.15, S.ChillersRunning, S.bChillerCapacityLimited ? 1 : 0, S.ChwSupplyK - 273.15, P.GetLedgerResidual(),
		D.GetNumDryCoolers(), SD.DryCoolerHeatW * 1e-3, TrimBefore, SD.TrimRunning, SD.TrimLoadW * 1e-3, SD.SupplyK - 273.15, D.GetLedgerResidual()));
	TestTrue(TEXT("one chiller out: N+1 holds 18 C (< 1e-6 K)"), FMath::Abs(OneOutK - 291.15) < 1e-6 && OneOutRunning >= 1);
	TestTrue(TEXT("two out: one chiller, capacity limited, chilled water above setpoint by > 0.5 K"), S.ChillersRunning == 1 && S.bChillerCapacityLimited && S.ChwSupplyK > 291.65);
	TestTrue(TEXT("dry cooler fans failed: no dry cooler heat, the trim chiller starts"), SD.DryCoolerHeatW == 0.0 && TrimBefore == 0 && SD.TrimRunning > 0);
	TestTrue(TEXT("ledgers (rel < 1e-12)"), FMath::Abs(P.GetLedgerResidual()) < 1e-12 && FMath::Abs(D.GetLedgerResidual()) < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlantFaultCoupledTest, "VRG.Plant.Fault.CoupledFwsLeak", VrgFaultFlags)
bool FPlantFaultCoupledTest::RunTest(const FString& Parameters)
{
	// The same room twice (separate topology, 25 C): one run loses facility water through a FWS supply header hole at
	// 10 s. The CDU gets less facility water, the rack coolant warms; both ledgers stay closed with the leak heat.
	double CoolantK[2] = { 0.0, 0.0 }, CduKgs[2] = { 0.0, 0.0 }, Site[2] = { 0.0, 0.0 }, Global[2] = { 0.0, 0.0 }, LeakJ = 0.0;
	bool bApplied = false, bSaturated = false;
	for (int32 Run = 0; Run < 2; ++Run)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false;
		Sim->MaxStepsPerFrame = 16;
		Sim->bFacilityPlant = true;
		Sim->PlantThermalSpec.AmbientK = 298.15;
		Sim->PlantThermalSpec.SupplySideTransitS = Sim->PlantThermalSpec.ReturnSideTransitS = 5.0;
		ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
		World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
		for (const double X : { 0.0, 400.0 })
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(X, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
		World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
		Room->RebuildRoomNow();
		if (Run == 1)
		{
			ADataCenterSimulationActor::FEvalEvent E;
			E.TimeS = 10.0; E.Kind = TEXT("plant");
			E.Args = { TEXT("leak"), TEXT("fws"), TEXT("supply"), TEXT("0"), TEXT("120") };
			Sim->EvalEvents.Add(E);
		}
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 300; ++F) Sim->Tick(.1f);   // 30 s
		const FFacilityPlant& Plant = Sim->GetFacilityPlant();
		CoolantK[Run] = Room->GetRoutedRacks()[0]->Input.CoolantInletTempK;
		CduKgs[Run] = Plant.GetBranchMassFlowKgs(0);
		Site[Run] = Sim->GetSiteEnergyResidual();
		Global[Run] = Sim->GetSnapshot().NormalizedEnergyResidual;
		if (Run == 1)
		{
			bApplied = Plant.GetFaultLog().Num() == 1 && FMath::Abs(Plant.GetFaultLog()[0].TimeS - 10.0) <= 0.02 + 1e-9;
			bSaturated = bApplied && Plant.GetFaultLog()[0].bPumpsSaturated;
			LeakJ = Plant.GetState().LeakOutJ;
		}
		Sim->EnterLayoutMode();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	AddInfo(FString::Printf(TEXT("FWS 공급 헤더 120 mm 누수(10 s): CDU 설비수 %.3f -> %.3f kg/s(포화 %d), 랙 냉각수 입구 %.3f -> %.3f C, 누수 열 %.3e J | 외기까지 장부 %.3e / %.3e, 방 %.3e / %.3e"),
		CduKgs[0], CduKgs[1], bSaturated ? 1 : 0, CoolantK[0] - 273.15, CoolantK[1] - 273.15, LeakJ, Site[0], Site[1], Global[0], Global[1]));
	TestTrue(TEXT("the event applied at the step starting at 10 s on the integrated clock (rule 4)"), bApplied);
	TestTrue(TEXT("the CDU lost facility water and the rack coolant is warmer"), CduKgs[1] < CduKgs[0] && CoolantK[1] > CoolantK[0]);
	TestTrue(TEXT("ledgers with the leak heat (site < 1e-9, room < 1e-9), leak heat > 0"), FMath::Abs(Site[1]) < 1e-9 && FMath::Abs(Global[1]) < 1e-9 && LeakJ > 0.0
		&& FMath::Abs(Site[0]) < 1e-9);
	return true;
}

#endif
