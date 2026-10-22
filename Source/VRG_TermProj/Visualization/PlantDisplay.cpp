// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/PlantDisplay.h"
#include "Visualization/PipeTemperatureDisplay.h"
#include "Algo/Reverse.h"

namespace
{
	// Schematic layout [m] (not plant dimensions from a catalog).
	constexpr double LaneGapM = 2.0, UnitPitchM = 2.8, PumpPitchM = 0.9;
	constexpr int32 UnitColumns = 6;
	// Yard columns, from the yard's X0 (room +X face + 3 m): pump suction header, pumps, plant supply header, plant return
	// header (coil supply), coil collector (back to the suction), condenser trunk, first unit column.
	constexpr double PumpBarDX = 0.3, PumpDX = 1.0, PlantSupplyDX = 1.8, PlantReturnDX = 3.5, CollectorDX = 3.75, YardTrunkDX = 4.1, UnitsDX = 6.0;
	constexpr double PumpSuctionZ = 0.3, PumpOutZ = 0.9, PlantSupplyZ = 1.2;
	constexpr double CoilInZ = 2.3, CoilOutZ = 2.55;            // yard coil lanes, over the 1.8 m chillers
	constexpr double YardBranchZ = 3.0;                          // condenser branches: supply layer (return one layer up)
	constexpr double LayerM = 0.25;                              // supply -> return -> middle layer of one loop
	constexpr double RoomTrunkDX = 1.2;                          // in-room loops' headers beyond the room's +X face
	constexpr double RoomTrayAboveM = 0.5, RoomLoopStepM = 0.85; // first in-room loop above the room top, next loops above it
	constexpr double RoomLanePitchM = 0.25, RoomClusterM = 0.6;  // one lane per branch; consumers this close share a band
	constexpr double AisleOffsetM = 0.35, AisleLanePitchM = 0.18;
	constexpr double ConsumerTopM = 1.9;                         // drop end above a CDU (1.9 m) / CRAH (2.24 m) base
	constexpr double ConsumerHalfM = 0.2;                        // supply / return drops either side of the consumer centre
	constexpr double MainOffsetM = 0.3;                          // supply main at the lane centre - this, return main + this
	constexpr double CoilOnUnitStepM = 0.12;                     // more coils than units: the next coil beside the first
	constexpr double EconLinkZ = 2.0;                            // CW side to the economizer: over 1.3 / 1.8 m units, under the coil lanes
	constexpr double EconLinkGapM = 1.0;                         // ... leaving the CW lane through the middle of the lane gap

	struct FPlantLane
	{
		EPlantLoopId Id = EPlantLoopId::Fws;
		const FFacilityWaterLoop* Loop = nullptr;
		double Y0 = 0.0, Width = 0.0;
		int32 Units = 0;                   // sink units drawn in the lane (dry coolers / chillers)
		int32 ExtraUnits = 0;              // trim chillers (FWS lane) after the dry coolers
		double Yc() const { return Y0 + 0.5 * Width; }
	};

	FVector UnitPosition(double X0, const FPlantLane& L, int32 K, double HalfZ)
	{
		return FVector(X0 + UnitsDX + (K % UnitColumns) * UnitPitchM, L.Y0 + 1.5 + (K / UnitColumns) * UnitPitchM, HalfZ);
	}

	EPlantVisualKind VisualOf(EPlantEquipmentKind K)
	{
		switch (K)
		{
		case EPlantEquipmentKind::DryCooler: return EPlantVisualKind::DryCooler;
		case EPlantEquipmentKind::Pump: return EPlantVisualKind::Pump;
		case EPlantEquipmentKind::Economizer: return EPlantVisualKind::Economizer;
		default: return EPlantVisualKind::Chiller;
		}
	}

	// Polyline builder: drops repeated points and merges straight runs (fewer drawn segments).
	void Push(TArray<FVector>& P, const FVector& V)
	{
		if (P.Num() > 0 && P.Last().Equals(V, 1e-9)) return;
		if (P.Num() >= 2)
		{
			const FVector A = (P.Last() - P[P.Num() - 2]).GetSafeNormal(), B = (V - P.Last()).GetSafeNormal();
			if (A.Equals(B, 1e-9)) { P.Last() = V; return; }
		}
		P.Add(V);
	}
	TArray<FVector> Line(std::initializer_list<FVector> Pts) { TArray<FVector> P; for (const FVector& V : Pts) Push(P, V); return P; }

	// A header drawn along world Y at fixed X / Z; every pipe attached to it adds its anchor Y.
	struct FBar
	{
		double X = 0.0, Z = 0.0;
		TArray<double> Ys;
		FVector At(double Y) { Ys.Add(Y); return FVector(X, Y, Z); }
		double Lo() const { double V = TNumericLimits<double>::Max(); for (double Y : Ys) V = FMath::Min(V, Y); return V; }
		double Hi() const { double V = -TNumericLimits<double>::Max(); for (double Y : Ys) V = FMath::Max(V, Y); return V; }
	};

	// One yard connection (a coil through its unit, or a condenser branch) and its aisle lane.
	struct FYardLink
	{
		int32 Pipe = INDEX_NONE;           // coil pipe edge, or the branch index for a condenser branch
		FVector In, Out;                   // unit connections [m]: In = the one nearer the header column
		double Side = -1.0;                // aisle side of the face: -1 = -Y, +1 = +Y
		double LaneY = 0.0;
		int64 Group = 0;
	};

	void AssignAisleLanes(TArray<FYardLink>& Links)
	{
		TMap<int64, TArray<int32>> Groups;
		for (int32 I = 0; I < Links.Num(); ++I) Groups.FindOrAdd(Links[I].Group).Add(I);
		for (auto& G : Groups)
		{
			G.Value.Sort([&](int32 A, int32 B) { return Links[A].In.X < Links[B].In.X || (Links[A].In.X == Links[B].In.X && A < B); });
			const double Side = Links[G.Value[0]].Side;
			double Face = Links[G.Value[0]].In.Y;
			for (int32 I : G.Value) Face = Side < 0 ? FMath::Min(Face, Links[I].In.Y) : FMath::Max(Face, Links[I].In.Y);
			// Condenser lanes sit half a pitch off the coil lanes: where a chiller row's condenser aisle meets the next
			// row's evaporator aisle, a condenser drop never lands on a coil lane.
			const double Shift = (G.Key >> 39) & 1 ? 0.5 * AisleLanePitchM : 0.0;
			for (int32 K = 0; K < G.Value.Num(); ++K) Links[G.Value[K]].LaneY = Face + Side * (AisleOffsetM + Shift + K * AisleLanePitchM);
		}
	}

	FYardLink MakeLink(const FPlantDisplayEquipment& E, bool bCondenser, int32 Index, int32 OnUnit, double HeaderX, int32 LoopTag)
	{
		FYardLink K;
		K.Pipe = Index;
		FVector A = PlantDisplay::PortM(E, bCondenser ? EPlantPort::CondenserIn : EPlantPort::CoilIn);
		FVector B = PlantDisplay::PortM(E, bCondenser ? EPlantPort::CondenserOut : EPlantPort::CoilOut);
		const FVector Step = E.Rotation.RotateVector(FVector(CoilOnUnitStepM * OnUnit, 0, 0));
		A += Step; B += Step;
		if (FMath::Abs(B.X - HeaderX) < FMath::Abs(A.X - HeaderX)) Swap(A, B);
		K.In = A; K.Out = B;
		const FVector Normal = E.Rotation.RotateVector(FVector(0, bCondenser ? 1 : -1, 0));
		K.Side = Normal.Y > 0.0 ? 1.0 : -1.0;
		K.Group = (int64(LoopTag) << 40) | (int64(bCondenser ? 1 : 0) << 39) | (int64(K.Side > 0 ? 1 : 0) << 38) | (int64(FMath::RoundToInt(A.Y * 2.0)) & 0x3FFFFFFFFF);
		return K;
	}

	// Posts along a run from Lo to Hi, in the gaps between the given lane / riser coordinates (never on a pipe).
	TArray<double> PostSpots(double Lo, double Hi, TArray<double> Avoid, double Spacing)
	{
		TArray<double> Out;
		Avoid.Add(Lo); Avoid.Add(Hi);
		Avoid.Sort();
		double Last = -TNumericLimits<double>::Max();
		for (int32 I = 0; I + 1 < Avoid.Num(); ++I)
		{
			if (Avoid[I + 1] - Avoid[I] < 0.5) continue;
			const double Mid = 0.5 * (Avoid[I] + Avoid[I + 1]);
			const bool bEdge = I == 0 || I + 2 == Avoid.Num();
			if (bEdge || Mid - Last >= Spacing) { Out.Add(Mid); Last = Mid; }
		}
		return Out;
	}

	// From a header anchor to a unit's connection (2026-10-22, placed pumps and the economizer): to the lane height,
	// along X, along Y, down to a point Offset off the connection along its outward normal, then into it.
	TArray<FVector> ToPort(const FVector& Anchor, double LaneZ, const FVector& Port, const FVector& Normal, double Offset)
	{
		const FVector Ap = Port + Normal * Offset;
		return Line({ Anchor, FVector(Anchor.X, Anchor.Y, LaneZ), FVector(Ap.X, Anchor.Y, LaneZ), FVector(Ap.X, Ap.Y, LaneZ), Ap, Port });
	}
	TArray<FVector> Reversed(TArray<FVector> P) { Algo::Reverse(P); return P; }
	TArray<FVector> Joined(TArray<FVector> A, const TArray<FVector>& B) { for (const FVector& V : B) Push(A, V); return A; }

	double SegmentGap(const FVector& A0, const FVector& A1, const FVector& B0, const FVector& B1)
	{
		FVector PA, PB;
		FMath::SegmentDistToSegmentSafe(A0, A1, B0, B1, PA, PB);
		return FVector::Distance(PA, PB);
	}
}

EPlantVisualKind PlantDisplay::VisualKind(EPlantEquipmentKind Kind) { return VisualOf(Kind); }

FVector PlantDisplay::PortM(const FPlantDisplayEquipment& E, EPlantPort Port)
{
	const FVector Local = PlantVisual::PortLocalM(VisualOf(E.Kind), E.SizeM, Port) - FVector(0, 0, 0.5 * E.SizeM.Z);
	return E.CenterM + E.Rotation.RotateVector(Local);
}

double PlantDisplay::VolumeTempK(const FFacilityPlant& Plant, EPlantLoopId Loop, uint8 Volume)
{
	const FFacilityPlantState& St = Plant.GetState();
	if (Volume == 2) return Plant.GetSupplyTempK();   // series: the middle header carries the CDU inlet mix
	const bool bSupply = Volume == 0;
	switch (Loop)
	{
	case EPlantLoopId::Chw: return bSupply ? St.ChwSupplyK : St.ChwReturnK;
	case EPlantLoopId::Cw: return bSupply ? St.CwSupplyK : St.CwReturnK;
	default: return bSupply ? St.SupplyK : St.ReturnK;
	}
}

FPlantDisplaySnapshot PlantDisplay::Build(const FFacilityPlant& Plant, const FPlantDisplayInput& In)
{
	FPlantDisplaySnapshot S;
	S.bLive = In.bLive;
	if (!Plant.IsBuilt()) return S;
	const FFacilityThermalSpec& Th = Plant.GetThermalSpec();
	const FFacilityPlantState& St = Plant.GetState();
	const FBox Room = In.RoomM.IsValid ? In.RoomM : FBox(FVector(-5, -5, 0), FVector(5, 5, 3));
	const double X0 = Room.Max.X + 3.0;
	const int32 NChw = FMath::Max(Th.DutyChillers, 1) + FMath::Max(Th.StandbyChillers, 0);
	const int32 NTrim = Plant.IsTrimActive() ? FMath::Max(Th.TrimDutyChillers, 1) + FMath::Max(Th.TrimStandbyChillers, 0) : 0;
	const FQuat Turned(FVector::UpVector, PI);   // pumps: suction (+X of the pump) toward the suction header (-X)

	// Lanes beside the room, one per built loop.
	TArray<FPlantLane> Lanes;
	double Y = Room.Min.Y;
	for (const EPlantLoopId Id : { EPlantLoopId::Fws, EPlantLoopId::Chw, EPlantLoopId::Cw })
	{
		const FFacilityWaterLoop* L = Plant.GetLoopFor(Id);
		if (L == nullptr) continue;
		FPlantLane& Lane = Lanes.AddDefaulted_GetRef();
		Lane.Id = Id; Lane.Loop = L; Lane.Y0 = Y;
		Lane.Units = Id == EPlantLoopId::Fws ? Plant.GetNumDryCoolers() : Id == EPlantLoopId::Chw ? NChw : Plant.GetNumCwDryCoolers();
		Lane.ExtraUnits = Id == EPlantLoopId::Fws ? NTrim : 0;
		const int32 Rows = FMath::Max(1, FMath::DivideAndRoundUp(Lane.Units + Lane.ExtraUnits + (Id == EPlantLoopId::Chw ? 1 : 0), UnitColumns));
		Lane.Width = FMath::Max(6.0, 3.0 + Rows * UnitPitchM);
		Y += Lane.Width + LaneGapM;
	}

	// F8: model unit K of a kind is drawn at placed unit K when there is one.
	auto Place = [](FPlantDisplayEquipment& E, const TArray<FPlantDisplayUnit>& Units, int32 K)
	{
		if (!Units.IsValidIndex(K)) return;
		const FPlantDisplayUnit& U = Units[K];
		E.CenterM = U.CenterM; E.SizeM = U.SizeM; E.Rotation = U.Rotation; E.bPlaced = true;
	};

	// Equipment of the lanes (built first: coils and condenser branches run to these units).
	TArray<int32> ChillerEq, TrimEq, FwsEq, CwEq, PumpEq[3];
	int32 EconEq = INDEX_NONE;
	for (const FPlantLane& L : Lanes)
	{
		const FFacilityLoopHandles& H = L.Loop->GetHandles();
		const TArray<FPlantDisplayUnit>& PlacedPumps = L.Id == EPlantLoopId::Fws ? In.FwsPumps : L.Id == EPlantLoopId::Chw ? In.ChwPumps : In.CwPumps;
		for (int32 P = 0; P < H.Pumps.Num(); ++P)
		{
			FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
			E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Pump; E.Index = P;
			E.CenterM = FVector(X0 + PumpDX, L.Y0 + 0.8 + P * PumpPitchM, 0.3); E.SizeM = FVector(0.6, 0.6, 0.6); E.Rotation = Turned;
			Place(E, PlacedPumps, P);
			E.bRunning = L.Loop->IsPumpRunning(P); E.bFailed = L.Loop->IsPumpTripped(P);
			PumpEq[uint8(L.Id)].Add(S.Equipment.Num() - 1);
		}
		if (L.Id == EPlantLoopId::Chw)
		{
			for (int32 K = 0; K < L.Units; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Chiller; E.Index = K;
				E.CenterM = UnitPosition(X0, L, K, 0.9); E.SizeM = FVector(2.4, 1.2, 1.8);
				Place(E, In.Chillers, K);
				// Healthy units first: the running count takes the lowest healthy indices, failed ones are the last.
				const int32 Healthy = NChw - St.TrippedChillers;
				E.bFailed = K >= Healthy; E.bRunning = !E.bFailed && K < St.ChillersRunning;
				ChillerEq.Add(S.Equipment.Num() - 1);
			}
			if (Plant.HasEconomizer())
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Economizer;
				E.CenterM = UnitPosition(X0, L, L.Units, 0.8); E.SizeM = FVector(0.8, 0.5, 1.6);
				Place(E, In.Economizers, 0);
				E.bRunning = St.bEconomizerOn;
				EconEq = S.Equipment.Num() - 1;
			}
		}
		else
		{
			const int32 Tripped = Plant.GetTrippedDryCoolers(L.Id);
			const TArray<FPlantDisplayUnit>& Units = L.Id == EPlantLoopId::Fws ? In.FwsDryCoolers : In.CwDryCoolers;
			TArray<int32>& UnitEq = L.Id == EPlantLoopId::Fws ? FwsEq : CwEq;
			for (int32 K = 0; K < L.Units; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::DryCooler; E.Index = K;
				E.CenterM = UnitPosition(X0, L, K, 0.65); E.SizeM = FVector(2.4, 1.2, 1.3);
				Place(E, Units, K);
				E.bFailed = K >= L.Units - Tripped;
				E.bRunning = !E.bFailed && (L.Id == EPlantLoopId::Fws ? St.FanRatio : St.CwDryCoolerHeatW > 0.0 ? 1.0 : 0.0) > 0.0;
				UnitEq.Add(S.Equipment.Num() - 1);
			}
			for (int32 K = 0; K < L.ExtraUnits; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::TrimChiller; E.Index = K;
				E.CenterM = UnitPosition(X0, L, L.Units + K, 0.9); E.SizeM = FVector(2.4, 1.2, 1.8);
				Place(E, In.TrimChillers, K);
				const int32 Healthy = NTrim - St.TrippedTrimChillers;
				E.bFailed = K >= Healthy; E.bRunning = !E.bFailed && K < St.TrimRunning;
				TrimEq.Add(S.Equipment.Num() - 1);
			}
		}
	}

	// Consumers of the room branches, all loops together: one lane per branch, so no two runs in the room overlap.
	const int32 NumCrah = In.CrahPositionsM.Num();
	const int32 NChwCond = Plant.IsChilledWaterActive() ? FMath::Max(Th.DutyChillers, 1) : 0;
	struct FRoomBranch { EPlantLoopId Loop; int32 Branch; FVector C; double LaneY = 0.0; };
	TArray<FRoomBranch> RoomBranches;
	for (const FPlantLane& L : Lanes)
	{
		if (L.Id == EPlantLoopId::Cw) continue;
		for (int32 B = 0; B < L.Loop->GetNumBranches(); ++B)
		{
			FVector C = FVector(Room.Max.X - 1.0, L.Yc(), 0.0);
			if (L.Id == EPlantLoopId::Fws) { if (In.CduPositionsM.IsValidIndex(B)) C = In.CduPositionsM[B]; }
			else if (B < NumCrah) C = In.CrahPositionsM[B];
			else if (In.CduPositionsM.IsValidIndex(B - NumCrah)) C = In.CduPositionsM[B - NumCrah];
			RoomBranches.Add({ L.Id, B, C });
		}
	}
	{
		TArray<int32> Order;
		for (int32 I = 0; I < RoomBranches.Num(); ++I) Order.Add(I);
		Order.Sort([&](int32 A, int32 B) { return RoomBranches[A].C.Y < RoomBranches[B].C.Y || (RoomBranches[A].C.Y == RoomBranches[B].C.Y && A < B); });
		for (int32 First = 0; First < Order.Num();)
		{
			int32 End = First + 1;
			while (End < Order.Num() && RoomBranches[Order[End]].C.Y - RoomBranches[Order[End - 1]].C.Y < RoomClusterM) ++End;
			TArray<int32> Band(&Order[First], End - First);
			double Mean = 0.0;
			for (int32 I : Band) Mean += RoomBranches[I].C.Y / Band.Num();
			Band.Sort([&](int32 A, int32 B) { return RoomBranches[A].C.X < RoomBranches[B].C.X || (RoomBranches[A].C.X == RoomBranches[B].C.X && A < B); });
			for (int32 K = 0; K < Band.Num(); ++K) RoomBranches[Band[K]].LaneY = Mean + (K - 0.5 * (Band.Num() - 1)) * RoomLanePitchM;
			First = End;
		}
	}
	auto RoomBranch = [&](EPlantLoopId Id, int32 B) -> const FRoomBranch*
	{
		for (const FRoomBranch& R : RoomBranches) if (R.Loop == Id && R.Branch == B) return &R;
		return nullptr;
	};

	// Network nodes and pipes of every lane.
	uint32 Hash = GetTypeHash(int32(Plant.GetTopology()));
	int32 RoomLoops = 0;
	struct FTrunk { double X = 0.0, Lo = TNumericLimits<double>::Max(), Hi = -TNumericLimits<double>::Max(); TArray<double> Ys, Bottoms; } RoomTrunk;
	TArray<FVector2D> YardLanes;   // (lane Y, lowest height) of every yard lane so far: posts stay off them
	for (const FPlantLane& L : Lanes)
	{
		const FFacilityWaterLoop& Loop = *L.Loop;
		const FHydraulicNetwork& N = Loop.GetNetwork();
		const FFacilityLoopHandles& H = Loop.GetHandles();
		const bool bRoom = L.Id != EPlantLoopId::Cw;
		const double Base = bRoom ? Room.Max.Z + RoomTrayAboveM + (RoomLoops++) * RoomLoopStepM : YardBranchZ;
		const double ZSup = Base, ZRet = Base + LayerM, ZMid = Base + 2.0 * LayerM;
		const double XT = bRoom ? Room.Max.X + RoomTrunkDX : X0 + YardTrunkDX;
		const double Yc = L.Yc(), YSupplyMain = Yc - MainOffsetM, YReturnMain = Yc + MainOffsetM;
		const double XPumpBar = X0 + PumpBarDX, XPump = X0 + PumpDX, XPS = X0 + PlantSupplyDX, XPR = X0 + PlantReturnDX, XSU = X0 + CollectorDX;

		TMap<int32, FBar> Bars;
		auto MakeBar = [&](int32 Node, double X, double Z) { if (Node != INDEX_NONE) { FBar& B = Bars.Add(Node); B.X = X; B.Z = Z; } };
		MakeBar(H.SupplyHeaderNode, XT, ZSup);
		MakeBar(H.ReturnHeaderNode, XT, ZRet);
		MakeBar(H.MiddleNode, XT, ZMid);
		MakeBar(H.PlantSupplyNode, XPS, PlantSupplyZ);
		MakeBar(H.PlantReturnNode, XPR, CoilInZ);
		MakeBar(H.SuctionNode, XSU, CoilOutZ);   // the coil collector; the pump suction header is added below
		FBar PumpBar; PumpBar.X = XPumpBar; PumpBar.Z = PumpSuctionZ;
		auto LayerOf = [&](int32 Node) { const FBar* B = Bars.Find(Node); return B ? B->Z : ZSup; };

		TArray<FVector>& Pos = S.NodePositionsM[uint8(L.Id)];
		TArray<TArray<FVector>>& Shapes = S.NodeShapesM[uint8(L.Id)];
		Pos.Init(FVector(X0, Yc, 0.6), N.GetNumNodes());
		Shapes.SetNum(N.GetNumNodes());
		auto SetPoint = [&](int32 Node, const FVector& P) { if (Pos.IsValidIndex(Node)) { Pos[Node] = P; Shapes[Node] = { P }; } };

		TArray<TArray<FVector>> Routes;   // by pipe edge
		Routes.SetNum(N.GetNumPipes());
		TArray<TArray<FVector>> PumpRoutes;
		PumpRoutes.SetNum(H.Pumps.Num());

		// Pumps and their check pipes into the plant supply header.
		for (int32 P = 0; P < H.Pumps.Num(); ++P)
		{
			const int32 Discharge = N.GetPumpToNodeIndex(H.Pumps[P]);
			const FPlantDisplayEquipment* Unit = PumpEq[uint8(L.Id)].IsValidIndex(P) ? &S.Equipment[PumpEq[uint8(L.Id)][P]] : nullptr;
			if (Unit != nullptr && Unit->bPlaced)
			{
				// A placed pump: suction header -> suction flange (low run), through the pump, discharge flange -> up to
				// the plant supply header's height and along to it.
				const FVector Suction = PortM(*Unit, EPlantPort::CoilIn), Out = PortM(*Unit, EPlantPort::CoilOut);
				const FVector SuctionN = Unit->Rotation.RotateVector(FVector::ForwardVector);
				SetPoint(Discharge, Out);
				const FVector SA = Suction + SuctionN * 0.3;
				PumpRoutes[P] = Joined(ToPort(PumpBar.At(SA.Y), PumpSuctionZ, Suction, SuctionN, 0.3), Line({ Suction, FVector(Out.X, Out.Y, Suction.Z), Out }));
				if (H.PumpCheckPipes.IsValidIndex(P))
					Routes[H.PumpCheckPipes[P]] = Reversed(ToPort(Bars[H.PlantSupplyNode].At(Out.Y), PlantSupplyZ, Out, FVector::UpVector, 0.3));
				continue;
			}
			const double PY = L.Y0 + 0.8 + P * PumpPitchM;
			const FVector Out(XPump, PY, PumpOutZ);
			SetPoint(Discharge, Out);
			PumpRoutes[P] = Line({ PumpBar.At(PY), FVector(XPump, PY, PumpSuctionZ), Out });
			if (H.PumpCheckPipes.IsValidIndex(P))
				Routes[H.PumpCheckPipes[P]] = Line({ Out, FVector(XPump, PY, PlantSupplyZ), Bars[H.PlantSupplyNode].At(PY) });
		}
		// Mains: plant supply header -> up -> along X -> supply header; return header -> along X -> down -> plant return.
		const FPlantDisplayEquipment* Econ = H.SeriesHxPipe != INDEX_NONE && S.Equipment.IsValidIndex(EconEq) ? &S.Equipment[EconEq] : nullptr;
		auto PlainSupplyMain = [&]()
		{
			const FVector From = Bars[H.PlantSupplyNode].At(YSupplyMain);
			const FVector To = Bars[H.SupplyHeaderNode].At(YSupplyMain);
			Routes[H.SupplyMainPipe] = Line({ From, FVector(From.X, From.Y, To.Z), To });
		};
		auto PlainReturnMain = [&]()
		{
			const FVector RFrom = Bars[H.ReturnHeaderNode].At(YReturnMain);
			const FVector RTo = Bars[H.PlantReturnNode].At(YReturnMain);
			Routes[H.ReturnMainPipe] = Line({ RFrom, FVector(RTo.X, RFrom.Y, RFrom.Z), RTo });
		};
		if (Econ != nullptr && L.Id == EPlantLoopId::Chw)
		{
			// Chilled water side in the return main: return header -> (high run) -> the exchanger's CHW inlet (top), down
			// its plate pack to the outlet (bottom), out and up to the coil-lane height and along to the plant return.
			const FVector EN = Econ->Rotation.RotateVector(FVector::ForwardVector);
			const FVector InP = PortM(*Econ, EPlantPort::CoilIn), OutP = PortM(*Econ, EPlantPort::CoilOut);
			SetPoint(H.SeriesHxNode, InP);
			PlainSupplyMain();
			Routes[H.ReturnMainPipe] = ToPort(Bars[H.ReturnHeaderNode].At(YReturnMain), ZRet, InP, EN, 0.3);
			// The outlet run steps sideways (the unit's -Y) before rising, clear of the inlet drop on the same nozzle pair.
			const FVector S1 = OutP + EN * 0.3, S2 = S1 + Econ->Rotation.RotateVector(FVector(0, -0.6, 0));
			Routes[H.SeriesHxPipe] = Line({ InP, OutP, S1, S2, FVector(S2.X, S2.Y, CoilInZ), Bars[H.PlantReturnNode].At(S2.Y) });
		}
		else if (Econ != nullptr && L.Id == EPlantLoopId::Cw)
		{
			// Condenser water side in the supply main: plant supply -> (out of the lane, under the coil lanes) -> the
			// exchanger's CW inlet (bottom), up its plate pack to the outlet (top), out and up to the branch layer and
			// along to the supply header.
			const FVector EN = Econ->Rotation.RotateVector(FVector::ForwardVector);
			const FVector InP = PortM(*Econ, EPlantPort::CondenserIn), OutP = PortM(*Econ, EPlantPort::CondenserOut);
			SetPoint(H.SeriesHxNode, OutP);
			Routes[H.SeriesHxPipe] = Joined(ToPort(Bars[H.PlantSupplyNode].At(L.Y0 - EconLinkGapM), EconLinkZ, InP, EN, 0.3), Line({ InP, OutP }));
			Routes[H.SupplyMainPipe] = Reversed(ToPort(Bars[H.SupplyHeaderNode].At(YSupplyMain), ZSup, OutP, EN, 0.55));
			PlainReturnMain();
		}
		else
		{
			PlainSupplyMain();
			PlainReturnMain();
		}

		// Branches: room consumers (FWS, CHW) or the chillers' condensers (CW).
		TArray<FYardLink> Links;
		// CW branches: duty chillers, duty trim chillers, then the isolated standby chillers and standby trim chillers.
		const int32 NTrimCond = Plant.IsTrimActive() ? FMath::Max(Th.TrimDutyChillers, 1) : 0;
		const int32 FirstStandby = Loop.GetNumBranches() - H.NumStandbyBranches;
		auto BranchUnit = [&](int32 B) -> const FPlantDisplayEquipment*
		{
			if (L.Id != EPlantLoopId::Cw) return nullptr;
			int32 Chw = INDEX_NONE, Trim = INDEX_NONE;
			if (B >= FirstStandby)
			{
				const int32 NChwStandby = NChwCond > 0 ? FMath::Max(Th.StandbyChillers, 0) : 0;
				if (B - FirstStandby < NChwStandby) Chw = NChwCond + (B - FirstStandby);
				else Trim = NTrimCond + (B - FirstStandby - NChwStandby);
			}
			else if (B < NChwCond) Chw = B;
			else Trim = B - NChwCond;
			if (ChillerEq.IsValidIndex(Chw)) return &S.Equipment[ChillerEq[Chw]];
			if (TrimEq.IsValidIndex(Trim)) return &S.Equipment[TrimEq[Trim]];
			return nullptr;
		};
		for (int32 B = 0; B < Loop.GetNumBranches(); ++B)
			if (const FPlantDisplayEquipment* E = BranchUnit(B)) Links.Add(MakeLink(*E, true, B, 0, XT, 100));
		// Coils: each through the sink unit it stands for (coil K of NumCoils -> unit K * Units / NumCoils). One coil per
		// unit: dry coolers (2026-10-22, also the automatic FWS bank) and chillers (duty evaporators, then the isolated
		// standby ones), so coil K is unit K.
		const int32 NumCoils = H.ReturnCoilPipes.Num();
		const TArray<int32>& Sinks = L.Id == EPlantLoopId::Chw ? ChillerEq : L.Id == EPlantLoopId::Fws ? FwsEq : CwEq;
		const int32 CoilUnits = Sinks.Num() > 0 ? Sinks.Num() : L.Units;
		TMap<int32, int32> CoilsOnUnit;
		for (int32 K = 0; K < NumCoils; ++K)
		{
			const int32 U = CoilUnits > 0 ? FMath::Min(K * CoilUnits / FMath::Max(NumCoils, 1), CoilUnits - 1) : 0;
			FPlantDisplayEquipment Fallback;
			Fallback.Kind = EPlantEquipmentKind::DryCooler; Fallback.SizeM = FVector(2.4, 1.2, 1.3);
			Fallback.CenterM = UnitPosition(X0, L, U, 0.65);
			const FPlantDisplayEquipment& E = Sinks.IsValidIndex(U) ? S.Equipment[Sinks[U]] : Fallback;
			int32& On = CoilsOnUnit.FindOrAdd(U);
			Links.Add(MakeLink(E, false, H.ReturnCoilPipes[K], On++, XPR, 200));
		}
		AssignAisleLanes(Links);
		TArray<double> TrunkAvoid;   // lanes leaving the trunk (posts go between them)
		for (const FYardLink& K : Links)
		{
			const double LY = K.LaneY;
			if (K.Group >> 40 == 200)
			{
				Routes[K.Pipe] = Line({ Bars[H.PlantReturnNode].At(LY), FVector(K.In.X, LY, CoilInZ), FVector(K.In.X, LY, K.In.Z), K.In,
					K.Out, FVector(K.Out.X, LY, K.Out.Z), FVector(K.Out.X, LY, CoilOutZ), Bars[H.SuctionNode].At(LY) });
				continue;
			}
			// Condenser branch in its aisle lane: valve drop at the nearer connection, return riser at the farther one.
			const int32 Bi = K.Pipe;
			const int32 A = N.GetPipeToNodeIndex(H.BranchSupplyRunPipes[Bi]);
			const int32 Bn = N.GetPipeToNodeIndex(H.ValvePipes[Bi]);
			const int32 Cn = N.GetPipeFromNodeIndex(H.BranchReturnRunPipes[Bi]);
			const FVector PA(K.In.X, LY, K.In.Z + 0.9), PB(K.In.X, LY, K.In.Z + 0.5), PC(K.Out.X, LY, K.Out.Z + 0.5);
			SetPoint(A, PA); SetPoint(Bn, PB); SetPoint(Cn, PC);
			const int32 SupNode = N.GetPipeFromNodeIndex(H.BranchSupplyRunPipes[Bi]), RetNode = N.GetPipeToNodeIndex(H.BranchReturnRunPipes[Bi]);
			Routes[H.BranchSupplyRunPipes[Bi]] = Line({ Bars[SupNode].At(LY), FVector(PA.X, LY, LayerOf(SupNode)), PA });
			Routes[H.ValvePipes[Bi]] = Line({ PA, PB });
			Routes[H.HxPipes[Bi]] = Line({ PB, FVector(PB.X, LY, K.In.Z), K.In, K.Out, FVector(K.Out.X, LY, K.Out.Z), PC });
			Routes[H.BranchReturnRunPipes[Bi]] = Line({ PC, FVector(PC.X, LY, LayerOf(RetNode)), Bars[RetNode].At(LY) });
			TrunkAvoid.Add(LY);
		}
		for (int32 B = 0; B < Loop.GetNumBranches(); ++B)
		{
			if (BranchUnit(B) != nullptr) continue;
			// Room consumer (or a condenser branch without its chiller): drops over the consumer, supply in the lower
			// layer. The upper-layer pipe ends farther from the trunk, so its riser never cuts the lower run.
			const FRoomBranch* R = RoomBranch(L.Id, B);
			const FVector C = R ? R->C : FVector(XT + 2.0, Yc, 0.0);
			const double LY = R ? R->LaneY : Yc + 0.5 + B * RoomLanePitchM;
			const int32 SupNode = N.GetPipeFromNodeIndex(H.BranchSupplyRunPipes[B]), RetNode = N.GetPipeToNodeIndex(H.BranchReturnRunPipes[B]);
			const double ZS = LayerOf(SupNode), ZR = LayerOf(RetNode);
			const double Toward = XT >= C.X ? 1.0 : -1.0;
			const double Near = C.X + Toward * ConsumerHalfM, Far = C.X - Toward * ConsumerHalfM;
			const double XA = ZS > ZR ? Far : Near, XC = ZS > ZR ? Near : Far;
			const double ZTop = C.Z + ConsumerTopM;
			const FVector PA(XA, LY, ZTop + 0.45), PB(XA, LY, ZTop + 0.15), PC(XC, LY, ZTop + 0.15);
			const int32 A = N.GetPipeToNodeIndex(H.BranchSupplyRunPipes[B]);
			const int32 Bn = N.GetPipeToNodeIndex(H.ValvePipes[B]);
			const int32 Cn = N.GetPipeFromNodeIndex(H.BranchReturnRunPipes[B]);
			SetPoint(A, PA); SetPoint(Bn, PB); SetPoint(Cn, PC);
			Routes[H.BranchSupplyRunPipes[B]] = Line({ Bars[SupNode].At(LY), FVector(XA, LY, ZS), PA });
			Routes[H.ValvePipes[B]] = Line({ PA, PB });
			Routes[H.HxPipes[B]] = Line({ PB, FVector(XA, LY, ZTop), FVector(XA, C.Y, ZTop), FVector(XC, C.Y, ZTop), FVector(XC, LY, ZTop), PC });
			Routes[H.BranchReturnRunPipes[B]] = Line({ PC, FVector(XC, LY, ZR), Bars[RetNode].At(LY) });
			TrunkAvoid.Add(LY);
		}
		// Series: the bypass from the supply to the middle header, past the end of every header (crosses none).
		if (H.BypassPipe != INDEX_NONE)
		{
			double Lo = TNumericLimits<double>::Max();
			for (const auto& Pair : Bars) if (Pair.Value.X == XT && Pair.Value.Ys.Num() > 0) Lo = FMath::Min(Lo, Pair.Value.Lo());
			const double BY = (Lo < TNumericLimits<double>::Max() ? Lo : Yc) - 0.5;
			const int32 From = N.GetPipeFromNodeIndex(H.BypassPipe), To = N.GetPipeToNodeIndex(H.BypassPipe);
			if (Bars.Contains(From) && Bars.Contains(To)) Routes[H.BypassPipe] = Line({ Bars[From].At(BY), Bars[To].At(BY) });
			TrunkAvoid.Add(BY);
		}

		// Headers: bars from their anchors; the suction is the coil collector, down and along to the pump suction header.
		for (auto& Pair : Bars)
		{
			const int32 Node = Pair.Key;
			FBar& B = Pair.Value;
			if (!Pos.IsValidIndex(Node)) continue;
			if (Node == H.SuctionNode) continue;
			if (B.Ys.IsEmpty()) B.Ys.Add(Yc);
			Shapes[Node] = Line({ FVector(B.X, B.Lo(), B.Z), FVector(B.X, B.Hi(), B.Z) });
			Pos[Node] = FVector(B.X, 0.5 * (B.Lo() + B.Hi()), B.Z);
		}
		if (Pos.IsValidIndex(H.SuctionNode))
		{
			FBar& C = Bars[H.SuctionNode];
			if (PumpBar.Ys.IsEmpty()) PumpBar.Ys.Add(Yc);
			double YConn = PumpBar.Lo();
			// Below this loop's coil lanes only (condenser lanes run in the chillers' lane, another loop's ground).
			for (const FYardLink& K : Links) if (K.Group >> 40 == 200) YConn = FMath::Min(YConn, FMath::Min(K.LaneY, K.In.Y));
			YConn = FMath::Min(YConn, YSupplyMain) - 0.4;
			TArray<FVector> Shape;
			if (C.Ys.Num() > 0) { Push(Shape, FVector(C.X, C.Hi(), C.Z)); Push(Shape, FVector(C.X, YConn, C.Z)); Push(Shape, FVector(C.X, YConn, PumpSuctionZ)); }
			Push(Shape, FVector(PumpBar.X, C.Ys.Num() > 0 ? YConn : PumpBar.Lo(), PumpSuctionZ));
			Push(Shape, FVector(PumpBar.X, PumpBar.Hi(), PumpSuctionZ));
			Shapes[H.SuctionNode] = Shape;
			Pos[H.SuctionNode] = FVector(PumpBar.X, 0.5 * (PumpBar.Lo() + PumpBar.Hi()), PumpSuctionZ);
		}

		// Temperatures of the loop's volumes (series: the middle header carries the CDU inlet mix).
		double Ts = St.SupplyK, Tr = St.ReturnK;
		if (L.Id == EPlantLoopId::Chw) { Ts = St.ChwSupplyK; Tr = St.ChwReturnK; }
		else if (L.Id == EPlantLoopId::Cw) { Ts = St.CwSupplyK; Tr = St.CwReturnK; }
		const bool bSeries = H.MiddleNode != INDEX_NONE;
		const double Tmid = bSeries ? Plant.GetSupplyTempK() : Tr;
		for (const auto& Pair : Bars)
		{
			if (!Shapes.IsValidIndex(Pair.Key) || Shapes[Pair.Key].Num() < 2) continue;
			FPlantDisplayHeader& Hd = S.Headers.AddDefaulted_GetRef();
			Hd.Loop = L.Id; Hd.Node = Pair.Key; Hd.PointsM = Shapes[Pair.Key];
			Hd.TempK = Pair.Key == H.ReturnHeaderNode || Pair.Key == H.PlantReturnNode ? Tr : Pair.Key == H.MiddleNode ? Tmid : Ts;
		}

		auto Add = [&](EPlantPipeRole Role, int32 Edge, int32 Branch, int32 From, int32 To, TArray<FVector> Pts, double Flow, double TempK, bool bClosed)
		{
			FPlantDisplayPipe& P = S.Pipes.AddDefaulted_GetRef();
			P.Loop = L.Id; P.Role = Role; P.Edge = Edge; P.Branch = Branch; P.FromNode = From; P.ToNode = To; P.FlowM3s = Flow; P.TempK = TempK;
			// A pipe nothing routed (an unknown role) runs X, then Y, then Z between its node points.
			if (Pts.Num() < 2 && Pos.IsValidIndex(From) && Pos.IsValidIndex(To))
				Pts = Line({ Pos[From], FVector(Pos[To].X, Pos[From].Y, Pos[From].Z), FVector(Pos[To].X, Pos[To].Y, Pos[From].Z), Pos[To] });
			P.PointsM = MoveTemp(Pts);
			P.bClosed = bClosed;
			P.bReversed = Branch != INDEX_NONE && Flow < -1e-12;
			P.bHeader = Role == EPlantPipeRole::SupplyMain || Role == EPlantPipeRole::ReturnMain || Role == EPlantPipeRole::Bypass;
		};
		for (int32 P = 0; P < H.Pumps.Num(); ++P)
		{
			const int32 Pump = H.Pumps[P];
			Add(EPlantPipeRole::Pump, P, INDEX_NONE, N.GetPumpFromNodeIndex(Pump), N.GetPumpToNodeIndex(Pump), PumpRoutes[P],
				Loop.IsPumpRunning(P) ? N.GetPumpFlowM3s(Pump) : 0.0, Ts, !Loop.IsPumpRunning(P));
		}
		// Every network pipe once, with its role.
		TMap<int32, TPair<EPlantPipeRole, int32>> Roles;
		for (int32 P = 0; P < H.PumpCheckPipes.Num(); ++P) Roles.Add(H.PumpCheckPipes[P], TPair<EPlantPipeRole, int32>(EPlantPipeRole::CheckValve, INDEX_NONE));
		Roles.Add(H.SupplyMainPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::SupplyMain, INDEX_NONE));
		Roles.Add(H.ReturnMainPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnMain, INDEX_NONE));
		if (H.BypassPipe != INDEX_NONE) Roles.Add(H.BypassPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::Bypass, INDEX_NONE));
		if (H.SeriesHxPipe != INDEX_NONE) Roles.Add(H.SeriesHxPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::Economizer, INDEX_NONE));
		for (int32 B = 0; B < Loop.GetNumBranches(); ++B)
		{
			Roles.Add(H.BranchSupplyRunPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::SupplyRun, B));
			Roles.Add(H.ValvePipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::Valve, B));
			Roles.Add(H.HxPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::Exchanger, B));
			Roles.Add(H.BranchReturnRunPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnRun, B));
		}
		for (int32 K = 0; K < H.ReturnCoilPipes.Num(); ++K) Roles.Add(H.ReturnCoilPipes[K], TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnCoil, K));
		for (int32 Edge = 0; Edge < N.GetNumPipes(); ++Edge)
		{
			const TPair<EPlantPipeRole, int32>* R = Roles.Find(Edge);
			const EPlantPipeRole Role = R ? R->Key : EPlantPipeRole::Other;
			const int32 Branch = R && Role != EPlantPipeRole::ReturnCoil ? R->Value : INDEX_NONE;
			// Series: the first stage returns into the middle header, the second stage leaves it.
			const bool bFirstStage = Branch != INDEX_NONE && Branch < H.NumFirstStage;
			uint8 V = 0;   // 0 supply, 1 return, 2 series middle (VolumeTempK)
			switch (Role)
			{
			case EPlantPipeRole::Exchanger: case EPlantPipeRole::ReturnRun: V = (bSeries && bFirstStage) ? 2 : 1; break;
			case EPlantPipeRole::SupplyRun: case EPlantPipeRole::Valve: V = (bSeries && !bFirstStage) ? 2 : 0; break;
			case EPlantPipeRole::ReturnMain: case EPlantPipeRole::ReturnCoil: V = 1; break;
			case EPlantPipeRole::Economizer: V = L.Id == EPlantLoopId::Chw ? 1 : 0; break;
			default: V = 0; break;
			}
			const double T = V == 2 ? Tmid : V == 1 ? Tr : Ts;
			Add(Role, Edge, Branch, N.GetPipeFromNodeIndex(Edge), N.GetPipeToNodeIndex(Edge), Routes[Edge],
				N.IsPipeClosed(Edge) ? 0.0 : N.GetPipeFlowM3s(Edge), T, N.IsPipeClosed(Edge));
			S.Pipes.Last().Volume = V;
		}
		for (int32 K = 0; K < Loop.GetNumLeaks(); ++K)
		{
			const FHydraulicLeak& Hole = N.GetLeak(Loop.GetLeak(K).NetworkLeak);
			if (!(Hole.HoleDiameterM > 0.0)) continue;
			FPlantDisplayLeak& Lk = S.Leaks.AddDefaulted_GetRef();
			Lk.Loop = L.Id; Lk.Leak = K; Lk.PositionM = Pos.IsValidIndex(Hole.NodeIndex) ? Pos[Hole.NodeIndex] : FVector::ZeroVector;
			Lk.FlowM3s = Loop.GetLeakFlowM3s(K);
		}

		// Supports (display only): a tray under the trunk headers and under every lane band, posts in the gaps.
		auto Tray = [&](const FVector& Lo, const FVector& Hi) { S.Trays.Add({ L.Id, 0.5 * (Lo + Hi), (Hi - Lo).GetAbs() }); };
		for (const FYardLink& K : Links) YardLanes.Add(FVector2D(K.LaneY, K.Group >> 40 == 200 ? CoilInZ : ZSup));
		if (const FBar* Sup = Bars.Find(H.SupplyHeaderNode))
		{
			const double Bottom = Sup->Z - 0.11;
			Tray(FVector(XT - 0.2, Sup->Lo() - 0.15, Bottom), FVector(XT + 0.2, Sup->Hi() + 0.15, Bottom + 0.03));
			TArray<double> Avoid;
			for (const auto& Pair : Bars) if (Pair.Value.X == XT) Avoid.Append(Pair.Value.Ys);
			if (bRoom)
			{
				// The room loops share one trunk column: their posts are placed once, after the last loop.
				RoomTrunk.X = XT; RoomTrunk.Ys.Append(Avoid); RoomTrunk.Bottoms.Add(Bottom);
				RoomTrunk.Lo = FMath::Min(RoomTrunk.Lo, Sup->Lo()); RoomTrunk.Hi = FMath::Max(RoomTrunk.Hi, Sup->Hi());
			}
			else
			{
				for (const FVector2D& Lane : YardLanes) if (Lane.Y < Bottom) Avoid.Add(Lane.X);
				for (const double PY : PostSpots(Sup->Lo() - 0.1, Sup->Hi() + 0.1, Avoid, 4.0))
				{
					S.PostTopsM.Add(FVector(XT + 0.3, PY, Bottom));
					Tray(FVector(XT - 0.2, PY - 0.04, Bottom - 0.06), FVector(XT + 0.34, PY + 0.04, Bottom));
				}
			}
		}
		TMap<int64, TArray<const FYardLink*>> Aisles;
		for (const FYardLink& K : Links) Aisles.FindOrAdd(K.Group).Add(&K);
		for (const auto& A : Aisles)
		{
			const bool bCoil = A.Key >> 40 == 200;
			double Lo = TNumericLimits<double>::Max(), Hi = -Lo, X1 = -TNumericLimits<double>::Max();
			TArray<double> Xs;
			for (const FYardLink* K : A.Value) { Lo = FMath::Min(Lo, K->LaneY); Hi = FMath::Max(Hi, K->LaneY); X1 = FMath::Max(X1, K->Out.X); Xs.Add(K->In.X); Xs.Add(K->Out.X); }
			const double X = bCoil ? XPR : XT, Bottom = (bCoil ? CoilInZ : ZSup) - 0.11;
			if (X1 <= X) continue;   // placed units behind the header column: no tray
			Tray(FVector(X - 0.1, Lo - 0.1, Bottom), FVector(X1 + 0.4, Hi + 0.1, Bottom + 0.03));
			const double MidY = 0.5 * (Lo + Hi);
			bool bClear = true;   // a post must not rise through another aisle's lane below this tray
			for (const FVector2D& Lane : YardLanes) bClear &= (Lane.X >= Lo - 1e-6 && Lane.X <= Hi + 1e-6) || Lane.Y >= Bottom || FMath::Abs(Lane.X - MidY) > 0.15;
			if (!bClear) continue;
			for (const double PX : PostSpots(X + 0.05, X1 + 0.35, Xs, 4.0))
			{
				S.PostTopsM.Add(FVector(PX, MidY, Bottom));
				Tray(FVector(PX - 0.04, Lo - 0.1, Bottom - 0.06), FVector(PX + 0.04, Hi + 0.1, Bottom));
			}
		}
		// Trays under the room lanes of this loop (hung from the ceiling: no posts).
		TMap<int32, FBox> RoomBands;
		for (const FRoomBranch& R : RoomBranches)
		{
			if (R.Loop != L.Id) continue;
			const int32 Key = FMath::RoundToInt(R.LaneY / 2.0);
			FBox& Band = RoomBands.FindOrAdd(Key, FBox(ForceInit));
			Band += FVector(R.C.X - ConsumerHalfM - 0.1, R.LaneY - 0.12, 0.0);
			Band += FVector(XT - 0.2, R.LaneY + 0.12, 0.0);
		}
		for (const auto& Band : RoomBands)
			Tray(FVector(Band.Value.Min.X, Band.Value.Min.Y, ZSup - 0.11), FVector(Band.Value.Max.X, Band.Value.Max.Y, ZSup - 0.08));

		Hash = HashCombine(Hash, GetTypeHash(N.GetNumPipes()));
		Hash = HashCombine(Hash, GetTypeHash(N.GetNumNodes()));
	}
	if (RoomTrunk.Bottoms.Num() > 0)
	{
		double Top = 0.0;
		for (const double B : RoomTrunk.Bottoms) Top = FMath::Max(Top, B);
		for (const double PY : PostSpots(RoomTrunk.Lo - 0.1, RoomTrunk.Hi + 0.1, RoomTrunk.Ys, 4.0))
		{
			S.PostTopsM.Add(FVector(RoomTrunk.X + 0.3, PY, Top));
			for (const double B : RoomTrunk.Bottoms)
				S.Trays.Add({ EPlantLoopId::Fws, FVector(RoomTrunk.X + 0.07, PY, B - 0.03), FVector(0.54, 0.08, 0.06) });
		}
	}
	for (int32 Id = 0; Id < 3; ++Id)
		for (int32 Node = 0; Node < S.NodeShapesM[Id].Num(); ++Node)
			if (S.NodeShapesM[Id][Node].IsEmpty()) S.NodeShapesM[Id][Node] = { S.NodePositionsM[Id][Node] };
	Hash = HashCombine(Hash, GetTypeHash(S.Equipment.Num()));
	Hash = HashCombine(Hash, GetTypeHash(int32(S.Equipment.FilterByPredicate([](const FPlantDisplayEquipment& E) { return E.bPlaced; }).Num())));
	Hash = HashCombine(Hash, GetTypeHash(S.Leaks.Num()));
	Hash = HashCombine(Hash, GetTypeHash(In.CduPositionsM.Num()));
	Hash = HashCombine(Hash, GetTypeHash(In.CrahPositionsM.Num()));
	S.StructureHash = Hash;
	return S;
}

namespace
{
	FLinearColor LoopHue(EPlantLoopId Loop)
	{
		return Loop == EPlantLoopId::Fws ? FLinearColor(1.f, .45f, .1f) : Loop == EPlantLoopId::Chw ? FLinearColor(.15f, .45f, 1.f) : FLinearColor(.2f, .85f, .3f);
	}
}

FLinearColor PlantDisplay::PipeColor(const FPlantDisplaySnapshot& S, const FPlantDisplayPipe& P, bool bTemperatureMode)
{
	// Structure only (plant off or layout mode): the loop's colour, no flow or temperature claim.
	if (!S.bLive) return LoopHue(P.Loop) * 0.6f;
	if (P.bReversed) return FLinearColor(0.9f, 0.1f, 0.9f);
	if (P.bClosed || FMath::Abs(P.FlowM3s) < 1e-9) return PipeTemperatureDisplay::UnknownColor();
	if (bTemperatureMode) return PipeTemperatureDisplay::Color(P.TempK, P.TempK > 0.0);
	double MaxFlow = 0.0;
	for (const FPlantDisplayPipe& Q : S.Pipes) if (Q.Loop == P.Loop) MaxFlow = FMath::Max(MaxFlow, FMath::Abs(Q.FlowM3s));
	// Brightness 0.35..1 by |flow| (dim pipes stay visible); hue by loop: FWS orange, CHW blue, CW green.
	const float B = 0.35f + 0.65f * static_cast<float>(MaxFlow > 0.0 ? FMath::Clamp(FMath::Abs(P.FlowM3s) / MaxFlow, 0.0, 1.0) : 0.0);
	return LoopHue(P.Loop) * B;
}

FLinearColor PlantDisplay::HeaderColor(const FPlantDisplaySnapshot& S, const FPlantDisplayHeader& H, bool bTemperatureMode)
{
	if (!S.bLive) return LoopHue(H.Loop) * 0.75f;
	if (bTemperatureMode) return PipeTemperatureDisplay::Color(H.TempK, H.TempK > 0.0);
	return LoopHue(H.Loop) * 0.85f;
}

FLinearColor PlantDisplay::KindColor(EPlantLoopId Loop, EPlantEquipmentKind Kind)
{
	switch (Kind)
	{
	case EPlantEquipmentKind::Chiller: return FLinearColor(0.2f, 0.5f, 0.9f);
	case EPlantEquipmentKind::TrimChiller: return FLinearColor(0.55f, 0.4f, 0.85f);
	case EPlantEquipmentKind::Economizer: return FLinearColor(0.3f, 0.8f, 0.6f);
	default: return LoopHue(Loop);
	}
}

FLinearColor PlantDisplay::EquipmentColor(const FPlantDisplayEquipment& E)
{
	if (E.bFailed) return FLinearColor(0.85f, 0.05f, 0.05f);
	if (!E.bRunning) return FLinearColor(0.3f, 0.3f, 0.3f);
	return KindColor(E.Loop, E.Kind);
}

TArray<FPlantDisplayClash> PlantDisplay::FindClashes(const FPlantDisplaySnapshot& S)
{
	using namespace PlantDisplayStyle;
	struct FItem { const TArray<FVector>* Pts; EPlantLoopId Loop; int32 From, To; double Dia; bool bHeader; int32 Index; };
	TArray<FItem> Items;
	for (int32 I = 0; I < S.Pipes.Num(); ++I)
	{
		const FPlantDisplayPipe& P = S.Pipes[I];
		Items.Add({ &P.PointsM, P.Loop, P.FromNode, P.ToNode, P.bHeader ? MainDiameterM : PipeDiameterM, false, I });
	}
	for (int32 I = 0; I < S.Headers.Num(); ++I)
		Items.Add({ &S.Headers[I].PointsM, S.Headers[I].Loop, S.Headers[I].Node, S.Headers[I].Node, HeaderDiameterM, true, I });
	TArray<FPlantDisplayClash> Out;
	for (int32 A = 0; A < Items.Num(); ++A)
		for (int32 B = A + 1; B < Items.Num(); ++B)
		{
			const FItem& X = Items[A];
			const FItem& Yi = Items[B];
			// Segments that end at a node the other item also touches meet there by design.
			auto Skip = [](const FItem& Self, const FItem& Other, int32 Seg)
			{
				if (Self.bHeader) return false;
				const bool bShareFrom = Self.Loop == Other.Loop && (Self.From == Other.From || Self.From == Other.To);
				const bool bShareTo = Self.Loop == Other.Loop && (Self.To == Other.From || Self.To == Other.To);
				return (Seg == 0 && bShareFrom) || (Seg == Self.Pts->Num() - 2 && bShareTo);
			};
			const double Touch = 0.5 * (X.Dia + Yi.Dia);
			double Worst = TNumericLimits<double>::Max();
			for (int32 I = 0; I + 1 < X.Pts->Num(); ++I)
			{
				if (Skip(X, Yi, I)) continue;
				for (int32 J = 0; J + 1 < Yi.Pts->Num(); ++J)
				{
					if (Skip(Yi, X, J)) continue;
					Worst = FMath::Min(Worst, SegmentGap((*X.Pts)[I], (*X.Pts)[I + 1], (*Yi.Pts)[J], (*Yi.Pts)[J + 1]) - Touch);
				}
			}
			if (Worst < -1e-9) Out.Add({ X.Index, Yi.Index, X.bHeader, Yi.bHeader, Worst });
		}
	return Out;
}
