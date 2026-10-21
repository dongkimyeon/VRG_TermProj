// Fill out your copyright notice in the Description page of Project Settings.

#include "Visualization/PlantDisplay.h"
#include "Visualization/PipeTemperatureDisplay.h"

namespace
{
	// Schematic sizes [m] (not equipment dimensions from a catalog).
	constexpr double LaneGapM = 2.0, UnitPitchM = 2.8, PumpPitchM = 0.9, HeaderZM = 3.0;
	constexpr int32 UnitColumns = 6;

	struct FPlantLane
	{
		EPlantLoopId Id = EPlantLoopId::Fws;
		const FFacilityWaterLoop* Loop = nullptr;
		double Y0 = 0.0, Width = 0.0;
		int32 Units = 0;                   // sink units drawn in the lane (dry coolers / chillers)
		int32 ExtraUnits = 0;              // trim chillers (FWS lane) after the dry coolers
	};

	FVector UnitPosition(double X0, const FPlantLane& L, int32 K)
	{
		return FVector(X0 + 6.0 + (K % UnitColumns) * UnitPitchM, L.Y0 + 1.5 + (K / UnitColumns) * UnitPitchM, 0.7);
	}
}

FPlantDisplaySnapshot PlantDisplay::Build(const FFacilityPlant& Plant, const FPlantDisplayInput& In)
{
	FPlantDisplaySnapshot S;
	if (!Plant.IsBuilt()) return S;
	const FFacilityThermalSpec& Th = Plant.GetThermalSpec();
	const FFacilityPlantState& St = Plant.GetState();
	const FBox Room = In.RoomM.IsValid ? In.RoomM : FBox(FVector(-5, -5, 0), FVector(5, 5, 3));
	const double X0 = Room.Max.X + 3.0;
	const int32 NChw = FMath::Max(Th.DutyChillers, 1) + FMath::Max(Th.StandbyChillers, 0);
	const int32 NTrim = Plant.IsTrimActive() ? FMath::Max(Th.TrimDutyChillers, 1) + FMath::Max(Th.TrimStandbyChillers, 0) : 0;

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
	auto LaneOf = [&](EPlantLoopId Id) -> const FPlantLane* { for (const FPlantLane& L : Lanes) if (L.Id == Id) return &L; return nullptr; };
	const FPlantLane* ChwLane = LaneOf(EPlantLoopId::Chw);
	const FPlantLane* FwsLane = LaneOf(EPlantLoopId::Fws);

	// F8: a kind whose placed units match the model's count is drawn at those units.
	auto Placed = [](const TArray<FPlantDisplayUnit>& Units, int32 Count) { return Count > 0 && Units.Num() == Count ? &Units : nullptr; };
	auto Place = [](FPlantDisplayEquipment& E, const TArray<FPlantDisplayUnit>* Units, int32 K)
	{
		if (Units == nullptr) return;
		const FPlantDisplayUnit& U = (*Units)[K];
		E.CenterM = U.CenterM; E.SizeM = U.SizeM; E.Rotation = U.Rotation; E.bPlaced = true;
	};

	// Equipment of the lanes (built first: condenser and evaporator branches run to these boxes).
	TArray<FVector> ChillerPos, TrimPos, FwsUnitPos, CwUnitPos;
	for (const FPlantLane& L : Lanes)
	{
		const FFacilityLoopHandles& H = L.Loop->GetHandles();
		for (int32 P = 0; P < H.Pumps.Num(); ++P)
		{
			FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
			E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Pump; E.Index = P;
			E.CenterM = FVector(X0 + 1.0, L.Y0 + 1.0 + P * PumpPitchM, 0.3); E.SizeM = FVector(0.6, 0.6, 0.6);
			E.bRunning = L.Loop->IsPumpRunning(P); E.bFailed = L.Loop->IsPumpTripped(P);
		}
		if (L.Id == EPlantLoopId::Chw)
		{
			const TArray<FPlantDisplayUnit>* Units = Placed(In.Chillers, L.Units);
			for (int32 K = 0; K < L.Units; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Chiller; E.Index = K;
				E.CenterM = UnitPosition(X0, L, K) + FVector(0, 0, 0.2); E.SizeM = FVector(2.4, 1.2, 1.8);
				Place(E, Units, K);
				// Healthy units first: the running count takes the lowest healthy indices, failed ones are the last.
				const int32 Healthy = NChw - St.TrippedChillers;
				E.bFailed = K >= Healthy; E.bRunning = !E.bFailed && K < St.ChillersRunning;
				ChillerPos.Add(E.CenterM);
			}
			if (Plant.GetEconomizerUAWK() > 0.0)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::Economizer;
				E.CenterM = UnitPosition(X0, L, L.Units) + FVector(0, 0, 0.1); E.SizeM = FVector(0.8, 0.5, 1.6);
				E.bRunning = St.bEconomizerOn;
			}
		}
		else
		{
			const int32 Tripped = Plant.GetTrippedDryCoolers(L.Id);
			const TArray<FPlantDisplayUnit>* Units = Placed(L.Id == EPlantLoopId::Fws ? In.FwsDryCoolers : In.CwDryCoolers, L.Units);
			TArray<FVector>& UnitPos = L.Id == EPlantLoopId::Fws ? FwsUnitPos : CwUnitPos;
			for (int32 K = 0; K < L.Units; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::DryCooler; E.Index = K;
				E.CenterM = UnitPosition(X0, L, K); E.SizeM = FVector(2.4, 1.2, 1.3);
				Place(E, Units, K);
				E.bFailed = K >= L.Units - Tripped;
				E.bRunning = !E.bFailed && (L.Id == EPlantLoopId::Fws ? St.FanRatio : St.CwDryCoolerHeatW > 0.0 ? 1.0 : 0.0) > 0.0;
				UnitPos.Add(E.CenterM);
			}
			const TArray<FPlantDisplayUnit>* Trims = Placed(In.TrimChillers, L.ExtraUnits);
			for (int32 K = 0; K < L.ExtraUnits; ++K)
			{
				FPlantDisplayEquipment& E = S.Equipment.AddDefaulted_GetRef();
				E.Loop = L.Id; E.Kind = EPlantEquipmentKind::TrimChiller; E.Index = K;
				E.CenterM = UnitPosition(X0, L, L.Units + K) + FVector(0, 0, 0.2); E.SizeM = FVector(2.4, 1.2, 1.8);
				Place(E, Trims, K);
				const int32 Healthy = NTrim - St.TrippedTrimChillers;
				E.bFailed = K >= Healthy; E.bRunning = !E.bFailed && K < St.TrimRunning;
				TrimPos.Add(E.CenterM);
			}
		}
	}

	// Network nodes and pipes of every lane.
	uint32 Hash = GetTypeHash(int32(Plant.GetTopology()));
	for (const FPlantLane& L : Lanes)
	{
		const FFacilityWaterLoop& Loop = *L.Loop;
		const FHydraulicNetwork& N = Loop.GetNetwork();
		const FFacilityLoopHandles& H = Loop.GetHandles();
		TArray<FVector>& Pos = S.NodePositionsM[uint8(L.Id)];
		Pos.Init(FVector(X0, L.Y0, 0.6), N.GetNumNodes());
		const double Yc = L.Y0 + 0.5 * L.Width;
		auto SetPos = [&](int32 Node, const FVector& P) { if (Pos.IsValidIndex(Node)) Pos[Node] = P; };
		SetPos(H.SuctionNode, FVector(X0, L.Y0 + 1.0, 0.6));
		SetPos(H.PlantSupplyNode, FVector(X0 + 2.2, Yc - 0.4, 1.2));
		SetPos(H.PlantReturnNode, FVector(X0 + 2.2, Yc + 0.4, 1.8));
		SetPos(H.SupplyHeaderNode, FVector(Room.Max.X + 0.6, Yc - 0.4, HeaderZM));
		SetPos(H.ReturnHeaderNode, FVector(Room.Max.X + 0.6, Yc + 0.4, HeaderZM + 0.3));
		SetPos(H.MiddleNode, FVector(Room.Max.X + 1.2, Yc, HeaderZM + 0.6));
		for (int32 P = 0; P < H.Pumps.Num(); ++P) SetPos(N.GetPumpToNodeIndex(H.Pumps[P]), FVector(X0 + 1.0, L.Y0 + 1.0 + P * PumpPitchM, 0.8));
		// Branch consumers.
		const int32 NumCrah = L.Id == EPlantLoopId::Chw ? In.CrahPositionsM.Num() : 0;
		const int32 NChwCond = Plant.IsChilledWaterActive() ? FMath::Max(Th.DutyChillers, 1) : 0;
		for (int32 B = 0; B < Loop.GetNumBranches(); ++B)
		{
			FVector C = FVector(X0 + 4.0, Yc, 1.0);
			if (L.Id == EPlantLoopId::Fws) { if (In.CduPositionsM.IsValidIndex(B)) C = In.CduPositionsM[B]; }
			else if (L.Id == EPlantLoopId::Chw)
			{
				if (B < NumCrah) C = In.CrahPositionsM[B];
				else if (In.CduPositionsM.IsValidIndex(B - NumCrah)) C = In.CduPositionsM[B - NumCrah];
			}
			else
			{
				// Condensers: the CHW chillers' first, then the trim chillers' (CW branch order of BuildCondenser).
				if (B < NChwCond && ChillerPos.IsValidIndex(B)) C = ChillerPos[B];
				else if (TrimPos.IsValidIndex(B - NChwCond)) C = TrimPos[B - NChwCond];
			}
			const int32 A = N.GetPipeToNodeIndex(H.BranchSupplyRunPipes[B]);
			const int32 Bn = N.GetPipeToNodeIndex(H.ValvePipes[B]);
			const int32 Cn = N.GetPipeFromNodeIndex(H.BranchReturnRunPipes[B]);
			SetPos(A, C + FVector(0, -0.3, 2.6));
			SetPos(Bn, C + FVector(0, -0.3, 1.9));
			SetPos(Cn, C + FVector(0, 0.3, 2.6));
		}

		// Temperatures of the loop's volumes (series: the middle header carries the CDU inlet mix).
		double Ts = St.SupplyK, Tr = St.ReturnK;
		if (L.Id == EPlantLoopId::Chw) { Ts = St.ChwSupplyK; Tr = St.ChwReturnK; }
		else if (L.Id == EPlantLoopId::Cw) { Ts = St.CwSupplyK; Tr = St.CwReturnK; }
		const bool bSeries = H.MiddleNode != INDEX_NONE;
		const double Tmid = bSeries ? Plant.GetSupplyTempK() : Tr;

		auto Add = [&](EPlantPipeRole Role, int32 Edge, int32 Branch, const TArray<FVector>& Pts, double Flow, double TempK, bool bClosed)
		{
			FPlantDisplayPipe& P = S.Pipes.AddDefaulted_GetRef();
			P.Loop = L.Id; P.Role = Role; P.Edge = Edge; P.Branch = Branch; P.PointsM = Pts; P.FlowM3s = Flow; P.TempK = TempK;
			P.bClosed = bClosed;
			P.bReversed = Branch != INDEX_NONE && Flow < -1e-12;
			P.bHeader = Role == EPlantPipeRole::SupplyMain || Role == EPlantPipeRole::ReturnMain || Role == EPlantPipeRole::Bypass;
		};
		for (int32 P = 0; P < H.Pumps.Num(); ++P)
		{
			const int32 Pump = H.Pumps[P];
			Add(EPlantPipeRole::Pump, P, INDEX_NONE, { Pos[N.GetPumpFromNodeIndex(Pump)], Pos[N.GetPumpToNodeIndex(Pump)] },
				Loop.IsPumpRunning(P) ? N.GetPumpFlowM3s(Pump) : 0.0, Ts, !Loop.IsPumpRunning(P));
		}
		// Every network pipe once, with its role.
		TMap<int32, TPair<EPlantPipeRole, int32>> Roles;
		for (int32 P = 0; P < H.PumpCheckPipes.Num(); ++P) Roles.Add(H.PumpCheckPipes[P], TPair<EPlantPipeRole, int32>(EPlantPipeRole::CheckValve, INDEX_NONE));
		Roles.Add(H.SupplyMainPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::SupplyMain, INDEX_NONE));
		Roles.Add(H.ReturnMainPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnMain, INDEX_NONE));
		if (H.BypassPipe != INDEX_NONE) Roles.Add(H.BypassPipe, TPair<EPlantPipeRole, int32>(EPlantPipeRole::Bypass, INDEX_NONE));
		for (int32 B = 0; B < Loop.GetNumBranches(); ++B)
		{
			Roles.Add(H.BranchSupplyRunPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::SupplyRun, B));
			Roles.Add(H.ValvePipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::Valve, B));
			Roles.Add(H.HxPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::Exchanger, B));
			Roles.Add(H.BranchReturnRunPipes[B], TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnRun, B));
		}
		for (int32 K = 0; K < H.ReturnCoilPipes.Num(); ++K) Roles.Add(H.ReturnCoilPipes[K], TPair<EPlantPipeRole, int32>(EPlantPipeRole::ReturnCoil, K));
		const int32 NumCoils = H.ReturnCoilPipes.Num();
		for (int32 Edge = 0; Edge < N.GetNumPipes(); ++Edge)
		{
			const TPair<EPlantPipeRole, int32>* R = Roles.Find(Edge);
			const EPlantPipeRole Role = R ? R->Key : EPlantPipeRole::Other;
			const int32 Branch = R && Role != EPlantPipeRole::ReturnCoil ? R->Value : INDEX_NONE;
			TArray<FVector> Pts = { Pos[N.GetPipeFromNodeIndex(Edge)], Pos[N.GetPipeToNodeIndex(Edge)] };
			if (Role == EPlantPipeRole::ReturnCoil)
			{
				// Through the sink unit it stands for: coil K of NumCoils -> unit K * Units / NumCoils (the FWS hydraulic
				// coils are counted by flow and the thermal units by heat, so the two counts can differ: limitation).
				const int32 Units = L.Id == EPlantLoopId::Chw ? FMath::Max(Th.DutyChillers, 1) : L.Units;
				const int32 U = Units > 0 ? FMath::Min(R->Value * Units / FMath::Max(NumCoils, 1), Units - 1) : 0;
				const TArray<FVector>& Sinks = L.Id == EPlantLoopId::Chw ? ChillerPos : L.Id == EPlantLoopId::Fws ? FwsUnitPos : CwUnitPos;
				const FVector Via = Sinks.IsValidIndex(U) ? Sinks[U] : UnitPosition(X0, L, U);
				Pts.Insert(Via + FVector(0, 0, 0.9), 1);
			}
			// Series: the first stage returns into the middle header, the second stage leaves it.
			const bool bFirstStage = Branch != INDEX_NONE && Branch < H.NumFirstStage;
			double T = Ts;
			switch (Role)
			{
			case EPlantPipeRole::Exchanger: case EPlantPipeRole::ReturnRun: T = (bSeries && bFirstStage) ? Tmid : Tr; break;
			case EPlantPipeRole::SupplyRun: case EPlantPipeRole::Valve: T = (bSeries && !bFirstStage) ? Tmid : Ts; break;
			case EPlantPipeRole::ReturnMain: case EPlantPipeRole::ReturnCoil: T = Tr; break;
			default: T = Ts; break;
			}
			Add(Role, Edge, Branch, Pts, N.IsPipeClosed(Edge) ? 0.0 : N.GetPipeFlowM3s(Edge), T, N.IsPipeClosed(Edge));
		}
		for (int32 K = 0; K < Loop.GetNumLeaks(); ++K)
		{
			const FHydraulicLeak& Hole = N.GetLeak(Loop.GetLeak(K).NetworkLeak);
			if (!(Hole.HoleDiameterM > 0.0)) continue;
			FPlantDisplayLeak& Lk = S.Leaks.AddDefaulted_GetRef();
			Lk.Loop = L.Id; Lk.Leak = K; Lk.PositionM = Pos.IsValidIndex(Hole.NodeIndex) ? Pos[Hole.NodeIndex] : FVector::ZeroVector;
			Lk.FlowM3s = Loop.GetLeakFlowM3s(K);
		}
		Hash = HashCombine(Hash, GetTypeHash(N.GetNumPipes()));
		Hash = HashCombine(Hash, GetTypeHash(N.GetNumNodes()));
	}
	Hash = HashCombine(Hash, GetTypeHash(S.Equipment.Num()));
	Hash = HashCombine(Hash, GetTypeHash(int32(S.Equipment.FilterByPredicate([](const FPlantDisplayEquipment& E) { return E.bPlaced; }).Num())));
	Hash = HashCombine(Hash, GetTypeHash(S.Leaks.Num()));
	Hash = HashCombine(Hash, GetTypeHash(In.CduPositionsM.Num()));
	Hash = HashCombine(Hash, GetTypeHash(In.CrahPositionsM.Num()));
	S.StructureHash = Hash;
	return S;
}

FLinearColor PlantDisplay::PipeColor(const FPlantDisplaySnapshot& S, const FPlantDisplayPipe& P, bool bTemperatureMode)
{
	if (P.bReversed) return FLinearColor(0.9f, 0.1f, 0.9f);
	if (P.bClosed || FMath::Abs(P.FlowM3s) < 1e-9) return PipeTemperatureDisplay::UnknownColor();
	if (bTemperatureMode) return PipeTemperatureDisplay::Color(P.TempK, P.TempK > 0.0);
	double MaxFlow = 0.0;
	for (const FPlantDisplayPipe& Q : S.Pipes) if (Q.Loop == P.Loop) MaxFlow = FMath::Max(MaxFlow, FMath::Abs(Q.FlowM3s));
	// Brightness 0.35..1 by |flow| (dim pipes stay visible); hue by loop: FWS orange, CHW blue, CW green.
	const float B = 0.35f + 0.65f * static_cast<float>(MaxFlow > 0.0 ? FMath::Clamp(FMath::Abs(P.FlowM3s) / MaxFlow, 0.0, 1.0) : 0.0);
	const FLinearColor Hue = P.Loop == EPlantLoopId::Fws ? FLinearColor(1.f, .45f, .1f) : P.Loop == EPlantLoopId::Chw ? FLinearColor(.15f, .45f, 1.f) : FLinearColor(.2f, .85f, .3f);
	return Hue * B;
}

FLinearColor PlantDisplay::EquipmentColor(const FPlantDisplayEquipment& E)
{
	if (E.bFailed) return FLinearColor(0.85f, 0.05f, 0.05f);
	if (!E.bRunning) return FLinearColor(0.3f, 0.3f, 0.3f);
	switch (E.Kind)
	{
	case EPlantEquipmentKind::Pump: return FLinearColor(0.9f, 0.9f, 0.9f);
	case EPlantEquipmentKind::DryCooler: return FLinearColor(0.55f, 0.75f, 0.85f);
	case EPlantEquipmentKind::Chiller: case EPlantEquipmentKind::TrimChiller: return FLinearColor(0.2f, 0.5f, 0.9f);
	default: return FLinearColor(0.3f, 0.8f, 0.6f);
	}
}
