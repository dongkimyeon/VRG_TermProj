#include "Misc/AutomationTest.h"
#include "Interaction/DataCenterDebugController.h"
#include "Interaction/VrgCduLifecycle.h"
#include "Interaction/VrgLayoutFile.h"
#include "Actors/DataCenterSimulationActor.h"
#include "Actors/DataCenterRoomActor.h"
#include "Actors/AirFieldDebugActor.h"
#include "Visualization/GjRackLayout.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLayoutCduPersistenceTest,"VRG.Interaction.Layout.CduPersistenceCoupling",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)
bool FLayoutCduPersistenceTest::RunTest(const FString& Parameters)
{
 auto NewWorld=[]() { auto* W=UWorld::CreateWorld(EWorldType::Game,false); GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); return W; };
 UWorld* W=NewWorld(); auto* Sim=W->SpawnActor<ADataCenterSimulationActor>(); Sim->bShowSummary=false;
 auto* C=W->SpawnActor<ADataCenterDebugController>(); auto* Room=W->SpawnActor<ADataCenterRoomActor>(); Room->bUseGjConnections=true;
 auto* Base=W->SpawnActor<ACoolingDistributionUnitActor>(FVector(-500,-150,0),FRotator::ZeroRotator);
 auto* Air=W->SpawnActor<AAirFieldDebugActor>(); auto* Rack=W->SpawnActor<ARackThermalActor>();
 Rack->ApplyGb200Nvl72Preset(); Rack->bUseGjModels=true; Rack->bShowReadout=false;
 Rack->RackSizeM=FVector(GjRackLayout::FrameDepthM,GjRackLayout::FrameWidthM,GjRackLayout::FrameHeightM);
 Room->RebuildRoomNow(); Sim->EnterSimulationMode(); Sim->Tick(.021f);
 const double BaselineIt=Sim->GetSnapshot().TotalItHeatW; Sim->EnterLayoutMode();
 const int32 Pipes=Room->GetMutableNetwork().GetNumPipes();
 const FString File=TEXT("automation_cdu_")+FGuid::NewGuid().ToString(), BeforeFile=File+TEXT("_before"); FString Summary;
 TestTrue(TEXT("Save before addition"),VrgLayoutFile::Save(W,BeforeFile,Summary));
 C->BeginCduAddition(); C->UpdateRackAdditionAlongRay(FVector(-200,0,500),FVector(0,0,-1));
 auto* Added=C->ConfirmCduAddition();
 if(!TestNotNull(TEXT("Confirm one CDU"),Added)) { W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return false; }
 const FString Id=VrgCduLifecycle::Identity(Added); const FName Name=Added->GetFName(); const FTransform T=Added->GetActorTransform();
 const auto Config=Added->CaptureLayoutSettings();
 double MaxIt=0,MaxGlobal=0,MaxCoolant=0,MaxDiv=0,MaxHydraulic=0,MinPump=DBL_MAX,MaxPump=0,MinFlow=DBL_MAX,MaxFlow=0;
 auto Count=[&](UWorld* World) { int32 N=0; for(TActorIterator<ACoolingDistributionUnitActor> It(World);It;++It) ++N; return N; };
 auto Physics=[&](bool Present,const TCHAR* Stage) {
  auto* Expected=Present?VrgCduLifecycle::Find(W,Id):Base;
  TestTrue(TEXT("Nearest CDU assignment rebuilt; one rack/branch retained"),Expected && Room->GetRowCdu(0)==Expected
   && Room->GetRoutedRacks().Num()==1 && Room->GetMutableNetwork().GetNumPipes()==Pipes && Room->GetMutableNetwork().GetNumPumps()==1);
  const auto& H=Room->GetLastSolveResult();
  TestTrue(TEXT("Hydraulic convergence and reference scales"),H.bConverged && !H.bIllPosed && !H.bLinearSolveFailed
   && H.NormalizedResidual<Room->SolveSettings.NormalizedResidualTarget && H.ReferenceFlowM3s>0 && H.ReferencePressurePa>0);
  MaxHydraulic=FMath::Max(MaxHydraulic,H.NormalizedResidual);
  Sim->EnterSimulationMode(); Sim->Tick(.021f); const auto First=Sim->GetSnapshot();
  const double ItErr=FMath::Abs(First.TotalItHeatW-BaselineIt)/BaselineIt; MaxIt=FMath::Max(MaxIt,ItErr);
  TestTrue(TEXT("CDU never becomes a rack IT heat source"),ItErr<1e-9 && First.RackHeatToAirW.Num()==1 && First.CduDutyW.Num()==1);
  TestEqual(TEXT("Fresh simulation clock"),First.SimTimeS,Sim->FixedStepS);
  for(int32 I=0;I<25;++I) Sim->Tick(.04f); const auto S=Sim->GetSnapshot(); const auto AS=Air->GetMutableSolver().MakeSnapshot(S.SimTimeS);
  TestTrue(TEXT("Coolant group and rack coupling valid"),S.RackCoolantMassFlowKgs.Num()==1 && S.RackCoolantMassFlowKgs[0]>0
   && S.CduDutyW.Num()==1 && S.CduDutyW[0]>0 && S.CduReturnTempK[0]>S.CduSupplyTempK[0]
   && S.CduSupplyTempK[0]>=Expected->FacilitySupplyTempK && S.CduFacilityReturnTempK[0]>=Expected->FacilitySupplyTempK);
  const double HotWK=S.RackCoolantMassFlowKgs[0]*Rack->Params.CoolantCpJKgK;
  const double Coolant=FMath::Abs(S.CduDutyW[0]-HotWK*(S.CduReturnTempK[0]-S.CduSupplyTempK[0]))/S.CduDutyW[0];
  TestTrue(TEXT("Existing CDU coolant transfer ledger"),Coolant<1e-12 && S.TotalRadiatorRejectedW==S.CduDutyW[0]);
  const double Pump=Room->GetTotalPumpElectricalW(),Hydro=Room->GetMutableNetwork().GetPumpHydraulicPowerW(0);
  TestTrue(TEXT("D8 pump electrical power uses operating point"),Pump>0 && FMath::Abs(Pump-Hydro/Expected->PumpEfficiency)/Pump<1e-12);
  const auto Power=Sim->GetEvaluation().GetWindowPower();
  TestTrue(TEXT("Existing electrical ledger includes pump once, separate from IT"),Power.PumpW>0 && FMath::Abs(Power.PumpW-Pump)/Pump<1e-12);
  // Existing pump/IT plausibility limit; positive branch flow and hydraulic mass closure, not new fitted hardware numbers.
  // CLAUDE.md GB200 operating point; https://docs.nvidia.com/dgx/dgxgb200-user-guide/hardware.html
  const double Flow=S.RackCoolantMassFlowKgs[0];
  TestTrue(TEXT("Flow/pump scale safety net"),Flow>0 && Pump/S.TotalItHeatW<.05);
  TestTrue(TEXT("Hydraulic mass conservation"),FMath::Abs(Room->GetMutableNetwork().GetPumpFlowM3s(0)*Sim->CoolantDensityKgM3-Flow)/Flow<1e-9);
  TestTrue(TEXT("Global conservation and pressure convergence"),FMath::Abs(S.NormalizedEnergyResidual)<1e-9
   && S.bHydraulicsConverged && S.bAirPressureConverged && AS.bPressureConverged && AS.NormalizedDivergence<1e-5);
  MaxGlobal=FMath::Max(MaxGlobal,FMath::Abs(S.NormalizedEnergyResidual)); MaxCoolant=FMath::Max(MaxCoolant,Coolant); MaxDiv=FMath::Max(MaxDiv,AS.NormalizedDivergence);
  MinPump=FMath::Min(MinPump,Pump); MaxPump=FMath::Max(MaxPump,Pump); MinFlow=FMath::Min(MinFlow,Flow); MaxFlow=FMath::Max(MaxFlow,Flow);
  AddInfo(FString::Printf(TEXT("CDU %s: IT %.6fW err%.3e hydraulic%.3e refs %.6e m3/s %.6e Pa coolant%.3e global%.3e div%.3e pump%.6fW flow%.6fkg/s t%.2fs"),
   Stage,First.TotalItHeatW,ItErr,H.NormalizedResidual,H.ReferenceFlowM3s,H.ReferencePressurePa,Coolant,S.NormalizedEnergyResidual,AS.NormalizedDivergence,Pump,Flow,S.SimTimeS));
  Sim->EnterLayoutMode();
 };
 Physics(true,TEXT("confirm")); TestTrue(TEXT("Undo addition"),C->TryUndoLayout()); Physics(false,TEXT("undo"));
 TestTrue(TEXT("Redo addition"),C->TryRedoLayout()); Physics(true,TEXT("redo"));
 TestTrue(TEXT("Save CDU inventory"),VrgLayoutFile::Save(W,File,Summary));
 TestTrue(TEXT("Undo then load runtime CDU"),C->TryUndoLayout() && VrgLayoutFile::Load(W,File,Summary));
 TestTrue(TEXT("Repeated load is idempotent and resets history"),VrgLayoutFile::Load(W,File,Summary)
  && Summary.Contains(TEXT("CDU0 생성/0 제거")) && Count(W)==2 && C->GetLayoutUndoCount()==0 && C->GetLayoutRedoCount()==0);
 Added=VrgCduLifecycle::Find(W,Id); TestTrue(TEXT("Same identity/config/transform/visuals"),Added && Added->GetFName()==Name
  && Added->CaptureLayoutSettings()==Config && Added->GetActorTransform().Equals(T,1e-9) && Added->bRuntimeLayoutCdu);
 Physics(true,TEXT("load"));
 TestTrue(TEXT("Earlier inventory removes only extra runtime CDU"),VrgLayoutFile::Load(W,BeforeFile,Summary) && Count(W)==1 && Room->GetRowCdu(0)==Base);
 Physics(false,TEXT("before load")); TestTrue(TEXT("Restore inventory"),VrgLayoutFile::Load(W,File,Summary));
 // Synthetic non-default authored configuration/scale for serialization; not measured or new physics defaults.
 Added=VrgCduLifecycle::Find(W,Id); auto NonDefault=Config; NonDefault.PumpSpeedRatio=.8; NonDefault.PumpEfficiency=.65;
 NonDefault.FacilitySupplyTempK+=2; NonDefault.FacilityMassFlowKgs=3; NonDefault.HxUAWK=5000; NonDefault.bUseReferenceModel=false;
 NonDefault.SizeM*=1.1; NonDefault.ConnectionHeightM=.3; NonDefault.ConnectionSpacingM=.4;
 Added->ApplyLayoutSettings(NonDefault); Added->SetActorTransform(FTransform(FRotator(1,90,2),T.GetLocation(),FVector(1.1,.9,1.2)));
 const auto NonT=Added->GetActorTransform(); TestTrue(TEXT("Save nondefault config"),VrgLayoutFile::Save(W,File,Summary));
 UWorld* Fresh=NewWorld(); auto* FreshSim=Fresh->SpawnActor<ADataCenterSimulationActor>(); auto* FC=Fresh->SpawnActor<ADataCenterDebugController>();
 FActorSpawnParameters P; P.Name=Rack->GetFName(); Fresh->SpawnActor<ARackThermalActor>(P);
 P.Name=Base->GetFName(); auto* FreshBase=Fresh->SpawnActor<ACoolingDistributionUnitActor>(P);
 auto* UnlistedPlaced=Fresh->SpawnActor<ACoolingDistributionUnitActor>(FVector(3000,0,0),FRotator::ZeroRotator);
 TestTrue(TEXT("Fresh world/repeat load"),VrgLayoutFile::Load(Fresh,File,Summary) && VrgLayoutFile::Load(Fresh,File,Summary)
  && Count(Fresh)==3 && Summary.Contains(TEXT("CDU0 생성/0 제거")) && FC->GetLayoutUndoCount()==0 && FC->GetLayoutRedoCount()==0);
 const auto* Restored=VrgCduLifecycle::Find(Fresh,Id);
 TestTrue(TEXT("Nondefault settings/ID/name/transform restored"),Restored && Restored->bRuntimeLayoutCdu && Restored->GetFName()==Name
  && Restored->CaptureLayoutSettings()==NonDefault && Restored->GetActorTransform().Equals(NonT,1e-9));
 TestTrue(TEXT("Unlisted placed CDU preserved"),IsValid(UnlistedPlaced) && IsValid(FreshBase));
 TestEqual(TEXT("Load does not copy simulation clock"),FreshSim->GetSnapshot().SimTimeS,0.0);
 // Malformed/duplicate/provenance input must leave actors, history and transforms unchanged.
 FC->BeginCduAddition(); FC->UpdateRackAdditionAlongRay(FVector(-1800,0,500),FVector(0,0,-1));
 TestNotNull(TEXT("Create a real history record before malformed Load"),FC->ConfirmCduAddition());
 const int32 FailedLoadCount=Count(Fresh), FailedLoadUndo=FC->GetLayoutUndoCount();
 FString Json; FFileHelper::LoadFileToString(Json,*VrgLayoutFile::PathFor(File)); TSharedPtr<FJsonObject> Root;
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Root);
 auto Write=[&]() { FString Text; FJsonSerializer::Serialize(Root.ToSharedRef(),TJsonWriterFactory<>::Create(&Text)); FFileHelper::SaveStringToFile(Text,*VrgLayoutFile::PathFor(File)); };
 TSharedPtr<FJsonObject> Entry;
 for(const auto& V:Root->GetArrayField(TEXT("equipment"))) if(V->AsObject()->GetStringField(TEXT("name"))==Name.ToString()) Entry=V->AsObject();
 auto Settings=Entry->GetObjectField(TEXT("cduSettings")); const double SavedEfficiency=Settings->GetNumberField(TEXT("pumpEfficiency"));
 Settings->SetNumberField(TEXT("pumpEfficiency"),0); Write();
 TestFalse(TEXT("Out-of-range config rejected"),VrgLayoutFile::Load(Fresh,File,Summary));
 TestTrue(TEXT("Rejected load leaves inventory/transform/config intact"),Count(Fresh)==FailedLoadCount && FC->GetLayoutUndoCount()==FailedLoadUndo && Restored->GetActorTransform().Equals(NonT,1e-9) && Restored->CaptureLayoutSettings()==NonDefault);
 Settings->SetNumberField(TEXT("pumpEfficiency"),SavedEfficiency);
 Entry->SetStringField(TEXT("cduId"),VrgCduLifecycle::Identity(FreshBase)); Write(); TestFalse(TEXT("Duplicate ID rejected"),VrgLayoutFile::Load(Fresh,File,Summary)); Entry->SetStringField(TEXT("cduId"),Id);
 Entry->SetBoolField(TEXT("runtimeCdu"),false); Write(); TestFalse(TEXT("Changed provenance rejected"),VrgLayoutFile::Load(Fresh,File,Summary)); Entry->SetBoolField(TEXT("runtimeCdu"),true);
 TestTrue(TEXT("Failed load history still usable"),FC->TryUndoLayout() && Count(Fresh)==3);
 // Old v2/v1 files retain transform-only CDU handling and never delete absent runtime or placed CDUs.
 TArray<TSharedPtr<FJsonValue>> LegacyItems;
 for(const auto& V:Root->GetArrayField(TEXT("equipment"))) if(V->AsObject()->GetStringField(TEXT("name"))!=Name.ToString()) {
  auto E=V->AsObject(); E->RemoveField(TEXT("cduId")); E->RemoveField(TEXT("cduSettings")); E->RemoveField(TEXT("runtimeCdu")); LegacyItems.Add(V); }
 Root->SetArrayField(TEXT("equipment"),LegacyItems); Root->RemoveField(TEXT("cduInventory")); Write();
 TestTrue(TEXT("Legacy v2 CDU omission preserves inventory"),VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==3);
 Root->SetNumberField(TEXT("version"),1); Write(); TestTrue(TEXT("Legacy v1 transform path preserved"),VrgLayoutFile::Load(Fresh,File,Summary) && Count(Fresh)==3);
 AddInfo(FString::Printf(TEXT("CDU maxima: IT %.3e coolant %.3e global %.3e hydraulic %.3e div %.3e; pump %.6f..%.6fW flow %.6f..%.6fkg/s. Confirm/Undo/Redo/Load; fresh/repeat/legacy; power separate from IT; pump loss thermal split remains existing model debt."),
  MaxIt,MaxCoolant,MaxGlobal,MaxHydraulic,MaxDiv,MinPump,MaxPump,MinFlow,MaxFlow));
 for(const auto& F:{File,BeforeFile}) IFileManager::Get().Delete(*VrgLayoutFile::PathFor(F));
 Fresh->DestroyWorld(false); GEngine->DestroyWorldContext(Fresh); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
}
#endif

