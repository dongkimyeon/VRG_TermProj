#include "Visualization/PatchPlanWidget.h"
#include "Interaction/DataCenterDebugController.h"
#include "Actors/DataCenterRoomActor.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

void UPatchPlanWidget::SetRoom(ADataCenterRoomActor* InRoom)
{
	Room=InRoom;
	SetIsFocusable(true);
}

void UPatchPlanWidget::NativeTick(const FGeometry& Geometry,float DeltaSeconds)
{
	Super::NativeTick(Geometry,DeltaSeconds);
	RefreshSeconds+=DeltaSeconds;
	if (RefreshSeconds<.5f) return;
	RefreshSeconds=0;
	Snapshot={}; Devices.Reset(); BoundsM=FBox(ForceInit);
	if (Room.IsValid()) { Snapshot=Room->GetPatchPlan(); Devices=Room->GetPatchDevices(); }
	for (const Patch::FCable& Cable:Snapshot.Cables) for (const FVector& Point:Cable.PointsM) BoundsM+=Point;
	for (const Patch::FDevice& Device:Devices)
		for (int32 X:{-1,1}) for (int32 Y:{-1,1})
			BoundsM+=Device.ToWorld.TransformPosition(FVector(X*Device.HalfExtentM.X,Y*Device.HalfExtentM.Y,0));
	Selected=FMath::Clamp(Selected,0,FMath::Max(0,Snapshot.Cables.Num()-1));
}

FVector2D UPatchPlanWidget::Project(const FVector& PointM,const FVector2D& Size) const
{
	if (!BoundsM.IsValid) return Size*.5;
	const FVector Extent=BoundsM.GetSize();
	const double Scale=FMath::Max(1.0,FMath::Min((Size.X-100)/FMath::Max(Extent.X,.1),(Size.Y-230)/FMath::Max(Extent.Y,.1)));
	const FVector Center=BoundsM.GetCenter();
	return FVector2D(Size.X*.5+(PointM.X-Center.X)*Scale,120+(Size.Y-230)*.5-(PointM.Y-Center.Y)*Scale);
}

int32 UPatchPlanWidget::NativePaint(const FPaintArgs& Args,const FGeometry& Geometry,const FSlateRect& CullingRect,
	FSlateWindowElementList& Elements,int32 Layer,const FWidgetStyle& Style,bool ParentEnabled) const
{
	const FVector2D Size=Geometry.GetLocalSize();
	FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(),FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,FLinearColor(.012f,.02f,.035f,.97f));
	auto Text=[&](const FVector2D& Position,const FString& Value,const FLinearColor& Color)
	{
		FSlateDrawElement::MakeText(Elements,Layer+3,Geometry.ToPaintGeometry(FVector2f(Size),FSlateLayoutTransform(FVector2f(Position))),Value,
			FCoreStyle::GetDefaultFontStyle("Regular",14),ESlateDrawEffect::None,Color);
	};
	auto Line=[&](const TArray<FVector>& Points,const FLinearColor& Color,float Width)
	{
		TArray<FVector2D> Projected; for (const FVector& Point:Points) Projected.Add(Project(Point,Size));
		if (Projected.Num()>1) FSlateDrawElement::MakeLines(Elements,Layer+1,Geometry.ToPaintGeometry(),Projected,ESlateDrawEffect::None,Color,true,Width);
	};
	Text({24,20},TEXT("PATCH MAP | world XY (m) | click cable / Left-Right select | Escape close"),FLinearColor::White);
	Text({24,48},FString::Printf(TEXT("%d cables | %d unconnected | %d rejected port claims | %d interfering pairs"),
		Snapshot.Cables.Num(),Snapshot.Unconnected.Num(),Snapshot.RejectedClaims,Snapshot.Interferences.Num()),FLinearColor(.7f,.85f,1));
	Text({24,76},TEXT("Cyan: uplink  White: spine  Red: 3D segment interference  Yellow: selected"),FLinearColor::White);
	for (const Patch::FDevice& Device:Devices)
	{
		TArray<FVector> Corners;
		for (FVector Point:{FVector(-1,-1,0),FVector(1,-1,0),FVector(1,1,0),FVector(-1,1,0),FVector(-1,-1,0)})
			Corners.Add(Device.ToWorld.TransformPosition(Point*Device.HalfExtentM));
		Line(Corners,FLinearColor(.3f,.6f,.35f),2);
		Text(Project(Device.ToWorld.GetLocation(),Size),Device.Label,FLinearColor(.6f,.9f,.6f));
	}
	TSet<int32> Interfering;
	for (const Patch::FInterference& Hit:Snapshot.Interferences) { Interfering.Add(Hit.CableA); Interfering.Add(Hit.CableB); }
	for (int32 Index=0;Index<Snapshot.Cables.Num();++Index)
	{
		const Patch::FCable& Cable=Snapshot.Cables[Index];
		Line(Cable.PointsM,Interfering.Contains(Index)?FLinearColor(1,.15f,.1f):Cable.bSpineToSpine?FLinearColor::White:FLinearColor(0,.7f,1),1);
	}
	if (Snapshot.Cables.IsValidIndex(Selected))
	{
		const Patch::FCable& Cable=Snapshot.Cables[Selected];
		Line(Cable.PointsM,FLinearColor::Yellow,3);
		double LengthM=0; for (int32 Index=1;Index<Cable.PointsM.Num();++Index) LengthM+=FVector::Distance(Cable.PointsM[Index-1],Cable.PointsM[Index]);
		auto Label=[&](int32 Index) { return Devices.IsValidIndex(Index)?Devices[Index].Label:TEXT("?"); };
		Text({24,Size.Y-80},FString::Printf(TEXT("Cable %d: %s port %d -> %s port %d | polyline %.3f m"),Selected+1,
			*Label(Cable.FromDevice),Cable.FromPort+1,*Label(Cable.ToDevice),Cable.ToPort+1,LengthM),FLinearColor::Yellow);
	}
	Text({24,Size.Y-48},TEXT("Diagnostic only: rounded bends, connectors, obstacles and bend radius are not certified."),FLinearColor(.8f,.8f,.8f));
	return Layer+4;
}

FReply UPatchPlanWidget::NativeOnMouseButtonDown(const FGeometry& Geometry,const FPointerEvent& Event)
{
	const FVector2D Mouse=Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
	double Best=12; // UI hit tolerance in Slate units, not physical clearance.
	for (int32 I=0;I<Snapshot.Cables.Num();++I)
		for (int32 J=1;J<Snapshot.Cables[I].PointsM.Num();++J)
		{
			const FVector2D A=Project(Snapshot.Cables[I].PointsM[J-1],Geometry.GetLocalSize()),B=Project(Snapshot.Cables[I].PointsM[J],Geometry.GetLocalSize());
			const FVector2D Edge=B-A;
			const double Alpha=FMath::Clamp(FVector2D::DotProduct(Mouse-A,Edge)/FMath::Max(Edge.SizeSquared(),1e-12),0.0,1.0);
			const double Distance=(Mouse-A-Alpha*Edge).Size();
			if (Distance<Best) { Best=Distance; Selected=I; }
		}
	return FReply::Handled();
}

void UPatchPlanWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->SetPatchMapOpen(true);
}

void UPatchPlanWidget::NativeDestruct()
{
	if (auto* Controller = Cast<ADataCenterDebugController>(GetOwningPlayer())) Controller->SetPatchMapOpen(false);
	Super::NativeDestruct();
}

FReply UPatchPlanWidget::NativeOnKeyDown(const FGeometry& Geometry,const FKeyEvent& Event)
{
	if (Event.GetKey()==EKeys::Escape)
	{
		RemoveFromParent();
		if (APlayerController* Controller=GetOwningPlayer(); Controller && !Cast<ADataCenterDebugController>(Controller))
		{ Controller->SetInputMode(FInputModeGameOnly()); Controller->bShowMouseCursor=false; }
		return FReply::Handled();
	}
	if (Event.GetKey()==EKeys::Left || Event.GetKey()==EKeys::Right)
	{
		Selected=FMath::Clamp(Selected+(Event.GetKey()==EKeys::Right?1:-1),0,FMath::Max(0,Snapshot.Cables.Num()-1));
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(Geometry,Event);
}

static FAutoConsoleCommandWithWorld OpenPatchMap(TEXT("vrg.Patch.Map"),TEXT("Open patch plan; Escape closes."),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
	{
		if (!World || !World->IsGameWorld()) return;
		APlayerController* Controller=World->GetFirstPlayerController();
		if (!Controller) return;
		for (TActorIterator<ADataCenterRoomActor> It(World);It;++It)
		{
			UPatchPlanWidget* Widget=CreateWidget<UPatchPlanWidget>(Controller);
			Widget->SetRoom(*It); Widget->AddToViewport(100);
			FInputModeGameAndUI Input; Input.SetWidgetToFocus(Widget->TakeWidget());
			Controller->SetInputMode(Input); Controller->bShowMouseCursor=true;
			UE_LOG(LogTemp,Display,TEXT("[PatchMap] %s"),*It->GetPatchSummary());
			break;
		}
	}));
