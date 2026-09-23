#include "Misc/AutomationTest.h"
#include "Actors/RackThermalActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
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
    Room->bUseGjConnections=true; Room->RefreshGjConnections();
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
    if(TestNotNull(TEXT("First node"),FirstNode) && TestNotNull(TEXT("First harness"),FirstHarness))
    {
        TestTrue(TEXT("Docked module pivot"),FirstNode->GetRelativeLocation().Equals(FVector(5.5,0,39.2),.001));
        A->SetSledDocked(0,false); A->Tick(.5f);
        TestTrue(TEXT("Slide preserves authored base X"),FMath::IsNearlyEqual(FirstNode->GetRelativeLocation().X,95.5,.001));
        TestFalse(TEXT("Undocked quick disconnect harness hidden"),FirstHarness->IsVisible());
        Room->Tick(0); TestTrue(TEXT("Undock rebuild consumed by coupling"),Room->ConsumeRebuiltFlag());
        A->SetSledDocked(0,true); A->Tick(.5f); Room->Tick(0);
        TestTrue(TEXT("Redock harness"),FirstHarness->IsVisible());
    }
    TArray<USplineMeshComponent*> Curves; Room->GetComponents(Curves);
    TestTrue(TEXT("Curved connections created"),Curves.Num()>0);
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
