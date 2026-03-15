# ==============================================================================
#  VeldraDB - Unreal Engine Plugin Integration Script
#  Usage:
#    .\install_unreal.ps1 -UnrealProjectPath "C:\MyUEGame" [-SkipBuild]
# ==============================================================================
param(
    [Parameter(Mandatory=$true)]
    [string]$UnrealProjectPath,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$VeldraRoot = Split-Path -Parent $ScriptDir
$BuildDir   = Join-Path $VeldraRoot "build_unreal"

$PluginRoot = Join-Path $UnrealProjectPath "Plugins\VeldraDB"
$SourceRoot = Join-Path $PluginRoot "Source\VeldraDB"
$BinDir     = Join-Path $PluginRoot "Binaries\Win64"
$ThirdParty = Join-Path $PluginRoot "Source\ThirdParty\VeldraDB"
$IncludeDir = Join-Path $ThirdParty "Include"
$LibDir     = Join-Path $ThirdParty "Win64"

# --- 1. Build veldra.dll ------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "[VeldraDB-UE] Building native DLL (CMake Release x64)..." -ForegroundColor Cyan
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }
    Push-Location $BuildDir
    try {
        cmake $VeldraRoot -G "Visual Studio 17 2022" -A x64
        cmake --build . --config Release --target veldra
    }
    finally {
        Pop-Location
    }
    $BuiltDll = Join-Path $BuildDir "Release\veldra.dll"
    if (-not (Test-Path $BuiltDll)) {
        $BuiltDll = Join-Path $BuildDir "veldra.dll"
    }
    if (-not (Test-Path $BuiltDll)) {
        throw "[VeldraDB-UE] Build failed - veldra.dll not found in $BuildDir"
    }
}
else {
    Write-Host "[VeldraDB-UE] Skipping build (-SkipBuild)." -ForegroundColor Yellow
    $BuiltDll = Join-Path $VeldraRoot "bindings\c#\veldra.dll"
    if (-not (Test-Path $BuiltDll)) {
        throw "[VeldraDB-UE] Pre-built veldra.dll not found at $BuiltDll"
    }
}

# --- 2. Validate UE project ---------------------------------------------------
$UprojectFiles = Get-ChildItem -Path $UnrealProjectPath -Filter "*.uproject" -ErrorAction SilentlyContinue
if ($UprojectFiles.Count -eq 0) {
    throw "[VeldraDB-UE] No .uproject found in '$UnrealProjectPath'."
}
Write-Host "[VeldraDB-UE] Found project: $($UprojectFiles[0].Name)" -ForegroundColor Cyan

# --- 3. Create folder structure -----------------------------------------------
Write-Host "[VeldraDB-UE] Creating plugin folder structure..." -ForegroundColor Cyan
foreach ($d in @($PluginRoot, $SourceRoot, "$SourceRoot\Public", "$SourceRoot\Private", $BinDir, $IncludeDir, $LibDir)) {
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}

# --- 4. Copy native binaries & headers ----------------------------------------
Write-Host "[VeldraDB-UE] Copying native files..." -ForegroundColor Cyan
Copy-Item -Path $BuiltDll -Destination (Join-Path $LibDir "veldra.dll") -Force
Copy-Item -Path (Join-Path $VeldraRoot "core\headers\veldra_c_api.h") `
          -Destination (Join-Path $IncludeDir "veldra_c_api.h") -Force

$ImportLib = Join-Path (Split-Path $BuiltDll) "veldra.lib"
if (Test-Path $ImportLib) {
    Copy-Item -Path $ImportLib -Destination (Join-Path $LibDir "veldra.lib") -Force
    Write-Host "[VeldraDB-UE] veldra.lib copied." -ForegroundColor Cyan
}

# --- 5. VeldraDB.uplugin ------------------------------------------------------
Set-Content -Path (Join-Path $PluginRoot "VeldraDB.uplugin") -Encoding UTF8 -Value @'
{
    "FileVersion": 3,
    "Version": 1,
    "VersionName": "0.1.0",
    "FriendlyName": "VeldraDB",
    "Description": "Embedded ECS + Spatial game database engine, accessible via Blueprints and C++.",
    "Category": "Database",
    "CreatedBy": "Veldra Engine Team",
    "CreatedByURL": "https://github.com/yourusername/veldradb",
    "DocsURL": "",
    "MarketplaceURL": "",
    "SupportURL": "",
    "CanContainContent": false,
    "IsBetaVersion": true,
    "IsExperimentalVersion": false,
    "Installed": false,
    "Modules": [
        {
            "Name": "VeldraDB",
            "Type": "Runtime",
            "LoadingPhase": "Default"
        }
    ]
}
'@

# --- 6. VeldraDB.Build.cs -----------------------------------------------------
Set-Content -Path (Join-Path $SourceRoot "VeldraDB.Build.cs") -Encoding UTF8 -Value @'
// VeldraDB.Build.cs - Unreal Build Tool module rules
using UnrealBuildTool;
using System.IO;

public class VeldraDB : ModuleRules
{
    public VeldraDB(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty", "VeldraDB");
        string IncPath        = Path.Combine(ThirdPartyPath, "Include");
        string LibPath        = Path.Combine(ThirdPartyPath, "Win64");

        PublicIncludePaths.Add(IncPath);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string ImportLib = Path.Combine(LibPath, "veldra.lib");
            if (File.Exists(ImportLib))
                PublicAdditionalLibraries.Add(ImportLib);

            RuntimeDependencies.Add(
                "$(BinaryOutputDir)/veldra.dll",
                Path.Combine(LibPath, "veldra.dll")
            );
            PublicDelayLoadDLLs.Add("veldra.dll");
        }
    }
}
'@

# --- 7. VeldraDBModule.h ------------------------------------------------------
Set-Content -Path (Join-Path $SourceRoot "Public\VeldraDBModule.h") -Encoding UTF8 -Value @'
// VeldraDBModule.h
#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FVeldraDBModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
'@

# --- 8. VeldraDBModule.cpp ----------------------------------------------------
Set-Content -Path (Join-Path $SourceRoot "Private\VeldraDBModule.cpp") -Encoding UTF8 -Value @'
// VeldraDBModule.cpp
#include "VeldraDBModule.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "FVeldraDBModule"

static void* VeldraDllHandle = nullptr;

void FVeldraDBModule::StartupModule()
{
#if PLATFORM_WINDOWS
    FString DllPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Win64/veldra.dll"));
    if (!FPaths::FileExists(DllPath))
        DllPath = FPaths::Combine(FPaths::LaunchDir(), TEXT("veldra.dll"));

    VeldraDllHandle = FPlatformProcess::GetDllHandle(*DllPath);
    if (!VeldraDllHandle)
        UE_LOG(LogTemp, Error, TEXT("[VeldraDB] Failed to load veldra.dll from: %s"), *DllPath);
    else
        UE_LOG(LogTemp, Log, TEXT("[VeldraDB] veldra.dll loaded successfully."));
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
'@

# --- 9. VeldraDBSubsystem.h ---------------------------------------------------
Set-Content -Path (Join-Path $SourceRoot "Public\VeldraDBSubsystem.h") -Encoding UTF8 -Value @'
// VeldraDBSubsystem.h
// UGameInstanceSubsystem - access from Blueprints via "Get VeldraDB Subsystem"
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

    /** Execute a VeldraDB query. Returns result as a string (JSON). */
    UFUNCTION(BlueprintCallable, Category = "VeldraDB",
              meta = (DisplayName = "Execute VeldraDB Query"))
    FString Query(const FString& SQL);

    /** Returns true if VeldraDB was successfully initialized. */
    UFUNCTION(BlueprintPure, Category = "VeldraDB")
    bool IsReady() const { return DbHandle != nullptr; }

private:
    VeldraDB* DbHandle = nullptr;
};
'@

# --- 10. VeldraDBSubsystem.cpp ------------------------------------------------
Set-Content -Path (Join-Path $SourceRoot "Private\VeldraDBSubsystem.cpp") -Encoding UTF8 -Value @'
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
        UE_LOG(LogVeldraDB, Error, TEXT("[VeldraDB] Init failed: %s"),
               Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }
    else
    {
        UE_LOG(LogVeldraDB, Log, TEXT("[VeldraDB] Initialized successfully."));
    }
}

void UVeldraDBSubsystem::Deinitialize()
{
    if (DbHandle)
    {
        veldra_close(DbHandle);
        DbHandle = nullptr;
        UE_LOG(LogVeldraDB, Log, TEXT("[VeldraDB] Shut down."));
    }
    Super::Deinitialize();
}

FString UVeldraDBSubsystem::Query(const FString& SQL)
{
    if (!DbHandle)
        return TEXT("[VeldraDB] Database not initialized.");

    std::string NativeSQL(TCHAR_TO_UTF8(*SQL));
    VeldraResult* Result = veldra_query(DbHandle, NativeSQL.c_str());

    if (!Result)
    {
        const char* Err = veldra_get_last_error();
        return FString::Printf(TEXT("[VeldraDB] Query error: %s"),
                               Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown"));
    }

    const char* Text = veldra_result_get_text(Result);
    FString Output   = Text ? UTF8_TO_TCHAR(Text) : TEXT("");
    veldra_free_result(Result);
    return Output;
}
'@

# --- Done ---------------------------------------------------------------------
Write-Host ""
Write-Host "[VeldraDB-UE] Integration complete!" -ForegroundColor Green
Write-Host "Plugin installed to: $PluginRoot" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Right-click your .uproject -> Generate Visual Studio project files"
Write-Host "  2. Build: Development Editor / Win64"
Write-Host "  3. Enable in editor: Edit -> Plugins -> search 'VeldraDB' -> Enable"
Write-Host "  4. Blueprints: Get VeldraDB Subsystem -> Execute VeldraDB Query"
Write-Host "  5. C++: GetGameInstance()->GetSubsystem<UVeldraDBSubsystem>()->Query(TEXT(...))"
