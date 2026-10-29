#include "Simulation/Thermal/TrayCoolantModel.h"

bool FTrayNetworkTransport::IsValid(int32 Trays) const
{
	if (Reservoirs != Trays + 1 || NetWK.Num() != Reservoirs * (Reservoirs + 2)
		|| BoundaryWK.Num() != Reservoirs + 2 || LeakWK.Num() != Reservoirs + 2
		|| ClassLeakWK.Num() != 3 * (Reservoirs + 2) || ClassLeakKgs.Num() != 3
		|| !FMath::IsFinite(LeakKgs) || LeakKgs < 0.0 || !FMath::IsFinite(MassResidualKgs)) return false;
	for (const auto* Values : { &NetWK, &BoundaryWK, &LeakWK, &ClassLeakWK, &ClassLeakKgs })
		for (double V : *Values) if (!FMath::IsFinite(V)) return false;
	return true;
}

double FTrayNetworkTransport::Dot(const TArray<double>& Coefficients, int32 Row, const TArray<double>& TrayK,
	double ReturnK, double SupplyK, double ReturnBoundaryK, double ReferenceK) const
{
	double W = 0.0;
	for (int32 C = 0; C < Reservoirs + 2; ++C)
	{
		const double K = C < TrayK.Num() ? TrayK[C] : C == Reservoirs - 1 ? ReturnK : C == Reservoirs ? SupplyK : ReturnBoundaryK;
		W += Coefficients[Row * (Reservoirs + 2) + C] * (K - ReferenceK);
	}
	return W;
}

bool FTrayCoolantModel::Initialize(const TArray<double>& Fractions, double TotalCapacityJK, double InitialTempK, double InCpJKgK)
{
	if (Fractions.IsEmpty() || !FMath::IsFinite(TotalCapacityJK) || !(TotalCapacityJK > 0.0)
		|| !FMath::IsFinite(InitialTempK) || !(InitialTempK > 0.0) || !FMath::IsFinite(InCpJKgK) || !(InCpJKgK > 0.0)) return false;
	double Sum = 0.0;
	for (double F : Fractions) { if (!FMath::IsFinite(F) || !(F > 0.0)) return false; Sum += F; }
	if (!FMath::IsFinite(Sum)) return false;
	TrayTempK.Init(InitialTempK, Fractions.Num()); NextTempK = TrayTempK;
	TrayCapacityJK.SetNum(Fractions.Num()); NetHeatW.Init(0.0, Fractions.Num());
	double Allocated = 0.0;
	for (int32 I = 0; I < Fractions.Num(); ++I) { TrayCapacityJK[I] = 0.6 * TotalCapacityJK * Fractions[I] / Sum; Allocated += TrayCapacityJK[I]; }
	ReturnCapacityJK = TotalCapacityJK - Allocated;
	ReturnTempK = ReferenceTempK = InitialTempK; CpJKgK = InCpJKgK;
	HeatInJ = HeatOutJ = LastLeakHeatW = LastBoundaryHeatOutW = LastMassResidualKgs = 0.0;
	bStepStable = true;
	return true;
}

double FTrayCoolantModel::GetStableStepLimitS(const FTrayCoolantInput& Input) const
{
	const int32 N = TrayTempK.Num();
	if (Input.Network.Reservoirs > 0)
	{
		if (!Input.Network.IsValid(N) || Input.HeatFromPartsW.Num() != N || Input.PartsConductanceWK.Num() != N
			|| !FMath::IsFinite(Input.SupplyTempK) || Input.SupplyTempK <= 0.0
			|| !FMath::IsFinite(Input.ReturnBoundaryTempK) || Input.ReturnBoundaryTempK <= 0.0) return 0.0;
		double Limit = TNumericLimits<double>::Max();
		for (int32 R = 0; R <= N; ++R)
		{
			if (R < N && (!FMath::IsFinite(Input.HeatFromPartsW[R]) || !FMath::IsFinite(Input.PartsConductanceWK[R]) || Input.PartsConductanceWK[R] < 0.0)) return 0.0;
			const double RateWK = -Input.Network.NetWK[R * (N + 3) + R] + (R < N ? Input.PartsConductanceWK[R] : 0.0);
			if (RateWK > 0.0) Limit = FMath::Min(Limit, (R < N ? TrayCapacityJK[R] : ReturnCapacityJK) / RateWK);
		}
		return Limit;
	}
	if (N == 0 || Input.InletMassFlowKgs.Num() != N || Input.OutletMassFlowKgs.Num() != N
		|| Input.LeakMassFlowKgs.Num() != N || Input.HeatFromPartsW.Num() != N
		|| Input.PartsConductanceWK.Num() != N
		|| !FMath::IsFinite(Input.SupplyTempK) || !(Input.SupplyTempK > 0.0)
		|| !FMath::IsFinite(Input.ReturnBoundaryTempK) || !(Input.ReturnBoundaryTempK > 0.0)
		|| !FMath::IsFinite(Input.ReturnMassFlowKgs) || !FMath::IsFinite(Input.ReturnLeakMassFlowKgs)
		|| Input.ReturnLeakMassFlowKgs < 0.0) return 0.0;
	double LimitS = TNumericLimits<double>::Max();
	double ReturnOutKgs = FMath::Max(Input.ReturnMassFlowKgs, 0.0) + Input.ReturnLeakMassFlowKgs;
	for (int32 I = 0; I < N; ++I)
	{
		if (!FMath::IsFinite(Input.InletMassFlowKgs[I]) || !FMath::IsFinite(Input.OutletMassFlowKgs[I])
			|| !FMath::IsFinite(Input.LeakMassFlowKgs[I]) || Input.LeakMassFlowKgs[I] < 0.0
			|| !FMath::IsFinite(Input.HeatFromPartsW[I]) || !FMath::IsFinite(Input.PartsConductanceWK[I])
			|| Input.PartsConductanceWK[I] < 0.0) return 0.0;
		const double OutKgs = FMath::Max(-Input.InletMassFlowKgs[I], 0.0)
			+ FMath::Max(Input.OutletMassFlowKgs[I], 0.0) + Input.LeakMassFlowKgs[I];
		const double Conductance = OutKgs * CpJKgK + Input.PartsConductanceWK[I];
		if (!FMath::IsFinite(Conductance)) return 0.0;
		if (Conductance > 0.0) LimitS = FMath::Min(LimitS, TrayCapacityJK[I] / Conductance);
		ReturnOutKgs += FMath::Max(-Input.OutletMassFlowKgs[I], 0.0);
	}
	if (!FMath::IsFinite(ReturnOutKgs * CpJKgK)) return 0.0;
	if (ReturnOutKgs > 0.0) LimitS = FMath::Min(LimitS, ReturnCapacityJK / (ReturnOutKgs * CpJKgK));
	return LimitS;
}

bool FTrayCoolantModel::Step(const FTrayCoolantInput& Input, double DtS)
{
	bStepStable = false;
	if (!FMath::IsFinite(DtS) || !(DtS > 0.0) || DtS > GetStableStepLimitS(Input)) return false;
	const int32 N = TrayTempK.Num();
	if (Input.Network.Reservoirs > 0)
	{
		const FTrayNetworkTransport& T = Input.Network;
		double PartsW = 0.0;
		for (int32 R = 0; R < N; ++R)
		{
			NetHeatW[R] = T.Dot(T.NetWK, R, TrayTempK, ReturnTempK, Input.SupplyTempK, Input.ReturnBoundaryTempK, ReferenceTempK) + Input.HeatFromPartsW[R];
			NextTempK[R] = TrayTempK[R] + DtS * NetHeatW[R] / TrayCapacityJK[R];
			PartsW += Input.HeatFromPartsW[R];
		}
		const double ReturnNetW = T.Dot(T.NetWK, N, TrayTempK, ReturnTempK, Input.SupplyTempK, Input.ReturnBoundaryTempK, ReferenceTempK);
		LastBoundaryHeatOutW = T.Dot(T.BoundaryWK, 0, TrayTempK, ReturnTempK, Input.SupplyTempK, Input.ReturnBoundaryTempK, ReferenceTempK);
		LastLeakHeatW = T.Dot(T.LeakWK, 0, TrayTempK, ReturnTempK, Input.SupplyTempK, Input.ReturnBoundaryTempK, ReferenceTempK);
		LastMassResidualKgs = T.MassResidualKgs;
		Swap(TrayTempK, NextTempK); ReturnTempK += DtS * ReturnNetW / ReturnCapacityJK;
		HeatInJ += DtS * PartsW; HeatOutJ += DtS * (LastBoundaryHeatOutW + LastLeakHeatW);
		bStepStable = true;
		return true;
	}

	bStepStable = true;
	LastBoundaryHeatOutW = LastLeakHeatW = LastMassResidualKgs = 0.0;
	double ReturnNetW = 0.0, PartsW = 0.0, ReturnMassBalance = -Input.ReturnMassFlowKgs - Input.ReturnLeakMassFlowKgs;
	double ReturnOutKgs = FMath::Max(Input.ReturnMassFlowKgs, 0.0) + Input.ReturnLeakMassFlowKgs;
	for (int32 I = 0; I < N; ++I)
	{
		const double InKgs = Input.InletMassFlowKgs[I], OutKgs = Input.OutletMassFlowKgs[I], LeakKgs = Input.LeakMassFlowKgs[I];
		LastMassResidualKgs = FMath::Max(LastMassResidualKgs, FMath::Abs(InKgs - OutKgs - LeakKgs));
		ReturnMassBalance += OutKgs;
		// 역류 때도 실제 상류 수온을 쓰며 음의 유량을 0으로 자르지 않는다.
		const double InHeatW = InKgs * CpJKgK * ((InKgs >= 0.0 ? Input.SupplyTempK : TrayTempK[I]) - ReferenceTempK);
		const double ToReturnW = OutKgs * CpJKgK * ((OutKgs >= 0.0 ? TrayTempK[I] : ReturnTempK) - ReferenceTempK);
		const double LeakW = LeakKgs * CpJKgK * (TrayTempK[I] - ReferenceTempK);
		NetHeatW[I] = Input.HeatFromPartsW[I] + InHeatW - ToReturnW - LeakW;
		ReturnNetW += ToReturnW; PartsW += Input.HeatFromPartsW[I];
		LastBoundaryHeatOutW -= InHeatW; LastLeakHeatW += LeakW;
		const double OutgoingKgs = FMath::Max(-InKgs, 0.0) + FMath::Max(OutKgs, 0.0) + LeakKgs;
		const double Rate = (OutgoingKgs * CpJKgK + Input.PartsConductanceWK[I]) / TrayCapacityJK[I];
		bStepStable &= DtS * Rate <= 1.0;
		ReturnOutKgs += FMath::Max(-OutKgs, 0.0);
	}
	LastMassResidualKgs = FMath::Max(LastMassResidualKgs, FMath::Abs(ReturnMassBalance));
	const double ReturnBoundaryW = Input.ReturnMassFlowKgs * CpJKgK
		* ((Input.ReturnMassFlowKgs >= 0.0 ? ReturnTempK : Input.ReturnBoundaryTempK) - ReferenceTempK);
	const double ReturnLeakW = Input.ReturnLeakMassFlowKgs * CpJKgK * (ReturnTempK - ReferenceTempK);
	LastBoundaryHeatOutW += ReturnBoundaryW; LastLeakHeatW += ReturnLeakW;
	ReturnNetW -= ReturnBoundaryW + ReturnLeakW;
	bStepStable &= DtS * ReturnOutKgs * CpJKgK / ReturnCapacityJK <= 1.0;
	// 불안정한 스텝은 상태·누적 장부를 바꾸지 않는다. 호출자가 같은 시간 구간을 더 작게 나눠야 한다.
	if (!bStepStable) return false;
	for (int32 I = 0; I < N; ++I) NextTempK[I] = TrayTempK[I] + DtS * NetHeatW[I] / TrayCapacityJK[I];
	const double NextReturnK = ReturnTempK + DtS * ReturnNetW / ReturnCapacityJK;
	Swap(TrayTempK, NextTempK); ReturnTempK = NextReturnK;
	HeatInJ += DtS * PartsW;
	HeatOutJ += DtS * (LastBoundaryHeatOutW + LastLeakHeatW);
	return true;
}

double FTrayCoolantModel::GetStoredJ() const
{
	double Stored = ReturnCapacityJK * (ReturnTempK - ReferenceTempK);
	for (int32 I = 0; I < TrayTempK.Num(); ++I) Stored += TrayCapacityJK[I] * (TrayTempK[I] - ReferenceTempK);
	return Stored;
}
