#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/RackThermalActor.h"
#include "Simulation/Core/SimUnits.h"

namespace
{
const TCHAR* InternalLeakNames[] = {TEXT("공급 매니폴드"), TEXT("공급 커넥터"), TEXT("냉각 플레이트"), TEXT("회수 커넥터"), TEXT("회수 매니폴드")};
const TCHAR* InternalLeakKeys[] = {TEXT("supply_manifold"), TEXT("supply_connector"), TEXT("plate"), TEXT("return_connector"), TEXT("return_manifold")};
}

void ADataCenterSimulationActor::AppendInternalLiveLeakSites()
{
	if (!Room) return;
	const auto Routed = Room->GetRoutedRacks();
	for (int32 R = 0; R < Routed.Num(); ++R)
	{
		const auto* Rack = Routed[R]; const auto* H = Room->GetInRackHandles(Rack);
		if (!Rack || !H) continue; // no internal sites in radiator/virtual rows
		for (int32 T = 0; T < H->Slots.Num(); ++T)
			for (int32 K = 0; K < 5; ++K)
			{
				const auto& Slot = Rack->Params.Slots[H->Slots[T]];
				auto& P = LiveLeakSites.AddDefaulted_GetRef();
				P.RackIndex = R; P.Tray = T; P.Slot = H->Slots[T]; P.InternalSite = K; P.NetworkLeak = H->Leaks[5 * T + K];
				P.Key = FString::Printf(TEXT("rack:%d:tray:%d:%s"), R, T, InternalLeakKeys[K]);
				P.Label = FString::Printf(TEXT("랙 %d 트레이 %02d %s %s"), R, T,
					*Rack->Params.DeviceTypes[Slot.DeviceType].Name.ToString(), InternalLeakNames[K]);
				// Schematic debug anchors on front/rear faces, NOT physical connector geometry or hydraulic node positions.
				const bool Rear = K == 0 || K == 4;
				const double Side = K < 2 ? -.3 : K > 2 ? .3 : 0.0; // display spacing only
				const FVector LocalM((Rear ? -.5 : .5) * Rack->RackSizeM.X, Side * Rack->RackSizeM.Y, Slot.BottomZM + .5 * Slot.HeightM);
				P.PositionM = SimUnits::UnrealToMeters(Rack->GetActorTransform().TransformPosition(SimUnits::MetersToUnreal(LocalM)));
			}
	}
}

int32 ADataCenterSimulationActor::ResolveLiveLeakSite(const FString& Token) const
{
	if (Token.IsEmpty()) return INDEX_NONE;
	bool Digits = true; for (TCHAR C : Token) Digits &= FChar::IsDigit(C);
	if (Digits) { const int32 I = FCString::Atoi(*Token); return LiveLeakSites.IsValidIndex(I) ? I : INDEX_NONE; }
	for (int32 I = 0; I < LiveLeakSites.Num(); ++I)
		if (LiveLeakSites[I].Key == Token || LiveLeakSites[I].Label == Token) return I;
	return INDEX_NONE;
}

int32 ADataCenterSimulationActor::FindInternalLeakOnRay(const FVector& EyeM, const FVector& Direction, int32 KindCycle) const
{
	if (!Room || EyeM.ContainsNaN() || Direction.ContainsNaN() || Direction.IsNearlyZero() || KindCycle < 0) return INDEX_NONE;
	const auto Routed = Room->GetRoutedRacks();
	double Best = TNumericLimits<double>::Max(); int32 Found = INDEX_NONE;
	for (int32 R = 0; R < Routed.Num(); ++R)
	{
		const auto* Rack = Routed[R]; const auto* H = Room->GetInRackHandles(Rack);
		if (!Rack || !H) continue;
		const FTransform Transform = Rack->GetActorTransform();
		const FVector Eye = SimUnits::UnrealToMeters(Transform.InverseTransformPosition(SimUnits::MetersToUnreal(EyeM)));
		const FVector Dir = Transform.InverseTransformVector(Direction.GetSafeNormal());
		if (FMath::Abs(Dir.X) < 1e-10) continue;
		const bool Rear = Eye.X < 0.0;
		const double Distance = ((Rear ? -.5 : .5) * Rack->RackSizeM.X - Eye.X) / Dir.X;
		if (Distance <= 0.0 || Distance >= Best) continue;
		const FVector Hit = Eye + Dir * Distance;
		if (FMath::Abs(Hit.Y) > .5 * Rack->RackSizeM.Y) continue;
		int32 Slot = INDEX_NONE;
		// First find the actual physical slot; air-cooled slots/blank space never snap to a liquid neighbour.
		for (int32 S = 0; S < Rack->Params.Slots.Num(); ++S)
		{
			const auto& P = Rack->Params.Slots[S];
			if (Hit.Z >= P.BottomZM && Hit.Z < P.BottomZM + P.HeightM) { Slot = S; break; }
		}
		const int32 Tray = H->Slots.IndexOfByKey(Slot);
		// A nearer air slot occludes a farther rack's internal target.
		if (Hit.Z < 0.0 || Hit.Z > Rack->RackSizeM.Z) continue;
		Best = Distance; Found = INDEX_NONE;
		if (Tray == INDEX_NONE) continue;
		const int32 FrontKinds[] = {2, 1, 3};
		const int32 Kind = Rear ? (KindCycle % 2 == 0 ? 0 : 4) : FrontKinds[KindCycle % 3];
		for (int32 I = 0; I < LiveLeakSites.Num(); ++I)
			if (LiveLeakSites[I].RackIndex == R && LiveLeakSites[I].Tray == Tray && LiveLeakSites[I].InternalSite == Kind) { Found = I; break; }
	}
	return Found;
}

double ADataCenterSimulationActor::GetLiveLeakToolHoleM(int32 Site, int32 ToolMode) const
{
	if (!LiveLeakSites.IsValidIndex(Site)) return 0.0;
	// Scenario controls from L3.5-e, ESTIMATES, not material failure thresholds; reuse existing hose ID for rupture.
	const bool Internal = LiveLeakSites[Site].InternalSite != INDEX_NONE;
	return ToolMode == 1 ? (Internal ? .001 : .005) : ToolMode == 2 ? (Internal ? .003 : .030)
		: ToolMode == 3 ? GetLiveLeakRuptureDiameterM(Site) : 0.0;
}

double ADataCenterSimulationActor::GetLiveLeakFlowM3s(int32 Index) const
{
	if (!LiveLeakSites.IsValidIndex(Index)) return 0.0;
	const auto& P = LiveLeakSites[Index];
	if (!P.bPlant) return Room ? Room->GetNetwork().GetLeakFlowM3s(P.InternalSite != INDEX_NONE ? P.NetworkLeak : Room->GetLeakNetworkIndex(P.RoomSpec)) : 0.0;
	const auto* L = Plant.GetLoopFor(P.Loop); const int32 K = L ? L->FindLeak(P.PlantSite, P.Branch) : INDEX_NONE;
	return K == INDEX_NONE ? 0.0 : L->GetLeakFlowM3s(K);
}
