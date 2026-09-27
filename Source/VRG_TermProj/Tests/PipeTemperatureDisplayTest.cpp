#include "Misc/AutomationTest.h"
#include "Actors/RackThermalActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/CoolingDistributionUnitActor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipeTemperatureDisplayTest, "VRG.Visualization.Pipe.TemperatureSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FPipeTemperatureDisplayTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("World"), World)) return false;
	auto* Rack = World->SpawnActor<ARackThermalActor>();
	auto* Cdu = World->SpawnActor<ACoolingDistributionUnitActor>();
	auto* Room = World->SpawnActor<ADataCenterRoomActor>();
	Rack->SetActorLocation(FVector(200,0,0));
	Rack->ResetSim(); Rack->PauseSim();
	Room->bShowSummary = false;
	Room->RefreshGjConnections();
	Rack->Input.CoolantInletTempK = 313.15; // Synthetic display sample, not a physical operating point.
	const auto Before = Rack->GetCoolantDisplaySnapshot();
	auto& Network = Room->GetMutableNetwork();
	const int32 Supply = Room->GetRackBranchPipeIndices()[0];
	const int32 RackNode = Network.GetPipeToNodeIndex(Supply);
	int32 Return = INDEX_NONE;
	for (int32 I=0; I<Network.GetNumPipes(); ++I)
		if (Network.GetPipeFromNodeIndex(I)==RackNode) Return=I;
	double TempK=0;
	TestTrue(TEXT("Supply has temperature"), Room->TryGetPipeTemperatureK(Supply, TempK));
	TestEqual(TEXT("Supply reads inlet K"), TempK, Before.InletTempK);
	TestTrue(TEXT("Return has temperature"), Room->TryGetPipeTemperatureK(Return, TempK));
	TestEqual(TEXT("Return reads lumped coolant K"), TempK, Before.OutletTempK);
	TestFalse(TEXT("Pump has no invented temperature"), Room->TryGetPipeTemperatureK(-1, TempK));
	for (int32 I=0; I<Network.GetNumPipes(); ++I)
		if (I!=Supply && I!=Return) TestFalse(TEXT("Header temperature unknown"), Room->TryGetPipeTemperatureK(I,TempK));
	const double FlowBefore = Network.GetPipeFlowM3s(Supply);
	IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("vrg.Pipes.Temperature"));
	const int32 OldMode=Mode->GetInt();
	Mode->Set(1,ECVF_SetByCode); Room->Tick(0);
	TArray<UStaticMeshComponent*> Meshes; Room->GetComponents(Meshes);
	bool bFoundTemperatureColor=false;
	for (auto* Mesh:Meshes)
		if (auto* Material=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0)))
			bFoundTemperatureColor |= Material->K2_GetVectorParameterValue(TEXT("Color")).Equals(PipeTemperatureDisplay::Color(Before.InletTempK,true),1e-6f);
	TestTrue(TEXT("Temperature reaches mesh material"), bFoundTemperatureColor);
	Mode->Set(0,ECVF_SetByCode); Room->Tick(0);
	TestEqual(TEXT("Display leaves hydraulic flow unchanged"),Network.GetPipeFlowM3s(Supply),FlowBefore);
	TestEqual(TEXT("Display leaves thermal state unchanged"),Rack->GetCoolantDisplaySnapshot().OutletTempK,Before.OutletTempK);
	Rack->Input.CoolantMassFlowKgs=0;
	TestFalse(TEXT("No thermal transport sample at zero flow"),Room->TryGetPipeTemperatureK(Supply,TempK));
	TestTrue(TEXT("Unknown uses grey"),PipeTemperatureDisplay::Color(300,false)==PipeTemperatureDisplay::UnknownColor());
	TestTrue(TEXT("Display saturates only the colour"),PipeTemperatureDisplay::Color(373.15,true)==PipeTemperatureDisplay::Color(333.15,true));
	Mode->Set(OldMode,ECVF_SetByCode);
	World->DestroyWorld(false);
	return true;
}
#endif
