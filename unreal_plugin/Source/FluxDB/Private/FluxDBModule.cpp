// FluxDBModule.cpp
#include "FluxDBModule.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "FFluxDBModule"

static void* FluxDllHandle = nullptr;

void FFluxDBModule::StartupModule()
{
#if PLATFORM_WINDOWS
    FString DllPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/FluxDB/Source/ThirdParty/FluxDB/Win64/flux.dll"));
    if (!FPaths::FileExists(DllPath))
        DllPath = FPaths::Combine(FPaths::LaunchDir(), TEXT("flux.dll")); // Packaged game fallback

    FluxDllHandle = FPlatformProcess::GetDllHandle(*DllPath);
    if (!FluxDllHandle)
        UE_LOG(LogTemp, Error, TEXT("[FluxDB] Failed to load native flux.dll from: %s"), *DllPath);
    else
        UE_LOG(LogTemp, Log, TEXT("[FluxDB] Native Engine loaded successfully."));
#endif
}

void FFluxDBModule::ShutdownModule()
{
    if (FluxDllHandle)
    {
        FPlatformProcess::FreeDllHandle(FluxDllHandle);
        FluxDllHandle = nullptr;
    }
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FFluxDBModule, FluxDB)
