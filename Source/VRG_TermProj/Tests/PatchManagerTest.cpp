#include "Misc/AutomationTest.h"
#include "Visualization/PatchManager.h"
#include "Visualization/GjRackLayout.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// Two facing rows like L_Test: row 0 at x=-1.8 m looks +X, row 1 at x=+1.8 m looks -X (rears outward).
	Patch::FDevice MakeCabinet(int32 Row, double X, double Y, double YawDeg, bool bSpine, int32 Uplinks, int32 SpineSwitches = 1)
	{
		using namespace GjRackLayout;
		Patch::FDevice D; D.Row = Row; D.bSpine = bSpine;
		D.ToWorld = FTransform(FRotator(0, YawDeg, 0), FVector(X, Y, 0));
		D.Outward = -D.ToWorld.GetRotation().GetForwardVector();
		if (bSpine)
			for (int32 S = 0; S < SpineSwitches; ++S)
				for (int32 P = 0; P < PortsPerSwitch; ++P)
					D.PortM.Add(D.ToWorld.TransformPosition(SwitchPortM(NodeZ(16 + S), P % PortsPerSwitchRow + 1, P >= PortsPerSwitchRow)));
		else
		{
			for (int32 C = 1; C <= PortsPerSwitchRow; ++C) D.PortM.Add(D.ToWorld.TransformPosition(SwitchPortM(SwitchZM, C, true)));
			for (int32 J = 0; J < Uplinks; ++J) D.UplinkPorts.Add(7 + J);
		}
		return D;
	}
	Patch::FTrayRow TrayFor(const Patch::FDevice& First, const FVector& Along)
	{
		const FVector Out = First.Outward;
		return {First.ToWorld.GetLocation() * FVector(1, 1, 0) + Out * (.534 + .27) + FVector(0, 0, 2.85), Along, Out};
	}
	Patch::FInput TwoRows(int32 RacksPerRow, int32 Uplinks, bool bSpineRow1, int32 SpineSwitches = 1)
	{
		Patch::FInput In;
		for (int32 I = 0; I < RacksPerRow; ++I) In.Devices.Add(MakeCabinet(0, -1.8, I * .66, 0, false, Uplinks));
		In.Devices.Add(MakeCabinet(0, -1.8, RacksPerRow * .66, 0, true, 0, SpineSwitches));
		for (int32 I = 0; I < RacksPerRow; ++I) In.Devices.Add(MakeCabinet(1, 1.8, I * .66, 180, false, Uplinks));
		if (bSpineRow1) In.Devices.Add(MakeCabinet(1, 1.8, RacksPerRow * .66, 180, true, 0, SpineSwitches));
		In.Rows.Add(TrayFor(In.Devices[0], FVector(0, 1, 0)));
		In.Rows.Add(TrayFor(In.Devices[RacksPerRow + 1], FVector(0, -1, 0)));  // antiparallel on purpose
		return In;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPatchManagerTest, "VRG.Visualization.Patch.LedgerAndRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FPatchManagerTest::RunTest(const FString& Parameters)
{
	{
		Patch::FPortLedger Ledger;
		TestTrue(TEXT("First claim succeeds"), Ledger.Claim(3, 7));
		TestFalse(TEXT("Second claim of the same port is refused"), Ledger.Claim(3, 7));
		TestTrue(TEXT("Other port of the same device is free"), Ledger.Claim(3, 8));
		TestEqual(TEXT("Refusal counted"), Ledger.GetRejected(), 1);
	}
	{
		const Patch::FInput In = TwoRows(3, 4, true);
		const Patch::FPlan Plan = Patch::PlanPatches(In);
		TestEqual(TEXT("Every uplink patched"), Plan.NumUplinks(), 2 * 3 * 4);
		TestEqual(TEXT("Spine-to-spine links"), Plan.Cables.Num() - Plan.NumUplinks(), 4);
		TestEqual(TEXT("Nothing unconnected"), Plan.Unconnected.Num(), 0);
		TestEqual(TEXT("No double claims"), Plan.RejectedClaims, 0);
		TestEqual(TEXT("Two row runs + two cross runs close the loop"), Plan.TrayRunsM.Num(), 4);
		TSet<TPair<int32, int32>> Ends;
		int32 Intrusions = 0;
		for (const Patch::FCable& C : Plan.Cables)
		{
			Ends.Add({C.FromDevice, C.FromPort}); Ends.Add({C.ToDevice, C.ToPort});
			TestTrue(TEXT("Cable starts on its port"), C.PointsM[0].Equals(In.Devices[C.FromDevice].PortM[C.FromPort], 1e-9));
			TestTrue(TEXT("Cable ends on its port"), C.PointsM.Last().Equals(In.Devices[C.ToDevice].PortM[C.ToPort], 1e-9));
			// Away from the two port stubs a cable must never be inside or over any cabinet footprint.
			for (int32 S = 2; S < C.PointsM.Num() - 3; ++S)
			{
				const FVector A = C.PointsM[S], B = C.PointsM[S + 1];
				const int32 N = FMath::Max(1, FMath::CeilToInt32(FVector::Distance(A, B) / .02));
				for (int32 K = 0; K <= N; ++K)
					for (const Patch::FDevice& D : In.Devices)
					{
						const FVector L = D.ToWorld.InverseTransformPosition(FMath::Lerp(A, B, double(K) / N));
						Intrusions += FMath::Abs(L.X) < D.HalfExtentM.X - 1e-3 && FMath::Abs(L.Y) < D.HalfExtentM.Y - 1e-3;
					}
			}
		}
		TestEqual(TEXT("Each port used by exactly one cable end"), Ends.Num(), 2 * Plan.Cables.Num());
		TestEqual(TEXT("No cable passes through or over a cabinet"), Intrusions, 0);
	}
	{
		// 10 racks x 4 uplinks = 40 > 36 spine ports: the overflow is reported, never double-booked.
		const Patch::FPlan Plan = Patch::PlanPatches(TwoRows(10, 4, false));
		int32 Full = 0, NoSpine = 0;
		for (const Patch::FUnconnected& U : Plan.Unconnected)
		{
			Full += U.Reason.Contains(TEXT("포트 부족")); NoSpine += U.Reason.Contains(TEXT("스파인 캐비닛 없음"));
		}
		TestEqual(TEXT("Spine capacity used exactly"), Plan.NumUplinks(), 36);
		TestTrue(TEXT("Overflow reported"), Full > 0);
		TestEqual(TEXT("Row without spine reports every rack"), NoSpine, 10);
		TestEqual(TEXT("Still no double claims"), Plan.RejectedClaims, 0);
	}
	return true;
}
#endif
