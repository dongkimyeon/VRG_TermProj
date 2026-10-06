#include "Misc/AutomationTest.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/RackThermalActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoomAirDomainCouplingTest, "VRG.Interaction.Layout.RoomAirDomainCoupling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FRoomAirDomainCouplingTest::RunTest(const FString& Parameters)
{
	double MaxIt = 0, MaxSizing = 0, MaxDuct = 0, MaxWater = 0, MaxEnergy = 0, MaxPressure = 0, MaxHydraulic = 0;
	double MinInlet = TNumericLimits<double>::Max(), MaxInlet = 0, MinCoil = TNumericLimits<double>::Max(), MaxCoil = 0;
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		auto* Sim = World->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary = false;
		Sim->CrahLayout = Mode == 0 ? ECrahLayout::InRow : ECrahLayout::Perimeter;
		auto* Room = World->SpawnActor<ADataCenterRoomActor>(); auto* Air = World->SpawnActor<AAirFieldDebugActor>();
		auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>(FVector(-400, -200, 0), FRotator::ZeroRotator);
		ARackThermalActor* Racks[2];
		for (int32 I = 0; I < 2; ++I)
		{
			Racks[I] = World->SpawnActor<ARackThermalActor>(FVector(I == 0 ? -150 : 150, 0, 0), FRotator(0, I == 0 ? 0 : 180, 0));
			Racks[I]->ApplyGb200Nvl72Preset(); Racks[I]->bShowReadout = false;
		}
		Room->RebuildRoomNow(); Sim->EnterLayoutMode();
		const auto InitialInput = Racks[0]->Input; double BaselineIt = 0;
		const int32 ExpectedUnits = Mode == 0 ? 4 : 2;
		auto Run = [&](bool bManual, bool bPlaced, const TCHAR* Stage)
		{
			for (auto* Rack : Racks) Rack->Input = InitialInput;
			Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First = Sim->GetSnapshot();
			if (BaselineIt == 0) BaselineIt = First.TotalItHeatW;
			for (int32 I = 0; I < 25; ++I) Sim->Tick(.04f);
			const auto S = Sim->GetSnapshot(); const auto AS = Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
			const auto& Grid = Air->GetMutableSolver().GetGrid(); const auto& GP = Grid.GetParams();
			TestTrue(TEXT("Manual domain stays applied throughout shared integration"), !bManual || (Air->GetActorLocation() == FVector(-550, -450, 0)
				&& GP.NumCellsX == 43 && GP.NumCellsY == 37 && GP.NumCellsZ == 13));
			const double It = FMath::Abs(First.TotalItHeatW - BaselineIt) / BaselineIt; MaxIt = FMath::Max(MaxIt, It);
			TestTrue(TEXT("Domain edit preserves IT input and resets only at simulation entry"), First.SimTimeS == Sim->FixedStepS && It < 1e-9);
			const double DesignFlow = Sim->GetDesignAirLoadW() / (GP.DensityKgM3 * GP.SpecificHeatJKgK * Sim->CrahDesignAirDeltaTK);
			const double Eps = Sim->CrahDesignAirDeltaTK / (Sim->CrahDesignAirDeltaTK + Sim->CrahDesignApproachK);
			const double DesignUA = -FMath::Loge(1 - Eps) * GP.DensityKgM3 * GP.SpecificHeatJKgK * DesignFlow;
			const double FanW = Sim->CrahFanPressurePa * DesignFlow / Sim->CrahFanEfficiency;
			const double Sizing = FMath::Max3(FMath::Abs(S.CrahFlowM3s - DesignFlow) / DesignFlow,
				FMath::Abs(Sim->GetCrahTotalUAWK() - DesignUA) / DesignUA, FMath::Abs(S.CrahFanPowerW - FanW) / FanW);
			MaxSizing = FMath::Max(MaxSizing, Sizing);
			TestTrue(TEXT("All planned CRAHs and shared flow/UA/fan policy retained"), Sim->IsUsingPlacedCrahs() == bPlaced
				&& Sim->GetNumCrahUnits() == ExpectedUnits && Sim->GetActiveCrahUnitCount() == ExpectedUnits && Sizing < 1e-12);
			double Flow = 0, Duct = 0;
			TestEqual(TEXT("Both racks coupled to air"), S.RackAirMassFlowKgs.Num(), 2);
			for (int32 I = 0; I < S.RackAirMassFlowKgs.Num(); ++I)
			{
				TestTrue(TEXT("Positive rack mass flow and actual duct coupling"), S.RackAirMassFlowKgs[I] > 0 && S.RackDuctMassFlowKgs.IsValidIndex(I));
				Flow += S.RackAirMassFlowKgs[I] / GP.DensityKgM3;
				if (S.RackDuctMassFlowKgs.IsValidIndex(I)) Duct = FMath::Max(Duct,
					FMath::Abs(S.RackAirMassFlowKgs[I] - S.RackDuctMassFlowKgs[I]) / S.RackAirMassFlowKgs[I]);
			}
			Duct = FMath::Max(Duct, FMath::Abs(AS.DuctFlowM3s - Flow - DesignFlow) / (Flow + DesignFlow)); MaxDuct = FMath::Max(MaxDuct, Duct);
			TestTrue(TEXT("No domain-clipped or rejected ducts"), !S.bAirDuctsRejected && !AS.bDuctsRejected && AS.NumDuctFaces > 0 && Duct < 1e-9);
			for (auto* Rack : Racks)
			{
				const FVector CentreM = (Rack->GetActorLocation() - Air->GetActorLocation()) / 100 + FVector(0, 0, Rack->RackSizeM.Z * .5);
				TestTrue(TEXT("Rack remains an air solid in manual grid"), Grid.IsSolidCell(FMath::FloorToInt32(CentreM.X / GP.CellSizeM),
					FMath::FloorToInt32(CentreM.Y / GP.CellSizeM), FMath::FloorToInt32(CentreM.Z / GP.CellSizeM)));
				// Existing transient inlet safety net from CRAH tests / CLAUDE.md, not new hardware limits.
				for (const double K : Rack->Input.SlotAirInletTempK)
				{ MinInlet = FMath::Min(MinInlet, K); MaxInlet = FMath::Max(MaxInlet, K); TestTrue(TEXT("Inlet 15..35C plausibility"), FMath::IsFinite(K) && K > 288.15 && K < 308.15); }
			}
			TestTrue(TEXT("Coil heat removed once through CRAH ducts"), S.CrahCoilRemovedW > 0
				&& FMath::Abs(S.TotalAirCoolingRemovedW - S.CrahCoilRemovedW) / S.CrahCoilRemovedW < 1e-9);
			double Water = 0, RemovedW = 0; const auto Routed = Room->GetRoutedRacks(); const auto Rows = Room->GetRoutedRackRows();
			TArray<double> Capacity; Capacity.Init(0, S.CduDutyW.Num());
			for (int32 I = 0; I < Routed.Num(); ++I)
			{
				TestTrue(TEXT("Positive coolant flow"), Routed[I]->Input.CoolantMassFlowKgs > 0);
				RemovedW += Routed[I]->GetMutableModel().GetLastHeatRemovedByCoolantW();
				if (TestTrue(TEXT("Row/CDU group retained"), Rows.IsValidIndex(I) && Capacity.IsValidIndex(Rows[I])))
					Capacity[Rows[I]] += Routed[I]->Input.CoolantMassFlowKgs * Routed[I]->Params.CoolantCpJKgK;
			}
			Water = FMath::Abs(RemovedW - S.TotalRadiatorRejectedW) / S.TotalRadiatorRejectedW;
			for (int32 I = 0; I < S.CduDutyW.Num(); ++I)
			{
				Water = FMath::Max(Water, FMath::Abs(Capacity[I] * (S.CduReturnTempK[I] - S.CduSupplyTempK[I]) - S.CduDutyW[I]) / S.CduDutyW[I]);
				TestTrue(TEXT("Coolant duty/temperature plausibility"), S.CduDutyW[I] > 0 && S.CduReturnTempK[I] > S.CduSupplyTempK[I]
					&& S.CduSupplyTempK[I] >= Cdu->FacilitySupplyTempK);
			}
			MaxWater = FMath::Max(MaxWater, Water); const auto& HS = Room->GetLastSolveResult();
			TestTrue(TEXT("Coolant conservation and pump scale safety net"), Water < 1e-12 && Room->GetTotalPumpElectricalW() > 0
				&& Room->GetTotalPumpElectricalW() / S.TotalItHeatW < .05);
			TestTrue(TEXT("Hydraulic and pressure convergence retain existing criteria"), HS.bConverged && S.bHydraulicsConverged
				&& HS.NormalizedResidual < Room->SolveSettings.NormalizedResidualTarget && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence < 1e-5);
			TestTrue(TEXT("Existing global heat balance"), FMath::Abs(S.NormalizedEnergyResidual) < 1e-9);
			MaxEnergy = FMath::Max(MaxEnergy, FMath::Abs(S.NormalizedEnergyResidual)); MaxPressure = FMath::Max(MaxPressure, AS.NormalizedDivergence);
			MaxHydraulic = FMath::Max(MaxHydraulic, HS.NormalizedResidual); MinCoil = FMath::Min(MinCoil, S.CrahCoilRemovedW); MaxCoil = FMath::Max(MaxCoil, S.CrahCoilRemovedW);
			AddInfo(FString::Printf(TEXT("Room %s mode%d cells%d ITdelta%.3e sizing%.3e duct%.3e water%.3e energy%.3e pressure%.3e hydraulic%.3e coil%.6fW t%.2fs"),
				Stage, Mode, Grid.GetNumCells(), It, Sizing, Duct, Water, S.NormalizedEnergyResidual, AS.NormalizedDivergence, HS.NormalizedResidual, S.CrahCoilRemovedW, S.SimTimeS));
			Sim->EnterLayoutMode();
		};
		Run(false, false, TEXT("legacy automatic"));
		FRoomAirDomainSettings Manual; Manual.bManual = true; Manual.MinXYM = FVector2D(-5.5, -4.5);
		Manual.MaxXYM = FVector2D(5.1, 4.6); Manual.CeilingHeightM = 3.1; // Synthetic fixture geometry only.
		FString Reason; TestTrue(TEXT("Manual domain accepted"), Sim->TrySetRoomAirDomain(Manual, Reason)); Run(true, false, TEXT("manual automatic CRAHs"));
		TArray<FCrahLayoutRecord> Automatic; Sim->CaptureAutomaticCrahLayout(Automatic);
		TestEqual(TEXT("Existing automatic planner conversion source unchanged"), Automatic.Num(), ExpectedUnits);
		for (const auto& Record : Automatic)
		{
			auto* Unit = World->SpawnActor<ACrahUnitActor>(Record.Transform.GetLocation(), Record.Transform.Rotator());
			Unit->ApplyLayoutSettings(Record.Settings);
		}
		Run(true, true, TEXT("manual placed CRAHs"));
		World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
	}
	AddInfo(FString::Printf(TEXT("Room max IT%.3e sizing%.3e duct%.3e water%.3e global%.3e pressure%.3e hydraulic%.3e; inlet%.9f..%.9fK coil%.6f..%.6fW"),
		MaxIt, MaxSizing, MaxDuct, MaxWater, MaxEnergy, MaxPressure, MaxHydraulic, MinInlet, MaxInlet, MinCoil, MaxCoil));
	return true;
}
#endif
