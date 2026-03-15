// VeldraDBSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "veldra_c_api.h"
#include "VeldraDBSubsystem.generated.h"

UCLASS()
class VELDRADB_API UVeldraDBSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "VeldraDB", meta = (DisplayName = "Execute VeldraDB Query"))
    FString Query(const FString& SQL);

    UFUNCTION(BlueprintPure, Category = "VeldraDB")
    bool IsReady() const { return DbHandle != nullptr; }

private:
    VeldraDB* DbHandle = nullptr;
};
