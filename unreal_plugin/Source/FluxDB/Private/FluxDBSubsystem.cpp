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
