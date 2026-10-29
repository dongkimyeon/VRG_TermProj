// Fill out your copyright notice in the Description page of Project Settings.

// W1 live leaks (Tools/누수_실시간_계획.md L1, 2026-10-23): every rack has three closed leak sites built with the room
// (supply branch, return branch, supply header tee). Closed, they change nothing; during a run a leak is a hole size
// change at a site (no rebuild, the coupling keeps its pipe numbers), and the inventory, coupling and ledgers follow.

#include "Misc/AutomationTest.h"
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
	constexpr EAutomationTestFlags VrgLiveLeakFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter;

	// One row of two GB200 racks + one CDU + air (the S2b test room), with or without the live sites.
	struct FLiveLeakRoom
	{
		UWorld* World = nullptr;
		ADataCenterSimulationActor* Sim = nullptr;
		ADataCenterRoomActor* Room = nullptr;

		explicit FLiveLeakRoom(bool bSites, bool bPlant = false)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			Sim = World->SpawnActor<ADataCenterSimulationActor>();
			Sim->bShowSummary = false;
			Sim->MaxStepsPerFrame = 16;
			Sim->bFacilityPlant = bPlant;
			Room = World->SpawnActor<ADataCenterRoomActor>();
			Room->bLiveLeakSites = bSites;
			World->SpawnActor<AAirFieldDebugActor>()->bShowSummary = false;
			for (const double Y : { 0.0, 66.0 })
			{
				ARackThermalActor* Rack = World->SpawnActor<ARackThermalActor>(FVector(0, Y, 0), FRotator::ZeroRotator);
				Rack->ApplyGb200Nvl72Preset(); Rack->bShowReadout = false;
				Rack->RackSizeM = FVector(GjRackLayout::FrameDepthM, GjRackLayout::FrameWidthM, GjRackLayout::FrameHeightM);
			}
			World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-300, 100, 0), FRotator::ZeroRotator);
			Room->RebuildRoomNow();
		}
		~FLiveLeakRoom()
		{
			Sim->EnterLayoutMode();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		void Run(double Seconds) { for (int32 F = 0; F < FMath::RoundToInt32(Seconds / 0.1); ++F) Sim->Tick(.1f); }
		// The live site with this label prefix ("랙 0 공급 분기", ...).
		int32 SiteOf(const FString& Label) const
		{
			const auto& Sites = Sim->GetLiveLeakSites();
			for (int32 I = 0; I < Sites.Num(); ++I) if (Sites[I].Label == Label) return I;
			return INDEX_NONE;
		}
		// Loop mass balance of a tank [m^3]: initial + make-up - leaked - water now (0 when the volumes are consistent).
		double TankMassError(int32 Tank) const
		{
			const FCoolantInventory::FTank& T = Room->GetCoolantInventory().GetTanks()[Tank];
			return T.State.InitialWaterM3 + T.State.MakeupM3 - T.State.LeakedM3 - T.State.WaterVolumeM3;
		}
		double CoolantLedgerError() const
		{
			const FCouplingSnapshot S = Sim->GetSnapshot();
			const double Out = S.CoolantRejectedJ + S.CoolantLeakHeatJ;
			return Out > 0.0 ? FMath::Abs(S.RackCoolantOutJ - Out) / Out : FMath::Abs(S.RackCoolantOutJ);
		}
		// Worst |coupled rack coolant flow - its branch flow x density| / coupled flow (the cross-check of CLAUDE.md).
		double WorstCoupledBranchRel() const
		{
			const FVrgDashboardLeaks D = Sim->BuildDashboardLeaks();
			double Worst = D.Racks.Num() == 0 ? 1.0 : 0.0;
			for (const FVrgDashboardLeakRack& R : D.Racks)
			{
				const double Expected = FMath::Abs(R.FlowM3s) * Sim->CoolantDensityKgM3;
				Worst = FMath::Max(Worst, FMath::Abs(R.CoolantKgs - Expected) / FMath::Max(Expected, 1e-30));
			}
			return Worst;
		}
	};

	double MaxRelDiff(const TArray<double>& A, const TArray<double>& B)
	{
		if (A.Num() != B.Num() || A.Num() == 0) return 1.0;
		double Worst = 0.0;
		for (int32 I = 0; I < A.Num(); ++I) Worst = FMath::Max(Worst, FMath::Abs(A[I] - B[I]) / FMath::Max(FMath::Abs(B[I]), 1e-30));
		return Worst;
	}
	double MaxAbsDiff(const TArray<double>& A, const TArray<double>& B)
	{
		if (A.Num() != B.Num() || A.Num() == 0) return 1.0;
		double Worst = 0.0;
		for (int32 I = 0; I < A.Num(); ++I) Worst = FMath::Max(Worst, FMath::Abs(A[I] - B[I]));
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveLeakClosedSitesTest, "VRG.Hydraulics.LiveLeak.ClosedSitesChangeNothing", VrgLiveLeakFlags)
bool FLiveLeakClosedSitesTest::RunTest(const FString& Parameters)
{
	FCouplingSnapshot Without, With;
	int32 LeaksWithout = -1, LeaksWith = -1, SitesWith = -1, SolvesWith = -1, ShownWith = -1, NodesWithout = 0, NodesWith = 0;
	{
		FLiveLeakRoom R(false);
		R.Sim->EnterSimulationMode();
		R.Run(3.0);
		Without = R.Sim->GetSnapshot();
		LeaksWithout = R.Room->GetNetwork().GetNumLeaks();
		NodesWithout = R.Room->GetNetwork().GetNumNodes();
	}
	{
		FLiveLeakRoom R(true);
		R.Sim->EnterSimulationMode();
		R.Run(3.0);
		With = R.Sim->GetSnapshot();
		LeaksWith = R.Room->GetNetwork().GetNumLeaks();
		NodesWith = R.Room->GetNetwork().GetNumNodes();
		SitesWith = R.Sim->GetLiveLeakSites().Num();
		SolvesWith = R.Room->GetCoolantInventory().GetNumSolves();
		ShownWith = 0;
		for (int32 S = 0; S < R.Room->GetNumLeakSpecs(); ++S) ShownWith += R.Room->IsLeakSpecShown(S) ? 1 : 0;
	}
	const double FlowRel = MaxRelDiff(With.RackCoolantMassFlowKgs, Without.RackCoolantMassFlowKgs);
	const double DutyRel = MaxRelDiff(With.CduDutyW, Without.CduDutyW);
	const double InletK = MaxAbsDiff(With.RackAirInletTempK, Without.RackAirInletTempK);
	const double ReturnK = MaxAbsDiff(With.CduReturnTempK, Without.CduReturnTempK);
	AddInfo(FString::Printf(TEXT("닫힌 지점 %d개(망 누수 %d / 없을 때 %d, 노드 %d / %d), 재고 재풀이 %d회, 보이는 누수 %d | 3 s 뒤 상대 차: 랙 냉각수 유량 %.3e, CDU 부하 %.3e | 절대 차: 랙 흡기 %.3e K, CDU 회수 %.3e K"),
		SitesWith, LeaksWith, LeaksWithout, NodesWith, NodesWithout, SolvesWith, ShownWith, FlowRel, DutyRel, InletK, ReturnK));
	// W3.5 internal sites exist in both worlds; W1 still adds only three public sites per rack.
	TestTrue(TEXT("three external closed sites per rack, internal sites retained, none shown, inventory idle"),
		SitesWith == 6 && LeaksWith - LeaksWithout == 6 && LeaksWithout == 2 * 27 * 5 && SolvesWith == 0 && ShownWith == 0);
	TestTrue(TEXT("closed sites change neither flows nor duties (relative < 1e-12)"), FlowRel < 1e-12 && DutyRel < 1e-12);
	TestTrue(TEXT("closed sites change no temperature (< 1e-9 K)"), InletK < 1e-9 && ReturnK < 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveLeakOpenRepairTest, "VRG.Hydraulics.LiveLeak.OpenRepairNoRebuild", VrgLiveLeakFlags)
bool FLiveLeakOpenRepairTest::RunTest(const FString& Parameters)
{
	FLiveLeakRoom R(true);
	const int32 PipesBuilt = R.Room->GetNetwork().GetNumPipes();
	R.Sim->RebuildLiveLeakSites();
	const int32 Site = R.SiteOf(TEXT("랙 0 공급 분기"));
	AddExpectedMessagePlain(TEXT("실시간 누수는 시뮬레이션 모드에서만"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const bool bLayoutRefused = !R.Sim->SetLiveLeak(Site, 5.0e-3, TEXT("test"));

	R.Sim->EnterSimulationMode();
	R.Run(0.5);
	const double FlowBefore = R.Sim->GetSnapshot().RackCoolantMassFlowKgs[0];
	const bool bOpened = R.Sim->SetLiveLeak(R.SiteOf(TEXT("랙 0 공급 분기")), 5.0e-3, TEXT("test"));
	R.Run(1.5);
	const FCoolantInventory::FTank& Tank = R.Room->GetCoolantInventory().GetTanks()[0];
	const double LeakM3s = Tank.LastLeakM3s, PressurePa = R.Room->GetCoolantInventory().GetTankGaugePressurePa(0), FillPa = Tank.FillGaugePa;
	const double FlowOpen = R.Sim->GetSnapshot().RackCoolantMassFlowKgs[0];
	const double LeakedOpen = Tank.State.LeakedM3;
	const double CrossOpen = R.WorstCoupledBranchRel();
	const double MassOpen = FMath::Abs(R.TankMassError(0)) / FMath::Max(LeakedOpen, 1e-30);
	const int32 ShownOpen = R.Sim->BuildDashboardLeaks().Leaks.Num();
	const bool bRepaired = R.Sim->SetLiveLeak(R.SiteOf(TEXT("랙 0 공급 분기")), 0.0, TEXT("test"));
	R.Run(0.1);
	const double LeakedAtRepair = Tank.State.LeakedM3;
	R.Run(1.0);
	const double LeakAfter = R.Room->GetNetwork().GetLeakFlowM3s(R.Room->GetLeakNetworkIndex(R.Sim->GetLiveLeakSites()[R.SiteOf(TEXT("랙 0 공급 분기"))].RoomSpec));
	const double LeakedAfter = Tank.State.LeakedM3;
	const double Ledger = R.CoolantLedgerError();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	const double MassAfter = FMath::Abs(R.TankMassError(0)) / FMath::Max(LeakedAfter, 1e-30);
	const TArray<FVrgLeakRecord>& Records = R.Sim->GetFaultMonitor().GetLeaks();
	AddInfo(FString::Printf(TEXT("배치 모드 거절 %d | 5 mm 열기 %d: 누수 %.4f L/s, 탱크 %.1f kPa(g) (충전 %.1f), 랙 0 냉각수 %.4f -> %.4f kg/s, 누적 %.4f L, 보이는 누수 %d | 결합/분기x밀도 최대 상대 차 %.3e | 재고 질량 상대 오차 %.3e / 수리 뒤 %.3e | 수리 %d: 누수 %.3e L/s, 누적 %.6f -> %.6f L | 냉각수 장부 %.3e, 전역 %.3e | 배관 %d = %d | 기록 %d건 (%s)"),
		bLayoutRefused ? 1 : 0, bOpened ? 1 : 0, LeakM3s * 1e3, PressurePa * 1e-3, FillPa * 1e-3, FlowBefore, FlowOpen, LeakedOpen * 1e3, ShownOpen,
		CrossOpen, MassOpen, MassAfter, bRepaired ? 1 : 0, LeakAfter * 1e3, LeakedAtRepair * 1e3, LeakedAfter * 1e3, Ledger, Global,
		R.Room->GetNetwork().GetNumPipes(), PipesBuilt, Records.Num(), Records.Num() ? *Records[0].Site : TEXT("-")));
	TestTrue(TEXT("a live leak is refused outside a run"), bLayoutRefused);
	TestTrue(TEXT("open: water leaks, the tank pressure falls, the rack gets less coolant, the leak is shown"),
		bOpened && LeakM3s > 0.0 && PressurePa < FillPa && FlowOpen < FlowBefore && LeakedOpen > 0.0 && ShownOpen == 1);
	TestTrue(TEXT("no rebuild: the network keeps its pipes"), R.Room->GetNetwork().GetNumPipes() == PipesBuilt);
	TestTrue(TEXT("coupled flow = branch flow x density for every rack (exact)"), CrossOpen == 0.0);
	TestTrue(TEXT("inventory mass balance (relative to the leaked volume < 1e-12)"), MassOpen < 1e-12 && MassAfter < 1e-12);
	TestTrue(TEXT("repair: no more flow, nothing more leaks"), bRepaired && LeakAfter == 0.0 && LeakedAfter == LeakedAtRepair);
	TestTrue(TEXT("coolant and global ledgers close (< 1e-9)"), Ledger < 1e-9 && Global < 1e-9);
	TestTrue(TEXT("both changes recorded with the site and its position"), Records.Num() == 2 && Records[0].Site == TEXT("랙 0 공급 분기")
		&& !Records[0].PointM.IsZero() && Records[1].HoleDiameterM == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveLeakHeaderTest, "VRG.Hydraulics.LiveLeak.HeaderSiteNearestPoint", VrgLiveLeakFlags)
bool FLiveLeakHeaderTest::RunTest(const FString& Parameters)
{
	FLiveLeakRoom R(true);
	R.Sim->EnterSimulationMode();
	R.Run(0.5);
	const auto& Sites = R.Sim->GetLiveLeakSites();
	const int32 Header = R.SiteOf(TEXT("랙 1 공급 헤더"));
	const FVector Near = Header != INDEX_NONE ? Sites[Header].PositionM + FVector(0.05, -0.03, 0.02) : FVector::ZeroVector;
	const int32 Found = R.Sim->FindLiveLeakSite(Near);
	const int32 Far = R.Sim->FindLiveLeakSite(Near + FVector(50, 0, 0));
	const TArray<double> Before = R.Sim->GetSnapshot().RackCoolantMassFlowKgs;
	const bool bOpened = R.Sim->SetLiveLeak(Found, 10.0e-3, TEXT("test"));
	R.Run(1.0);
	const TArray<double> After = R.Sim->GetSnapshot().RackCoolantMassFlowKgs;
	const FVrgDashboardLeaks D = R.Sim->BuildDashboardLeaks();
	const double Ledger = R.CoolantLedgerError();
	const double Global = FMath::Abs(R.Sim->GetSnapshot().NormalizedEnergyResidual);
	const double Cross = R.WorstCoupledBranchRel();
	AddInfo(FString::Printf(TEXT("지점 %d개, 헤더 지점 %d, 가까운 점 -> %d, 50 m 밖 -> %d | 10 mm 헤더 누수 %.4f L/s, 랙 냉각수 %.4f/%.4f -> %.4f/%.4f kg/s, 누수 열 %.3f kW | 냉각수 장부 %.3e, 전역 %.3e, 결합/분기 %.3e"),
		Sites.Num(), Header, Found, Far, D.Leaks.Num() ? D.Leaks[0].FlowM3s * 1e3 : -1.0, Before[0], Before[1], After[0], After[1], D.LeakHeatW * 1e-3, Ledger, Global, Cross));
	TestTrue(TEXT("the nearest site to a point is found, nothing beyond 2 m"), Header != INDEX_NONE && Found == Header && Far == INDEX_NONE);
	TestTrue(TEXT("a header leak (no split) flows and lowers the racks' coolant (the solve decides how much, D4)"),
		bOpened && D.Leaks.Num() == 1 && D.Leaks[0].FlowM3s > 0.0 && After[0] < Before[0] && After[1] < Before[1]);
	// Leak heat = m cp (T_leak - T_tank reference): supply water leaks colder than the reference, so it is negative here.
	TestTrue(TEXT("the header leak heat is booked and closes the coolant and global ledgers (< 1e-9)"), Ledger < 1e-9 && Global < 1e-9 && D.LeakHeatW != 0.0);
	TestTrue(TEXT("coupled flow = branch flow x density (exact)"), Cross == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveLeakEventPlantTest, "VRG.Hydraulics.LiveLeak.EventAndPlantSite", VrgLiveLeakFlags)
bool FLiveLeakEventPlantTest::RunTest(const FString& Parameters)
{
	FLiveLeakRoom R(true, true);
	R.Sim->RebuildLiveLeakSites();
	const FVector Point = R.Sim->GetLiveLeakSites()[R.SiteOf(TEXT("랙 1 회수 분기"))].PositionM;
	ADataCenterSimulationActor::FEvalEvent E;
	E.TimeS = 0.5; E.Kind = TEXT("leakat"); E.Value = 3.0e-3;
	E.Args = { FString::SanitizeFloat(Point.X), FString::SanitizeFloat(Point.Y), FString::SanitizeFloat(Point.Z) };
	R.Sim->EvalEvents.Add(E);
	R.Sim->EnterSimulationMode();
	R.Run(1.0);
	const TArray<FVrgLeakRecord>& Records = R.Sim->GetFaultMonitor().GetLeaks();
	const bool bEvent = Records.Num() == 1 && Records[0].Site == TEXT("랙 1 회수 분기") && Records[0].TimeS >= 0.5 - 1e-9 && Records[0].TimeS < 0.5 + 0.02;

	int32 PlantSites = 0, FwsHeader = INDEX_NONE;
	for (int32 I = 0; I < R.Sim->GetLiveLeakSites().Num(); ++I)
	{
		PlantSites += R.Sim->GetLiveLeakSites()[I].bPlant ? 1 : 0;
		if (R.Sim->GetLiveLeakSites()[I].Label == TEXT("설비 FWS 공급 헤더")) FwsHeader = I;
	}
	const int32 FaultsBefore = R.Sim->GetFacilityPlant().GetFaultLog().Num();
	const bool bPlantOpen = R.Sim->SetLiveLeak(FwsHeader, 20.0e-3, TEXT("test"));
	R.Run(0.5);
	const FFacilityWaterLoop& Fws = R.Sim->GetFacilityPlant().GetLoop();
	const int32 K = Fws.FindLeak(EPlantLeakSite::SupplyHeader, INDEX_NONE);
	const double PlantLeak = K != INDEX_NONE ? Fws.GetLeakFlowM3s(K) : -1.0;
	const bool bPlantRepair = R.Sim->SetLiveLeak(FwsHeader, 0.0, TEXT("test"));
	R.Run(0.2);
	const double PlantLeakAfter = K != INDEX_NONE ? Fws.GetLeakFlowM3s(K) : -1.0;
	const int32 FaultsAfter = R.Sim->GetFacilityPlant().GetFaultLog().Num();
	const double SiteResidual = FMath::Abs(R.Sim->GetSiteEnergyResidual());
	AddInfo(FString::Printf(TEXT("이벤트 leakat t=0.5 s -> 기록 %d건 t=%.4f s (%s) | 설비 지점 %d개, FWS 공급 헤더 %d: 20 mm 누수 %.4f L/s, 수리 뒤 %.3e L/s, 설비 장애 기록 %d -> %d, 외기까지 장부 %.3e"),
		Records.Num(), Records.Num() ? Records[0].TimeS : -1.0, Records.Num() ? *Records[0].Site : TEXT("-"), PlantSites, FwsHeader,
		PlantLeak * 1e3, PlantLeakAfter * 1e3, FaultsBefore, FaultsAfter, SiteResidual));
	TestTrue(TEXT("leakat opens the nearest site at its time (within one step)"), bEvent);
	TestTrue(TEXT("plant sites exist while the plant is built"), PlantSites > 0 && FwsHeader != INDEX_NONE);
	TestTrue(TEXT("a plant site opens and repairs through the plant's fault path (re-solved, D4)"),
		bPlantOpen && bPlantRepair && PlantLeak > 0.0 && PlantLeakAfter == 0.0 && FaultsAfter == FaultsBefore + 2);
	TestTrue(TEXT("the site ledger closes (< 1e-9)"), SiteResidual < 1e-9);
	return true;
}

#endif
