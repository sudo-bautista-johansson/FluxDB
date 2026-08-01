// FluxDBSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "flux_c_api.h"
#include "FluxDBSubsystem.generated.h"

UCLASS()
class FLUXDB_API UFluxDBSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "FluxDB", meta = (DisplayName = "Execute FluxDB Query"))
    FString Query(const FString& SQL);

    UFUNCTION(BlueprintCallable, Category = "FluxDB", meta = (DisplayName = "Advance FluxDB Tick"))
    void AdvanceTick();

    UFUNCTION(BlueprintPure, Category = "FluxDB")
    uint64 GetCurrentTick() const;

    UFUNCTION(BlueprintCallable, Category = "FluxDB", meta = (DisplayName = "Get FluxDB Delta Payload"))
    int32 GetDeltaPayload(uint64 LastAckTick, TArray<uint8>& OutPayload);

    UFUNCTION(BlueprintCallable, Category = "FluxDB", meta = (DisplayName = "Run FluxDB Lua Script"))
    bool RunScript(const FString& LuaCode);

    UFUNCTION(BlueprintPure, Category = "FluxDB")
    bool IsReady() const { return DbHandle != nullptr; }

private:
    FluxDB* DbHandle = nullptr;
};
