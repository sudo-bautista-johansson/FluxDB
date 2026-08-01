// FluxDBSubsystem.cpp
#include "FluxDBSubsystem.h"
#include "Logging/LogMacros.h"
#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogFluxDB, Log, All);

void UFluxDBSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    DbHandle = flux_init();
    if (!DbHandle)
    {
        const char* Err = flux_get_last_error();
        UE_LOG(LogFluxDB, Error, TEXT("[FluxDB] Initialization failed: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }
    else
    {
        UE_LOG(LogFluxDB, Log, TEXT("[FluxDB] Instance initialized correctly."));
    }
}

void UFluxDBSubsystem::Deinitialize()
{
    if (DbHandle)
    {
        flux_close(DbHandle);
        DbHandle = nullptr;
        UE_LOG(LogFluxDB, Log, TEXT("[FluxDB] Instance destroyed."));
    }
    Super::Deinitialize();
}

FString UFluxDBSubsystem::Query(const FString& SQL)
{
    if (!DbHandle) return TEXT("[FluxDB] Database not initialized.");

    std::string NativeSQL(TCHAR_TO_UTF8(*SQL));
    FluxResult* Result = flux_query(DbHandle, NativeSQL.c_str());

    if (!Result)
    {
        const char* Err = flux_get_last_error();
        return FString::Printf(TEXT("[FluxDB] Query error: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }

    const char* Text = flux_result_get_text(Result);
    FString Output = Text ? UTF8_TO_TCHAR(Text) : TEXT("");
    flux_free_result(Result);
    return Output;
}

void UFluxDBSubsystem::AdvanceTick()
{
    if (!DbHandle) return;
    flux_advance_tick(DbHandle);
}

uint64 UFluxDBSubsystem::GetCurrentTick() const
{
    if (!DbHandle) return 0;
    return flux_get_current_tick(DbHandle);
}

int32 UFluxDBSubsystem::GetDeltaPayload(uint64 LastAckTick, TArray<uint8>& OutPayload)
{
    OutPayload.Reset();
    if (!DbHandle) return 0;

    constexpr size_t MaxDeltaBytes = 65536;
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(MaxDeltaBytes);

    size_t Written = flux_get_delta_payload(DbHandle, LastAckTick, Buffer.GetData(), static_cast<size_t>(Buffer.Num()));
    if (Written == 0)
        return 0;

    OutPayload = Buffer;
    OutPayload.SetNum(static_cast<int32>(Written), false);
    return static_cast<int32>(Written);
}

bool UFluxDBSubsystem::RunScript(const FString& LuaCode)
{
    if (!DbHandle) return false;
    std::string NativeLua(TCHAR_TO_UTF8(*LuaCode));
    return flux_run_script(DbHandle, NativeLua.c_str());
}
