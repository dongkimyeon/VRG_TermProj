// Fill out your copyright notice in the Description page of Project Settings.

// 2026-11-25 (vrg.Size.Rated): real product ratings instead of exact design sizing, so the number of CDUs and
// chillers changes the temperatures. CDU: one Vertiv CoolChip CDU 2300 rating point (2300 kW @ 4 K ATD, 3400 L/min)
// shared by the rows it serves; chillers: the curve set's reference capacity (Carrier 19XR 1557.8 kW).

#include "Misc/AutomationTest.h"
#include "Simulation/Thermal/CduHeatExchanger.h"
#include "Simulation/Plant/FacilityPlant.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags VrgRatedFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// Vertiv CoolChip CDU 2300 rating, PG25 secondary (TcsCoolant), primary water at a 10 K rise (the CDU actor default).
	constexpr double RatedW = 2.3e6, AtdK = 4.0, FacilityK = 305.15;
	const double HotWK = 3400.0 / 60000.0 * TcsCoolant::DensityKgM3 * TcsCoolant::CpJKgK;
	const double ColdWK = RatedW / 10.0;   // W/K

	// Steady secondary supply above the primary supply for a hot stream HotWK carrying DutyW through Hx.
	double SupplyApproachK(const FCduHeatExchangerParams& Hx, double StreamWK, double DutyW)
	{
		// Duty is linear in (T_mix - T_fac): solve T_mix - T_fac = DutyW / (Duty at 1 K), then supply = mix - DutyW / C_hot.
		const double PerK = CduHeatExchanger::DutyW(Hx, StreamWK, Hx.FacilitySupplyTempK + 1.0);
		return DutyW / PerK - DutyW / StreamWK;
	}

	FCduHeatExchangerParams Product(double Share)
	{
		FCduHeatExchangerParams Hx;
		Hx.FacilitySupplyTempK = FacilityK;
		Hx.FacilityCpJKgK = FacilityWater::CpJKgK;
		Hx.FacilityMassFlowKgs = Share * ColdWK / FacilityWater::CpJKgK;
		Hx.UAWK = Share * CduHeatExchanger::RatedUAWK(RatedW, AtdK, HotWK, ColdWK);
		return Hx;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRatedCduTest, "VRG.Size.CduRatedPointAndShare", VrgRatedFlags)
bool FRatedCduTest::RunTest(const FString& Parameters)
{
	// 1) The rating point is reproduced: at the rated flows the exchanger moves 2300 kW with the supply 4 K above.
	const FCduHeatExchangerParams One = Product(1.0);
	const double Mix = FacilityK + AtdK + RatedW / HotWK;
	const double Duty = CduHeatExchanger::DutyW(One, HotWK, Mix);
	const double Atd = SupplyApproachK(One, HotWK, RatedW);
	// 2) Slices (rows sharing one CDU in proportion to duty) perform like the whole.
	const double SliceSum = CduHeatExchanger::DutyW(Product(0.25), 0.25 * HotWK, Mix) + CduHeatExchanger::DutyW(Product(0.75), 0.75 * HotWK, Mix);
	// 3) Rows with twice the rated duty (and twice the flow) on one product: the approach grows; two products restore it.
	const double OverAtd = SupplyApproachK(One, 2.0 * HotWK, 2.0 * RatedW);
	const double TwoAtd = SupplyApproachK(Product(2.0), 2.0 * HotWK, 2.0 * RatedW);
	// 4) 64 GB200 racks (6.535 MW design) on 1..4 products: the approach falls with the count.
	TArray<double> Counts;
	for (int32 N = 1; N <= 4; ++N) Counts.Add(SupplyApproachK(Product(N), 6.535e6 / RatedW * HotWK, 6.535e6));
	AddInfo(FString::Printf(TEXT("정격점 %.6f kW, 접근 %.9f K | 조각 합 상대차 %.2e | 2배 부하 접근 %.3f K, 2대 %.9f K | 64랙 6.535 MW: CDU 1~4대 접근 %.2f / %.2f / %.2f / %.2f K"),
		Duty * 1e-3, Atd, FMath::Abs(SliceSum - Duty) / Duty, OverAtd, TwoAtd, Counts[0], Counts[1], Counts[2], Counts[3]));
	TestTrue(TEXT("rating point: 2300 kW at 4 K (rel < 1e-9)"), FMath::Abs(Duty - RatedW) / RatedW < 1e-9 && FMath::Abs(Atd - AtdK) < 1e-6);
	TestTrue(TEXT("slices in proportion to duty perform like the whole (rel < 1e-12)"), FMath::Abs(SliceSum - Duty) / Duty < 1e-12);
	TestTrue(TEXT("twice the rated duty on one product: larger approach; two products: the rated approach"), OverAtd > AtdK + 1.0 && FMath::Abs(TwoAtd - AtdK) < 1e-6);
	TestTrue(TEXT("more CDUs, smaller approach (strictly falling)"), Counts[0] > Counts[1] && Counts[1] > Counts[2] && Counts[2] > Counts[3]);
	TestTrue(TEXT("plausibility: under the rating (4 products, 71 %) the approach is below the rated 4 K"), Counts[3] < AtdK && Counts[3] > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRatedChillerTest, "VRG.Size.ChillerReferenceCapacity", VrgRatedFlags)
bool FRatedChillerTest::RunTest(const FString& Parameters)
{
	// The FacilityChillerTest plant (one 0.8 MW CDU branch, four CRAH coils of 5 m^3/s), sized vs rated chillers.
	auto Build = [](bool bRated, FFacilityPlant& Plant)
	{
		FFacilityLoopSpec Loop;
		Loop.BranchDesignFlowM3s = { 0.8e6 / (4180.0 * 10.0) / 997.0 };
		FFacilityThermalSpec Th;
		Th.AmbientK = 298.15;
		Th.bRatedChillers = bRated;
		TArray<FPlantCrahCoil> Coils;
		for (int32 U = 0; U < 4; ++U)
		{
			FPlantCrahCoil& C = Coils.AddDefaulted_GetRef();
			C.AirWK = 1.184 * 5.0 * 1005.0; C.DesignAirEps = 12.0 / 17.0; C.DesignDutyW = C.AirWK * 12.0;
			C.OldUAWK = -FMath::Loge(1.0 - C.DesignAirEps) * C.AirWK;
		}
		return Plant.Build(Loop, Th, 0.8e6, 997.0, 4180.0, FHydraulicSolveSettings(), Coils, 291.15);
	};
	FFacilityPlant Sized, Rated;
	TestTrue(TEXT("both plants built"), Build(false, Sized) && Build(true, Rated));
	const double Design = Sized.GetDesignChwLoadW();
	const TArray<double> Duty = { 0.8e6 };
	for (int32 I = 0; I < 600; ++I) { Sized.Step(0.5, Duty, 1.6 * Design); Rated.Step(0.5, Duty, 1.6 * Design); }
	const FFacilityPlantState& S = Sized.GetState();
	const FFacilityPlantState& R = Rated.GetState();
	AddInfo(FString::Printf(TEXT("칠러 기준 용량: 설계 맞춤 %.1f kW, 정격 %.1f kW | CRAH 1.6배 300 s: 맞춤 CHW %.3f C(용량 부족 %d), 정격 %.6f C(용량 부족 %d), 장부 %.2e / %.2e"),
		Sized.GetChillerRefCapacityW() * 1e-3, Rated.GetChillerRefCapacityW() * 1e-3, S.ChwSupplyK - 273.15, S.bChillerCapacityLimited ? 1 : 0,
		R.ChwSupplyK - 273.15, R.bChillerCapacityLimited ? 1 : 0, Sized.GetLedgerResidual(), Rated.GetLedgerResidual()));
	TestTrue(TEXT("rated: the reference capacity of the curve set (Carrier 19XR 1557.8 kW)"), Rated.GetChillerRefCapacityW() == FChillerCurves().RefCapacityW);
	TestTrue(TEXT("sized: smaller than one product for this small plant"), Sized.GetChillerRefCapacityW() < Rated.GetChillerRefCapacityW());
	TestTrue(TEXT("1.6x load: sized chillers fall short, rated ones hold 18 C (< 1e-6 K)"), S.bChillerCapacityLimited && !R.bChillerCapacityLimited
		&& FMath::Abs(R.ChwSupplyK - 291.15) < 1e-6);
	TestTrue(TEXT("plant ledgers close (rel < 1e-12)"), FMath::Abs(Sized.GetLedgerResidual()) < 1e-12 && FMath::Abs(Rated.GetLedgerResidual()) < 1e-12);
	return true;
}

#endif
