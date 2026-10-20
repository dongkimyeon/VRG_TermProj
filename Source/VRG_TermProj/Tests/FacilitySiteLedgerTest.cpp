// Fill out your copyright notice in the Description page of Project Settings.

// S8 F5 (Tools/설비냉각_계획.md, 2026-10-20): the ledger out to the outdoor air and the cooling PUE.
// Room ledger (now with the CDU pump heat as an input) + plant ledger (with the plant pumps) close together; the
// coolant loop identity carries the pump heat; the cooling PUE is the window average of its own definition; a
// fixed-boundary run has no pump heat and no cooling PUE.

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
	struct FSiteRun
	{
		double Site = 0.0, Global = 0.0, CoolantId = 0.0, PumpHeatW = 0.0, PumpElectricW = 0.0, CoolingPue = 0.0, PartialPue = 0.0, PueRecompute = 0.0;
		double PumpHeatJ = 0.0, PlantW = 0.0;
		bool bCoolingPue = false, bPartial = false;
	};

	FSiteRun RunSite(bool bPlant, double Seconds)
	{
		FSiteRun Out;
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
		Sim->bShowSummary = false;
		Sim->MaxStepsPerFrame = 16;
		Sim->bFacilityPlant = bPlant;
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
		for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f);
		const FCouplingSnapshot S = Sim->GetSnapshot();
		Out.Site = Sim->GetSiteEnergyResidual();
		Out.Global = S.NormalizedEnergyResidual;
		const double OutJ = S.CoolantRejectedJ + S.CoolantLeakHeatJ;
		Out.CoolantId = OutJ > 0.0 ? (S.RackCoolantOutJ + S.CoolantPumpHeatJ - OutJ) / OutJ : 1.0;
		Out.PumpHeatJ = S.CoolantPumpHeatJ;
		Out.PumpHeatW = Sim->GetCouplingBridge().GetLastCduPumpHeatW();
		Out.PumpElectricW = Room->GetTotalPumpElectricalW();
		Out.bCoolingPue = Sim->GetEvaluation().GetCoolingPue(Out.CoolingPue);
		Out.bPartial = Sim->GetEvaluation().GetPartialPue(Out.PartialPue);
		const FVrgEvaluationSample P = Sim->GetEvaluation().GetWindowPower();
		Out.PlantW = P.PlantW;
		Out.PueRecompute = P.ItW > 0.0 ? (P.ItW + P.CrahFanW + P.PumpW + P.PlantW) / P.ItW : 0.0;
		Sim->EnterLayoutMode();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteLedgerTest, "VRG.Plant.Site.LedgerAndCoolingPue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSiteLedgerTest::RunTest(const FString& Parameters)
{
	const FSiteRun On = RunSite(true, 30.0), Off = RunSite(false, 30.0);
	AddInfo(FString::Printf(TEXT("설비 30 s: 외기까지 장부 %.3e, 방 장부 %.3e, 냉각수 항등(펌프 열 포함) %.3e, CDU 펌프 열 %.3f kW = 전기 %.3f kW, 설비 전력 창 평균 %.2f kW, 냉각 PUE %.5f(재계산 %.5f), 부분 PUE %.5f | 고정 경계: 펌프 열 %.3e J, 냉각 PUE 유효 %d, 장부 %.3e"),
		On.Site, On.Global, On.CoolantId, On.PumpHeatW * 1e-3, On.PumpElectricW * 1e-3, On.PlantW * 1e-3, On.CoolingPue, On.PueRecompute, On.PartialPue,
		Off.PumpHeatJ, Off.bCoolingPue ? 1 : 0, Off.Global));
	TestTrue(TEXT("ledger out to the outdoor air closes (|residual| < 1e-9 of all inputs)"), FMath::Abs(On.Site) < 1e-9 && On.Site != 0.0);
	TestTrue(TEXT("room ledger with the CDU pump heat as an input (< 1e-9)"), FMath::Abs(On.Global) < 1e-9);
	TestTrue(TEXT("coolant loop: rack heat + CDU pump heat = CDU duty + leak heat (rel < 1e-12)"), FMath::Abs(On.CoolantId) < 1e-12);
	TestTrue(TEXT("CDU pump heat = the CDU pumps' electric input (rel < 1e-12)"), On.PumpElectricW > 0.0 && FMath::Abs(On.PumpHeatW - On.PumpElectricW) <= 1e-12 * On.PumpElectricW);
	TestTrue(TEXT("cooling PUE = its definition over the window (exact)"), On.bCoolingPue && On.CoolingPue == On.PueRecompute);
	TestTrue(TEXT("plausibility: partial PUE < cooling PUE < 1.6"), On.bPartial && On.PartialPue < On.CoolingPue && On.CoolingPue < 1.6);
	TestTrue(TEXT("fixed boundary: no pump heat, no cooling PUE, ledger as before"), Off.PumpHeatJ == 0.0 && !Off.bCoolingPue && FMath::Abs(Off.Global) < 1e-9);
	return true;
}

#endif
