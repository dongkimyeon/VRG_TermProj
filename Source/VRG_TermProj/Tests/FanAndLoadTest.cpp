// Fill out your copyright notice in the Description page of Project Settings.

#include "Misc/AutomationTest.h"
#include "Simulation/Air/AirSolver.h"
#include "Simulation/Coupling/CouplingBridge.h"
#include "Simulation/Thermal/RackThermalModel.h"
#include "Simulation/Thermal/RadiatorModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// Load-dependent heat, device fans (operating point, affinity law, speed control) and fan-driven air
// through-flow in the air grid (2026-09-25).

namespace
{
	constexpr EAutomationTestFlags VrgFanTestFlags =
		EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	// One-slot rack: two air-cooled parts (one load dependent) and two fan parts, San Ace 9CRJ-like fans.
	FRackThermalParams MakeFanTestRack(bool bWithFans)
	{
		FRackThermalParams P;
		FRackDeviceType D;
		D.Name = TEXT("Fan test device");
		D.DockedResistancePaS2Kg2 = 7.0e4;
		if (bWithFans)
		{
			D.Fans.NumFans = 4; D.Fans.MaxFlowM3s = 1.06 / 60.0; D.Fans.ShutoffPressurePa = 2400.0; D.Fans.RatedInputW = 37.2;
			D.Fans.MinSpeedRatio = .3; D.Fans.ControlStartK = 318.15; D.Fans.ControlFullK = 338.15; D.Fans.MaxSlewPerS = .2;
		}
		FRackComponentParams A; A.Name = TEXT("Chip"); A.Kind = ERackPartKind::Dpu; A.DefaultHeatW = 100.0; A.HeatCapacityJK = 200.0;
		A.ToAirUAWK = 3.0; A.ToAmbientUAWK = .02; A.IdleHeatFraction = .2;
		FRackComponentParams B = A; B.Name = TEXT("Board"); B.Kind = ERackPartKind::Management; B.DefaultHeatW = 50.0; B.IdleHeatFraction = 1.0;
		FRackComponentParams F = A; F.Name = TEXT("Fan"); F.Kind = ERackPartKind::Fan; F.DefaultHeatW = 10.0; F.ToAirUAWK = 1.0; F.HeatCapacityJK = 30.0;
		D.Components = {A, B, F, F};
		P.DeviceTypes = {D};
		P.Slots.Add({0, 0.0, .04445});
		P.NumSleds = 1;
		P.CoolantHeatCapacityJK = 1.0e3;
		return P;
	}

	FRackThermalInput MakeFanTestInput(const FRackThermalParams& P)
	{
		FRackThermalInput In;
		In.ComponentHeatW = FRackThermalModel::MakeDefaultHeatW(P);
		In.SledDocked.Init(true, 1);
		In.CoolantMassFlowKgs = 0.0;
		In.AirInletTempK = 297.15;
		return In;
	}
}

// Heat follows utilization linearly between the idle fraction and full load; load-independent parts and
// the legacy uniform rack do not move at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLoadUtilizationTest, "VRG.Thermal.Load.UtilizationLinear", VrgFanTestFlags)
bool FLoadUtilizationTest::RunTest(const FString& Parameters)
{
	// 1) Analytic: parts 100 W (f 0.2) + 50 W (f 1) + fans. Fan override 0 -> no fan heat.
	const FRackThermalParams P = MakeFanTestRack(true);
	double MaxError = 0.0;
	for (const double U : {0.0, .25, .5, 1.0})
	{
		FRackThermalModel Model; Model.Initialize(P, FRackThermalState());
		FRackThermalInput In = MakeFanTestInput(P); In.Utilization = U; In.FanSpeedOverride = 0.0;
		Model.Step(In, .01);
		const double Expected = 100.0 * (.2 + .8 * U) + 50.0;
		MaxError = FMath::Max(MaxError, FMath::Abs(Model.GetLastTotalHeatW() - Expected) / 150.0);
	}
	AddInfo(FString::Printf(TEXT("부하율 0/0.25/0.5/1 발열 해석해 대비 최대 정규화 오차 %.3e (전부하 150 W 기준)"), MaxError));
	TestTrue(TEXT("Heat = P * (f + (1 - f) u)"), MaxError < 1e-15);

	// 2) Per-slot utilization overrides the scalar only when its length matches.
	{
		FRackThermalModel Model; Model.Initialize(P, FRackThermalState());
		FRackThermalInput In = MakeFanTestInput(P); In.Utilization = 1.0; In.SlotUtilization = {0.0}; In.FanSpeedOverride = 0.0;
		Model.Step(In, .01);
		TestEqual(TEXT("Slot utilization 0 -> idle heat"), Model.GetLastTotalHeatW(), 20.0 + 50.0);
	}

	// 3) Legacy uniform rack (15 kW hybrid, every part load independent): bit-identical at any utilization.
	{
		FRackThermalParams Legacy; Legacy.NumSleds = 12;
		FRackThermalState Init; Init.CoolantTempK = 300.15;
		FRackThermalModel M1, M2; M1.Initialize(Legacy, Init); M2.Initialize(Legacy, Init);
		FRackThermalInput I1; I1.ComponentHeatW = FRackThermalModel::MakeUniformHeatW(12);
		FRackThermalInput I2 = I1; I2.Utilization = .3;
		for (int32 S = 0; S < 500; ++S) { M1.Step(I1, .05); M2.Step(I2, .05); }
		double Diff = 0.0;
		for (int32 N = 0; N < M1.GetState().ComponentTempK.Num(); ++N)
			Diff = FMath::Max(Diff, FMath::Abs(M1.GetState().ComponentTempK[N] - M2.GetState().ComponentTempK[N]));
		AddInfo(FString::Printf(TEXT("레거시 균일 랙 부하율 1 대 0.3 최대 온도 차 %.3e K"), Diff));
		TestTrue(TEXT("Load-independent legacy rack is unchanged"), Diff == 0.0);
	}

	// 4) GB200 NVL72: idle rack power as a share of full load, and conservation through a load step.
	{
		const FRackThermalParams G = FRackThermalModel::MakeGb200Nvl72Params();
		FRackThermalState Init; Init.CoolantTempK = 300.15;
		FRackThermalModel Model; Model.Initialize(G, Init);
		FRackThermalInput In; In.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW(); In.SledDocked.Init(true, G.Slots.Num());
		In.CoolantMassFlowKgs = 2.3; In.FanSpeedOverride = .3;
		In.Utilization = 1.0; Model.Step(In, .05); const double FullW = Model.GetLastTotalHeatW();
		In.Utilization = 0.0; Model.Step(In, .05); const double IdleW = Model.GetLastTotalHeatW();
		for (int32 S = 0; S < 4000; ++S) { In.Utilization = (S / 1000) % 2 ? 1.0 : .1; Model.Step(In, .05); }
		AddInfo(FString::Printf(TEXT("GB200 NVL72 전부하 %.2f kW / 유휴 %.2f kW (%.1f%%) | 부하 계단 200 s 뒤 랙 열수지 정규화 잔차 %.3e"),
			FullW / 1000.0, IdleW / 1000.0, IdleW / FullW * 100.0, Model.GetNormalizedEnergyResidual()));
		// Plausibility: GPU servers idle at a sizeable fraction of peak (static power, links, fans); not zero, not full.
		TestTrue(TEXT("Idle rack power 10~40% of full load"), IdleW > .10 * FullW && IdleW < .40 * FullW);
		TestTrue(TEXT("Rack ledger closes through load steps"), FMath::Abs(Model.GetNormalizedEnergyResidual()) < 1e-10);
	}
	return true;
}

// Fan operating point is the intersection of the fan curve and the device impedance; affinity law for speed;
// fan electric input becomes heat in the fan parts and is counted in the rack ledger.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFanOperatingPointTest, "VRG.Thermal.Fan.OperatingPointAnalytic", VrgFanTestFlags)
bool FFanOperatingPointTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams P = MakeFanTestRack(true);
	const FRackFanSpec& Fans = P.DeviceTypes[0].Fans;
	const double R = P.DeviceTypes[0].DockedResistancePaS2Kg2, Rho = P.AirDensityKgM3;

	// 1) dP_system(mdot) == dP_fan(Q) at the returned point, for several speeds.
	double MaxCurveError = 0.0, MaxAffinityError = 0.0;
	const double FullKgs = FRackThermalModel::FanOperatingMassFlowKgs(Fans, R, Rho, 1.0);
	for (const double Ratio : {.3, .5, .75, 1.0})
	{
		const double Mdot = FRackThermalModel::FanOperatingMassFlowKgs(Fans, R, Rho, Ratio);
		const double Q = Mdot / Rho, Qt = Fans.NumFans * Fans.MaxFlowM3s;
		const double FanPa = Ratio * Ratio * Fans.ShutoffPressurePa * (1.0 - FMath::Square(Q / (Ratio * Qt)));
		const double SystemPa = R * Mdot * Mdot;
		MaxCurveError = FMath::Max(MaxCurveError, FMath::Abs(FanPa - SystemPa) / Fans.ShutoffPressurePa);
		MaxAffinityError = FMath::Max(MaxAffinityError, FMath::Abs(Mdot / FullKgs - Ratio));
	}
	AddInfo(FString::Printf(TEXT("전속 운전점 %.4f kg/s, 압력 %.1f Pa | 팬 곡선-임피던스 불일치 %.3e (체절압 2400 Pa 기준) | 상사칙(유량∝r) 오차 %.3e"),
		FullKgs, R * FullKgs * FullKgs, MaxCurveError, MaxAffinityError));
	TestTrue(TEXT("Operating point lies on both curves"), MaxCurveError < 1e-12);
	TestTrue(TEXT("Flow scales with speed ratio"), MaxAffinityError < 1e-12);
	TestEqual(TEXT("Stopped fans move no air (no division by flow)"), FRackThermalModel::FanOperatingMassFlowKgs(Fans, R, Rho, 0.0), 0.0);

	// 2) Fan electric input = N * P_rated * r^3 (D8: rated input is electric) and it enters the ledger as heat.
	FRackThermalModel Model; Model.Initialize(P, FRackThermalState());
	FRackThermalInput In = MakeFanTestInput(P); In.FanSpeedOverride = .5;
	for (int32 S = 0; S < 2000; ++S) Model.Step(In, .02);
	const FRackThermalSnapshot Snap = Model.MakeSnapshot(40.0);
	const double ExpectedFanW = 4 * 37.2 * .125;
	AddInfo(FString::Printf(TEXT("r=0.5 팬 전력 %.4f W (해석 %.4f W) | 슬롯 유량 %.4f kg/s | 소자 압력 상승 %.2f Pa | 랙 열수지 잔차 %.3e"),
		Snap.TotalFanPowerW, ExpectedFanW, Snap.SlotAirMassFlowKgs[0], Snap.SlotPressureRisePa[0], Model.GetNormalizedEnergyResidual()));
	TestTrue(TEXT("Fan power from the affinity law"), FMath::Abs(Snap.TotalFanPowerW - ExpectedFanW) < 1e-12 * ExpectedFanW);
	TestTrue(TEXT("Fan heat is part of the rack heat"), FMath::Abs(Snap.TotalHeatW - (150.0 + ExpectedFanW)) < 1e-12 * 150.0);
	TestTrue(TEXT("Rack ledger closes with fan heat"), FMath::Abs(Model.GetNormalizedEnergyResidual()) < 1e-11);
	TestTrue(TEXT("Slot flow is the fan operating point, not TotalAirMassFlowKgs"),
		FMath::Abs(Snap.SlotAirMassFlowKgs[0] - .5 * FullKgs) < 1e-14);
	return true;
}

// The controller raises fan speed with load, stays in [min, 1], respects its slew limit and cools the parts
// better than fans held at minimum speed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFanControlTest, "VRG.Thermal.Fan.ControllerResponds", VrgFanTestFlags)
bool FFanControlTest::RunTest(const FString& Parameters)
{
	const FRackThermalParams P = MakeFanTestRack(true);
	auto Run = [&](double Utilization, double Override, double& OutRatio, double& OutChipK, double& OutMaxSlew)
	{
		FRackThermalModel Model; FRackThermalState Init; Init.CoolantTempK = 297.15; Model.Initialize(P, Init);
		FRackThermalInput In = MakeFanTestInput(P); In.Utilization = Utilization; In.FanSpeedOverride = Override;
		OutMaxSlew = 0.0;
		double Prev = Model.GetState().SlotFanSpeedRatio[0];
		for (int32 S = 0; S < 30000; ++S)
		{
			Model.Step(In, .02);
			const double Now = Model.GetState().SlotFanSpeedRatio[0];
			OutMaxSlew = FMath::Max(OutMaxSlew, FMath::Abs(Now - Prev) / .02); Prev = Now;
		}
		OutRatio = Model.GetState().SlotFanSpeedRatio[0];
		OutChipK = Model.GetState().ComponentTempK[0];
	};
	double LowR, LowK, LowSlew, HighR, HighK, HighSlew, MinR, MinK, MinSlew;
	Run(0.0, -1.0, LowR, LowK, LowSlew);
	Run(1.0, -1.0, HighR, HighK, HighSlew);
	Run(1.0, .3, MinR, MinK, MinSlew);
	AddInfo(FString::Printf(TEXT("유휴: r=%.3f 칩 %.2f C | 전부하 자동: r=%.3f 칩 %.2f C | 전부하 최소속 고정: 칩 %.2f C | 최대 속도 변화율 %.3f /s (한도 0.2)"),
		LowR, LowK - 273.15, HighR, HighK - 273.15, MinK - 273.15, FMath::Max(LowSlew, HighSlew)));
	TestTrue(TEXT("Idle load keeps fans at minimum"), FMath::Abs(LowR - .3) < 1e-12);
	TestTrue(TEXT("Full load speeds fans up"), HighR > LowR + .05 && HighR <= 1.0);
	TestTrue(TEXT("Faster fans run the chip cooler than minimum speed"), HighK < MinK - .5);
	TestTrue(TEXT("Slew limit respected"), FMath::Max(LowSlew, HighSlew) <= .2 + 1e-9);
	return true;
}

// Fan ducts in the air grid: prescribed through-flow survives the projection, the field stays divergence
// free, the duct moves enthalpy without creating or destroying heat, and bad ducts are rejected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirDuctTest, "VRG.Air.Duct.ThroughFlowConservation", VrgFanTestFlags)
bool FAirDuctTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 12, NY = 6, NZ = 6;
	constexpr double H = .25, Q = .02;  // m, m^3/s
	FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;

	auto Setup = [&](FAirSolver& Air, FAirSolverInput& In, bool bHeat, bool bBuoyancy)
	{
		Air.Initialize(GP, 297.15);
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-10;
		Air.GetMutablePressureSettings().MaxIterations = 200;
		FUniformAirGrid& G = Air.GetMutableGrid();
		// Solid body X = 5..6, Y = 2..3, Z = 1..3. Front at +X (intake face X = 7), rear exhaust face X = 5.
		for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y) for (int32 X = 5; X <= 6; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		In = FAirSolverInput(); In.bEnableBuoyancy = bBuoyancy; In.CellHeatW.Init(0.0, G.GetNumCells());
		In.NumDucts = 1;
		for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y)
		{
			const double V = -Q / (6 * H * H);  // 6 faces per side, air moves -X
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(7, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(7, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(0); In.DuctFaceVelocityMs.Add(V);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(5, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(4, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(1); In.DuctFaceVelocityMs.Add(V);
			if (bHeat) In.CellHeatW[G.CellIndex(4, Y, Z)] = 500.0 / 6.0;
		}
	};

	// 1) Uniform air, no heat, no buoyancy: the duct must not change any temperature.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, false, false);
		for (int32 S = 0; S < 200; ++S) Air.Step(In, .02);
		double MaxDevK = 0.0;
		const FUniformAirGrid& G = Air.GetGrid();
		for (int32 I = 0; I < G.GetNumCells(); ++I) if (!G.bSolidCell[I]) MaxDevK = FMath::Max(MaxDevK, FMath::Abs(G.TemperatureK[I] - 297.15));
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(4.0);
		AddInfo(FString::Printf(TEXT("균일 공기 200스텝: 최대 온도 변화 %.3e K | 덕트 유량 %.6f m3/s | 정규화 발산 %.3e | 최대 속도 %.3f m/s"),
			MaxDevK, Snap.DuctFlowM3s, Snap.NormalizedDivergence, Snap.MaxSpeedMs));
		TestTrue(TEXT("Duct creates no spurious heat"), MaxDevK < 1e-10);
		TestTrue(TEXT("Duct flow reported"), FMath::Abs(Snap.DuctFlowM3s - Q) < 1e-12 && Snap.NumDuctFaces == 12 && !Snap.bDuctsRejected);
		TestTrue(TEXT("Divergence free with duct sources"), Snap.NormalizedDivergence < 1e-6);
		// Prescribed face velocity survives the projection exactly.
		double MaxFaceError = 0.0;
		for (int32 F = 0; F < In.DuctFaceIndex.Num(); ++F)
			MaxFaceError = FMath::Max(MaxFaceError, FMath::Abs(G.VelocityXMs[In.DuctFaceIndex[F]] - In.DuctFaceVelocityMs[F]));
		TestTrue(TEXT("Face velocity kept"), MaxFaceError == 0.0);
		// Air really moves through: the exhaust cell has a -X jet.
		TestTrue(TEXT("Jet leaves the exhaust face"), G.VelocityXMs[G.FaceXIndex(4, 2, 2)] < -.5 * Q / (6 * H * H));
	}

	// 2) Heat behind the body, buoyancy on: ledger closes, exhaust side warmer than intake side.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, true, true);
		for (int32 S = 0; S < 500; ++S) Air.Step(In, .02);
		const FUniformAirGrid& G = Air.GetGrid();
		const double InK = G.TemperatureK[G.CellIndex(7, 2, 2)], OutK = G.TemperatureK[G.CellIndex(4, 2, 2)];
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(10.0);
		AddInfo(FString::Printf(TEXT("500 W 10 s: 흡기 셀 %.3f C / 배기 셀 %.3f C | 공기 열수지 정규화 잔차 %.3e (유입 %.1f J) | 정규화 발산 %.3e | 압력 %s"),
			InK - 273.15, OutK - 273.15, Air.GetNormalizedEnergyResidual(), Air.GetEnergyInJ(), Snap.NormalizedDivergence,
			Snap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
		TestTrue(TEXT("Air ledger closes with a duct"), FMath::Abs(Air.GetNormalizedEnergyResidual()) < 1e-12);
		TestTrue(TEXT("Exhaust warmer than intake"), OutK > InK + .1);
		TestTrue(TEXT("Pressure converged"), Snap.bPressureConverged);
	}

	// 3) Unequal intake / exhaust flow is rejected (it would make the pressure problem incompatible).
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, false, false);
		In.DuctFaceVelocityMs[0] *= 2.0;
		AddExpectedError(TEXT("팬 덕트 입력 거부"), EAutomationExpectedErrorFlags::Contains, 1);
		Air.Step(In, .02);
		TestTrue(TEXT("Bad duct rejected and reported"), Air.MakeSnapshot(.02).bDuctsRejected && Air.GetGrid().MaxAbsVelocityMs() == 0.0);
	}
	return true;
}

// A cooling coil inside a duct (CRAH unit): epsilon-NTU exhaust temperature, saturation, zero flow, ledger.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirDuctCoilTest, "VRG.Air.Duct.CoilEffectivenessAndLedger", VrgFanTestFlags)
bool FAirDuctCoilTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 12, NY = 6, NZ = 6;
	constexpr double H = .25, Q = .2, WaterK = 285.15;  // m, m^3/s, K
	FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;
	const double CapacityWK = GP.DensityKgM3 * Q * GP.SpecificHeatJKgK;  // mdot * cp of the unit

	// Same body as the duct test: X = 5..6, Y = 2..3, Z = 1..3, intake at X = 7, exhaust at X = 4.
	auto Setup = [&](FAirSolver& Air, FAirSolverInput& In, double InitialK, double CoilUAWK, double FlowM3s)
	{
		Air.Initialize(GP, InitialK);
		Air.GetMutablePressureSettings().NormalizedResidualTarget = 1e-10;
		Air.GetMutablePressureSettings().MaxIterations = 200;
		FUniformAirGrid& G = Air.GetMutableGrid();
		for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y) for (int32 X = 5; X <= 6; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
		Air.OnSolidCellsChanged();
		In = FAirSolverInput(); In.bEnableBuoyancy = false; In.CellHeatW.Init(0.0, G.GetNumCells());
		In.NumDucts = 1; In.DuctCoilUAWK = {CoilUAWK}; In.DuctCoilWaterTempK = {WaterK};
		for (int32 Z = 1; Z <= 3; ++Z) for (int32 Y = 2; Y <= 3; ++Y)
		{
			const double V = -FlowM3s / (6 * H * H);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(7, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(7, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(0); In.DuctFaceVelocityMs.Add(V);
			In.DuctFaceAxis.Add(0); In.DuctFaceIndex.Add(G.FaceXIndex(5, Y, Z)); In.DuctFaceFluidCell.Add(G.CellIndex(4, Y, Z));
			In.DuctFaceDuct.Add(0); In.DuctFaceIsExhaust.Add(1); In.DuctFaceVelocityMs.Add(V);
		}
	};

	// 1) Uniform warm air, one step: removed heat = mdot cp eps (T - T_water), eps = 1 - exp(-NTU), NTU = 1.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 305.15, CapacityWK, Q);
		Air.Step(In, .02);
		const double Expected = CapacityWK * (1.0 - FMath::Exp(-1.0)) * (305.15 - WaterK);
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(.02);
		const double Err = FMath::Abs(Snap.DuctCoilRemovedW - Expected) / Expected;
		AddInfo(FString::Printf(TEXT("NTU 1 한 스텝: 제거 %.6f W / 해석 %.6f W, 정규화 오차 %.3e | 냉각 합계 %.6f W | 공기 열수지 %.3e"),
			Snap.DuctCoilRemovedW, Expected, Err, Snap.CoolingRemovedW, Air.GetNormalizedEnergyResidual()));
		TestTrue(TEXT("Coil removal matches epsilon-NTU"), Err < 1e-12);
		TestTrue(TEXT("Coil counted as air cooling"), Snap.CoolingRemovedW == Snap.DuctCoilRemovedW && !Snap.bDuctsRejected);
		// No heat source: the ledger is -out - stored = 0 (normalised by the removed heat here).
		const double Resid = FMath::Abs(Air.GetEnergyResidualJ()) / Air.GetEnergyOutJ();
		AddInfo(FString::Printf(TEXT("한 스텝 장부 잔차 %.3e (제거 %.6f J 기준)"), Resid, Air.GetEnergyOutJ()));
		TestTrue(TEXT("Ledger closes over one step"), Resid < 1e-12);
	}

	// 2) Saturation: a huge UA takes the exhaust to the water temperature, never below (eps -> 1).
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 305.15, 1e9, Q);
		for (int32 S = 0; S < 50; ++S) Air.Step(In, .02);
		const FUniformAirGrid& G = Air.GetGrid();
		double MinK = 1e9;
		for (int32 I = 0; I < G.GetNumCells(); ++I) if (!G.bSolidCell[I]) MinK = FMath::Min(MinK, G.TemperatureK[I]);
		FAirSolver Air1; FAirSolverInput In1; Setup(Air1, In1, 305.15, 1e9, Q);
		Air1.Step(In1, .02);
		const double Limit = CapacityWK * (305.15 - WaterK);
		const double Err = FMath::Abs(Air1.MakeSnapshot(.02).DuctCoilRemovedW - Limit) / Limit;
		AddInfo(FString::Printf(TEXT("UA 1e9: 한 스텝 제거 %.6f W / 한계 mdot*cp*dT %.6f W (정규화 %.3e) | 50스텝 뒤 최저 %.9f K (냉수 %.2f K)"),
			Air1.MakeSnapshot(.02).DuctCoilRemovedW, Limit, Err, MinK, WaterK));
		TestTrue(TEXT("Coil saturates at mdot cp (T - T_water)"), Err < 1e-12);
		TestTrue(TEXT("Air never colder than the water"), MinK >= WaterK - 1e-9);
	}

	// 3) Zero flow: no removal, no singular 1/(mdot cp) (D2).
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 305.15, CapacityWK, 0.0);
		Air.Step(In, .02);
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(.02);
		TestTrue(TEXT("Zero flow removes nothing"), Snap.DuctCoilRemovedW == 0.0 && FMath::IsFinite(Snap.MaxTemperatureK));
	}

	// 4) 2000 W source upstream of the unit with buoyancy: ledger closes and the coil carries the load at steady state.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 297.15, CapacityWK, Q);
		In.bEnableBuoyancy = true;
		FUniformAirGrid& G = Air.GetMutableGrid();
		for (int32 Y = 2; Y <= 3; ++Y) In.CellHeatW[G.CellIndex(10, Y, 1)] = 1000.0;
		const double Dt = .02;
		for (int32 S = 0; S < 20000; ++S) Air.Step(In, Dt);
		const FAirSolverSnapshot Snap = Air.MakeSnapshot(20000 * Dt);
		AddInfo(FString::Printf(TEXT("2000 W 400 s: 코일 %.2f W (발열 대비 %.4f) | 공기 %.2f ~ %.2f C | 열수지 %.3e (유입 %.4e J) | 발산 %.3e | 압력 %s"),
			Snap.DuctCoilRemovedW, Snap.DuctCoilRemovedW / 2000.0, Snap.MinTemperatureK - 273.15, Snap.MaxTemperatureK - 273.15,
			Air.GetNormalizedEnergyResidual(), Air.GetEnergyInJ(), Snap.NormalizedDivergence, Snap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
		TestTrue(TEXT("Ledger closes with a coil"), FMath::Abs(Air.GetNormalizedEnergyResidual()) < 1e-12);
		TestTrue(TEXT("Coil carries the load at steady state"), FMath::Abs(Snap.DuctCoilRemovedW / 2000.0 - 1.0) < .02);
		TestTrue(TEXT("Pressure converged"), Snap.bPressureConverged);
	}

	// 5) Fan heat (draw-through, after the coil): uniform air, no coil. One step adds exactly P*dt to the air,
	//    the supply is P/(mdot cp) warmer than the intake, and the ledger counts it as an input.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 297.15, 0.0, Q);
		constexpr double FanW = 1500.0;
		In.DuctFanHeatW = {FanW};
		Air.Step(In, .02);
		const FUniformAirGrid& G = Air.GetGrid();
		const double RiseK = G.TemperatureK[G.CellIndex(4, 2, 2)] - 297.15;
		const double StoredErr = FMath::Abs(Air.GetEnergyStoredJ() - FanW * .02) / (FanW * .02);
		const double InErr = FMath::Abs(Air.GetDuctFanHeatJ() - FanW * .02) / (FanW * .02);
		// The exhaust cell took one step of supply at +P/C over dt; its own volume dilutes it (checked loosely).
		AddInfo(FString::Printf(TEXT("팬 1500 W 한 스텝: 축적 오차 %.3e, 입력 장부 오차 %.3e (기준 %.1f J) | 토출 셀 상승 %.4e K, P/(mdot cp) = %.4f K | 열수지 %.3e"),
			StoredErr, InErr, FanW * .02, RiseK, FanW / CapacityWK, Air.GetNormalizedEnergyResidual()));
		TestTrue(TEXT("Fan heat reaches the air exactly"), StoredErr < 1e-12);
		TestTrue(TEXT("Fan heat booked as input"), InErr < 1e-12 && FMath::Abs(Air.GetNormalizedEnergyResidual()) < 1e-12);
		TestTrue(TEXT("Supply side warmed"), RiseK > 0.0);
		// No flow, no fan: nothing is added.
		FAirSolver Air0; FAirSolverInput In0; Setup(Air0, In0, 297.15, 0.0, 0.0);
		In0.DuctFanHeatW = {FanW};
		Air0.Step(In0, .02);
		TestTrue(TEXT("Stopped fan adds nothing"), Air0.GetDuctFanHeatJ() == 0.0 && Air0.GetEnergyStoredJ() == 0.0);
	}

	// 6) Coil arrays of the wrong length are rejected like any bad duct.
	{
		FAirSolver Air; FAirSolverInput In; Setup(Air, In, 305.15, CapacityWK, Q);
		In.DuctCoilWaterTempK.Add(WaterK);
		AddExpectedError(TEXT("팬 덕트 입력 거부"), EAutomationExpectedErrorFlags::Contains, 1);
		Air.Step(In, .02);
		TestTrue(TEXT("Bad coil rejected"), Air.MakeSnapshot(.02).bDuctsRejected && Air.MakeSnapshot(.02).DuctCoilRemovedW == 0.0);
	}
	return true;
}

// Full coupling with fan ducts: a GB200 NVL72 rack in a small room. Its slots breathe the duct intake air,
// the grid carries the fans' mass flow, and the global ledger still closes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFanCouplingTest, "VRG.Coupling.FanDuctsEnergyAndFlow", VrgFanTestFlags)
bool FFanCouplingTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 10, NY = 6, NZ = 12;
	constexpr double H = .25;
	FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;
	FAirSolver Air; Air.Initialize(GP, 297.15);
	FUniformAirGrid& G = Air.GetMutableGrid();
	// Rack body X = 4..5, Y = 2..3, Z = 0..8 (2.25 m). Front +X: intake faces X = 6, exhaust faces X = 4.
	for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 2; Y <= 3; ++Y) for (int32 X = 4; X <= 5; ++X) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
	Air.OnSolidCellsChanged();

	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalModel Model; FRackThermalState Init; Init.CoolantTempK = 300.15; Model.Initialize(Params, Init);
	FRackThermalInput Input; Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.Slots.Num()); Input.CoolantMassFlowKgs = 2.3;
	FRadiatorParams Radiator; Radiator.UAWK = 1.1e4;

	FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs = 2.3;
	TArray<double> LayerZ;
	for (int32 Z = 0; Z <= 8; ++Z)
	{
		LayerZ.Add((Z + .5) * H);
		for (int32 Y = 2; Y <= 3; ++Y)
		{
			Link.FanFaceAxis.Add(0); Link.FanFaceIndex.Add(G.FaceXIndex(6, Y, Z)); Link.FanFaceFluidCell.Add(G.CellIndex(6, Y, Z));
			Link.FanFaceLayer.Add(Z); Link.FanFaceIsExhaust.Add(0); Link.FanFaceSign.Add(-1);
			Link.FanFaceAxis.Add(0); Link.FanFaceIndex.Add(G.FaceXIndex(4, Y, Z)); Link.FanFaceFluidCell.Add(G.CellIndex(3, Y, Z));
			Link.FanFaceLayer.Add(Z); Link.FanFaceIsExhaust.Add(1); Link.FanFaceSign.Add(-1);
			Link.InletCellIndices.Add(G.CellIndex(6, Y, Z)); Link.ExhaustCellIndices.Add(G.CellIndex(3, Y, Z));
		}
	}
	TArray<double> SlotBottom, SlotHeight;
	for (const FRackSlot& S : Params.Slots) { SlotBottom.Add(S.BottomZM); SlotHeight.Add(S.HeightM); }
	FCouplingBridge::AssignSlotsToFanLayers(Link, LayerZ, H, SlotBottom, SlotHeight);

	FCouplingBridge Bridge;
	Bridge.Initialize({{&Model, &Input, &Radiator}}, {Link});
	Bridge.SetAirSolver(&Air);
	// Ceiling coil sized for the rack's air-side design load at a 10 K approach (as the integrated actor does).
	Bridge.AirSolverInput.CellCoolingUAWK.Init(0.0, G.GetNumCells());
	const double CoilUA = FRackThermalModel::DesignHeatToAirW(Params, Input) / 10.0;
	for (int32 Y = 0; Y < NY; ++Y) for (int32 X = 0; X < NX; ++X) Bridge.AirSolverInput.CellCoolingUAWK[G.CellIndex(X, Y, NZ - 1)] = CoilUA / (NX * NY);
	Bridge.AirSolverInput.CoolingSupplyTempK = 291.15;

	const double Dt = .02;
	for (int32 S = 0; S < 6000; ++S) Bridge.Step(Dt, false, true);
	const FCouplingSnapshot Snap = Bridge.MakeSnapshot(6000 * Dt);
	const FRackThermalSnapshot RackSnap = Model.MakeSnapshot(6000 * Dt);
	const FAirSolverSnapshot AirSnap = Air.MakeSnapshot(6000 * Dt);

	// Slot inlet = duct intake temperature of its layers (the same air the grid removed).
	double MaxInletMismatchK = 0.0;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
	{
		double Sum = 0.0, W = 0.0;
		for (int32 L = 0; L < Link.NumFanLayers(); ++L)
			for (int32 K = Link.LayerSlotStart[L]; K < Link.LayerSlotStart[L + 1]; ++K)
				if (Link.LayerSlot[K] == Slot)
				{
					const double LayerK = .5 * (G.TemperatureK[G.CellIndex(6, 2, L)] + G.TemperatureK[G.CellIndex(6, 3, L)]);
					Sum += Link.LayerSlotWeight[K] * LayerK; W += Link.LayerSlotWeight[K];
				}
		// Inlet was sampled at the start of the last step; compare to that step's grid state is not possible
		// here, so check against the current grid with a loose band (one step of change).
		if (W > 0.0) MaxInletMismatchK = FMath::Max(MaxInletMismatchK, FMath::Abs(Input.SlotAirInletTempK[Slot] - Sum / W));
	}
	double MaxTrayRiseK = 0.0, MinTrayRiseK = 1e9;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
		if (Params.Slots[Slot].DeviceType == 0)
		{
			const double Rise = RackSnap.SledAirOutletTempK[Slot] - Input.SlotAirInletTempK[Slot];
			MaxTrayRiseK = FMath::Max(MaxTrayRiseK, Rise); MinTrayRiseK = FMath::Min(MinTrayRiseK, Rise);
		}
	double MinR = 1.0, MaxR = 0.0;
	for (int32 Slot = 0; Slot < Params.Slots.Num(); ++Slot)
		if (FRackThermalModel::FansOf(Params, Slot)) { MinR = FMath::Min(MinR, RackSnap.SlotFanSpeedRatio[Slot]); MaxR = FMath::Max(MaxR, RackSnap.SlotFanSpeedRatio[Slot]); }

	AddInfo(FString::Printf(TEXT("120 s: IT %.2f kW (팬 %.3f kW) | 랙 풍량 %.4f kg/s, 격자 관통 %.4f kg/s | 팬 속도 %.2f ~ %.2f"),
		Snap.TotalItHeatW / 1000.0, Snap.TotalFanPowerW / 1000.0, Snap.RackAirMassFlowKgs[0], Snap.RackDuctMassFlowKgs[0], MinR, MaxR));
	AddInfo(FString::Printf(TEXT("컴퓨트 트레이 공기 상승 %.2f ~ %.2f K | 슬롯 흡기-층 흡기 차 %.3e K | 격자 덕트 유량 %.4f m3/s | CFL %.3f"),
		MinTrayRiseK, MaxTrayRiseK, MaxInletMismatchK, AirSnap.DuctFlowM3s, AirSnap.CourantNumber));
	AddInfo(FString::Printf(TEXT("전역 열수지 정규화 잔차 %.3e (누적 IT 입력 %.4e J 기준) | 공기 정규화 발산 %.3e | 압력 %s"),
		Snap.NormalizedEnergyResidual, Snap.EnergyInJ, AirSnap.NormalizedDivergence, AirSnap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
	AddInfo(Bridge.DescribeEnergyLedger());

	TestTrue(TEXT("Global ledger closes"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("Every slot's flow passes through the grid"), FMath::Abs(Snap.RackDuctMassFlowKgs[0] - Snap.RackAirMassFlowKgs[0]) < 1e-12 * Snap.RackAirMassFlowKgs[0]);
	TestTrue(TEXT("Grid duct volume flow = rack mass flow / rho"), FMath::Abs(AirSnap.DuctFlowM3s - Snap.RackAirMassFlowKgs[0] / GP.DensityKgM3) < 1e-9 * AirSnap.DuctFlowM3s);
	TestTrue(TEXT("Slots breathe their duct intake air"), MaxInletMismatchK < .5);
	TestTrue(TEXT("Divergence free"), AirSnap.NormalizedDivergence < 1e-5);
	// Plausibility (not physics): fan power < 2% of IT; compute-tray air rise within 5~30 K; speeds in range.
	TestTrue(TEXT("Fan power below 2% of IT"), Snap.TotalFanPowerW > 0.0 && Snap.TotalFanPowerW < .02 * Snap.TotalItHeatW);
	TestTrue(TEXT("Compute tray air rise 5~30 K"), MinTrayRiseK > 5.0 && MaxTrayRiseK < 30.0);
	TestTrue(TEXT("Fan speeds within [min, 1]"), MinR >= .3 - 1e-12 && MaxR <= 1.0);
	return true;
}

// The same rack cooled by one in-row CRAH unit instead of the lumped ceiling coil: the unit breathes the hot
// side, supplies the cold side, and is the only way heat leaves the air. The global ledger still closes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrahCouplingTest, "VRG.Coupling.CrahUnitCarriesAirLoad", VrgFanTestFlags)
bool FCrahCouplingTest::RunTest(const FString& Parameters)
{
	constexpr int32 NX = 10, NY = 6, NZ = 12;
	constexpr double H = .25, DesignDeltaTK = 12.0, ApproachK = 5.0, WaterK = 291.15;
	FAirGridParams GP; GP.NumCellsX = NX; GP.NumCellsY = NY; GP.NumCellsZ = NZ; GP.CellSizeM = H;
	FAirSolver Air; Air.Initialize(GP, 297.15);
	FUniformAirGrid& G = Air.GetMutableGrid();
	// Rack body X = 4..5, Y = 1..2; CRAH body X = 4..5, Y = 4..5 (Z = 0..8 both). Rack front +X (cold side X = 6),
	// rear X = 3 (hot side). The CRAH takes in at X = 3 and supplies at X = 6.
	for (int32 Z = 0; Z <= 8; ++Z) for (int32 X = 4; X <= 5; ++X)
		for (const int32 Y : {1, 2, 4, 5}) G.bSolidCell[G.CellIndex(X, Y, Z)] = true;
	Air.OnSolidCellsChanged();

	const FRackThermalParams Params = FRackThermalModel::MakeGb200Nvl72Params();
	FRackThermalModel Model; FRackThermalState Init; Init.CoolantTempK = 300.15; Model.Initialize(Params, Init);
	FRackThermalInput Input; Input.ComponentHeatW = FRackThermalModel::MakeGb200Nvl72HeatW();
	Input.SledDocked.Init(true, Params.Slots.Num()); Input.CoolantMassFlowKgs = 2.3;
	FRadiatorParams Radiator; Radiator.UAWK = 1.1e4;

	FRackCouplingLink Link; Link.FallbackCoolantMassFlowKgs = 2.3;
	FCrahCouplingUnit Crah;
	TArray<double> LayerZ;
	for (int32 Z = 0; Z <= 8; ++Z)
	{
		LayerZ.Add((Z + .5) * H);
		for (int32 Y = 1; Y <= 2; ++Y)
		{
			Link.FanFaceAxis.Add(0); Link.FanFaceIndex.Add(G.FaceXIndex(6, Y, Z)); Link.FanFaceFluidCell.Add(G.CellIndex(6, Y, Z));
			Link.FanFaceLayer.Add(Z); Link.FanFaceIsExhaust.Add(0); Link.FanFaceSign.Add(-1);
			Link.FanFaceAxis.Add(0); Link.FanFaceIndex.Add(G.FaceXIndex(4, Y, Z)); Link.FanFaceFluidCell.Add(G.CellIndex(3, Y, Z));
			Link.FanFaceLayer.Add(Z); Link.FanFaceIsExhaust.Add(1); Link.FanFaceSign.Add(-1);
			Link.InletCellIndices.Add(G.CellIndex(6, Y, Z)); Link.ExhaustCellIndices.Add(G.CellIndex(3, Y, Z));
		}
		for (int32 Y = 4; Y <= 5; ++Y)
		{
			Crah.FaceAxis.Add(0); Crah.FaceIndex.Add(G.FaceXIndex(4, Y, Z)); Crah.FaceFluidCell.Add(G.CellIndex(3, Y, Z));
			Crah.FaceIsExhaust.Add(0); Crah.FaceSign.Add(1);
			Crah.FaceAxis.Add(0); Crah.FaceIndex.Add(G.FaceXIndex(6, Y, Z)); Crah.FaceFluidCell.Add(G.CellIndex(6, Y, Z));
			Crah.FaceIsExhaust.Add(1); Crah.FaceSign.Add(1);
		}
	}
	TArray<double> SlotBottom, SlotHeight;
	for (const FRackSlot& S : Params.Slots) { SlotBottom.Add(S.BottomZM); SlotHeight.Add(S.HeightM); }
	FCouplingBridge::AssignSlotsToFanLayers(Link, LayerZ, H, SlotBottom, SlotHeight);

	// Sized as the integrated actor does (design assumptions): flow from the design air load at a 12 K rise,
	// UA from epsilon-NTU so the supply is 5 K above the water at design flow.
	const double DesignAirW = FRackThermalModel::DesignHeatToAirW(Params, Input);
	Crah.VolumeFlowM3s = DesignAirW / (GP.DensityKgM3 * GP.SpecificHeatJKgK * DesignDeltaTK);
	Crah.CoilUAWK = -FMath::Loge(1.0 - DesignDeltaTK / (DesignDeltaTK + ApproachK)) * GP.DensityKgM3 * Crah.VolumeFlowM3s * GP.SpecificHeatJKgK;
	Crah.WaterSupplyTempK = WaterK;
	// Operating-point fan input (D8), the actor's estimates: 250 Pa, efficiency 0.6.
	Crah.FanPowerW = 250.0 * Crah.VolumeFlowM3s / .6;

	FCouplingBridge Bridge;
	Bridge.Initialize({{&Model, &Input, &Radiator}}, {Link});
	Bridge.SetCrahUnits({Crah});
	Bridge.SetAirSolver(&Air);

	const double Dt = .02;
	constexpr int32 Steps = 15000;
	for (int32 S = 0; S < Steps; ++S) Bridge.Step(Dt, false, true);
	const FCouplingSnapshot Snap = Bridge.MakeSnapshot(Steps * Dt);
	const FAirSolverSnapshot AirSnap = Air.MakeSnapshot(Steps * Dt);
	// Rack air heat now = forced air + case convection handed to the grid.
	const double RackAirW = Snap.RackHeatToAirW[0];
	double SupplyK = 0.0, ReturnK = 0.0;
	for (int32 Z = 0; Z <= 8; ++Z) for (int32 Y = 4; Y <= 5; ++Y) { SupplyK += G.TemperatureK[G.CellIndex(6, Y, Z)]; ReturnK += G.TemperatureK[G.CellIndex(3, Y, Z)]; }
	SupplyK /= 18.0; ReturnK /= 18.0;
	double MinInletK = 1e9, MaxInletK = 0.0;
	for (const double K : Input.SlotAirInletTempK) { MinInletK = FMath::Min(MinInletK, K); MaxInletK = FMath::Max(MaxInletK, K); }

	AddInfo(FString::Printf(TEXT("%d s: 설계 공기 부하 %.2f kW -> CRAH %.4f m3/s, UA %.1f W/K | 랙 공기 반출 %.2f kW, CRAH 코일 %.2f kW (비 %.4f) | 공기 냉각 합계 %.2f kW"),
		int32(Steps * Dt), DesignAirW / 1000.0, Crah.VolumeFlowM3s, Crah.CoilUAWK, RackAirW / 1000.0, Snap.CrahCoilRemovedW / 1000.0,
		Snap.CrahCoilRemovedW / RackAirW, Snap.TotalAirCoolingRemovedW / 1000.0));
	AddInfo(FString::Printf(TEXT("CRAH 환기 %.2f C / 토출 %.2f C (냉수 %.2f C) | 슬롯 흡기 %.2f ~ %.2f C | 전역 잔차 %.3e (유입 %.4e J) | 발산 %.3e | 압력 %s"),
		ReturnK - 273.15, SupplyK - 273.15, WaterK - 273.15, MinInletK - 273.15, MaxInletK - 273.15, Snap.NormalizedEnergyResidual, Snap.EnergyInJ,
		AirSnap.NormalizedDivergence, AirSnap.bPressureConverged ? TEXT("수렴") : TEXT("실패")));
	AddInfo(Bridge.DescribeEnergyLedger());
	AddInfo(FString::Printf(TEXT("CRAH 팬 %.1f W (스냅샷 %.1f W) | 전역 유입 %.6e J = IT + 팬"), Crah.FanPowerW, Snap.CrahFanPowerW, Snap.EnergyInJ));

	TestTrue(TEXT("Global ledger closes"), FMath::Abs(Snap.NormalizedEnergyResidual) < 1e-9);
	TestTrue(TEXT("CRAH fan power reported"), FMath::Abs(Snap.CrahFanPowerW - Crah.FanPowerW) < 1e-9 * Crah.FanPowerW);
	TestTrue(TEXT("Only the CRAH cools the air"), Snap.CrahCoilRemovedW > 0.0 && FMath::Abs(Snap.TotalAirCoolingRemovedW - Snap.CrahCoilRemovedW) < 1e-9 * Snap.CrahCoilRemovedW);
	TestTrue(TEXT("CRAH flow reported"), FMath::Abs(Snap.CrahFlowM3s - Crah.VolumeFlowM3s) < 1e-12);
	TestTrue(TEXT("Divergence free"), AirSnap.NormalizedDivergence < 1e-5);
	TestTrue(TEXT("Pressure converged"), AirSnap.bPressureConverged);
	// Plausibility (not physics): near steady state the unit carries the rack's air heat; supply between the
	// water and the return; slot inlets within 15~35 C.
	TestTrue(TEXT("CRAH carries the air heat and its own fan heat"), FMath::Abs(Snap.CrahCoilRemovedW / (RackAirW + Crah.FanPowerW) - 1.0) < .05);
	TestTrue(TEXT("Supply between water and return"), SupplyK > WaterK && SupplyK < ReturnK);
	TestTrue(TEXT("Slot inlets 15~35 C"), MinInletK > 288.15 && MaxInletK < 308.15);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
