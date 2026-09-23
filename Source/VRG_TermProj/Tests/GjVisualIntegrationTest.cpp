#include "Misc/AutomationTest.h"
#include "Actors/RackThermalActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Actors/SpineCabinetActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGjVisualIntegrationTest,"VRG.Visualization.GJ.ExistingActorsAndHydraulics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FGjVisualIntegrationTest::RunTest(const FString& Parameters)
{
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if (!TestNotNull(TEXT("World"),World)) return false;
    auto* A=World->SpawnActor<ARackThermalActor>(); auto* B=World->SpawnActor<ARackThermalActor>();
    auto* Cdu=World->SpawnActor<ACoolingDistributionUnitActor>(); auto* Room=World->SpawnActor<ADataCenterRoomActor>();
    A->SetActorLocation(FVector(130,200,0)); B->SetActorLocation(FVector(196,200,0));
    A->SetActorRotation(FRotator(0,90,0)); B->SetActorRotation(FRotator(0,90,0));
    Cdu->SetActorLocation(FVector(0,0,0)); Cdu->SetActorRotation(FRotator(0,90,0));
    Room->SetActorLocation(FVector(40,-20,0)); Room->SetActorRotation(FRotator(0,30,0));
    for(auto* Rack:{A,B})
    {
        Rack->Params=FRackThermalModel::MakeGb200Nvl72Params(); Rack->RackSizeM=FVector(1.068,.6,2.236);
        Rack->bShowReadout=false; Rack->ResetSim(); Rack->PauseSim(); Rack->RefreshVisuals();
    }
    Room->bShowSummary=false; Room->RefreshGjConnections();
    TArray<double> Flows; const int32 Nodes=Room->GetMutableNetwork().GetNumNodes();
    for(int32 I=0;I<Room->GetMutableNetwork().GetNumPipes();++I) Flows.Add(Room->GetMutableNetwork().GetPipeFlowM3s(I));
    const auto HeatBefore=A->Input.ComponentHeatW;
    for(auto* Rack:{A,B}) { Rack->bUseGjModels=true; Rack->RefreshVisuals(); }
    // Row-end spine cabinet, same facing as the row (the patch manager's uplink target).
    auto* Spine=World->SpawnActor<ASpineCabinetActor>(FVector(262,200,0),FRotator(0,90,0));
    Room->bUseGjConnections=true; Room->RefreshGjConnections();
    const Patch::FPlan& Plan=Room->GetPatchPlan();
    TestEqual(TEXT("Patch: every rack uplink goes to the spine"),Plan.NumUplinks(),2*Room->CablesPerRack);
    TestEqual(TEXT("Patch: nothing unconnected"),Plan.Unconnected.Num(),0);
    TestEqual(TEXT("Patch: no double-booked port"),Plan.RejectedClaims,0);
    for(const Patch::FCable& C:Plan.Cables)
        TestTrue(TEXT("Patch: cable lands on a real spine port"),C.PointsM.Last().Equals(Spine->GetPortWorldM(C.ToPort),1e-6));
    TestEqual(TEXT("Same hydraulic node count"),Room->GetMutableNetwork().GetNumNodes(),Nodes);
    TestEqual(TEXT("Same hydraulic edge count"),Room->GetMutableNetwork().GetNumPipes(),Flows.Num());
    for(int32 I=0;I<Flows.Num();++I) TestEqual(TEXT("Visual replacement leaves solved flow unchanged"),Room->GetMutableNetwork().GetPipeFlowM3s(I),Flows[I]);
    TestTrue(TEXT("Heat inputs unchanged"),A->Input.ComponentHeatW==HeatBefore);
    TArray<UStaticMeshComponent*> Meshes; A->GetComponents(Meshes);
    UStaticMeshComponent* FirstNode=nullptr; UStaticMeshComponent* FirstHarness=nullptr; int32 Count=0;
    for(auto* M:Meshes) if(M->GetStaticMesh())
    {
        const FString Name=M->GetStaticMesh()->GetName();
        if(Name==TEXT("SM_GB200_ComputeNode")) { ++Count; if(!FirstNode || M->GetRelativeLocation().Z<FirstNode->GetRelativeLocation().Z) FirstNode=M; }
        if(Name==TEXT("SM_GJ_Harness_00")) FirstHarness=M;
    }
    TestEqual(TEXT("18 authored compute nodes"),Count,18);
    TArray<UInstancedStaticMeshComponent*> BlankSets; A->GetComponents(BlankSets);
    TestTrue(TEXT("Every empty U closed by a 1U blank (10 slots)"),BlankSets.Num()==1 && BlankSets[0]->GetInstanceCount()==10);
    const int32 FirstComputeSlot=A->Params.Slots.IndexOfByPredicate([](const FRackSlot& Slot) { return Slot.DeviceType==FRackThermalModel::Gb200DevCompute; });
    TestEqual(TEXT("First compute slot follows four power shelves"),FirstComputeSlot,4);
    if(TestNotNull(TEXT("First node"),FirstNode) && TestNotNull(TEXT("First harness"),FirstHarness))
    {
        TestTrue(TEXT("Docked module pivot"),FirstNode->GetRelativeLocation().Equals(FVector(5.5,0,34.755),.001));
        A->SetSledDocked(FirstComputeSlot,false); A->Tick(.5f);
        TestTrue(TEXT("Slide preserves authored base X"),FMath::IsNearlyEqual(FirstNode->GetRelativeLocation().X,95.5,.001));
        TestFalse(TEXT("Undocked quick disconnect harness hidden"),FirstHarness->IsVisible());
        Room->Tick(0); TestTrue(TEXT("Undock rebuild consumed by coupling"),Room->ConsumeRebuiltFlag());
        A->SetSledDocked(FirstComputeSlot,true); A->Tick(.5f); Room->Tick(0);
        TestTrue(TEXT("Redock harness"),FirstHarness->IsVisible());
    }
    TArray<USplineMeshComponent*> Curves; Room->GetComponents(Curves);
    TestTrue(TEXT("Curved connections created"),Curves.Num()>0);
    // Data side is the rear (-X): switch mounted reversed, rack actors themselves unrotated.
    int32 Switches=0;
    for(auto* M:Meshes) if(M->GetStaticMesh() && M->GetStaticMesh()->GetName()==TEXT("SM_QuantumIB_Switch_Photo"))
    {
        ++Switches;
        TestTrue(TEXT("Switch reversed at rear slot"),(M->GetRelativeLocation().Equals(FVector(-13.4,0,172.55),.001) || M->GetRelativeLocation().Equals(FVector(-13.4,0,176.995),.001))
            && FMath::IsNearlyEqual(FMath::Abs(M->GetRelativeRotation().Yaw),180.0,.001));
    }
    TestEqual(TEXT("Two management switches per rack"),Switches,2);
    TestTrue(TEXT("Rack yaw untouched"),A->GetActorRotation().Equals(FRotator(0,90,0),.001));
    int32 DataSpans=0;
    for(auto* C:Curves) if(C->GetStaticMesh() && C->GetStaticMesh()->GetName()==TEXT("SM_Connection_Data"))
    {
        ++DataSpans;
        for(const FVector& L:{C->GetStartPosition(),C->GetEndPosition()})
        {
            const FVector W=C->GetComponentTransform().TransformPosition(L);
            for(ARackThermalActor* R:{A,B})
            {
                const FVector P=R->GetActorTransform().InverseTransformPosition(W);
                TestTrue(TEXT("Inter-rack data line stays behind the rack (local X < 0)"),P.X<0);
                // Port tails sit just inside the rear rails; above the roof the line must be in the aisle.
                const bool OverRoof=P.Z>223.6 && FMath::Abs(P.X)<53.4 && FMath::Abs(P.Y)<30;
                TestFalse(TEXT("Inter-rack data line never passes over a rack roof"),OverRoof);
            }
        }
    }
    TestTrue(TEXT("Inter-rack data lines created"),DataSpans>0);
    auto HasEndpoint=[&](FVector Wanted)
    {
        for(auto* C:Curves)
        {
            if(C->GetComponentTransform().TransformPosition(C->GetStartPosition()).Equals(Wanted,.01)
              || C->GetComponentTransform().TransformPosition(C->GetEndPosition()).Equals(Wanted,.01)) return true;
        }
        return false;
    };
    TestTrue(TEXT("Real rotated CDU supply port"),HasEndpoint(Cdu->GetSupplyPortWorldM()*100));
    TestTrue(TEXT("Real rotated CDU return port"),HasEndpoint(Cdu->GetReturnPortWorldM()*100));
    Room->ConsumeRebuiltFlag(); Cdu->AddActorWorldOffset(FVector(25,0,0)); Room->Tick(0);
    Curves.Reset(); Room->GetComponents(Curves);
    TestTrue(TEXT("Move triggers automatic rebuild"),Room->ConsumeRebuiltFlag());
    TestTrue(TEXT("Moved CDU endpoint follows"),HasEndpoint(Cdu->GetSupplyPortWorldM()*100));
    // Runtime rebuild must not leak stale generated components.
    const int32 Before=Curves.Num(); Room->RefreshGjConnections(); Curves.Reset(); Room->GetComponents(Curves);
    TestEqual(TEXT("Rebuild keeps component count"),Curves.Num(),Before);
    World->DestroyWorld(false); return true;
}
#endif
