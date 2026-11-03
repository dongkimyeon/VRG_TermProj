// Fill out your copyright notice in the Description page of Project Settings.

// 2026-10-22 (user request): instruments of the integrated actor. Pipe points every vrg.Sensors.PipeIntervalM along
// every drawn pipe of the room (TCS) and the plant yard (FWS / CHW / CW) read the pipe's flow and water temperature and
// carry a leak-detection cable section; every plant unit and CDU reads its key values (fan, water in / out, flow,
// head, power, COP). All are simulated measurements through FInstrumentBank (D9); the true values come from the same
// solved networks and plant state the coupling uses (no separate model), leaks from the solved leak flows (D4).

#include "Actors/DataCenterSimulationActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/PlantEquipmentActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"
#include "Visualization/PlantDisplay.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"

namespace
{
	// Spacing of the pipe instrument points [m]: a layout choice (spot leak detectors and meters along the runs),
	// ESTIMATE, not a design code value.
	TAutoConsoleVariable<float> CVarPipeIntervalM(TEXT("vrg.Sensors.PipeIntervalM"), 3.f,
		TEXT("Spacing [m] of the pipe instrument points (flow, water temperature, leak-detection cable). Applied at the next simulation entry."));

	// W5 (2026-11-03): operator alarm reset. Water still on/in the detector raises the alarm again after its debounce.
	FAutoConsoleCommandWithWorldAndArgs GVrgSensorsResetLeak(TEXT("vrg.Sensors.ResetLeak"),
		TEXT("누수 감지 경보 해제(물이 남아 있으면 다시 울림). vrg.Sensors.ResetLeak <계측 지점 번호|all>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World) return;
			const int32 Index = Args.Num() == 0 || Args[0].Equals(TEXT("all"), ESearchCase::IgnoreCase) ? INDEX_NONE : FCString::Atoi(*Args[0]);
			for (TActorIterator<ADataCenterSimulationActor> It(World); It; ++It)
				UE_LOG(LogTemp, Display, TEXT("[Sensor] 누수 경보 해제 %d건(%s)"), It->ResetLeakAlarms(Index), Index == INDEX_NONE ? TEXT("all") : *Args[0]);
		}));

	// Point placement along a polyline: N = round(length / interval) >= 1 points at the centres of N equal sections,
	// each covering its section (the cover radius reaches both section ends, plus a 0.3 m allowance for the drawn
	// fittings - a display abstraction, not a physical range).
	void PlaceAlong(const TArray<FVector>& Pts, double IntervalM, TArray<FVector>& OutPositions, double& OutCoverM, TArray<FVector2D>* OutRanges = nullptr)
	{
		OutPositions.Reset();
		if (OutRanges) OutRanges->Reset();
		double Length = 0.0;
		for (int32 K = 0; K + 1 < Pts.Num(); ++K) Length += FVector::Distance(Pts[K], Pts[K + 1]);
		if (Pts.Num() < 2 || Length <= UE_DOUBLE_SMALL_NUMBER) { if (Pts.Num() > 0) { OutPositions.Add(Pts[0]); if (OutRanges) OutRanges->Add(FVector2D(0.0, 0.0)); } OutCoverM = 0.3; return; }
		const int32 N = FMath::Max(1, FMath::RoundToInt32(Length / FMath::Max(IntervalM, 0.1)));
		const double Section = Length / N;
		OutCoverM = 0.5 * Section + 0.3;
		for (int32 I = 0; I < N; ++I)
		{
			if (OutRanges) OutRanges->Add(FVector2D(I * Section, (I + 1) * Section));
			double Target = (I + 0.5) * Section, Walked = 0.0;
			for (int32 K = 0; K + 1 < Pts.Num(); ++K)
			{
				const double Seg = FVector::Distance(Pts[K], Pts[K + 1]);
				if (Walked + Seg >= Target || K + 2 == Pts.Num())
				{
					OutPositions.Add(Pts[K] + (Pts[K + 1] - Pts[K]) * (Seg > 0.0 ? FMath::Clamp((Target - Walked) / Seg, 0.0, 1.0) : 0.0));
					break;
				}
				Walked += Seg;
			}
		}
	}

	// Points every StepM along the polyline between arc lengths [Range.X, Range.Y] (W5: where a rope under a pipe lies).
	TArray<FVector> SampleAlong(const TArray<FVector>& Pts, const FVector2D& Range, double StepM)
	{
		TArray<FVector> Out;
		double Walked = 0.0;
		for (int32 K = 0; K + 1 < Pts.Num(); ++K)
		{
			const double Seg = FVector::Distance(Pts[K], Pts[K + 1]);
			for (double D = FMath::Max(Range.X, Walked); D <= FMath::Min(Range.Y, Walked + Seg) + 1e-9; D += StepM)
				Out.Add(Pts[K] + (Pts[K + 1] - Pts[K]) * (Seg > 0.0 ? FMath::Clamp((D - Walked) / Seg, 0.0, 1.0) : 0.0));
			Walked += Seg;
		}
		if (Out.IsEmpty() && Pts.Num() > 0) Out.Add(Pts[0]);
		return Out;
	}

	// W5 contact specs (2026-11-03). ESTIMATES for generic detectors (no product rating): a floor rope / spot sensor is
	// wet with 5 mL on the cells it covers now; a tray cold-plate or inner-manifold detector with 2 mL; a drip-tray spot
	// sensor with 30 mL (the W3.5 drip tray value). Poll 1 s, debounce 1 s (the existing generic detector assumptions).
	FWaterContactSpec ContactSpec(double WettingM3, bool bCurrent)
	{
		FWaterContactSpec S; S.WettingVolumeM3 = WettingM3; S.bCurrentWater = bCurrent; return S;
	}

	// W5-2: a make-up water meter is a positive-displacement / pulse meter: it reads zero without flow (no zero noise,
	// only its resolution and low-flow cutoff). ESTIMATE of a generic meter.
	FInstrumentChannelSpec MakeupMeter(double ScaleM3s)
	{
		FInstrumentChannelSpec C = InstrumentSpecs::Make(TEXT("makeup_flow"), EInstrumentQuantity::FlowM3s, ScaleM3s);
		C.Sensor.NoiseStdK = 0.0;
		return C;
	}

	const TCHAR* LoopKorean(EPlantLoopId L) { return L == EPlantLoopId::Fws ? TEXT("FWS") : L == EPlantLoopId::Chw ? TEXT("냉수") : TEXT("응축수"); }
	const TCHAR* RoleKorean(EPlantPipeRole R)
	{
		switch (R)
		{
		case EPlantPipeRole::Pump: return TEXT("펌프");
		case EPlantPipeRole::CheckValve: return TEXT("체크밸브");
		case EPlantPipeRole::SupplyMain: return TEXT("공급 메인");
		case EPlantPipeRole::SupplyRun: return TEXT("공급 분기");
		case EPlantPipeRole::Valve: return TEXT("제어 밸브");
		case EPlantPipeRole::Exchanger: return TEXT("열교환기");
		case EPlantPipeRole::ReturnRun: return TEXT("회수 분기");
		case EPlantPipeRole::ReturnMain: return TEXT("회수 메인");
		case EPlantPipeRole::ReturnCoil: return TEXT("코일");
		case EPlantPipeRole::Bypass: return TEXT("바이패스");
		case EPlantPipeRole::Economizer: return TEXT("이코노마이저");
		default: return TEXT("배관");
		}
	}

	// Channel names of the unit points (the value each reads from FPlantUnitTruth).
	const FName NIn(TEXT("in")), NOut(TEXT("out")), NFlow(TEXT("flow")), NCondIn(TEXT("cond_in")), NCondOut(TEXT("cond_out")),
		NCondFlow(TEXT("cond_flow")), NFan(TEXT("fan")), NPower(TEXT("power")), NHead(TEXT("head")), NHeat(TEXT("heat")), NCop(TEXT("cop")),
		NSpeed(TEXT("speed")), NTemp(TEXT("temp"));

	double TruthOf(const FPlantUnitTruth& T, FName Name)
	{
		if (Name == NIn) return T.WaterInK;
		if (Name == NOut) return T.WaterOutK;
		if (Name == NFlow) return T.FlowM3s;
		if (Name == NCondIn) return T.CondInK;
		if (Name == NCondOut) return T.CondOutK;
		if (Name == NCondFlow) return T.CondFlowM3s;
		if (Name == NFan) return T.FanRatio;
		if (Name == NPower) return T.PowerW;
		if (Name == NHead) return T.HeadPa;
		if (Name == NHeat) return T.HeatW;
		if (Name == NCop) return T.Cop;
		if (Name == NSpeed) return T.SpeedRatio;
		return NAN;
	}

	// Plant unit kind of the point (FInstrumentPoint::Key = index, Loop = loop, the kind in the label's owner field).
	enum class EUnitSource : uint8 { DryCooler, Chiller, TrimChiller, Pump, Economizer };
}

void ADataCenterSimulationActor::RebuildInstruments()
{
	Instruments.Reset();
	InstrumentFloorCells.Reset(); InstrumentGroundBins.Reset(); InstrumentLeakIndices.Reset();
	ProcessLoopPoints.Reset();
	TArray<FString> ProcessNames; TArray<double> ProcessDesign;
	const double IntervalM = FMath::Max(double(CVarPipeIntervalM.GetValueOnGameThread()), 0.1);
	using namespace InstrumentSpecs;
	// W5: floor cells under a set of points (room floor grid, open cells only), ground bins under them (yard).
	const double CellM = FloorWater.GetCellM();
	auto FloorCellsUnder = [&](const TArray<FVector>& Samples)
	{
		TArray<int32> Cells;
		for (const FVector& P : Samples)
		{
			const int32 I = FMath::FloorToInt32((P.X - FloorWater.GetOriginM().X) / CellM), J = FMath::FloorToInt32((P.Y - FloorWater.GetOriginM().Y) / CellM);
			if (I < 0 || J < 0 || I >= FloorWater.GetNx() || J >= FloorWater.GetNy() || FloorWater.GetSolid()[FloorWater.Index(I, J)]) continue;
			Cells.AddUnique(FloorWater.Index(I, J));
		}
		return Cells;
	};
	auto GroundBinsUnder = [&](const TArray<FVector>& Samples)
	{
		TArray<FIntPoint> Bins;
		const double Bin = Spray.GetSettings().FloorBinM;
		for (const FVector& P : Samples) Bins.AddUnique(FIntPoint(FMath::FloorToInt32(P.X / Bin), FMath::FloorToInt32(P.Y / Bin)));
		return Bins;
	};
	auto AddPoint = [&](const FInstrumentPoint& Pt, const TArray<double>& Initial, TArray<int32> Cells = {}, TArray<FIntPoint> Bins = {}, TArray<int32> Leaks = {})
	{
		Instruments.AddPoint(Pt, Initial);
		InstrumentFloorCells.Add(MoveTemp(Cells)); InstrumentGroundBins.Add(MoveTemp(Bins)); InstrumentLeakIndices.Add(MoveTemp(Leaks));
	};
	auto AddPipePoints = [&](EInstrumentSite Site, const TArray<FVector>& PointsM, const FString& Label, int32 Key, int32 Loop, double FlowScale, double FlowNow, double TempNow)
	{
		TArray<FVector> Positions; TArray<FVector2D> Ranges; double Cover = 0.0;
		PlaceAlong(PointsM, IntervalM, Positions, Cover, &Ranges);
		for (int32 K = 0; K < Positions.Num(); ++K)
		{
			FInstrumentPoint Pt;
			Pt.Site = Site; Pt.Label = Label; Pt.PositionM = Positions[K]; Pt.Key = Key; Pt.Loop = Loop;
			Pt.Channels = { Make(NFlow, EInstrumentQuantity::FlowM3s, FlowScale), Make(NTemp, EInstrumentQuantity::TemperatureK, 300.0) };
			Pt.bLeakCable = true; Pt.CoverRadiusM = Cover;
			// W5 (2026-11-03): the cable is a rope laid on the floor (room) or the ground (yard) under its pipe section and
			// reads the computed water there - the water shown is the water sensed. Room: water on its floor cells now
			// (W3); yard: the spray's cumulative ground deposits under it (no outdoor spreading model).
			Pt.bWaterContact = true;
			Pt.Contact = ContactSpec(5.0e-6, Site == EInstrumentSite::RoomPipe);
			const TArray<FVector> Samples = SampleAlong(PointsM, Ranges[K], 0.5 * (Site == EInstrumentSite::RoomPipe ? CellM : Spray.GetSettings().FloorBinM));
			Pt.CoverageStartM = Samples[0]; Pt.CoverageEndM = Samples.Last();
			if (Site == EInstrumentSite::RoomPipe) AddPoint(Pt, { FlowNow, TempNow }, FloorCellsUnder(Samples));
			else AddPoint(Pt, { FlowNow, TempNow }, {}, GroundBinsUnder(Samples));
		}
	};

	// Plant yard: one snapshot of the drawn plant gives the pipe routes, the unit boxes and the node positions.
	if (bPlantBuilt && Plant.IsBuilt())
	{
		const FPlantDisplaySnapshot S = PlantDisplay::Build(Plant, MakePlantDisplayInput());
		for (const FPlantDisplayPipe& P : S.Pipes)
		{
			// Key = edge (pump index for pump pipes); Loop packs the loop and, above it, the volume and the pump flag.
			const int32 Packed = int32(P.Loop) | (int32(P.Volume) << 2) | (P.Role == EPlantPipeRole::Pump ? 16 : 0);
			const FFacilityWaterLoop* WL = Plant.GetLoopFor(P.Loop);
			const double Scale = WL ? WL->GetDesignTotalFlowM3s() / FMath::Max(WL->GetNumBranches(), 1) : 1e-3;
			AddPipePoints(EInstrumentSite::PlantPipe, P.PointsM, FString::Printf(TEXT("%s %s"), LoopKorean(P.Loop), RoleKorean(P.Role)),
				P.Edge, Packed, FMath::Max(FMath::Abs(P.FlowM3s), Scale), P.FlowM3s, P.TempK);
		}
		int32 UnitIndex[NumPlantUnitKinds] = {};
		for (const FPlantDisplayEquipment& E : S.Equipment)
		{
			EUnitSource Source; FString Name; TArray<FInstrumentChannelSpec> Ch;
			FPlantUnitTruth T;
			switch (E.Kind)
			{
			case EPlantEquipmentKind::DryCooler:
				Source = EUnitSource::DryCooler; T = Plant.GetDryCoolerTruth(E.Loop, E.Index);
				Name = FString::Printf(TEXT("%s 드라이쿨러 %d"), LoopKorean(E.Loop), E.Index + 1);
				Ch = { Make(NFan, EInstrumentQuantity::Ratio, 1.0), Make(NIn, EInstrumentQuantity::TemperatureK, 300.0), Make(NOut, EInstrumentQuantity::TemperatureK, 300.0),
					Make(NFlow, EInstrumentQuantity::FlowM3s, FMath::Max(FMath::Abs(T.FlowM3s), 1e-4)), Make(NPower, EInstrumentQuantity::PowerW, Plant.GetDryCoolerParams().RatedFanElectricW) };
				break;
			case EPlantEquipmentKind::Chiller: case EPlantEquipmentKind::TrimChiller:
			{
				const bool bTrimUnit = E.Kind == EPlantEquipmentKind::TrimChiller;
				Source = bTrimUnit ? EUnitSource::TrimChiller : EUnitSource::Chiller; T = Plant.GetChillerTruth(E.Index, bTrimUnit);
				Name = FString::Printf(TEXT("%s %d"), bTrimUnit ? TEXT("트림 칠러") : TEXT("냉수 칠러"), E.Index + 1);
				const double Cap = bTrimUnit ? Plant.GetTrimRefCapacityW() : Plant.GetChillerRefCapacityW();
				Ch = { Make(NIn, EInstrumentQuantity::TemperatureK, 300.0), Make(NOut, EInstrumentQuantity::TemperatureK, 300.0),
					Make(NCondIn, EInstrumentQuantity::TemperatureK, 300.0), Make(NCondOut, EInstrumentQuantity::TemperatureK, 300.0),
					Make(NCop, EInstrumentQuantity::Cop, 5.0), Make(NPower, EInstrumentQuantity::PowerW, FMath::Max(Cap / 5.0, 1.0)) };
				break;
			}
			case EPlantEquipmentKind::Pump:
			{
				Source = EUnitSource::Pump; T = Plant.GetPumpTruth(E.Loop, E.Index);
				Name = FString::Printf(TEXT("%s 펌프 %d"), LoopKorean(E.Loop), E.Index + 1);
				const FFacilityWaterLoop* WL = Plant.GetLoopFor(E.Loop);
				Ch = { Make(NSpeed, EInstrumentQuantity::Ratio, 1.0), Make(NFlow, EInstrumentQuantity::FlowM3s, WL ? WL->GetDesignTotalFlowM3s() / FMath::Max(WL->GetSpec().DutyPumps, 1) : 1e-3),
					Make(NHead, EInstrumentQuantity::PressurePa, WL ? WL->GetDesignPumpHeadPa() : 1e5), Make(NPower, EInstrumentQuantity::PowerW, 1e4) };
				break;
			}
			default:
				Source = EUnitSource::Economizer; T = Plant.GetEconomizerTruth();
				Name = TEXT("이코노마이저");
				Ch = { Make(NIn, EInstrumentQuantity::TemperatureK, 300.0), Make(NOut, EInstrumentQuantity::TemperatureK, 300.0),
					Make(NCondIn, EInstrumentQuantity::TemperatureK, 300.0), Make(NCondOut, EInstrumentQuantity::TemperatureK, 300.0),
					Make(NHeat, EInstrumentQuantity::PowerW, FMath::Max(Plant.GetDesignChwLoadW(), 1.0)) };
				break;
			}
			FInstrumentPoint Pt;
			Pt.Site = EInstrumentSite::PlantUnit; Pt.Label = Name; Pt.Key = E.Index; Pt.Loop = int32(E.Loop) | (int32(Source) << 2);
			Pt.PositionM = E.CenterM + FVector(0, 0, 0.5 * E.SizeM.Z + 0.35);
			Pt.Channels = Ch;
			TArray<double> Initial;
			for (const FInstrumentChannelSpec& C : Ch) Initial.Add(TruthOf(T, C.Name));
			AddPoint(Pt, Initial);
		}
		// W5-2: the make-up water meter of every facility loop (the plant's make-up keeps a leaking loop full, so its flow
		// is the plant model's make-up flow, read through the meter).
		for (const EPlantLoopId L : { EPlantLoopId::Fws, EPlantLoopId::Chw, EPlantLoopId::Cw })
		{
			if (!Plant.GetLoopFor(L)) continue;
			FInstrumentPoint Pt;
			Pt.Site = EInstrumentSite::PlantMakeup; Pt.Loop = int32(L);
			Pt.Label = FString::Printf(TEXT("%s 보충수"), LoopKorean(L));
			Pt.Channels = { MakeupMeter(1.0e-3) };
			ProcessLoopPoints.Add(Instruments.Num()); ProcessNames.Add(Pt.Label); ProcessDesign.Add(0.0);
			AddPoint(Pt, { 0.0 });
		}
	}

	// Room: every routed TCS pipe (Edge < 0 = a row pump's run) and the CDUs.
	if (Room != nullptr)
	{
		const FHydraulicNetwork& N = Room->GetNetwork();
		const TArray<FRoomPipeRoute>& Routes = Room->GetPipeRoutesM();
		auto Distance = [](const FRoomPipeRoute& A, const FRoomPipeRoute& B)
		{
			double D = TNumericLimits<double>::Max();
			for (const FVector& P : A.PointsM) for (const FVector& Q : B.PointsM) D = FMath::Min(D, FVector::Distance(P, Q));
			return D;
		};
		for (const FRoomPipeRoute& R : Routes)
		{
			const double Flow = R.Edge >= 0 ? N.GetPipeFlowM3s(R.Edge) : N.GetPumpFlowM3s(-1 - R.Edge);
			// The room model has a water temperature on the rack branches only. A supply header carries the same water
			// (no pipe heat loss is modelled), so it reads its nearest supply branch; a return header is a mix the model
			// does not resolve, so it reads no temperature (flow only). Loop = the edge whose temperature it reads.
			int32 TempEdge = INDEX_NONE;
			if (R.bBranch || !R.bSupply) TempEdge = R.bBranch ? R.Edge : INDEX_NONE;
			else
			{
				double Best = TNumericLimits<double>::Max();
				for (const FRoomPipeRoute& B : Routes)
					if (B.bBranch && B.bSupply && B.Edge >= 0) { const double D = Distance(R, B); if (D < Best) { Best = D; TempEdge = B.Edge; } }
			}
			double TempK = NAN;
			if (TempEdge < 0 || !Room->TryGetPipeTemperatureK(TempEdge, TempK)) TempK = NAN;
			AddPipePoints(EInstrumentSite::RoomPipe, R.PointsM, R.bSupply ? TEXT("냉각수 공급") : TEXT("냉각수 회수"), R.Edge, TempEdge,
				FMath::Max(FMath::Abs(Flow), 1e-4), Flow, TempK);
		}
		for (int32 G = 0; G < CduGroupRows.Num(); ++G)
		{
			const ACoolingDistributionUnitActor* Cdu = Room->GetRowCdu(CduGroupRows[G]);
			if (Cdu == nullptr) continue;
			FInstrumentPoint Pt;
			Pt.Site = EInstrumentSite::Cdu; Pt.Label = FString::Printf(TEXT("CDU %d"), G + 1); Pt.Key = G;
			Pt.PositionM = SimUnits::UnrealToMeters(Cdu->GetActorLocation()) + FVector(0, 0, 2.3);
			const double FacilityM3s = CduSummaries.IsValidIndex(G) ? CduSummaries[G].FacilityMassFlowKgs / FacilityWater::DensityKgM3 : 1e-3;
			Pt.Channels = { Make(TEXT("fws_in"), EInstrumentQuantity::TemperatureK, 300.0), Make(TEXT("fws_out"), EInstrumentQuantity::TemperatureK, 300.0),
				Make(TEXT("fws_flow"), EInstrumentQuantity::FlowM3s, FMath::Max(FacilityM3s, 1e-4)),
				Make(TEXT("tcs_supply"), EInstrumentQuantity::TemperatureK, 300.0), Make(TEXT("tcs_return"), EInstrumentQuantity::TemperatureK, 300.0),
				// 2026-11-03 (re-verification vs NVIDIA Mission Control CDU telemetry: liquid flow, differential pressure,
				// system pressure, status): coolant (secondary) flow, supply-return header differential pressure at the CDU
				// end of the row, system (gauge) pressure at the CDU node, and pump speed feedback (0 when unpowered).
				Make(TEXT("tcs_flow"), EInstrumentQuantity::FlowM3s, FMath::Max(N.GetPumpFlowM3s(Room->GetRowPumpIndex(CduGroupRows[G])), 1e-4)),
				Make(TEXT("tcs_dp"), EInstrumentQuantity::PressurePa, 1e5), Make(TEXT("tcs_pressure"), EInstrumentQuantity::PressurePa, 2e5),
				Make(TEXT("pump_speed"), EInstrumentQuantity::Ratio, 1.0),
				// W5-2 (2026-11-03) process signals: the return flow meter (supply - return = what left between them),
				// the expansion vessel level transmitter and the make-up water meter.
				Make(TEXT("tcs_return_flow"), EInstrumentQuantity::FlowM3s, FMath::Max(N.GetPumpFlowM3s(Room->GetRowPumpIndex(CduGroupRows[G])), 1e-4)),
				Make(TEXT("tank_level"), EInstrumentQuantity::VolumeM3, Cdu->ExpansionTank.TankVolumeM3),
				MakeupMeter(FMath::Max(Cdu->ExpansionTank.MakeupMaxFlowM3s, 1e-6)) };
			ProcessLoopPoints.Add(Instruments.Num());
			ProcessNames.Add(Pt.Label); ProcessDesign.Add(FMath::Max(N.GetPumpFlowM3s(Room->GetRowPumpIndex(CduGroupRows[G])), 1e-4));
			AddPoint(Pt, { NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN });
		}
		// W5: CDU spot sensors (BMS level in NVIDIA's three-level detection): the drip tray under it and the floor of its
		// footprint. Trays are the floor model's trays in CDU actor order (P5); the footprint cells from its oriented box.
		int32 TrayIndex = 0;
		for (TActorIterator<ACoolingDistributionUnitActor> It(GetWorld()); It; ++It)
		{
			const FVector CenterM = SimUnits::UnrealToMeters(It->GetActorLocation());
			const FString Id = It->GetName();
			if (bCduDripTrays && CduDripTrayDepthM > 0.0 && FloorWater.GetTrays().IsValidIndex(TrayIndex))
			{
				FInstrumentPoint Pt;
				Pt.Site = EInstrumentSite::CduDripTray; Pt.Key = TrayIndex++; Pt.Id = Id + TEXT(":drip_tray");
				Pt.Label = FString::Printf(TEXT("%s 드립 트레이"), *Id); Pt.PositionM = CenterM;
				Pt.bWaterContact = true; Pt.Contact = ContactSpec(30.0e-6, false);
				AddPoint(Pt, {});
			}
			TArray<FVector> Footprint;
			for (double X = -.5 * It->SizeM.X; X <= .5 * It->SizeM.X; X += .5 * CellM)
				for (double Y = -.5 * It->SizeM.Y; Y <= .5 * It->SizeM.Y; Y += .5 * CellM)
					Footprint.Add(CenterM + It->GetActorRotation().RotateVector(FVector(X, Y, 0.0)));
			FInstrumentPoint Pt;
			Pt.Site = EInstrumentSite::CduFloorSpot; Pt.Id = Id + TEXT(":floor_spot");
			Pt.Label = FString::Printf(TEXT("%s 바닥 스팟"), *Id); Pt.PositionM = CenterM;
			Pt.bWaterContact = true; Pt.Contact = ContactSpec(5.0e-6, true);
			AddPoint(Pt, {}, FloorCellsUnder(Footprint));
		}
	}
	for (int32 R = 0; bRackLeakSensors && R < Racks.Num(); ++R)
	{
		const ARackThermalActor* Rack = Racks[R];
		if (!Rack || !Room || !Room->GetInRackHandles(Rack) || Rack->Params.Slots.IsEmpty()) continue;
		double Bottom = TNumericLimits<double>::Max(), Top = -Bottom;
		for (const auto& Slot : Rack->Params.Slots) { Bottom = FMath::Min(Bottom, Slot.BottomZM); Top = FMath::Max(Top, Slot.BottomZM + Slot.HeightM); }
		auto WorldM = [&](double X, double Z) { return SimUnits::UnrealToMeters(Rack->GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(FVector(X, 0.0, Z)))); };
		for (bool bRope : {false, true})
		{
			FInstrumentPoint Pt;
			Pt.Site = bRope ? EInstrumentSite::RackManifoldRope : EInstrumentSite::RackDripTray;
			Pt.Key = R; Pt.RackId = Rack->LayoutId.IsEmpty() ? Rack->GetFName().ToString() : Rack->LayoutId;
			Pt.Id = Pt.RackId + (bRope ? TEXT(":manifold_rope") : TEXT(":drip_tray"));
			Pt.Label = FString::Printf(TEXT("랙 %s %s"), *Pt.RackId, bRope ? TEXT("매니폴드 로프") : TEXT("드립 트레이"));
			Pt.bWaterContact = true;
			if (bRope) Pt.Contact.WettingVolumeM3 = 0.0; // any positive local rear exposure; ESTIMATE, not cable sensitivity
			Pt.PositionM = bRope ? WorldM(-.5 * Rack->RackSizeM.X, .5 * (Bottom + Top)) : WorldM(0.0, 0.0);
			Pt.CoverageStartM = bRope ? WorldM(-.5 * Rack->RackSizeM.X, Bottom) : Pt.PositionM;
			Pt.CoverageEndM = bRope ? WorldM(-.5 * Rack->RackSizeM.X, Top) : Pt.PositionM;
			AddPoint(Pt, {});
		}
		// W5 (2026-11-03): tray-level detectors (NVIDIA Mission Control: per chassis a cold plate and an inner manifold
		// leak detector, read by the tray BMC). Cold plate: the water that came into the tray (its plate leak, its
		// connectors, water from above); inner manifold: the water out of the tray's own connector sites.
		const FInRackHandles* H = Room->GetInRackHandles(Rack);
		for (int32 T = 0; T < H->Slots.Num(); ++T)
		{
			const int32 Slot = H->Slots[T];
			const FRackSlot& S = Rack->Params.Slots[Slot];
			for (const bool bManifold : { false, true })
			{
				FInstrumentPoint Pt;
				Pt.Site = bManifold ? EInstrumentSite::TrayManifold : EInstrumentSite::TrayColdPlate;
				Pt.Key = R; Pt.Loop = Slot; Pt.RackId = Rack->LayoutId.IsEmpty() ? Rack->GetFName().ToString() : Rack->LayoutId;
				Pt.Id = FString::Printf(TEXT("%s:tray%d:%s"), *Pt.RackId, T, bManifold ? TEXT("manifold") : TEXT("cold_plate"));
				Pt.Label = FString::Printf(TEXT("랙 %s 트레이 %d %s"), *Pt.RackId, T, bManifold ? TEXT("내부 매니폴드") : TEXT("냉각판"));
				Pt.PositionM = WorldM(bManifold ? -.4 * Rack->RackSizeM.X : 0.0, S.BottomZM + .5 * S.HeightM);
				Pt.bWaterContact = true; Pt.Contact = ContactSpec(2.0e-6, false);
				TArray<int32> Leaks;
				if (bManifold) { Leaks.Add(H->Leaks[5 * T + 1]); Leaks.Add(H->Leaks[5 * T + 3]); }   // supply / return connector
				AddPoint(Pt, {}, {}, {}, Leaks);
			}
		}
	}
	ProcessMonitor.Reset(ProcessNames, ProcessDesign);
	ResetLeakProtection();   // W5-3 follows this run's detectors from their first alarm
	UE_LOG(LogTemp, Display, TEXT("[Sensor] 계측 지점 %d개(누수 감지 케이블 %d개, 배관 간격 %.1f m), 공정값 감시 루프 %d개 - 모의 계측값(D9), 계측기 사양은 추정값"),
		Instruments.Num(), Instruments.GetNumLeakCables(), IntervalM, ProcessMonitor.NumLoops());
}

void ADataCenterSimulationActor::InstrumentsAfterStep()
{
	if (Instruments.Num() == 0) return;
	TArray<TArray<double>> Truth;
	Truth.SetNum(Instruments.Num());
	const FHydraulicNetwork* RoomNet = Room != nullptr ? &Room->GetNetwork() : nullptr;
	const bool bPlant = bPlantBuilt && Plant.IsBuilt();
	for (int32 I = 0; I < Instruments.Num(); ++I)
	{
		const FInstrumentPoint& P = Instruments.GetPoint(I);
		TArray<double>& V = Truth[I];
		switch (P.Site)
		{
		case EInstrumentSite::PlantPipe:
		{
			if (!bPlant) break;
			const EPlantLoopId L = static_cast<EPlantLoopId>(P.Loop & 3);
			const FFacilityWaterLoop* WL = Plant.GetLoopFor(L);
			if (WL == nullptr) break;
			const FHydraulicNetwork& N = WL->GetNetwork();
			double Flow = 0.0;
			if (P.Loop & 16) Flow = WL->IsPumpRunning(P.Key) && WL->GetHandles().Pumps.IsValidIndex(P.Key) ? N.GetPumpFlowM3s(WL->GetHandles().Pumps[P.Key]) : 0.0;
			else if (P.Key >= 0 && P.Key < N.GetNumPipes()) Flow = N.IsPipeClosed(P.Key) ? 0.0 : N.GetPipeFlowM3s(P.Key);
			V = { Flow, PlantDisplay::VolumeTempK(Plant, L, uint8((P.Loop >> 2) & 3)) };
			break;
		}
		case EInstrumentSite::RoomPipe:
		{
			if (RoomNet == nullptr) break;
			double TempK = NAN;
			if (P.Loop < 0 || !Room->TryGetPipeTemperatureK(P.Loop, TempK)) TempK = NAN;   // Loop = the edge it reads (build)
			V = { P.Key >= 0 ? RoomNet->GetPipeFlowM3s(P.Key) : RoomNet->GetPumpFlowM3s(-1 - P.Key), TempK };
			break;
		}
		case EInstrumentSite::PlantUnit:
		{
			if (!bPlant) break;
			const EPlantLoopId L = static_cast<EPlantLoopId>(P.Loop & 3);
			const EUnitSource Source = static_cast<EUnitSource>(P.Loop >> 2);
			const FPlantUnitTruth T = Source == EUnitSource::DryCooler ? Plant.GetDryCoolerTruth(L, P.Key)
				: Source == EUnitSource::Chiller ? Plant.GetChillerTruth(P.Key, false) : Source == EUnitSource::TrimChiller ? Plant.GetChillerTruth(P.Key, true)
				: Source == EUnitSource::Pump ? Plant.GetPumpTruth(L, P.Key) : Plant.GetEconomizerTruth();
			for (const FInstrumentChannelSpec& C : P.Channels) V.Add(TruthOf(T, C.Name));
			break;
		}
		case EInstrumentSite::PlantMakeup:
		{
			const FFacilityWaterLoop* WL = bPlant ? Plant.GetLoopFor(static_cast<EPlantLoopId>(P.Loop)) : nullptr;
			V = { WL ? WL->GetSupplySideLeakM3s() + WL->GetReturnSideLeakM3s() : NAN };   // make-up = loss (plant make-up model)
			break;
		}
		case EInstrumentSite::Cdu:
		{
			const int32 G = P.Key;
			const TArray<FCduCouplingGroup>& Groups = Bridge.GetCduGroups();
			if (!Groups.IsValidIndex(G)) break;
			auto At = [](const TArray<double>& A, int32 K) { return A.IsValidIndex(K) ? A[K] : NAN; };
			V = { Groups[G].Hx.FacilitySupplyTempK, At(Bridge.GetLastCduFacilityReturnK(), G), Groups[G].Hx.FacilityMassFlowKgs / FacilityWater::DensityKgM3,
				At(Bridge.GetLastCduSupplyK(), G), At(Bridge.GetLastCduReturnK(), G) };
			const int32 Row = CduGroupRows.IsValidIndex(G) ? CduGroupRows[G] : INDEX_NONE;
			const int32 Pump = Room != nullptr ? Room->GetRowPumpIndex(Row) : INDEX_NONE;
			if (RoomNet == nullptr || Pump == INDEX_NONE) { V.Append({ NAN, NAN, NAN, NAN, NAN, NAN, NAN }); break; }
			const int32 S = Room->GetRowCduSupplyNodeIndex(Row), Rt = Room->GetRowCduReturnNodeIndex(Row), C = Room->GetRowCduNodeIndex(Row);
			V.Append({ RoomNet->GetPumpFlowM3s(Pump),
				S != INDEX_NONE && Rt != INDEX_NONE ? RoomNet->GetNodePressurePa(S) - RoomNet->GetNodePressurePa(Rt) : NAN,
				C != INDEX_NONE ? RoomNet->GetNodePressurePa(C) : NAN,
				RoomNet->IsPumpPowered(Pump) ? RoomNet->GetPumpSpeedRatio(Pump) : 0.0 });
			const int32 Ret = Room->GetRowCduReturnPipeIndex(Row), Tank = Room->GetRowTankIndex(Row);
			const TArray<FCoolantInventory::FTank>& Tanks = Room->GetCoolantInventory().GetTanks();
			V.Append({ Ret != INDEX_NONE ? RoomNet->GetPipeFlowM3s(Ret) : NAN,
				Tanks.IsValidIndex(Tank) ? Tanks[Tank].State.WaterVolumeM3 : NAN,
				Tanks.IsValidIndex(Tank) ? Tanks[Tank].LastMakeupM3s : NAN });
			break;
		}
		}
	}
	TArray<double> Contact; Contact.Init(NAN, Instruments.Num());
	const FRackInteriorWater& Water = RackInterior.GetWater();
	const double CellA = FMath::Square(FloorWater.GetCellM());
	const TArray<double>& Depth = FloorWater.GetDepthM();
	for (int32 I = 0; I < Instruments.Num(); ++I)
	{
		const FInstrumentPoint& P = Instruments.GetPoint(I);
		if (!P.bWaterContact) continue;
		// W5: water computed by W2/W3/W3.5 under or inside the detector (never the solve's leak flow).
		switch (P.Site)
		{
		case EInstrumentSite::RoomPipe: case EInstrumentSite::CduFloorSpot:
		{
			double V = 0.0;
			if (InstrumentFloorCells.IsValidIndex(I)) for (const int32 C : InstrumentFloorCells[I]) V += Depth.IsValidIndex(C) ? FMath::Max(Depth[C], 0.0) * CellA : 0.0;
			Contact[I] = V;
			continue;
		}
		case EInstrumentSite::PlantPipe:
		{
			double V = 0.0;
			if (InstrumentGroundBins.IsValidIndex(I)) for (const FIntPoint& B : InstrumentGroundBins[I]) V += Spray.GetGroundBinsM3().FindRef(B);
			Contact[I] = V;
			continue;
		}
		case EInstrumentSite::CduDripTray:
			Contact[I] = FloorWater.GetTrays().IsValidIndex(P.Key) ? FloorWater.GetTrays()[P.Key].StoredM3 : NAN;
			continue;
		case EInstrumentSite::TrayManifold:
		{
			double V = 0.0;
			if (InstrumentLeakIndices.IsValidIndex(I)) for (const int32 L : InstrumentLeakIndices[I]) V += GetInternalLeakedM3().FindRef(L);
			Contact[I] = V;
			continue;
		}
		case EInstrumentSite::TrayColdPlate:
		{
			const int32 S = Water.Start.IsValidIndex(P.Key) ? Water.Start[P.Key] + P.Loop : INDEX_NONE;
			// Water that entered the tray itself: everything that passed its slot minus what only ran down the rear.
			Contact[I] = Water.PassedM3.IsValidIndex(S) ? FMath::Max(0.0, Water.PassedM3[S] - Water.RearExposureM3[S]) : NAN;
			continue;
		}
		default: break;
		}
		const int32 R = P.Key;
		if (!Water.FloorM3.IsValidIndex(R)) continue;
		if (P.Site == EInstrumentSite::RackDripTray) Contact[I] = Water.FloorM3[R] + Water.RearFloorM3[R];
		else if (P.Site == EInstrumentSite::RackManifoldRope)
		{
			// One rope covers both rear manifold legs over the slot span (installation ESTIMATE).
			// Max is the largest LOCAL cumulative contact, not a sum of the same water at successive slots.
			Contact[I] = 0.0;
			for (int32 S = Water.Start[R]; S < Water.Start[R + 1]; ++S) Contact[I] = FMath::Max(Contact[I], Water.RearExposureM3[S]);
		}
	}
	// W5 (2026-11-03): every leak detector reads computed water (Contact); the solve's leak flows are not passed.
	for (const FLeakAlarm& A : Instruments.Advance(Clock.GetSimTimeS(), Clock.GetFixedStepS(), Truth, {}, Contact))
	{
		const FInstrumentPoint& P = Instruments.GetPoint(A.Point);
		if (P.bWaterContact)
		{
			UE_LOG(LogTemp, Display, TEXT("[Sensor] %s t=%.2fs, rack=%s type=%s contact=%.12g m3 - 모의 계측값(D9), 문턱/지연 추정"),
				P.Id.IsEmpty() ? *P.Label : *P.Id, A.TimeS, *P.RackId, InstrumentSpecs::SiteCode(P.Site), A.WaterM3);
			continue;
		}
		UE_LOG(LogTemp, Warning, TEXT("[Sensor] 누수 감지 t=%.1f s: %s (%.2f, %.2f, %.2f m), 덮는 누수 유량 %.3f L/s - 모의 계측값(D9)"),
			A.TimeS, *P.Label, P.PositionM.X, P.PositionM.Y, P.PositionM.Z, A.FlowM3s * 1e3);
	}
	// W5-2: process values from the readings only (the instruments' sampling, lag, noise and resolution).
	TArray<FProcessLeakReading> Readings;
	for (const int32 Point : ProcessLoopPoints)
	{
		FProcessLeakReading R;
		const FInstrumentPoint& P = Instruments.GetPoint(Point);
		auto Read = [&](const TCHAR* Name) -> double { for (int32 C = 0; C < P.Channels.Num(); ++C) if (P.Channels[C].Name == FName(Name)) return Instruments.HasReading(Point, C) ? Instruments.GetReading(Point, C) : NAN; return NAN; };
		R.MakeupM3s = Read(TEXT("makeup_flow"));
		if (P.Site == EInstrumentSite::Cdu)
		{
			R.LevelM3 = Read(TEXT("tank_level")); R.SupplyM3s = Read(TEXT("tcs_flow")); R.ReturnM3s = Read(TEXT("tcs_return_flow")); R.PressurePa = Read(TEXT("tcs_pressure"));
		}
		Readings.Add(R);
	}
	for (const FProcessLeakAlarm& A : ProcessMonitor.Step(Clock.GetSimTimeS(), Readings))
		UE_LOG(LogTemp, Warning, TEXT("[Sensor] 공정값 누수 경보 t=%.2f s: %s %s = %.6g (위치 미상, 모의 계측값 D9·문턱 추정)"),
			A.TimeS, *ProcessMonitor.GetLoopName(A.Loop), FProcessLeakMonitor::SignalName(A.Signal), A.Value);
}

FString ADataCenterSimulationActor::DescribeInstrument(int32 Index) const
{
	if (Index < 0 || Index >= Instruments.Num()) return FString();
	const FInstrumentPoint& P = Instruments.GetPoint(Index);
	auto Value = [&](int32 C) -> FString
	{
		if (!Instruments.HasReading(Index, C)) return TEXT("-");
		const double R = Instruments.GetReading(Index, C);
		// Rule 2: K -> C, m^3/s -> L/s, Pa -> kPa, W -> kW only for the screen.
		switch (P.Channels[C].Quantity)
		{
		case EInstrumentQuantity::TemperatureK: return FString::Printf(TEXT("%.1f °C"), SimUnits::KelvinToCelsius(R));
		case EInstrumentQuantity::FlowM3s: return FString::Printf(TEXT("%.2f L/s"), R * 1e3);
		case EInstrumentQuantity::PressurePa: return FString::Printf(TEXT("%.0f kPa"), R * 1e-3);
		case EInstrumentQuantity::Ratio: return FString::Printf(TEXT("%.0f %%"), R * 100.0);
		case EInstrumentQuantity::PowerW: return FString::Printf(TEXT("%.1f kW"), R * 1e-3);
		case EInstrumentQuantity::VolumeM3: return FString::Printf(TEXT("%.2f L"), R * 1e3);
		default: return FString::Printf(TEXT("%.2f"), R);
		}
	};
	auto Find = [&](const TCHAR* Name) { for (int32 C = 0; C < P.Channels.Num(); ++C) if (P.Channels[C].Name == FName(Name)) return C; return int32(INDEX_NONE); };
	auto Pair = [&](const TCHAR* A, const TCHAR* B) { const int32 IA = Find(A), IB = Find(B); return FString::Printf(TEXT("%s -> %s"), *Value(IA), *Value(IB)); };
	FString Text;
	switch (P.Site)
	{
	case EInstrumentSite::PlantPipe: case EInstrumentSite::RoomPipe:
	{
		// A pipe without a water temperature (a pump run) shows its flow only.
		const int32 Temp = Find(TEXT("temp"));
		Text = Instruments.HasReading(Index, Temp) ? FString::Printf(TEXT("%s %s"), *Value(Find(TEXT("flow"))), *Value(Temp)) : Value(Find(TEXT("flow")));
		break;
	}
	case EInstrumentSite::Cdu:
		Text = FString::Printf(TEXT("%s | 설비수 %s, %s | 2차 %s, %s, 차압 %s, 압력 %s | 펌프 %s"), *P.Label, *Pair(TEXT("fws_in"), TEXT("fws_out")), *Value(Find(TEXT("fws_flow"))),
			*Pair(TEXT("tcs_supply"), TEXT("tcs_return")), *Value(Find(TEXT("tcs_flow"))), *Value(Find(TEXT("tcs_dp"))), *Value(Find(TEXT("tcs_pressure"))), *Value(Find(TEXT("pump_speed"))));
		break;
	default:
		switch (static_cast<EUnitSource>(P.Loop >> 2))
		{
		case EUnitSource::DryCooler:
			Text = FString::Printf(TEXT("%s | 팬 %s | 물 %s | %s"), *P.Label, *Value(Find(TEXT("fan"))), *Pair(TEXT("in"), TEXT("out")), *Value(Find(TEXT("flow"))));
			break;
		case EUnitSource::Chiller: case EUnitSource::TrimChiller:
			Text = FString::Printf(TEXT("%s | 증발 %s | 응축 %s | COP %s"), *P.Label, *Pair(TEXT("in"), TEXT("out")), *Pair(TEXT("cond_in"), TEXT("cond_out")), *Value(Find(TEXT("cop"))));
			break;
		case EUnitSource::Pump:
			Text = FString::Printf(TEXT("%s | 회전 %s | %s | 양정 %s | %s"), *P.Label, *Value(Find(TEXT("speed"))), *Value(Find(TEXT("flow"))), *Value(Find(TEXT("head"))), *Value(Find(TEXT("power"))));
			break;
		default:
			Text = FString::Printf(TEXT("%s | 냉수 %s | 응축수 %s | %s"), *P.Label, *Pair(TEXT("in"), TEXT("out")), *Pair(TEXT("cond_in"), TEXT("cond_out")), *Value(Find(TEXT("heat"))));
			break;
		}
		break;
	}
	return Text;
}
