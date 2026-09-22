#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GB200PatchCable.generated.h"
class USplineComponent;
class USplineMeshComponent;
class UStaticMeshComponent;

// Visual authoring only. Coordinates exposed in cm at the Unreal boundary.
UCLASS(Blueprintable)
class GB200CONNECTIONS_API AGB200PatchCable : public AActor
{
    GENERATED_BODY()
public:
    AGB200PatchCable();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Connection") TObjectPtr<USplineComponent> Path;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TObjectPtr<UStaticMesh> BodyMesh;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TObjectPtr<UMaterialInterface> BodyMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TObjectPtr<UStaticMesh> StartPlugMesh;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TObjectPtr<UStaticMesh> EndPlugMesh;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") TObjectPtr<AActor> StartAnchor;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") TObjectPtr<AActor> EndAnchor;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FString StartPortId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FString EndPortId;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FVector StartOffsetCm=FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FVector EndOffsetCm=FVector(100,0,0);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FVector StartOutward=FVector::ForwardVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") FVector EndOutward=-FVector::ForwardVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch", meta=(ClampMin="0")) float StartPlugLengthCm=4.4f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch", meta=(ClampMin="0")) float EndPlugLengthCm=4.4f;
    // Auto mode follows explicit anchor pairs; it does not infer network topology or avoid obstacles.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch") bool bAutoPath=true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch", meta=(ClampMin="0")) float SagCm=12.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch", meta=(ClampMin="1")) float LeadCm=12.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Patch", meta=(ClampMin="1")) float MaxSegmentCm=20.f;
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Connection") void RebuildConnection();
    UFUNCTION(BlueprintPure, Category="Connection") int32 GetVisualSegmentCount() const { return Segments.Num(); }
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
private:
    UPROPERTY(Transient) TArray<TObjectPtr<USplineMeshComponent>> Segments;
    UPROPERTY(VisibleAnywhere, Category="Connection") TObjectPtr<UStaticMeshComponent> StartPlug;
    UPROPERTY(VisibleAnywhere, Category="Connection") TObjectPtr<UStaticMeshComponent> EndPlug;
    FVector LastStart=FVector::ZeroVector,LastEnd=FVector::ZeroVector;
    FVector LastStartDir=FVector::ZeroVector,LastEndDir=FVector::ZeroVector;
    void ResolveEnds(FVector& A,FVector& B,FVector& DA,FVector& DB) const;
};

