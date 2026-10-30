#include "Misc/AutomationTest.h"
#include "Simulation/Hydraulics/SparseLinearSystem.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSparseSymbolicReuseTest, "VRG.Hydraulics.Sparse.SymbolicReuse",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FSparseSymbolicReuseTest::RunTest(const FString& Parameters)
{
 FSparseLinearWorkspace Cached;
 auto Check = [&](const TArray<FHydraulicMatrixEntry>& A, int32 N, bool ExpectReuse) {
  TArray<double> Rhs; Rhs.Init(1.0,N); auto Fresh=Rhs;
  FSparseSolveCosts Costs;
  const bool Ref=SolveSparseLinearSystem(A,Fresh,N);
  const bool Got=Cached.Solve(A,Rhs,N,&Costs);
  TestTrue(TEXT("both finite solves succeed, exact RHS equality"),Ref && Got && Rhs==Fresh);
  TestTrue(TEXT("analysis only reused for exact compressed structure"),ExpectReuse ? Costs.ReuseHits==1 && Costs.AnalyzeCalls==0 : Costs.AnalyzeCalls==1 && Costs.ReuseHits==0);
 };
 TArray<FHydraulicMatrixEntry> A={{0,0,4},{0,1,1},{1,0,1},{1,1,3}}; // synthetic algebra, no equipment constants
 Check(A,2,false); A[0].Value=5; Check(A,2,true);
 A.RemoveAt(1); Check(A,2,false); // structural change, same dimension
 A.Add({2,2,2}); Check(A,3,false); // dimension change
 TArray<double> Bad={7,8}; const auto Before=Bad;
 TArray<FHydraulicMatrixEntry> Singular={{0,0,0},{0,1,0},{1,0,0},{1,1,0}};
 TestFalse(TEXT("singular failure reported"),Cached.Solve(Singular,Bad,2));
 TestTrue(TEXT("failure leaves RHS unchanged"),Bad==Before);
 A={{0,0,4},{0,1,1},{1,0,1},{1,1,3}}; Check(A,2,true); // recovery after numeric failure
 FSparseLinearWorkspace Copied=Cached, Assigned; Assigned=Cached;
 for(auto* C:{&Copied,&Assigned}) {
  TArray<double> B={1,2},Ref=B; FSparseSolveCosts Costs;
  TestTrue(TEXT("independent copy solve"),C->Solve(A,B,2,&Costs) && SolveSparseLinearSystem(A,Ref,2) && B==Ref);
  TestTrue(TEXT("copied workspace starts unanalyzed, never shares mutable Eigen state"),Costs.AnalyzeCalls==1 && Costs.ReuseHits==0);
 }
 AddInfo(TEXT("fresh/reuse exact equality; values/structure/dimension changes, singular RHS atomicity/recovery, independent copy/assignment"));
 return true;
}
#endif
