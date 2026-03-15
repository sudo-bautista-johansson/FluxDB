// VeldraDBModule.cpp
#include "VeldraDBModule.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "FVeldraDBModule"

static void* VeldraDllHandle = nullptr;

void FVeldraDBModule::StartupModule()
{
#if PLATFORM_WINDOWS
    FString DllPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/VeldraDB/Source/ThirdParty/VeldraDB/Win64/veldra.dll"));
    if (!FPaths::FileExists(DllPath))
        DllPath = FPaths::Combine(FPaths::LaunchDir(), TEXT("veldra.dll")); // Packaged game fallback

    VeldraDllHandle = FPlatformProcess::GetDllHandle(*DllPath);
    if (!VeldraDllHandle)
        UE_LOG(LogTemp, Error, TEXT("[VeldraDB] Failed to load native veldra.dll from: %s"), *DllPath);
    else
        UE_LOG(LogTemp, Log, TEXT("[VeldraDB] Native Engine loaded successfully."));
#endif
}

void FVeldraDBModule::ShutdownModule()
{
    if (VeldraDllHandle)
    {
        FPlatformProcess::FreeDllHandle(VeldraDllHandle);
        VeldraDllHandle = nullptr;
    }
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FVeldraDBModule, VeldraDB)
