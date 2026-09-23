#include "Visualization/PatchManager.h"

namespace
{
	using namespace Patch;
	struct FLane { double Lat, Layer; };

	FLane LaneOf(const FInput& In, int32 L)
	{
		const int32 W = FMath::Max(1, In.LanesPerLayer);
		return {((L % W) - (W - 1) * .5) * In.LanePitchM, (L / W) * In.LanePitchM};
	}
	double AlongOf(const FTrayRow& R, const FVector& P) { return FVector::DotProduct(P - R.OriginM, R.Along); }
	FVector TrayPoint(const FTrayRow& R, double A, const FLane& L)
	{
		return R.OriginM + R.Along * A + R.Out * L.Lat + FVector(0, 0, L.Layer);
	}
	// Port -> tail -> straight out to the cable's lane -> up into the tray.
	void Leave(TArray<FVector>& Pts, const FTrayRow& R, const FVector& Port, const FVector& Out, const FLane& L, double Tail)
	{
		const FVector T = Port + Out * Tail, Top = TrayPoint(R, AlongOf(R, T), L);
		Pts.Append({Port, T, FVector(Top.X, Top.Y, T.Z), Top});
	}
	void Arrive(TArray<FVector>& Pts, const FTrayRow& R, const FVector& Port, const FVector& Out, const FLane& L, double Tail)
	{
		const FVector T = Port + Out * Tail, Top = TrayPoint(R, AlongOf(R, T), L);
		Pts.Append({Top, FVector(Top.X, Top.Y, T.Z), T, Port});
	}
	FVector Location(const FDevice& D) { return D.ToWorld.GetLocation(); }
}

bool Patch::FPortLedger::Claim(int32 Device, int32 Port)
{
	bool bAlready = false;
	Claimed.Add(Key(Device, Port), &bAlready);
	Rejected += bAlready ? 1 : 0;
	return !bAlready;
}

int32 Patch::FPlan::NumUplinks() const
{
	int32 N = 0;
	for (const FCable& C : Cables) N += C.bSpineToSpine ? 0 : 1;
	return N;
}

FString Patch::FPlan::Summary() const
{
	FString S = FString::Printf(TEXT("[패치] 케이블 %d (업링크 %d, 스파인 간 %d) | 중복 점유 거부 %d"),
		Cables.Num(), NumUplinks(), Cables.Num() - NumUplinks(), RejectedClaims);
	if (!Unconnected.IsEmpty()) S += FString::Printf(TEXT(" | 미연결 %d: %s"), Unconnected.Num(), *Unconnected[0].Reason);
	return S + TEXT(" | 표시용 배선 계획이며 계산에 쓰이지 않음");
}

Patch::FPlan Patch::PlanPatches(const FInput& In)
{
	FPlan Plan;
	FPortLedger Ledger;
	const int32 NumRows = In.Rows.Num();
	TArray<int32> LaneCount; LaneCount.SetNumZeroed(NumRows);
	TArray<TArray<int32>> Racks, Spines; Racks.SetNum(NumRows); Spines.SetNum(NumRows);
	for (int32 I = 0; I < In.Devices.Num(); ++I)
	{
		const FDevice& D = In.Devices[I];
		if (!In.Rows.IsValidIndex(D.Row)) { if (!D.bSpine) Plan.Unconnected.Add({I, TEXT("줄에 속하지 않음")}); continue; }
		(D.bSpine ? Spines : Racks)[D.Row].Add(I);
	}
	for (int32 R = 0; R < NumRows; ++R)
	{
		auto ByAlong = [&](int32 A, int32 B)
		{ return AlongOf(In.Rows[R], Location(In.Devices[A])) < AlongOf(In.Rows[R], Location(In.Devices[B])); };
		Racks[R].Sort(ByAlong); Spines[R].Sort(ByAlong);
	}
	auto NextSpinePort = [&](int32 Row, int32& OutDev, int32& OutPort)
	{
		for (int32 S : Spines[Row])
			for (int32 P = 0; P < In.Devices[S].PortM.Num(); ++P)
				if (!Ledger.IsClaimed(S, P)) { OutDev = S; OutPort = P; return true; }
		return false;
	};
	auto Route = [&](int32 FromDev, int32 FromPort, int32 ToDev, int32 ToPort, int32 RowA, int32 RowB, const FVector* Corner)
	{
		const FDevice& A = In.Devices[FromDev]; const FDevice& B = In.Devices[ToDev];
		FCable C{FromDev, FromPort, ToDev, ToPort, RowA != RowB};
		const FLane LA = LaneOf(In, LaneCount[RowA]++);
		const FLane LB = RowA == RowB ? LA : LaneOf(In, LaneCount[RowB]++);
		Leave(C.PointsM, In.Rows[RowA], A.PortM[FromPort], A.Outward, LA, In.PlugTailM);
		if (Corner)  // along row A to the loop end, across, then along row B
		{
			C.PointsM.Add(TrayPoint(In.Rows[RowA], AlongOf(In.Rows[RowA], Corner[0]), LA));
			C.PointsM.Add(TrayPoint(In.Rows[RowB], AlongOf(In.Rows[RowB], Corner[1]), LB));
		}
		Arrive(C.PointsM, In.Rows[RowB], B.PortM[ToPort], B.Outward, LB, In.PlugTailM);
		Plan.Cables.Add(MoveTemp(C));
	};

	// 1. Rack uplinks to the spine cabinet(s) of the same row.
	for (int32 R = 0; R < NumRows; ++R)
		for (int32 Rack : Racks[R])
		{
			if (Spines[R].IsEmpty()) { Plan.Unconnected.Add({Rack, TEXT("줄에 스파인 캐비닛 없음")}); continue; }
			for (int32 Up : In.Devices[Rack].UplinkPorts)
			{
				int32 SD, SP;
				if (!NextSpinePort(R, SD, SP)) { Plan.Unconnected.Add({Rack, TEXT("스파인 포트 부족")}); break; }
				if (!In.Devices[Rack].PortM.IsValidIndex(Up) || !Ledger.Claim(Rack, Up)) continue;
				Ledger.Claim(SD, SP);
				Route(Rack, Up, SD, SP, R, R, nullptr);
			}
		}

	// 2. Tray runs. Parallel neighbouring rows share common end lines so the trays close into a loop.
	TArray<double> Lo, Hi; TArray<FVector> Axis;
	for (int32 R = 0; R < NumRows; ++R)
	{
		const FVector RowAxis = R > 0 && FMath::Abs(FVector::DotProduct(In.Rows[R].Along, Axis[R - 1])) > .99 ? Axis[R - 1] : In.Rows[R].Along;
		Axis.Add(RowAxis);  // a copy: Add() must not read from the array it grows
		double L = TNumericLimits<double>::Max(), H = -L;
		for (int32 D : Racks[R]) { const double X = FVector::DotProduct(Location(In.Devices[D]), Axis[R]); L = FMath::Min(L, X); H = FMath::Max(H, X); }
		for (int32 D : Spines[R]) { const double X = FVector::DotProduct(Location(In.Devices[D]), Axis[R]); L = FMath::Min(L, X); H = FMath::Max(H, X); }
		Lo.Add(L - In.EndMarginM); Hi.Add(H + In.EndMarginM);
	}
	for (int32 R = 1; R < NumRows; ++R)
		if (Axis[R] == Axis[R - 1] && Lo[R] < Hi[R] && Lo[R - 1] < Hi[R - 1])
		{
			Lo[R] = Lo[R - 1] = FMath::Min(Lo[R], Lo[R - 1]);
			Hi[R] = Hi[R - 1] = FMath::Max(Hi[R], Hi[R - 1]);
		}
	// Point on row R's tray centreline whose coordinate along Axis[R] is X.
	auto EndPoint = [&](int32 R, double X)
	{
		const FTrayRow& T = In.Rows[R];
		return T.OriginM + T.Along * ((X - FVector::DotProduct(T.OriginM, Axis[R])) * FVector::DotProduct(T.Along, Axis[R]));
	};
	for (int32 R = 0; R < NumRows; ++R)
	{
		if (Lo[R] >= Hi[R]) continue;
		Plan.TrayRunsM.Add({EndPoint(R, Lo[R]), EndPoint(R, Hi[R])});
		if (R > 0 && Axis[R] == Axis[R - 1] && Lo[R - 1] < Hi[R - 1])
		{
			Plan.TrayRunsM.Add({EndPoint(R - 1, Lo[R]), EndPoint(R, Lo[R])});
			Plan.TrayRunsM.Add({EndPoint(R - 1, Hi[R]), EndPoint(R, Hi[R])});
		}
	}

	// 3. Spine-to-spine links between neighbouring parallel rows, through the nearer loop end.
	for (int32 R = 1; R < NumRows; ++R)
	{
		if (Spines[R - 1].IsEmpty() || Spines[R].IsEmpty()) continue;
		if (Axis[R] != Axis[R - 1]) { Plan.Unconnected.Add({Spines[R][0], TEXT("스파인 간 링크: 줄이 평행하지 않음")}); continue; }
		const double X = FVector::DotProduct(Location(In.Devices[Spines[R - 1][0]]), Axis[R]);
		const double End = FMath::Abs(X - Hi[R]) < FMath::Abs(X - Lo[R]) ? Hi[R] : Lo[R];
		const FVector Corner[2] = {EndPoint(R - 1, End), EndPoint(R, End)};
		for (int32 K = 0; K < In.SpineLinksPerRowPair; ++K)
		{
			int32 AD, AP, BD, BP;
			if (!NextSpinePort(R - 1, AD, AP)) { Plan.Unconnected.Add({Spines[R - 1][0], TEXT("스파인 포트 부족")}); break; }
			Ledger.Claim(AD, AP);
			if (!NextSpinePort(R, BD, BP)) { Plan.Unconnected.Add({Spines[R][0], TEXT("스파인 포트 부족")}); break; }
			Ledger.Claim(BD, BP);
			Route(AD, AP, BD, BP, R - 1, R, Corner);
		}
	}
	Plan.RejectedClaims = Ledger.GetRejected();
	return Plan;
}
