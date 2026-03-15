// VeldraDBSubsystem.cpp
#include "VeldraDBSubsystem.h"
#include "Logging/LogMacros.h"
#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogVeldraDB, Log, All);

void UVeldraDBSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    DbHandle = veldra_init();
    if (!DbHandle)
    {
        const char* Err = veldra_get_last_error();
        UE_LOG(LogVeldraDB, Error, TEXT("[VeldraDB] Initialization failed: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }
    else
    {
        UE_LOG(LogVeldraDB, Log, TEXT("[VeldraDB] Instance initialized correctly."));
    }
}

void UVeldraDBSubsystem::Deinitialize()
{
    if (DbHandle)
    {
        veldra_close(DbHandle);
        DbHandle = nullptr;
        UE_LOG(LogVeldraDB, Log, TEXT("[VeldraDB] Instance destroyed."));
    }
    Super::Deinitialize();
}

FString UVeldraDBSubsystem::Query(const FString& SQL)
{
    if (!DbHandle) return TEXT("[VeldraDB] Database not initialized.");

    std::string NativeSQL(TCHAR_TO_UTF8(*SQL));
    VeldraResult* Result = veldra_query(DbHandle, NativeSQL.c_str());

    if (!Result)
    {
        const char* Err = veldra_get_last_error();
        return FString::Printf(TEXT("[VeldraDB] Query error: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }

    const char* Text = veldra_result_get_text(Result);
    FString Output = Text ? UTF8_TO_TCHAR(Text) : TEXT("");
    veldra_free_result(Result);
    return Output;
}
