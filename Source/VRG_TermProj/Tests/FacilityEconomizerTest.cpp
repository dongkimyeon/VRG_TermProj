// Fill out your copyright notice in the Description page of Project Settings.

// S8 F4 (Tools/설비냉각_계획.md, 2026-10-20): FWS trim chiller and the water-side economizer.
// Trim: holds the FWS supply where the dry coolers alone cannot (design air), off in cool air, limited above design.
// Economizer: cold outdoor air -> no compressor work (plan's extreme case), warm air -> chillers, no chattering when the
// air drifts across the switch point, plant ledger through every mode.

#include "Misc/AutomationTest.h"
#include "Simulation/Plant/FacilityPlant.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgEconFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;
	constexpr double EcoRho = 997.0, EcoCp = 4180.0;

	FFacilityPlant MakeFullPlant(double AmbientC, bool bWithCrah)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.8e6 / (EcoCp * 10.0) / EcoRho };
		FFacilityThermalSpec Th;
		Th.AmbientK = AmbientC + 273.15;
		TArray<FPlantCrahCoil> Coils;
		for (int32 U = 0; bWithCrah && U < 4; ++U)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		FFacilityPlant Plant;
		Plant.Build(Loop, Th, 0.8e6, EcoRho, EcoCp, FHydraulicSolveSettings(), Coils, 291.15);
		return Plant;
	}

	// Runs and returns the compressor energy of the last LastS seconds [J].
	double RunFor(FFacilityPlant& P, double CduW, double CrahW, double Seconds, double LastS, double Dt = 0.5)
	{
		const TArray<double> Duty = { CduW };
		const int32 N = FMath::RoundToInt32(Seconds / Dt), NLast = FMath::RoundToInt32(LastS / Dt);
		double Before = 0.0;
		for (int32 I = 0; I < N; ++I)
		{
			if (I == N - NLast) Before = P.GetState().CompressorInJ;
			P.Step(Dt, Duty, CrahW);
		}
		return P.GetState().CompressorInJ - Before;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrimChillerTest, "VRG.Plant.Trim.HoldsFwsSupply", VrgEconFlags)
bool FTrimChillerTest::RunTest(const FString& Parameters)
{
	FFacilityPlant Design = MakeFullPlant(35.0, false);
	TestTrue(TEXT("trim chiller built (dry coolers leave a gap at the design air), CW built for it"), Design.IsTrimActive() && Design.IsCondenserActive());
	RunFor(Design, Design.GetDesignHeatW(), 0.0, 4000.0, 10.0);
	const FFacilityPlantState& D = Design.GetState();
	AddInfo(FString::Printf(TEXT("외기 35 C(설계): 트림 설계 %.1f kW, 운전 %d대 %.1f kW(압축기 %.1f kW), FWS 공급 %.6f C, 드라이쿨러 팬 %.3f, CW 공급 %.3f C, 장부 %.3e"),
		Design.GetDesignTrimW() * 1e-3, D.TrimRunning, D.TrimLoadW * 1e-3, D.TrimPowerW * 1e-3, D.SupplyK - 273.15, D.FanRatio, D.CwSupplyK - 273.15, Design.GetLedgerResidual()));
	TestTrue(TEXT("design air: trim holds the FWS supply at 32 C (< 1e-6 K), dry coolers at full fans"), FMath::Abs(D.SupplyK - 305.15) < 1e-6 && D.TrimRunning > 0
		&& D.FanRatio == 1.0 && !D.bTrimLimited);
	TestTrue(TEXT("steady: dry coolers + trim take the duty + FWS pump heat (rel < 1e-6)"), FMath::Abs(D.DryCoolerHeatW + D.TrimLoadW - Design.GetDesignHeatW() - D.FwsPumpHydraulicW) / Design.GetDesignHeatW() < 1e-6);
	TestTrue(TEXT("plant ledger with the trim compressor (rel < 1e-12)"), FMath::Abs(Design.GetLedgerResidual()) < 1e-12);

	FFacilityPlant Cool = MakeFullPlant(20.0, false);
	const double CoolWork = RunFor(Cool, Cool.GetDesignHeatW(), 0.0, 4000.0, 100.0);
	AddInfo(FString::Printf(TEXT("외기 20 C: 트림 %d대, 마지막 100 s 압축기 %.3e J, FWS 공급 %.6f C"), Cool.GetState().TrimRunning, CoolWork, Cool.GetState().SupplyK - 273.15));
	TestTrue(TEXT("cool air: trim off, no compressor work, supply at the setpoint"), Cool.GetState().TrimRunning == 0 && CoolWork == 0.0 && FMath::Abs(Cool.GetState().SupplyK - 305.15) < 1e-6);

	FFacilityPlant Hot = MakeFullPlant(45.0, false);
	RunFor(Hot, Hot.GetDesignHeatW(), 0.0, 4000.0, 10.0);
	AddInfo(FString::Printf(TEXT("외기 45 C(설계 초과): 트림 %d대 용량 부족 %d, FWS 공급 %.3f C, 설정 미달 누적 %.0f s, 장부 %.3e"),
		Hot.GetState().TrimRunning, Hot.GetState().bTrimLimited ? 1 : 0, Hot.GetState().SupplyK - 273.15, Hot.GetState().FwsSupplyNotMetS, Hot.GetLedgerResidual()));
	TestTrue(TEXT("beyond the design air: trim limited, supply above the setpoint, flagged"), Hot.GetState().bTrimLimited && Hot.GetState().SupplyK > 305.15 + 0.1
		&& Hot.GetState().bFwsSupplyNotMet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEconomizerModesTest, "VRG.Plant.Economizer.ColdAirNoCompressor", VrgEconFlags)
bool FEconomizerModesTest::RunTest(const FString& Parameters)
{
	FFacilityPlant Cold = MakeFullPlant(5.0, true);
	TestTrue(TEXT("CHW + CW + economizer built"), Cold.IsChilledWaterActive() && Cold.IsCondenserActive() && Cold.GetEconomizerUAWK() > 0.0);
	const double ColdWork = RunFor(Cold, 0.8e6, Cold.GetDesignChwLoadW(), 4000.0, 100.0);
	const FFacilityPlantState& C = Cold.GetState();
	AddInfo(FString::Printf(TEXT("외기 5 C: 이코노마이저 %d %.1f kW, 칠러 %d대, 마지막 100 s 압축기 %.3e J, CHW 공급 %.4f C, CW 공급 %.3f C, 장부 %.3e"),
		C.bEconomizerOn ? 1 : 0, C.EconomizerW * 1e-3, C.ChillersRunning, ColdWork, C.ChwSupplyK - 273.15, C.CwSupplyK - 273.15, Cold.GetLedgerResidual()));
	TestTrue(TEXT("cold air: economizer on, chillers off, no compressor work (plan: extreme case)"), C.bEconomizerOn && C.ChillersRunning == 0 && ColdWork == 0.0);
	TestTrue(TEXT("cold air: the economizer carries the CRAH load (rel < 1e-6), CHW at the setpoint within its approach"),
		FMath::Abs(C.EconomizerW - Cold.GetDesignChwLoadW() - C.ChwPumpHydraulicW) / Cold.GetDesignChwLoadW() < 1e-6 && FMath::Abs(C.ChwSupplyK - 291.15) < Cold.GetThermalSpec().EconomizerApproachK);
	TestTrue(TEXT("ledger in economizer mode (rel < 1e-12)"), FMath::Abs(Cold.GetLedgerResidual()) < 1e-12);

	FFacilityPlant Warm = MakeFullPlant(25.0, true);
	RunFor(Warm, 0.8e6, Warm.GetDesignChwLoadW(), 4000.0, 10.0);
	const FFacilityPlantState& W = Warm.GetState();
	AddInfo(FString::Printf(TEXT("외기 25 C: 이코노마이저 %d, 칠러 %d대 %.1f kW, CHW 공급 %.6f C, 장부 %.3e"), W.bEconomizerOn ? 1 : 0, W.ChillersRunning, W.ChillerLoadW * 1e-3,
		W.ChwSupplyK - 273.15, Warm.GetLedgerResidual()));
	TestTrue(TEXT("warm air: economizer off, chillers hold 18 C (< 1e-6 K)"), !W.bEconomizerOn && W.ChillersRunning > 0 && FMath::Abs(W.ChwSupplyK - 291.15) < 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEconomizerHysteresisTest, "VRG.Plant.Economizer.HysteresisNoChatter", VrgEconFlags)
bool FEconomizerHysteresisTest::RunTest(const FString& Parameters)
{
	// Outdoor air rises slowly 5 -> 25 C over 2 h, then falls back: the economizer must switch off once and on once.
	FFacilityPlant P = MakeFullPlant(5.0, true);
	const TArray<double> Duty = { 0.8e6 };
	const double CrahW = P.GetDesignChwLoadW();
	for (int32 I = 0; I < 4000; ++I) P.Step(0.5, Duty, CrahW);   // settle at 5 C
	const int32 Start = P.GetState().EconomizerSwitches;
	for (int32 I = 0; I < 14400; ++I) { P.SetAmbientK(278.15 + 20.0 * I / 14400.0); P.Step(0.5, Duty, CrahW); }
	const bool bOffAtTop = !P.GetState().bEconomizerOn;
	for (int32 I = 0; I < 14400; ++I) { P.SetAmbientK(298.15 - 20.0 * I / 14400.0); P.Step(0.5, Duty, CrahW); }
	const int32 Switches = P.GetState().EconomizerSwitches - Start;
	AddInfo(FString::Printf(TEXT("5 -> 25 -> 5 C (각 2 h): 전환 %d회, 25 C에서 꺼짐 %d, 끝에서 켜짐 %d, 장부 %.3e"), Switches, bOffAtTop ? 1 : 0, P.GetState().bEconomizerOn ? 1 : 0, P.GetLedgerResidual()));
	TestTrue(TEXT("one switch each way (hysteresis, no chattering)"), Switches == 2 && bOffAtTop && P.GetState().bEconomizerOn);
	TestTrue(TEXT("ledger across the mode changes (rel < 1e-12)"), FMath::Abs(P.GetLedgerResidual()) < 1e-12);
	return true;
}

#endif
