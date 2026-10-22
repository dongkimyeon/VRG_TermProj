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
#include "Simulation/Core/SimUnits.h"
#include "Visualization/PlantDisplay.h"
#include "HAL/IConsoleManager.h"

namespace
{
	// Spacing of the pipe instrument points [m]: a layout choice (spot leak detectors and meters along the runs),
	// ESTIMATE, not a design code value.
	TAutoConsoleVariable<float> CVarPipeIntervalM(TEXT("vrg.Sensors.PipeIntervalM"), 3.f,
		TEXT("Spacing [m] of the pipe instrument points (flow, water temperature, leak-detection cable). Applied at the next simulation entry."));

	// Point placement along a polyline: N = round(length / interval) >= 1 points at the centres of N equal sections,
	// each covering its section (the cover radius reaches both section ends, plus a 0.3 m allowance for the drawn
	// fittings - a display abstraction, not a physical range).
	void PlaceAlong(const TArray<FVector>& Pts, double IntervalM, TArray<FVector>& OutPositions, double& OutCoverM)
	{
		OutPositions.Reset();
		double Length = 0.0;
		for (int32 K = 0; K + 1 < Pts.Num(); ++K) Length += FVector::Distance(Pts[K], Pts[K + 1]);
		if (Pts.Num() < 2 || Length <= UE_DOUBLE_SMALL_NUMBER) { if (Pts.Num() > 0) OutPositions.Add(Pts[0]); OutCoverM = 0.3; return; }
		const int32 N = FMath::Max(1, FMath::RoundToInt32(Length / FMath::Max(IntervalM, 0.1)));
		const double Section = Length / N;
		OutCoverM = 0.5 * Section + 0.3;
		for (int32 I = 0; I < N; ++I)
		{
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
	for (TArray<FVector>& N : InstrumentPlantNodesM) N.Reset();
	const double IntervalM = FMath::Max(double(CVarPipeIntervalM.GetValueOnGameThread()), 0.1);
	using namespace InstrumentSpecs;
	auto AddPipePoints = [&](EInstrumentSite Site, const TArray<FVector>& PointsM, const FString& Label, int32 Key, int32 Loop, double FlowScale, double FlowNow, double TempNow)
	{
		TArray<FVector> Positions; double Cover = 0.0;
		PlaceAlong(PointsM, IntervalM, Positions, Cover);
		for (const FVector& P : Positions)
		{
			FInstrumentPoint Pt;
			Pt.Site = Site; Pt.Label = Label; Pt.PositionM = P; Pt.Key = Key; Pt.Loop = Loop;
			Pt.Channels = { Make(NFlow, EInstrumentQuantity::FlowM3s, FlowScale), Make(NTemp, EInstrumentQuantity::TemperatureK, 300.0) };
			Pt.bLeakCable = true; Pt.CoverRadiusM = Cover;
			Instruments.AddPoint(Pt, { FlowNow, TempNow });
		}
	};

	// Plant yard: one snapshot of the drawn plant gives the pipe routes, the unit boxes and the node positions.
	if (bPlantBuilt && Plant.IsBuilt())
	{
		const FPlantDisplaySnapshot S = PlantDisplay::Build(Plant, MakePlantDisplayInput());
		for (int32 L = 0; L < 3; ++L) InstrumentPlantNodesM[L] = S.NodePositionsM[L];
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
			Instruments.AddPoint(Pt, Initial);
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
			const double FacilityM3s = CduSummaries.IsValidIndex(G) ? CduSummaries[G].FacilityMassFlowKgs / FMath::Max(CoolantDensityKgM3, 1.0) : 1e-3;
			Pt.Channels = { Make(TEXT("fws_in"), EInstrumentQuantity::TemperatureK, 300.0), Make(TEXT("fws_out"), EInstrumentQuantity::TemperatureK, 300.0),
				Make(TEXT("fws_flow"), EInstrumentQuantity::FlowM3s, FMath::Max(FacilityM3s, 1e-4)),
				Make(TEXT("tcs_supply"), EInstrumentQuantity::TemperatureK, 300.0), Make(TEXT("tcs_return"), EInstrumentQuantity::TemperatureK, 300.0) };
			Instruments.AddPoint(Pt, { NAN, NAN, NAN, NAN, NAN });
		}
	}
	UE_LOG(LogTemp, Display, TEXT("[Sensor] 계측 지점 %d개(누수 감지 케이블 %d개, 배관 간격 %.1f m) - 모의 계측값(D9), 계측기 사양은 추정값"),
		Instruments.Num(), Instruments.GetNumLeakCables(), IntervalM);
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
		case EInstrumentSite::Cdu:
		{
			const int32 G = P.Key;
			const TArray<FCduCouplingGroup>& Groups = Bridge.GetCduGroups();
			if (!Groups.IsValidIndex(G)) break;
			auto At = [](const TArray<double>& A, int32 K) { return A.IsValidIndex(K) ? A[K] : NAN; };
			V = { Groups[G].Hx.FacilitySupplyTempK, At(Bridge.GetLastCduFacilityReturnK(), G), Groups[G].Hx.FacilityMassFlowKgs / FMath::Max(CoolantDensityKgM3, 1.0),
				At(Bridge.GetLastCduSupplyK(), G), At(Bridge.GetLastCduReturnK(), G) };
			break;
		}
		}
	}
	// Leaks as the solves have them now: plant loops (positions of their nodes in the yard) and the room TCS.
	TArray<FInstrumentLeak> Leaks;
	if (bPlant)
		for (const EPlantLoopId L : { EPlantLoopId::Fws, EPlantLoopId::Chw, EPlantLoopId::Cw })
			if (const FFacilityWaterLoop* WL = Plant.GetLoopFor(L))
				for (int32 K = 0; K < WL->GetNumLeaks(); ++K)
				{
					const int32 Node = WL->GetNetwork().GetLeak(WL->GetLeak(K).NetworkLeak).NodeIndex;
					if (InstrumentPlantNodesM[uint8(L)].IsValidIndex(Node)) Leaks.Add({ InstrumentPlantNodesM[uint8(L)][Node], WL->GetLeakFlowM3s(K) });
				}
	if (RoomNet != nullptr)
		for (int32 K = 0; K < RoomNet->GetNumLeaks(); ++K)
			Leaks.Add({ Room->GetNodePositionM(RoomNet->GetLeak(K).NodeIndex), RoomNet->GetLeakFlowM3s(K) });
	for (const FLeakAlarm& A : Instruments.Advance(Clock.GetSimTimeS(), Clock.GetFixedStepS(), Truth, Leaks))
	{
		const FInstrumentPoint& P = Instruments.GetPoint(A.Point);
		UE_LOG(LogTemp, Warning, TEXT("[Sensor] 누수 감지 t=%.1f s: %s (%.2f, %.2f, %.2f m), 덮는 누수 유량 %.3f L/s - 모의 계측값(D9)"),
			A.TimeS, *P.Label, P.PositionM.X, P.PositionM.Y, P.PositionM.Z, A.FlowM3s * 1e3);
	}
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
		Text = FString::Printf(TEXT("%s | 설비수 %s, %s | 2차 %s"), *P.Label, *Pair(TEXT("fws_in"), TEXT("fws_out")), *Value(Find(TEXT("fws_flow"))), *Pair(TEXT("tcs_supply"), TEXT("tcs_return")));
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
