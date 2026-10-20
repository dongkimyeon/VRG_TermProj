// Fill out your copyright notice in the Description page of Project Settings.

// S8 F6 (Tools/설비냉각_계획.md "설비 구성 세 가지"): the three plant topologies built by the same pieces.
// Common: plant ledger, chillers hold the chilled water. Separate: the FWS solution does not move when the CHW setpoint
// does. Shared: CDU inlet = CHW supply, CDU + CRAH branches = pump flow. Series: CDU inlet = the CRAH return mix,
// first stage + bypass = second stage = pump flow. Coupled: the same room under each topology; the order of their
// cooling PUEs is reported, never asserted (plan: comparisons are results, not hard-coded).

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
	constexpr EAutomationTestFlags VrgTopoFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double TopoRho = 997.0, TopoCp = 4180.0;

	// Two CDU branches (0.4 MW each at 10 K) and four CRAH coils (as in the chiller tests).
	FFacilityPlant MakeTopo(EPlantTopology T, double AmbientC, double ChwSetpointK = 291.15)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.4e6 / (TopoCp * 10.0) / TopoRho, 0.4e6 / (TopoCp * 10.0) / TopoRho };
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
		Plant.Build(Loop, Th, 0.8e6, TopoRho, TopoCp, FHydraulicSolveSettings(), Coils, ChwSetpointK);
		return Plant;
	}

	void RunTopo(FFacilityPlant& P, double Seconds)
	{
		const TArray<double> Duty = { 0.4e6, 0.4e6 };
		for (int32 I = 0; I < FMath::RoundToInt32(Seconds / 0.5); ++I) P.Step(0.5, Duty, P.GetDesignChwLoadW());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTopoSeparateTest, "VRG.Plant.Topology.SeparateIndependent", VrgTopoFlags)
bool FTopoSeparateTest::RunTest(const FString& Parameters)
{
	// 20 C outdoor air: the trim chiller stays off, so nothing couples the FWS to the chilled water side.
	FFacilityPlant A = MakeTopo(EPlantTopology::Separate, 20.0, 291.15), B = MakeTopo(EPlantTopology::Separate, 20.0, 288.15);
	RunTopo(A, 2000.0); RunTopo(B, 2000.0);
	AddInfo(FString::Printf(TEXT("분리, CHW 18 vs 15 C: FWS 공급 %.12f / %.12f C, 회수 %.12f / %.12f C, CHW 공급 %.4f / %.4f C, 장부 %.3e / %.3e"),
		A.GetState().SupplyK - 273.15, B.GetState().SupplyK - 273.15, A.GetState().ReturnK - 273.15, B.GetState().ReturnK - 273.15,
		A.GetState().ChwSupplyK - 273.15, B.GetState().ChwSupplyK - 273.15, A.GetLedgerResidual(), B.GetLedgerResidual()));
	TestTrue(TEXT("separate topology built"), A.GetTopology() == EPlantTopology::Separate && A.IsTrimActive() && A.GetState().TrimRunning == 0);
	TestTrue(TEXT("CHW setpoint change leaves the FWS solution exactly as it was"), A.GetState().SupplyK == B.GetState().SupplyK
		&& A.GetState().ReturnK == B.GetState().ReturnK && A.GetBranchMassFlowKgs(0) == B.GetBranchMassFlowKgs(0));
	TestTrue(TEXT("the chilled water did change"), FMath::Abs(A.GetState().ChwSupplyK - B.GetState().ChwSupplyK) > 1.0);
	TestTrue(TEXT("ledgers (rel < 1e-12)"), FMath::Abs(A.GetLedgerResidual()) < 1e-12 && FMath::Abs(B.GetLedgerResidual()) < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTopoSharedTest, "VRG.Plant.Topology.SharedChw", VrgTopoFlags)
bool FTopoSharedTest::RunTest(const FString& Parameters)
{
	FFacilityPlant P = MakeTopo(EPlantTopology::SharedChw, 25.0);
	TestTrue(TEXT("shared topology built, no FWS dry coolers or trim"), P.GetTopology() == EPlantTopology::SharedChw && P.GetNumDryCoolers() == 0 && !P.IsTrimActive());
	RunTopo(P, 3000.0);
	const FFacilityWaterLoop& L = P.GetChwLoop();
	double Branches = 0.0;
	for (int32 B = 0; B < L.GetNumBranches(); ++B) Branches += L.GetBranchFlowM3s(B);
	const double FlowRel = FMath::Abs(Branches - L.GetTotalPumpFlowM3s()) / L.GetTotalPumpFlowM3s();
	const FFacilityPlantState& S = P.GetState();
	AddInfo(FString::Printf(TEXT("공용: CDU 입구 %.6f C = CHW 공급 %.6f C, 분기 %d개 합/펌프 상대 %.3e, CDU 유량 %.3f kg/s, 칠러 %d대 %.1f kW, 장부 %.3e"),
		P.GetSupplyTempK() - 273.15, S.ChwSupplyK - 273.15, L.GetNumBranches(), FlowRel, P.GetBranchMassFlowKgs(0), S.ChillersRunning, S.ChillerLoadW * 1e-3, P.GetLedgerResidual()));
	TestTrue(TEXT("CDU inlet = CHW supply (same node, exact)"), P.GetSupplyTempK() == S.ChwSupplyK);
	TestTrue(TEXT("CDU + CRAH branch flows = pump flow (rel < 1e-12), six branches"), FlowRel < 1e-12 && L.GetNumBranches() == 6);
	TestTrue(TEXT("chillers carry CRAH + CDU + pump heat and hold 18 C (< 1e-6 K)"), FMath::Abs(S.ChwSupplyK - 291.15) < 1e-6
		&& FMath::Abs(S.ChillerLoadW - P.GetDesignChwLoadW() - 0.8e6 - S.ChwPumpHydraulicW) / 0.8e6 < 1e-6);
	TestTrue(TEXT("ledger (rel < 1e-12)"), FMath::Abs(P.GetLedgerResidual()) < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTopoSeriesTest, "VRG.Plant.Topology.SeriesMixNode", VrgTopoFlags)
bool FTopoSeriesTest::RunTest(const FString& Parameters)
{
	FFacilityPlant P = MakeTopo(EPlantTopology::Series, 25.0);
	TestTrue(TEXT("series topology built"), P.GetTopology() == EPlantTopology::Series && P.GetNumDryCoolers() == 0);
	RunTopo(P, 3000.0);
	const FFacilityWaterLoop& L = P.GetChwLoop();
	const FFacilityLoopHandles& H = L.GetHandles();
	double First = 0.0, Second = 0.0;
	for (int32 B = 0; B < H.NumFirstStage; ++B) First += L.GetBranchFlowM3s(B);
	for (int32 B = H.NumFirstStage; B < L.GetNumBranches(); ++B) Second += L.GetBranchFlowM3s(B);
	const double Pump = L.GetTotalPumpFlowM3s();
	const double Cont1 = FMath::Abs(First + L.GetBypassFlowM3s() - Second) / Pump, Cont2 = FMath::Abs(Second - Pump) / Pump;
	const FFacilityPlantState& S = P.GetState();
	const double MixK = S.ChwSupplyK + S.CrahDutyW / (Pump * TopoRho * TopoCp);
	AddInfo(FString::Printf(TEXT("직렬: CDU 입구 %.6f C, 혼합 %.6f C(CHW 공급 %.4f + CRAH %.1f kW / 루프), 1단 %.3f + 바이패스 %.3f = 2단 %.3f = 펌프 %.3f L/s (상대 %.3e / %.3e), 칠러 %d대, 장부 %.3e"),
		P.GetSupplyTempK() - 273.15, MixK - 273.15, S.ChwSupplyK - 273.15, S.CrahDutyW * 1e-3, First * 1e3, L.GetBypassFlowM3s() * 1e3, Second * 1e3, Pump * 1e3,
		Cont1, Cont2, S.ChillersRunning, P.GetLedgerResidual()));
	TestTrue(TEXT("CDU inlet = CRAH return mixed over the loop flow (rel < 1e-12)"), FMath::Abs(P.GetSupplyTempK() - MixK) < 1e-9 && P.GetSupplyTempK() > S.ChwSupplyK);
	TestTrue(TEXT("flow continuity: first stage + bypass = second stage = pumps (rel < 1e-12)"), Cont1 < 1e-12 && Cont2 < 1e-12 && L.GetBypassFlowM3s() > 0.0);
	TestTrue(TEXT("chillers hold 18 C (< 1e-6 K), ledger (rel < 1e-12)"), FMath::Abs(S.ChwSupplyK - 291.15) < 1e-6 && FMath::Abs(P.GetLedgerResidual()) < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTopoCoupledTest, "VRG.Plant.Topology.CoupledComparison", VrgTopoFlags)
bool FTopoCoupledTest::RunTest(const FString& Parameters)
{
	FString Lines;
	for (const EPlantTopology T : { EPlantTopology::Separate, EPlantTopology::SharedChw, EPlantTopology::Series })
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false;
		Sim->MaxStepsPerFrame = 16;
		Sim->bFacilityPlant = true;
		Sim->PlantThermalSpec.Topology = T;
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
		Sim->EnterSimulationMode();
		for (int32 F = 0; F < 300; ++F) Sim->Tick(.1f);   // 30 s
		const FFacilityPlant& Plant = Sim->GetFacilityPlant();
		double Pue = 0.0;
		const bool bPue = Sim->GetEvaluation().GetCoolingPue(Pue);
		const double Site = Sim->GetSiteEnergyResidual();
		const double Global = Sim->GetSnapshot().NormalizedEnergyResidual;
		const double CduIn = Sim->GetCouplingBridge().GetCduGroups().Num() > 0 ? Sim->GetCouplingBridge().GetCduGroups()[0].Hx.FacilitySupplyTempK : 0.0;
		const double CoolantK = Room->GetRoutedRacks()[0]->Input.CoolantInletTempK;
		Lines += FString::Printf(TEXT(" [%s: 냉각 PUE %.5f, CDU 설비수 입구 %.2f C, 랙 냉각수 입구 %.2f C, 칠러 %d대, 외기까지 장부 %.3e, 방 %.3e]"),
			FFacilityPlant::TopologyName(Plant.GetTopology()), Pue, CduIn - 273.15, CoolantK - 273.15, Plant.GetState().ChillersRunning, Site, Global);
		TestTrue(FString::Printf(TEXT("%s: built as requested, cooling PUE valid"), FFacilityPlant::TopologyName(T)), Plant.GetTopology() == T && bPue && Pue > 1.0);
		TestTrue(FString::Printf(TEXT("%s: ledgers (site < 1e-9, room < 1e-9)"), FFacilityPlant::TopologyName(T)), FMath::Abs(Site) < 1e-9 && FMath::Abs(Global) < 1e-9);
		Sim->EnterLayoutMode();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	AddInfo(FString::Printf(TEXT("같은 방, 외기 25 C, 30 s:%s (순서는 결과로만 보고, 판정하지 않음)"), *Lines));
	return true;
}

#endif
