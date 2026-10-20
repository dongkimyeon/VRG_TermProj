// Fill out your copyright notice in the Description page of Project Settings.

// S8 F3 (Tools/설비냉각_계획.md, 2026-10-20): the chiller against its dataset curves, the Carnot extrapolation and its
// cap, the capacity limit, the chilled water / condenser water plant (setpoint, ledger, outdoor air), and the coupled
// room where the CRAH coils' equivalent UA equals the old design UA at the design flows.

#include "Misc/AutomationTest.h"
#include "Simulation/Plant/FacilityPlant.h"
#include "Actors/AirFieldDebugActor.h"
#include "Simulation/Air/AirSolver.h"
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
	constexpr EAutomationTestFlags VrgChillerFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double ChwRho = 997.0, ChwCp = 4180.0;

	// One CDU branch (0.8 MW at 10 K) and four CRAH coils (5 m^3/s each, design air drop 12 K, approach 5 K).
	FFacilityPlant MakeChwPlant(double AmbientC, TArray<FPlantCrahCoil>& OutCoils)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.8e6 / (ChwCp * 10.0) / ChwRho };
		FFacilityThermalSpec Th;
		Th.AmbientK = AmbientC + 273.15;
		OutCoils.Reset();
		for (int32 U = 0; U < 4; ++U)
		{
			FPlantCrahCoil& C = OutCoils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0;
			C.DesignAirEps = 12.0 / 17.0;
			C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		FFacilityPlant Plant;
		Plant.Build(Loop, Th, 0.8e6, ChwRho, ChwCp, FHydraulicSolveSettings(), OutCoils, 291.15);
		return Plant;
	}

	// The dataset's best part-load efficiency gain max PLR / EIRFPLR(PLR) over its PLR range.
	double PartLoadGain()
	{
		const FChillerCurves K;
		double Best = 0.0;
		for (int32 I = 0; I <= 200; ++I) { const double P = K.MinPlr + (K.MaxPlr - K.MinPlr) * I / 200.0; Best = FMath::Max(Best, P / Chiller::Quadratic(K.EirFplr, P)); }
		return Best;
	}

	void RunPlant(FFacilityPlant& Plant, double CduW, double CrahW, double Seconds, double Dt)
	{
		const TArray<double> Duty = { CduW };
		for (int32 I = 0; I < FMath::RoundToInt32(Seconds / Dt); ++I) Plant.Step(Dt, Duty, CrahW);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChillerDatasetTest, "VRG.Plant.Chiller.DatasetReferencePoint", VrgChillerFlags)
bool FChillerDatasetTest::RunTest(const FString& Parameters)
{
	const FChillerCurves K;
	const double MaxCop = Chiller::MaxCopInRange(K);
	// Full load at the reference temperatures: leaving 5.56 C, condenser in 21.11 C, inlet chosen for PLR ~1.
	const double WaterWK = K.RefChilledFlowM3s * ChwRho * ChwCp;
	const double CapRef = K.RefCapacityW;
	const double Cap = Chiller::CapacityW(K, CapRef, K.RefLeavingChilledC, K.RefEnteringCondenserC);
	const FChillerOperatingPoint P = Chiller::Evaluate(K, CapRef, MaxCop, 273.15 + K.RefLeavingChilledC + 0.999 * Cap / WaterWK, WaterWK,
		273.15 + K.RefLeavingChilledC, 273.15 + K.RefEnteringCondenserC);
	const double CapFt = Cap / CapRef;
	AddInfo(FString::Printf(TEXT("Carrier 19XR 1558 kW 기준점: CAPFT %.5f, 부하 %.1f kW(PLR %.4f), COP %.4f (데이터셋 5.81), 응축 %.1f kW, 범위 밖 %d, 범위 안 최고 COP %.3f"),
		CapFt, P.LoadW * 1e-3, P.Plr, P.Cop(), P.CondenserW * 1e-3, P.bOutsideCurves ? 1 : 0, MaxCop));
	TestTrue(TEXT("curves normalized at the reference point (CAPFT within 1 %)"), FMath::Abs(CapFt - 1.0) < 0.01);
	TestTrue(TEXT("reference COP reproduced within 1 % (EIRFT * EIRFPLR ~ 1 there)"), FMath::Abs(P.Cop() - K.RefCop) / K.RefCop < 0.01 && !P.bOutsideCurves);
	TestTrue(TEXT("condenser heat = load + compressor work (exact)"), P.CondenserW == P.LoadW + P.PowerW);
	TestTrue(TEXT("setpoint reached at full load"), !P.bCapacityLimited && FMath::Abs(P.LeavingChilledK - (273.15 + K.RefLeavingChilledC)) < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChillerExtrapolationTest, "VRG.Plant.Chiller.CarnotExtrapolationAndCap", VrgChillerFlags)
bool FChillerExtrapolationTest::RunTest(const FString& Parameters)
{
	const FChillerCurves K;
	const double MaxCop = Chiller::MaxCopInRange(K);
	bool bOut = false;
	// Continuity at the range edge: just inside vs at the edge vs just outside.
	const double EdgeIn = Chiller::FullLoadEir(K, 8.0, K.MaxY, MaxCop, bOut);
	const bool bEdgeOutside = bOut;
	const double JustOut = Chiller::FullLoadEir(K, 8.0, K.MaxY + 1e-9, MaxCop, bOut);
	const double ContRel = FMath::Abs(JustOut - EdgeIn) / EdgeIn;
	// Higher condensing beyond the range: COP falls monotonically (more lift).
	bool bMonotonic = true;
	double Prev = 0.0;
	for (double Y = K.MaxY; Y <= 50.0; Y += 1.0)
	{
		const double Eir = Chiller::FullLoadEir(K, 8.0, Y, MaxCop, bOut);
		bMonotonic &= Eir >= Prev;
		Prev = Eir;
	}
	// Cap: warm chilled water and cool condenser water (tiny lift) never beat the best in-range COP.
	double WorstCop = 0.0;
	for (double X = 5.0; X <= 25.0; X += 1.0)
		for (double Y = 8.0; Y <= 45.0; Y += 1.0)
			WorstCop = FMath::Max(WorstCop, 1.0 / Chiller::FullLoadEir(K, X, Y, MaxCop, bOut));
	const double Cop18At40 = 1.0 / Chiller::FullLoadEir(K, 18.0, 40.0, MaxCop, bOut);
	// Part load below the minimum PLR cycles: power proportional to load.
	const double WaterWK = 0.04 * ChwRho * ChwCp;
	const FChillerOperatingPoint A = Chiller::Evaluate(K, K.RefCapacityW, MaxCop, 273.15 + 5.56 + 0.05 * K.RefCapacityW / WaterWK, WaterWK, 278.71, 294.26);
	const FChillerOperatingPoint B = Chiller::Evaluate(K, K.RefCapacityW, MaxCop, 273.15 + 5.56 + 0.10 * K.RefCapacityW / WaterWK, WaterWK, 278.71, 294.26);
	const double CycleRel = FMath::Abs(B.PowerW / A.PowerW - B.LoadW / A.LoadW) / (B.LoadW / A.LoadW);
	AddInfo(FString::Printf(TEXT("경계 연속 %.3e(경계 바깥 판정 %d), 응축 상승 단조 %d, 격자 최고 COP %.4f(상한 %.4f), 18/40 C COP %.3f, 최소 PLR 아래 비례 오차 %.3e"),
		ContRel, bEdgeOutside ? 1 : 0, bMonotonic ? 1 : 0, WorstCop, MaxCop, Cop18At40, CycleRel));
	TestTrue(TEXT("continuous at the range edge (rel < 1e-6)"), ContRel < 1e-6 && !bEdgeOutside);
	TestTrue(TEXT("more lift, never better: EIR rises with condensing temperature outside the range"), bMonotonic);
	TestTrue(TEXT("COP never above the best in-range COP"), WorstCop <= MaxCop * (1.0 + 1e-12));
	TestTrue(TEXT("plausibility: 18 C chilled water, 40 C condenser water COP between 2 and the cap"), Cop18At40 > 2.0 && Cop18At40 <= MaxCop);
	TestTrue(TEXT("below the minimum PLR the power scales with the load (rel < 1e-12)"), CycleRel < 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChillerCapacityTest, "VRG.Plant.Chiller.CapacityLimit", VrgChillerFlags)
bool FChillerCapacityTest::RunTest(const FString& Parameters)
{
	const FChillerCurves K;
	const double MaxCop = Chiller::MaxCopInRange(K);
	const double WaterWK = 0.04 * ChwRho * ChwCp;
	const FChillerOperatingPoint P = Chiller::Evaluate(K, K.RefCapacityW, MaxCop, 273.15 + 5.56 + 1.5 * K.RefCapacityW / WaterWK, WaterWK, 278.71, 294.26);
	AddInfo(FString::Printf(TEXT("요청 1.5배: 부하 %.1f kW = 용량 %.1f kW, 출구 %.3f C (설정 5.56), 제한 %d"), P.LoadW * 1e-3, P.CapacityW * 1e-3, P.LeavingChilledK - 273.15, P.bCapacityLimited ? 1 : 0));
	TestTrue(TEXT("capacity limited: load = capacity, leaving above the setpoint, flagged"), P.bCapacityLimited && FMath::Abs(P.LoadW - P.CapacityW) <= 1e-9 * P.CapacityW
		&& P.LeavingChilledK > 278.71 + 1.0);
	const FChillerOperatingPoint Z = Chiller::Evaluate(K, K.RefCapacityW, MaxCop, 280.0, 0.0, 278.71, 294.26);
	TestTrue(TEXT("no water flow: nothing (D2)"), Z.LoadW == 0.0 && Z.PowerW == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChwPlantTest, "VRG.Plant.Chw.PlantSetpointLedgerOutdoor", VrgChillerFlags)
bool FChwPlantTest::RunTest(const FString& Parameters)
{
	TArray<FPlantCrahCoil> Coils;
	FFacilityPlant Cool = MakeChwPlant(25.0, Coils);
	TestTrue(TEXT("built with chilled water"), Cool.IsChilledWaterActive());
	double WorstUA = 0.0;
	for (int32 U = 0; U < Coils.Num(); ++U) WorstUA = FMath::Max(WorstUA, FMath::Abs(Cool.GetCrahEquivalentUAWK(U) - Coils[U].OldUAWK) / Coils[U].OldUAWK);
	TestTrue(TEXT("design flows: equivalent UA = the old infinite-water UA (rel < 1e-9)"), WorstUA < 1e-9);
	const double CrahW = Cool.GetDesignChwLoadW();
	RunPlant(Cool, 0.8e6, CrahW, 3000.0, 0.5);
	const FFacilityPlantState& S = Cool.GetState();
	const double CopCool = S.ChillerPowerW > 0.0 ? S.ChillerLoadW / S.ChillerPowerW : 0.0;
	AddInfo(FString::Printf(TEXT("외기 25 C: CHW 공급 %.6f C(설정 18) 회수 %.4f C, 칠러 %d대 %.1f kW COP %.3f, CW 공급 %.3f C, 장부 %.3e, 등가 UA 최대 차 %.3e, 칠러 기준 %.1f kW, CW 드라이쿨러 %d"),
		S.ChwSupplyK - 273.15, S.ChwReturnK - 273.15, S.ChillersRunning, S.ChillerLoadW * 1e-3, CopCool, S.CwSupplyK - 273.15, Cool.GetLedgerResidual(), WorstUA,
		Cool.GetChillerRefCapacityW() * 1e-3, Cool.GetNumCwDryCoolers()));
	TestTrue(TEXT("chillers hold the CHW setpoint (< 1e-6 K)"), FMath::Abs(S.ChwSupplyK - 291.15) < 1e-6 && !S.bChillerCapacityLimited);
	TestTrue(TEXT("steady: chillers take the CRAH load (rel < 1e-6)"), FMath::Abs(S.ChillerLoadW - CrahW) / CrahW < 1e-6);
	TestTrue(TEXT("condenser = load + work, plant ledger closes (rel < 1e-12)"), S.ChillerCondenserW == S.ChillerLoadW + S.ChillerPowerW
		&& FMath::Abs(Cool.GetLedgerResidual()) < 1e-12);
	TestTrue(TEXT("plausibility: chiller COP between 2 and the cap times the dataset's best part-load gain"), CopCool > 2.0 && CopCool <= Cool.GetChillerMaxCop() * PartLoadGain());

	FFacilityPlant Hot = MakeChwPlant(40.0, Coils);
	RunPlant(Hot, 0.8e6, CrahW, 3000.0, 0.5);
	const FFacilityPlantState& H = Hot.GetState();
	const double CopHot = H.ChillerPowerW > 0.0 ? H.ChillerLoadW / H.ChillerPowerW : 0.0;
	AddInfo(FString::Printf(TEXT("외기 40 C: CHW 공급 %.4f C, COP %.3f, CW 공급 %.3f C, 용량 부족 %d, 장부 %.3e"), H.ChwSupplyK - 273.15, CopHot, H.CwSupplyK - 273.15,
		H.bChillerCapacityLimited ? 1 : 0, Hot.GetLedgerResidual()));
	TestTrue(TEXT("hotter outdoor air -> warmer condenser water -> lower COP"), H.CwSupplyK > S.CwSupplyK && CopHot < CopCool);
	TestTrue(TEXT("ledger at 40 C (rel < 1e-12)"), FMath::Abs(Hot.GetLedgerResidual()) < 1e-12);

	FFacilityPlant Over = MakeChwPlant(25.0, Coils);
	RunPlant(Over, 0.8e6, 1.6 * CrahW, 300.0, 0.5);   // a fixed heat input ignores the water temperature: keep it short
	const FFacilityPlantState& O = Over.GetState();
	AddInfo(FString::Printf(TEXT("CRAH 1.6배: CHW 공급 %.4f C, 칠러 %d대 용량 부족 %d(누적 %.0f s), 장부 %.3e"), O.ChwSupplyK - 273.15, O.ChillersRunning,
		O.bChillerCapacityLimited ? 1 : 0, O.ChillerLimitedS, Over.GetLedgerResidual()));
	AddInfo(FString::Printf(TEXT("부분부하 이득 최대 %.3f"), PartLoadGain()));
	TestTrue(TEXT("over capacity: all duty chillers run, CHW supply rises above the setpoint, flagged"), O.ChillersRunning == Over.GetThermalSpec().DutyChillers
		&& O.bChillerCapacityLimited && O.ChwSupplyK > 291.15 + 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChwCoupledRoomTest, "VRG.Plant.Chw.CoupledRoom", VrgChillerFlags)
bool FChwCoupledRoomTest::RunTest(const FString& Parameters)
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
	AAirFieldDebugActor* AirActor = World->SpawnActor<AAirFieldDebugActor>();
	AirActor->bShowSummary = false;
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
	const FAirGridParams& AirParams = AirActor->GetSolver().GetGrid().GetParams();
	const FFacilityPlant& Plant = Sim->GetFacilityPlant();
	const TArray<FCrahCouplingUnit>& Units = Sim->GetCouplingBridge().GetCrahUnits();
	TestTrue(TEXT("chilled water plant active with CRAH units"), Plant.IsChilledWaterActive() && Units.Num() > 0);
	// At entry the bridge's coils carry the equivalent UA, which at the design flows is the old design UA.
	const double DesignEps = Sim->CrahDesignAirDeltaTK / (Sim->CrahDesignAirDeltaTK + Sim->CrahDesignApproachK);
	double WorstUA = 0.0;
	bool bWater = Units.Num() > 0;
	for (int32 U = 0; U < Units.Num(); ++U)
	{
		const double AirWK = AirParams.DensityKgM3 * Units[U].VolumeFlowM3s * AirParams.SpecificHeatJKgK;
		const double OldUA = -FMath::Loge(1.0 - DesignEps) * AirWK;
		WorstUA = FMath::Max(WorstUA, FMath::Abs(Units[U].CoilUAWK - OldUA) / OldUA);
		bWater &= Units[U].WaterSupplyTempK == Plant.GetChwSupplyTempK();
	}
	TestTrue(TEXT("entry: coil UA = old design UA (rel < 1e-9), water = plant CHW supply"), WorstUA < 1e-9 && bWater);
	for (int32 F = 0; F < 200; ++F) Sim->Tick(.1f);   // 20 s
	const FFacilityPlantState& S = Plant.GetState();
	const double Cop = S.ChillerPowerW > 0.0 ? S.ChillerLoadW / S.ChillerPowerW : 0.0;
	const double Global = Sim->GetSnapshot().NormalizedEnergyResidual;
	AddInfo(FString::Printf(TEXT("CRAH %d대, 등가 UA 최대 차 %.3e | 20 s: CRAH 열 %.1f kW, CHW 공급 %.3f C, 칠러 %d대 COP %.2f, CW 공급 %.3f C, 설비 장부 %.3e, 전역 %.3e, 펌프 %.2f kW"),
		Units.Num(), WorstUA, S.CrahDutyW * 1e-3, S.ChwSupplyK - 273.15, S.ChillersRunning, Cop, S.CwSupplyK - 273.15, Plant.GetLedgerResidual(), Global, Plant.GetPumpElectricW() * 1e-3));
	TestTrue(TEXT("CRAH coil heat reaches the plant, chillers run"), S.CrahDutyW > 0.0 && S.ChillersRunning > 0);
	TestTrue(TEXT("plant ledger (rel < 1e-12), global ledger (< 1e-9)"), FMath::Abs(Plant.GetLedgerResidual()) < 1e-12 && FMath::Abs(Global) < 1e-9);
	TestTrue(TEXT("plausibility: CHW supply within 3 K of the setpoint, COP between 2 and the cap times the part-load gain"), FMath::Abs(S.ChwSupplyK - Plant.GetChwSetpointK()) < 3.0
		&& Cop > 2.0 && Cop <= Plant.GetChillerMaxCop() * PartLoadGain());
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChwValveInverseTest, "VRG.Plant.Chw.ValveInverseExact", VrgChillerFlags)
bool FChwValveInverseTest::RunTest(const FString& Parameters)
{
	// Same plant as above with a coil leaving setpoint 23 C (18 C water + 5 K approach) on every unit.
	FFacilityLoopSpec Loop;
	Loop.BranchDesignFlowM3s = { 0.8e6 / (ChwCp * 10.0) / ChwRho };
	FFacilityThermalSpec Th;
	Th.AmbientK = 298.15;
	TArray<FPlantCrahCoil> Coils;
	for (int32 U = 0; U < 4; ++U)
	{
		FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
		C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
		C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		C.CoilLeavingSetpointK = 296.15;
	}
	FFacilityPlant Plant;
	TestTrue(TEXT("built"), Plant.Build(Loop, Th, 0.8e6, ChwRho, ChwCp, FHydraulicSolveSettings(), Coils, 291.15));
	// Intakes: 30 C (throttles), 35 C (design: full open), 40 C (above design: saturated), 22 C (below target: shut).
	const TArray<double> Mix = { 303.15, 308.15, 313.15, 295.15 };
	bool bSat[4] = {};
	double Target[4];
	for (int32 U = 0; U < 4; ++U) Target[U] = Plant.CrahOpeningFor(U, Mix[U], 296.15, bSat[U]);
	Plant.ControlCrahValves(1e6, Mix);   // a huge step: no stroke limit
	const double Dp = Plant.GetChwResult().HeaderDeltaPPa;
	const double Leave0 = Mix[0] - Plant.CrahAirEffectiveness(0, Plant.GetCrahWaterKgs(0)) * (Mix[0] - Plant.GetChwSupplyTempK());
	const double Leave1 = Mix[1] - Plant.CrahAirEffectiveness(1, Plant.GetCrahWaterKgs(1)) * (Mix[1] - Plant.GetChwSupplyTempK());
	const double EqRel = FMath::Abs(Plant.GetCrahEquivalentUAWK(0) - (-Coils[0].AirWK * FMath::Loge(1.0 - Plant.CrahAirEffectiveness(0, Plant.GetCrahWaterKgs(0))))) / Plant.GetCrahEquivalentUAWK(0);
	AddInfo(FString::Printf(TEXT("흡입 30/35/40/22 C -> 열림 %.4f / %.4f / %.4f / %.4f (포화 %d%d%d%d), 출구 30 C 유닛 %.9f C(설정 23), 35 C 유닛 %.6f C, 냉수 %.3f / %.3f / %.3f / %.3e kg/s, 헤더 %.1f kPa(설정 %.1f), 제어 수렴 %d"),
		Plant.GetCrahOpening(0), Plant.GetCrahOpening(1), Plant.GetCrahOpening(2), Plant.GetCrahOpening(3), bSat[0], bSat[1], bSat[2], bSat[3], Leave0 - 273.15, Leave1 - 273.15,
		Plant.GetCrahWaterKgs(0), Plant.GetCrahWaterKgs(1), Plant.GetCrahWaterKgs(2), Plant.GetCrahWaterKgs(3), Dp * 1e-3, Plant.GetChwResult().SetpointPa * 1e-3, Plant.GetChwResult().bControlConverged ? 1 : 0));
	TestTrue(TEXT("CHW header differential held"), Plant.GetChwResult().bControlConverged);
	TestTrue(TEXT("throttled unit leaves at the setpoint (< 1e-6 K)"), Target[0] > 0.0 && Target[0] < 1.0 && FMath::Abs(Leave0 - 296.15) < 1e-6);
	TestTrue(TEXT("design intake: valve fully open, leaves at the setpoint (design point, < 1e-6 K)"), Plant.GetCrahOpening(1) >= 1.0 - 1e-9 && FMath::Abs(Leave1 - 296.15) < 1e-6);
	TestTrue(TEXT("above design: saturated open"), bSat[2] && Plant.GetCrahOpening(2) == 1.0);
	TestTrue(TEXT("intake below the target: valve shut, no water"), Plant.GetCrahOpening(3) == 0.0 && Plant.GetCrahWaterKgs(3) == 0.0 && Plant.GetCrahEquivalentUAWK(3) == 0.0);
	TestTrue(TEXT("equivalent UA follows the new flow (rel < 1e-12)"), EqRel < 1e-12);
	// Stroke limit: from fully open, one 1 s step moves at most 1/60.
	FFacilityPlant Ramp;
	Ramp.Build(Loop, Th, 0.8e6, ChwRho, ChwCp, FHydraulicSolveSettings(), Coils, 291.15);
	Ramp.ControlCrahValves(1.0, Mix);
	TestTrue(TEXT("stroke rate: 1 s moves 1/60 of the travel"), FMath::Abs(Ramp.GetCrahOpening(0) - (1.0 - 1.0 / 60.0)) < 1e-12 && Ramp.GetCrahOpening(3) == 1.0 - 1.0 / 60.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChwValveRoomTest, "VRG.Plant.Chw.ValveControlCoupledRoom", VrgChillerFlags)
bool FChwValveRoomTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ADataCenterSimulationActor* Sim = World->SpawnActor<ADataCenterSimulationActor>();
	Sim->bShowSummary = false;
	Sim->MaxStepsPerFrame = 16;
	Sim->bFacilityPlant = true;
	Sim->PlantThermalSpec.AmbientK = 298.15;
	Sim->PlantThermalSpec.SupplySideTransitS = Sim->PlantThermalSpec.ReturnSideTransitS = 5.0;
	Sim->PlantThermalSpec.ValveStrokeS = 5.0;   // short stroke so 60 s settles (test setting)
	ADataCenterRoomActor* Room = World->SpawnActor<ADataCenterRoomActor>();
	AAirFieldDebugActor* AirActor = World->SpawnActor<AAirFieldDebugActor>();
	AirActor->bShowSummary = false;
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
	for (int32 F = 0; F < 600; ++F) Sim->Tick(.1f);   // 60 s
	const FFacilityPlant& Plant = Sim->GetFacilityPlant();
	TArray<double> RemovedW, MixK, SupplyK;
	const bool bReadings = Sim->GetCouplingBridge().GetLastCrahUnitReadings(RemovedW, MixK, SupplyK);
	const FAirGridParams& Air = AirActor->GetSolver().GetGrid().GetParams();
	double WorstThrottled = 0.0;
	int32 Throttled = 0;
	FString Units;
	const TArray<FCrahCouplingUnit>& CrahUnits = Sim->GetCouplingBridge().GetCrahUnits();
	for (int32 U = 0; bReadings && U < CrahUnits.Num(); ++U)
	{
		const double AirWK = Air.DensityKgM3 * CrahUnits[U].VolumeFlowM3s * Air.SpecificHeatJKgK;
		const double LeaveK = MixK[U] - RemovedW[U] / AirWK;
		const double X = Plant.GetCrahOpening(U);
		Units += FString::Printf(TEXT(" [흡입 %.2f 출구 %.3f 열림 %.3f]"), MixK[U] - 273.15, LeaveK - 273.15, X);
		if (X > 0.0 && X < 1.0) { ++Throttled; WorstThrottled = FMath::Max(WorstThrottled, FMath::Abs(LeaveK - (Sim->CoilSupplyTempK + Sim->CrahDesignApproachK))); }
	}
	const double Global = Sim->GetSnapshot().NormalizedEnergyResidual;
	AddInfo(FString::Printf(TEXT("60 s: CRAH%s | 조절 중 %d대 출구-설정 최대 %.3f K, CHW %.2f L/s(설계 %.2f), 설비 장부 %.3e, 전역 %.3e"),
		*Units, Throttled, WorstThrottled, Plant.GetChwLoop().GetTotalPumpFlowM3s() * 1e3, Plant.GetChwLoop().GetDesignTotalFlowM3s() * 1e3, Plant.GetLedgerResidual(), Global));
	TestTrue(TEXT("CPU path records per-unit intakes"), bReadings);
	TestTrue(TEXT("throttled units leave within 0.3 K of the setpoint (one step of lag, plausibility)"), WorstThrottled < 0.3);
	TestTrue(TEXT("ledgers: plant (rel < 1e-12), global (< 1e-9)"), FMath::Abs(Plant.GetLedgerResidual()) < 1e-12 && FMath::Abs(Global) < 1e-9);
	Sim->EnterLayoutMode();
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif
