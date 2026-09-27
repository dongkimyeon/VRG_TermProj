#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/HydraulicNetwork.h"
#include "Simulation/Hydraulics/SparseLinearSystem.h"
#include "Simulation/Hydraulics/InRowLoopBuilder.h"
#include "Visualization/PatchManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSparseHydraulicTest, "VRG.Hydraulics.Sparse.AnalyticPumpLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSparseHydraulicTest::RunTest(const FString& Parameters)
{
	// Synthetic verification problem, not a data-centre equipment specification.
	// Constant total resistance with 20/300 serial edges gives the same analytic operating point.
	for (int32 Count : {20, 300})
	{
		FHydraulicNetwork Network;
		for (int32 I = 0; I <= Count; ++I)
		{
			FHydraulicNode Node; Node.bFixedPressure = I == 0; Node.PressurePa = 0;
			Network.AddNode(Node);
		}
		FHydraulicPump Pump; Pump.FromNodeIndex=0; Pump.ToNodeIndex=Count;
		Pump.ShutoffHeadPa=100000; Pump.CurveCoeffPaS2M6=1e10; Pump.Efficiency=.7;
		Network.AddPump(Pump);
		for (int32 I = Count; I > 0; --I)
		{
			FHydraulicPipe Pipe; Pipe.FromNodeIndex=I; Pipe.ToNodeIndex=I-1; Pipe.ResistancePaS2M6=9e10/Count;
			Network.AddPipe(Pipe);
		}
		const FHydraulicSolveResult Result=Network.Solve(FHydraulicSolveSettings());
		TestTrue(TEXT("Converged"), Result.bConverged);
		TestEqual(TEXT("Sparse threshold"), Result.bUsedSparse, Count>200);
		const double FlowError=FMath::Abs(Network.GetPumpFlowM3s(0)-.001)/.001;
		double PressureError=0, Imbalance=0;
		for (int32 I=1; I<=Count; ++I)
		{
			PressureError=FMath::Max(PressureError,FMath::Abs(Network.GetNodePressurePa(I)-90000.0*I/Count)/90000);
			Imbalance=FMath::Max(Imbalance,FMath::Abs(Network.GetNodeImbalanceM3s(I))/.001);
		}
		AddInfo(FString::Printf(TEXT("nodes=%d flowErr=%.12e pressureErr=%.12e massErr=%.12e residual=%.12e powerW=%.9f"),
			Count+1,FlowError,PressureError,Imbalance,Result.NormalizedResidual,Network.GetPumpElectricalInputPowerW(0)));
		// Existing hydraulic regression tolerance; measured values will be recorded in CLAUDE.md.
		TestTrue(TEXT("Flow analytic 0.001 m3/s"),FlowError<1e-9);
		TestTrue(TEXT("Pressure analytic 90000 Pa"),PressureError<1e-9);
		TestTrue(TEXT("Mass conservation / 0.001 m3/s"),Imbalance<1e-9);
		TestTrue(TEXT("Positive plausible pump input"),Network.GetPumpElectricalInputPowerW(0)>100 && Network.GetPumpElectricalInputPowerW(0)<150);
	}
	TArray<FHydraulicMatrixEntry> Singular{{0,0,1},{0,1,1},{1,0,1},{1,1,1}};
	TArray<double> Rhs{1,2};
	TestFalse(TEXT("Singular failure"),SolveSparseLinearSystem(Singular,Rhs,2));
	TestEqual(TEXT("Failed solve preserves RHS"),Rhs[1],2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPatchInterferenceTest, "VRG.Visualization.Patch.SegmentInterference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FPatchInterferenceTest::RunTest(const FString& Parameters)
{
	TArray<Patch::FCable> Cables; Cables.SetNum(2);
	Cables[0].PointsM={FVector(-1,0,0),FVector(1,0,0)};
	Cables[1].PointsM={FVector(0,-1,0),FVector(0,1,0)};
	TestEqual(TEXT("Crossing"),Patch::FindInterferences(Cables,.0048).Num(),1);
	for (FVector& Point:Cables[1].PointsM) Point.Z=.01;
	TestEqual(TEXT("XY crossing separated vertically"),Patch::FindInterferences(Cables,.0048).Num(),0);
	Cables[1].PointsM={FVector(-1,.002,0),FVector(1,.002,0)};
	TestEqual(TEXT("Parallel overlapping tubes"),Patch::FindInterferences(Cables,.0048).Num(),1);
	Cables[1].PointsM={FVector::ZeroVector,FVector::ZeroVector};
	TestEqual(TEXT("Degenerate segment"),Patch::FindInterferences(Cables,.0048).Num(),1);
	Cables[1].PointsM.Reset();
	TestTrue(TEXT("Empty cable"),Patch::FindInterferences(Cables,.0048).IsEmpty());
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSparseRoomTest, "VRG.Hydraulics.Sparse.EightRowsMatchesDenseRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSparseRoomTest::RunTest(const FString& Parameters)
{
	// FInRowLoopSpec defaults: the GB200 NVL72 row (same as the room actor).
	FInRowLoopSpec Spec; Spec.NumRacks=16;
	FHydraulicNetwork Dense, Sparse;
	const FInRowLoopHandles Reference=InRowLoopBuilder::AddLoop(Dense,Spec);
	TArray<FInRowLoopHandles> Rows;
	for (int32 I=0;I<8;++I) Rows.Add(InRowLoopBuilder::AddLoop(Sparse,Spec));
	const FHydraulicSolveResult DenseResult=Dense.Solve(FHydraulicSolveSettings());
	const FHydraulicSolveResult SparseResult=Sparse.Solve(FHydraulicSolveSettings());
	TestTrue(TEXT("Both converge"),DenseResult.bConverged && SparseResult.bConverged);
	TestTrue(TEXT("Dense and sparse paths"),!DenseResult.bUsedSparse && SparseResult.bUsedSparse);
	TestEqual(TEXT("392 nodes"),Sparse.GetNumNodes(),392);
	double Error=0, Imbalance=0;
	for (const FInRowLoopHandles& Row:Rows)
		for (int32 I=0;I<16;++I)
		{
			const double Flow=Dense.GetPipeFlowM3s(Reference.RackBranchPipeIndices[I]);
			Error=FMath::Max(Error,FMath::Abs(Sparse.GetPipeFlowM3s(Row.RackBranchPipeIndices[I])-Flow)/FMath::Abs(Flow));
		}
	for (int32 I=0;I<Sparse.GetNumNodes();++I)
		Imbalance=FMath::Max(Imbalance,FMath::Abs(Sparse.GetNodeImbalanceM3s(I))/SparseResult.ReferenceFlowM3s);
	AddInfo(FString::Printf(TEXT("392-node dense comparison=%.12e mass/referenceFlow=%.12e referenceFlow=%.12e m3/s"),Error,Imbalance,SparseResult.ReferenceFlowM3s));
	TestTrue(TEXT("Per-branch relative flow"),Error<1e-9);
	TestTrue(TEXT("Mass conservation including fixed reservoirs"),Imbalance<1e-9);
	return true;
}
#endif
